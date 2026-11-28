#include "conversation_lines.h"
#include "dump_lines.h"
#include "operation_lines.h"
#include "settings_home.h"

#include "commands.h"
#include "profile_source.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/dump.h"
#include "ecuio/operation.h"
#include "ecuio/plugin.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace cmd {

namespace {

struct DumpOption {
    const char* name;
    const char* words;
    const char* DumpArgs::* field;
};

constexpr DumpOption kDumpOptions[] = {
    {"--plugin", "--plugin takes one package", &DumpArgs::plugin},
    {"--out", "--out takes one folder", &DumpArgs::out},
    {"--operation", "--operation takes one plan's id", &DumpArgs::operation},
    {"--consent-store", "--consent-store takes one file",
     &DumpArgs::consent_store},
    {"--security-store", "--security-store takes one file",
     &DumpArgs::security_store},
    {"--vehicle", "--vehicle takes one label", &DumpArgs::vehicle},
};

constexpr char kParkedWords[] =
    "confirm the vehicle is parked first: ignition on, not Ready; engine off "
    "by the key, not by start-stop; in P or neutral with the parking brake "
    "on";

constexpr unsigned long kTalkingMs = 60 * 1000;

int dump_usage(const char* words)
{
    std::fprintf(stderr, "havne-cli: %s\n", words);
    return kExitUsage;
}

const DumpOption* option_of(const char* word)
{
    for (const DumpOption& o : kDumpOptions) {
        if (std::strcmp(word, o.name) == 0) {
            return &o;
        }
    }
    return nullptr;
}

int read_dump(int argc, char** argv, DumpArgs* a)
{
    for (int i = 2; i < argc; ++i) {
        const char* w = argv[i];
        if (std::strcmp(w, "--parked") == 0) {
            if (a->parked) {
                return dump_usage("--parked is given once");
            }
            a->parked = true;
            continue;
        }
        if (const DumpOption* o = option_of(w)) {
            const char*& to = a->*(o->field);
            if (to != nullptr || i + 1 >= argc || argv[i + 1][0] == '\0') {
                return dump_usage(o->words);
            }
            to = argv[++i];
            continue;
        }
        if (std::strncmp(w, "--", 2) == 0 || a->uri != nullptr) {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", w);
            usage(stderr);
            return kExitUsage;
        }
        a->uri = w;
    }
    if (a->uri == nullptr) {
        return dump_usage("dump needs a URI");
    }
    if (a->plugin == nullptr) {
        return dump_usage("dump needs the plugin whose plan it runs: "
                          "--plugin <pkg>");
    }
    if (a->out == nullptr) {
        return dump_usage("dump needs the folder its copy is kept in: "
                          "--out <folder>");
    }
    return -1;
}

bool plan_at(uint32_t i, ecuio_plugin_operation_t* o)
{
    *o = ecuio_plugin_operation_t{};
    o->struct_size = sizeof *o;
    return ecuio_plugin_operation(g_plugin, i, o) == ECUIO_OK;
}

int pick_plan(DumpArgs* a)
{
    uint32_t count = 0;
    if (!(plugin_info(g_plugin).tiers & ECUIO_PLUGIN_TIER_OPERATION) ||
        ecuio_plugin_operation_count(g_plugin, &count) != ECUIO_OK ||
        count == 0) {
        return dump_usage("the plugin declares no plan to run (the "
                          "operation tier; plugins check lists its plans)");
    }
    std::string ids;
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_plugin_operation_t o;
        if (!plan_at(i, &o)) {
            continue;
        }
        ids += (ids.empty() ? "" : ", ") + std::string(o.id);
        if (a->operation != nullptr && std::strcmp(a->operation, o.id) == 0) {
            a->index = i;
            return -1;
        }
    }
    if (a->operation == nullptr && count == 1) {
        a->index = 0;
        return -1;
    }
    if (a->operation != nullptr) {
        std::fprintf(stderr,
                     "havne-cli: the plugin declares no plan '%s': %s\n",
                     a->operation, ids.c_str());
    } else {
        std::fprintf(stderr,
                     "havne-cli: the plugin declares %u plans: name one with "
                     "--operation (%s)\n",
                     count, ids.c_str());
    }
    return kExitUsage;
}

bool stdin_is_terminal()
{
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

int confirm_parked(DumpArgs* a)
{
    if (a->parked) {
        return -1;
    }
    if (!stdin_is_terminal()) {
        std::fprintf(stderr, "havne-cli: %s, then give --parked\n",
                     kParkedWords);
        return kExitUsage;
    }
    std::fprintf(stderr, "havne-cli: %s.\nType yes once it is: ", kParkedWords);
    std::fflush(stderr);
    char line[16] = {};
    if (std::fgets(line, sizeof line, stdin) != nullptr &&
        cli::dump_said_yes(line)) {
        a->parked = true;
        return -1;
    }
    std::fprintf(stderr, "havne-cli: not confirmed: nothing was asked of the "
                         "module\n");
    return kExitUsage;
}

void write_plan(Output& out, uint32_t index)
{
    ecuio_plugin_conversation_t c{};
    c.struct_size = sizeof c;
    ecuio_plugin_operation_t o;
    if (ecuio_plugin_conversation(g_plugin, &c) != ECUIO_OK ||
        !plan_at(index, &o)) {
        return;
    }
    BigLine b(cli::kOperationLineBytes);
    cli::operation_line(b.line, 0, g_plugin, index, o, c.ecu_count);
    write_line(out, b.line);
}

bool arm_store(Output& out, ecuio_session_t* session, const DumpArgs& a)
{
    const std::string path = a.security_store != nullptr
                                 ? a.security_store
                                 : cli::settings_file("security.json");
    if (path.empty()) {
        write_error(out, ECUIO_ERR_INVALID_ARGUMENT,
                    "no settings folder is known: name the security store "
                    "with --security-store");
        return false;
    }
    if (a.security_store == nullptr) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::u8path(cli::settings_home()), ec);
    }
    const ecuio_status_t st =
        ecuio_session_security_store(session, path.c_str(), a.vehicle);
    if (st != ECUIO_OK) {
        write_error(out, st);
        return false;
    }
    return true;
}

bool plan_channel(Output& out, ecuio_session_t* session, uint32_t index,
                  uint32_t channels, uint32_t* channel)
{
    ecuio_plugin_operation_t o;
    if (plan_at(index, &o)) {
        for (uint32_t c = 0; c < channels; ++c) {
            ecuio_channel_decl_t d{};
            if (ecuio_session_get_module(session, c, &d) == ECUIO_OK &&
                d.rx_id == o.rx_id) {
                *channel = c;
                return true;
            }
        }
    }
    write_error(out, ECUIO_ERR_INVALID_ARGUMENT,
                "the plan's module isn't one of the session's: the URI's "
                "profile names the plugin's variant");
    return false;
}

const char* conversation_word(uint32_t state)
{
    switch (state) {
    case ECUIO_CONVERSATION_DECLINED:
        return "its begin declined";
    case ECUIO_CONVERSATION_FAULTED:
        return "a fault ended it";
    case ECUIO_CONVERSATION_ENDED:
        return "the session closed";
    default:
        return "it didn't begin in time";
    }
}

bool await_talking(Output& out, ecuio_session_t* session, uint32_t channel)
{
    const auto until =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(env_ms("HAVNE_CLI_CONNECT_MS", kTalkingMs));
    ecuio_channel_conversation_t r{};
    while (!g_interrupted.load()) {
        r = ecuio_channel_conversation_t{};
        ecuio_session_get_channel_conversation(session, channel, &r);
        const bool over = r.state == ECUIO_CONVERSATION_DECLINED ||
                          r.state == ECUIO_CONVERSATION_FAULTED ||
                          r.state == ECUIO_CONVERSATION_ENDED;
        if (r.state == ECUIO_CONVERSATION_TALKING || over ||
            std::chrono::steady_clock::now() >= until) {
            break;
        }
        std::this_thread::sleep_for(kPoll);
    }
    if (r.state != ECUIO_CONVERSATION_NONE) {
        char buf[512];
        cli::Line l(buf, sizeof buf);
        cli::conversation_line(l, r);
        write_line(out, l);
    }
    if (r.state == ECUIO_CONVERSATION_TALKING || g_interrupted.load()) {
        return r.state == ECUIO_CONVERSATION_TALKING;
    }
    std::string why = "the plugin's code isn't talking to the plan's "
                      "module: ";
    why += conversation_word(r.state);
    write_error(out, ECUIO_ERR_INVALID_ARGUMENT, why.c_str());
    return false;
}

int start_plan(Output& out, ecuio_session_t* session, const DumpArgs& a)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(a.out), ec);
    ecuio_operation_request_t r{};
    r.struct_size = sizeof r;
    r.operation = a.index;
    r.flags =
        ECUIO_OPERATION_MAY_UNLOCK | (a.parked ? ECUIO_OPERATION_PARKED : 0u);
    const ecuio_status_t st = ecuio_session_operation_start(session, &r, a.out);
    if (st == ECUIO_OK) {
        return -1;
    }
    const std::string why = ecuio_last_error_detail();
    write_error(out, st, why.c_str());
    std::fprintf(stderr, "havne-cli: the dump wasn't started: %s\n",
                 why.c_str());
    return cli::dump_start_is_usage(st) ? kExitUsage : kExitConnect;
}

bool cancel_at(const char* where)
{
    return cli::env_utf8("HAVNE_CLI_DUMP_CANCEL_AT") == where;
}

std::string step_name(uint32_t index, uint32_t step)
{
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    if (ecuio_plugin_operation_step(g_plugin, index, step, &s) != ECUIO_OK) {
        return "";
    }
    return std::string(s.name, strnlen(s.name, sizeof s.name));
}

uint8_t helper_dfi(uint32_t index)
{
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    if (ecuio_plugin_operation_step(g_plugin, index, 0, &s) != ECUIO_OK ||
        s.kind != ECUIO_STEP_DOWNLOAD) {
        return 0;
    }
    return s.dfi;
}

struct Watch {
    uint32_t index = 0;
    uint32_t phase = ECUIO_OPERATION_PHASE_NONE;
    uint32_t step = UINT32_MAX;
    uint32_t pass = UINT32_MAX;
    uint32_t warned = 0;
    bool canceled = false;
    std::chrono::steady_clock::time_point next_progress{};
};

void show_warnings(Output& out, const ecuio_operation_state_t& s, Watch* w)
{
    for (const cli::DumpWarning& d : cli::kDumpWarnings) {
        if ((s.checks & d.bit) == 0 || (w->warned & d.bit) != 0) {
            continue;
        }
        w->warned |= d.bit;
        const std::string words =
            cli::dump_warning_words(d.bit, s, helper_dfi(w->index));
        char buf[cli::kDumpLineBytes];
        cli::Line l(buf, sizeof buf);
        cli::warning_line(l, d.word, words);
        write_line(out, l);
        std::fprintf(stderr, "havne-cli: %s\n", words.c_str());
    }
}

void show_phase(Output& out, const ecuio_operation_state_t& s, Watch* w)
{
    if (s.phase == w->phase && s.step == w->step && s.pass == w->pass) {
        return;
    }
    w->phase = s.phase;
    w->step = s.step;
    w->pass = s.pass;
    const bool ranged = s.phase == ECUIO_OPERATION_PHASE_READING ||
                        s.phase == ECUIO_OPERATION_PHASE_COMPARING;
    const std::string name = ranged ? step_name(w->index, s.step) : "";
    char buf[cli::kDumpLineBytes];
    cli::Line l(buf, sizeof buf);
    cli::phase_line(l, s, name.c_str());
    write_line(out, l);
    if (ranged) {
        std::fprintf(stderr, "havne-cli: %s '%s', pass %u\n",
                     cli::dump_phase_word(s.phase), name.c_str(), s.pass);
    } else if (s.phase != ECUIO_OPERATION_PHASE_DONE) {
        std::fprintf(stderr, "havne-cli: %s\n", cli::dump_phase_word(s.phase));
    }
}

void show_progress(Output& out, const ecuio_operation_state_t& s, Watch* w)
{
    const auto now = std::chrono::steady_clock::now();
    if (now < w->next_progress || s.bytes_total == 0 ||
        s.phase == ECUIO_OPERATION_PHASE_DONE) {
        return;
    }
    w->next_progress = now + std::chrono::seconds(1);
    char buf[cli::kDumpLineBytes];
    cli::Line l(buf, sizeof buf);
    cli::progress_line(l, s);
    write_line(out, l);
    if (s.waiting_ms > 0) {
        std::fprintf(stderr, "havne-cli: the module is still working (%u s)\n",
                     (s.waiting_ms + 999u) / 1000u);
    } else {
        std::fprintf(stderr, "havne-cli: %u%%\n", cli::dump_percent(s));
    }
}

ecuio_operation_state_t follow(Output& out, ecuio_session_t* session,
                               uint32_t index)
{
    Watch w;
    w.index = index;
    ecuio_operation_state_t s{};
    for (;;) {
        s = ecuio_operation_state_t{};
        s.struct_size = sizeof s;
        ecuio_session_get_operation(session, &s);
        if (g_interrupted.load() && !w.canceled) {
            w.canceled = true;
            ecuio_session_operation_cancel(session);
            std::fprintf(stderr, "havne-cli: stopping: the module's session "
                                 "is left first\n");
        }
        show_warnings(out, s, &w);
        show_phase(out, s, &w);
        show_progress(out, s, &w);
        out.flush();
        if (s.phase == ECUIO_OPERATION_PHASE_DONE ||
            s.end != ECUIO_OPERATION_END_NONE) {
            return s;
        }
        std::this_thread::sleep_for(kPoll);
    }
}

bool verify_copy(const std::string& path)
{
    ecuio_dump_t* d = nullptr;
    if (ecuio_dump_open(path.c_str(), &d) != ECUIO_OK) {
        return false;
    }
    char name[ECUIO_DUMP_NAME_MAX] = {};
    const bool ok =
        ecuio_dump_verify(d, nullptr, nullptr, name, sizeof name) == ECUIO_OK;
    ecuio_dump_close(d);
    return ok;
}

int finish(Output& out, const ecuio_operation_state_t& s, const DumpArgs& a)
{
    const bool saved = s.end == ECUIO_OPERATION_END_SAVED;
    std::string path;
    int verified = -1;
    if (saved) {
        path =
            (std::filesystem::u8path(a.out) / std::filesystem::u8path(s.name))
                .u8string();
        verified = verify_copy(path) ? 1 : 0;
    }
    BigLine b(cli::kDumpLineBytes + cli::Line::string_bytes(path.c_str()));
    cli::dump_line(b.line, s, path, verified);
    write_line(out, b.line);
    if (saved) {
        std::fprintf(stderr, "havne-cli: the copy is kept%s: %s\n",
                     verified == 1 ? " and reads back whole"
                                   : ", but doesn't read back whole",
                     path.c_str());
    } else {
        const char* note = cli::dump_end_note(s);
        std::fprintf(stderr, "havne-cli: the dump ended (%s): %s\n",
                     cli::dump_end_word(s.end),
                     note != nullptr ? note : s.words);
    }
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    const char* w = cli::dump_end_word(s.end);
    l.string(w, std::strlen(w));
    l.ch('}');
    write_line(out, l);
    if (!saved) {
        return kExitNoCopy;
    }
    return verified == 1 ? kExitOk : kExitFailed;
}

int run_plan(Output& out, ecuio_session_t* session, const DumpArgs& a,
             uint32_t channels)
{
    uint32_t channel = 0;
    if (!arm_store(out, session, a) ||
        !plan_channel(out, session, a.index, channels, &channel)) {
        return kExitConnect;
    }
    const ecuio_status_t st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st, ecuio_last_error_detail());
        return kExitConnect;
    }
    const bool talking = await_talking(out, session, channel);
    if (cancel_at("start")) {
        g_interrupted.store(true);
    }
    if (g_interrupted.load()) {
        ecuio_operation_state_t s{};
        s.end = ECUIO_OPERATION_END_CANCELED;
        cli::dump_set_words(s, "interrupted before the run began: nothing "
                               "was asked of the module");
        return finish(out, s, a);
    }
    if (!talking) {
        return kExitConnect;
    }
    const int refused = start_plan(out, session, a);
    if (refused >= 0) {
        return refused;
    }
    if (cancel_at("run")) {
        g_interrupted.store(true);
    }
    const ecuio_operation_state_t s = follow(out, session, a.index);
    return finish(out, s, a);
}

} // namespace

int run_dump(const DumpArgs& a)
{
    Output out;
    ecuio_session_t* session = nullptr;
    std::string detail;
    const ecuio_status_t st = create_consented(out, a.uri, &session, &detail,
                                               /*start_first=*/true);
    if (st != ECUIO_OK && consent_refused_exit() >= 0) {
        return consent_refused_exit();
    }
    write_start(out);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        return ended(out, kExitConnect);
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    write_session(out, session, info);
    write_plan(out, a.index);
    out.flush();
    install_handlers();
    const int code = run_plan(out, session, a, module_count(info));
    ecuio_session_destroy(session);
#ifdef _WIN32
    g_done.store(true);
#endif
    return ended(out, code);
}

int cmd_dump(int argc, char** argv)
{
    DumpArgs a;
    int rc = read_dump(argc, argv, &a);
    if (rc >= 0) {
        return rc;
    }
    rc = use_plugin(a.plugin, a.uri);
    if (rc >= 0) {
        return rc;
    }
    rc = pick_plan(&a);
    if (rc >= 0) {
        return rc;
    }
    rc = need_consent(a.uri, a.plugin, a.consent_store);
    if (rc >= 0) {
        return rc;
    }
    rc = confirm_parked(&a);
    if (rc >= 0) {
        return rc;
    }
    return run_dump(a);
}

} // namespace cmd
