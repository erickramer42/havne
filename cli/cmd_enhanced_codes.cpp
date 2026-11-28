#include "enhanced_codes_line.h"
#include "jsonl.h"

#include "commands.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"

#include <chrono>
#include <functional>
#include <thread>

namespace cmd {

namespace {

using clock = std::chrono::steady_clock;

constexpr uint64_t kMaxTimeoutS = 3600;

bool wait_for(clock::time_point deadline, const std::function<bool()>& done)
{
    while (!done()) {
        if (g_interrupted.load() || clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

bool parse_hex(const char* text, uint32_t max, uint32_t& out)
{
    if (text == nullptr || *text == '\0' || *text == '-' || *text == '+') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long v = std::strtoul(text, &end, 16);
    if (errno != 0 || *end != '\0' || v > max) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

struct Tally {
    uint32_t read = 0;
    uint32_t silent = 0;
    uint32_t not_reached = 0;
};

ecuio_status_t visit_module(ecuio_session_t* session, uint32_t c,
                            const EnhancedCodesArgs& args,
                            std::vector<char>& text, Output& out, Tally& t)
{
    const auto deadline = clock::now() + std::chrono::seconds(args.timeout_s);
    ecuio_channel_diagnostics_t standard;
    std::memset(&standard, 0, sizeof standard);
    bool have_standard = false;
    wait_for(deadline, [&] {
        if (ecuio_session_get_channel_diagnostics(session, c, &standard) !=
            ECUIO_OK) {
            return true;
        }
        have_standard = true;
        return (standard.flags & ECUIO_DIAG_ASKED) == 0 ||
               (standard.flags & ECUIO_DIAG_COMPLETE) != 0;
    });
    uint32_t read_id = 0;
    const ecuio_status_t st =
        ecuio_session_read_module_codes(session, c, args.mask, &read_id);
    if (st != ECUIO_OK) {
        ++t.not_reached;
        return st;
    }
    ecuio_module_codes_t codes;
    std::memset(&codes, 0, sizeof codes);
    const bool done = wait_for(deadline, [&] {
        return ecuio_session_get_module_codes(session, c, &codes) == ECUIO_OK &&
               (codes.flags & ECUIO_MODULE_CODES_COMPLETE) != 0 &&
               codes.read_id >= read_id;
    });
    if (!done) {
        ++t.not_reached;
        return ECUIO_OK;
    }
    cli::Line l(text.data(), text.size());
    cli::enhanced_codes_line(l, codes, have_standard ? &standard : nullptr);
    write_line(out, l);
    out.flush();
    if ((codes.flags & ECUIO_MODULE_CODES_NOT_OPENED) != 0) {
        ++t.not_reached;
    } else if (cli::codes_answered_nothing(codes)) {
        ++t.silent;
    } else {
        ++t.read;
    }
    return ECUIO_OK;
}

} // namespace

int run_enhanced_codes(const EnhancedCodesArgs& args)
{
    Output out;
    write_start(out);
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, args.uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        return ended(out, kExitConnect);
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    write_session(out, session, info);
    out.flush();
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        ecuio_session_destroy(session);
        return ended(out, kExitConnect);
    }

    std::vector<uint32_t> channels;
    const uint32_t count = module_count(info);
    for (uint32_t c = 0; c < count; ++c) {
        if (!args.has_ecu || info.rx_ids[c] == args.ecu) {
            channels.push_back(c);
        }
    }
    if (channels.empty() && count > 0) {
        std::fprintf(stderr, "havne-cli: no module answers at 0x%X\n",
                     static_cast<unsigned>(args.ecu));
        ecuio_session_destroy(session);
        return ended(out, kExitUsage);
    }
    if (count == 0) {
        channels.push_back(0);
    }

    std::vector<char> text(cli::kEnhancedCodesLineBytes);
    Tally t;
    bool first = true;
    for (const uint32_t c : channels) {
        st = visit_module(session, c, args, text, out, t);
        if (st != ECUIO_OK && first) {
            write_error(out, st);
            ecuio_session_destroy(session);
            return ended(out, kExitConnect);
        }
        first = false;
    }
    ecuio_session_destroy(session);
    const uint32_t visited = t.read + t.silent + t.not_reached;
    const uint32_t modules = static_cast<uint32_t>(channels.size());
    if (visited < modules) {
        t.not_reached += modules - visited;
    }
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::enhanced_codes_end_line(l, modules, t.read, t.silent, t.not_reached);
    write_line(out, l);
    out.flush();
    if (!out.ok()) {
        return kExitFailed;
    }
    return t.not_reached > 0 ? kExitFailed : kExitOk;
}

int cmd_enhanced_codes(int argc, char** argv)
{
    EnhancedCodesArgs a;
    for (int i = 2; i < argc; ++i) {
        const std::string o = argv[i];
        const bool valued = o == "--ecu" || o == "--mask" || o == "--timeout";
        if (valued && i + 1 < argc) {
            const char* v = argv[++i];
            uint64_t s = 0;
            uint32_t m = 0;
            if (o == "--ecu") {
                a.has_ecu = parse_hex(v, 0x1FFFFFFF, a.ecu);
                if (!a.has_ecu) {
                    std::fprintf(stderr,
                                 "havne-cli: --ecu takes a CAN ID in hex, "
                                 "not '%s'\n",
                                 v);
                    return kExitUsage;
                }
            } else if (o == "--mask") {
                if (!parse_hex(v, 0xFF, m)) {
                    std::fprintf(stderr,
                                 "havne-cli: --mask takes one byte in hex, "
                                 "not '%s'\n",
                                 v);
                    return kExitUsage;
                }
                a.mask = static_cast<uint8_t>(m);
            } else if (!parse_count(v, s) || s > kMaxTimeoutS) {
                std::fprintf(stderr,
                             "havne-cli: --timeout takes seconds (1 to %llu), "
                             "not '%s'\n",
                             static_cast<unsigned long long>(kMaxTimeoutS), v);
                return kExitUsage;
            } else {
                a.timeout_s = s;
            }
        } else if (valued || o.rfind("--", 0) == 0 || a.uri != nullptr) {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        } else {
            a.uri = argv[i];
        }
    }
    if (a.uri == nullptr) {
        std::fprintf(stderr, "havne-cli: enhanced-codes needs a URI\n");
        usage(stderr);
        return kExitUsage;
    }
    return run_enhanced_codes(a);
}

} // namespace cmd
