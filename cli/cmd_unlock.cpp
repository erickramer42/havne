// File/layer: cli/cmd_unlock.cpp | havne-cli, stream's --unlock (ADR-0066
//             sections 2.3 and 2.4)
// Testing: UNIT: CliUsageTest (ctest), CliUnlockLineTest (test_cli);
//          HARNESS: CliConsentRunTest.AnUnlockOpensTheGatedLevelOnce
//          (tests/cli_consent.py unlock)
//
// Public headers only. --unlock names the word: the manifest's ECU and a
// level the plugin declares for it, so no --yes. The security store
// (--security-store, else security.json beside the consent store) is
// named before start; the word goes once, when the module's conversation
// is talking, against its report's generation, and never again once core
// took it or refused it.

#include "conversation_lines.h"
#include "settings_home.h"

#include "commands.h"
#include "profile_source.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace cmd {

namespace {

struct UnlockOption {
    const char* name;
    const char* words; // its refusal's
    const char* StreamArgs::* field;
};

constexpr UnlockOption kUnlockOptions[] = {
    {"--unlock", "--unlock takes <ecu>:<level>", &StreamArgs::unlock},
    {"--security-store", "--security-store takes one file",
     &StreamArgs::security_store},
    {"--vehicle", "--vehicle takes one label", &StreamArgs::vehicle},
};

int unlock_usage(const char* words)
{
    std::fprintf(stderr, "havne-cli: %s\n", words);
    return kExitUsage;
}

// The plugin's ECU named id: its reply ID, or 0 for none
uint32_t plugin_ecu_rx(const std::string& id, uint32_t ecus)
{
    for (uint32_t i = 0; i < ecus; ++i) {
        ecuio_plugin_conversation_ecu_t e{};
        e.struct_size = sizeof e;
        if (ecuio_plugin_conversation_ecu(g_plugin, i, &e) != ECUIO_OK) {
            continue;
        }
        const void* nul = std::memchr(e.id, 0, sizeof e.id);
        const size_t n =
            nul != nullptr
                ? static_cast<size_t>(static_cast<const char*>(nul) - e.id)
                : sizeof e.id;
        if (id == std::string(e.id, n)) {
            return e.rx_id;
        }
    }
    return 0;
}

// Whether the plugin declares level for the ECU answering on rx
bool plugin_declares(uint32_t rx, uint8_t level, uint32_t levels)
{
    for (uint32_t i = 0; i < levels; ++i) {
        ecuio_plugin_security_level_t l{};
        l.struct_size = sizeof l;
        if (ecuio_plugin_security_level(g_plugin, i, &l) == ECUIO_OK &&
            l.rx_id == rx && l.level == level) {
            return true;
        }
    }
    return false;
}

// The word given against report r; it waits for the next pass only when
// nothing was sent: the report moved, or the module's identity (the
// store's key) is still being read
void give_word(Output& out, ecuio_session_t* session, UnlockRun* run,
               const ecuio_channel_conversation_t& r)
{
    uint32_t wait = 0;
    const ecuio_status_t st = ecuio_session_security_unlock(
        session, run->channel, run->level, r.generation, &wait);
    if (st == ECUIO_ERR_REPORT_CHANGED ||
        (st == ECUIO_ERR_TOO_SOON && wait == 0)) {
        return;
    }
    run->asked = true;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::unlock_line(l, r.rx_id, run->level, st, wait,
                     st == ECUIO_OK ? nullptr : ecuio_last_error_detail());
    write_line(out, l);
}

} // namespace

bool take_unlock_options(int& argc, char** argv, StreamArgs* args)
{
    for (int i = 2; i < argc;) {
        const UnlockOption* o = nullptr;
        for (const UnlockOption& k : kUnlockOptions) {
            if (std::strcmp(argv[i], k.name) == 0) {
                o = &k;
            }
        }
        if (o == nullptr) {
            ++i;
            continue;
        }
        const char*& to = args->*(o->field);
        if (to != nullptr || i + 1 >= argc || argv[i + 1][0] == '\0') {
            std::fprintf(stderr, "havne-cli: %s\n", o->words);
            return false;
        }
        to = argv[i + 1];
        for (int k = i; k + 2 < argc; ++k) {
            argv[k] = argv[k + 2];
        }
        argc -= 2;
    }
    return true;
}

int check_unlock(StreamArgs& args)
{
    if (args.unlock == nullptr) {
        return args.security_store != nullptr || args.vehicle != nullptr
                   ? unlock_usage("--security-store and --vehicle go with "
                                  "--unlock")
                   : -1;
    }
    if (g_plugin == nullptr) {
        return unlock_usage("--unlock names a module of a plugin's: give "
                            "it with --plugin");
    }
    ecuio_plugin_conversation_t c{};
    c.struct_size = sizeof c;
    if (!(plugin_info(g_plugin).tiers & ECUIO_PLUGIN_TIER_CONVERSATION) ||
        ecuio_plugin_conversation(g_plugin, &c) != ECUIO_OK) {
        return unlock_usage("--unlock needs a plugin whose code talks to a "
                            "module (the conversation tier): this one reads "
                            "signals only");
    }
    std::string ecu;
    uint8_t level = 0;
    if (!cli::parse_unlock(args.unlock, &ecu, &level)) {
        return unlock_usage("--unlock takes <ecu>:<level>: the plugin's ECU "
                            "id and the level in hex (pcm:01)");
    }
    const uint32_t rx = plugin_ecu_rx(ecu, c.ecu_count);
    if (rx == 0) {
        std::fprintf(stderr,
                     "havne-cli: --unlock: the plugin's code talks to no "
                     "ECU '%s' (plugins check lists its ECUs)\n",
                     ecu.c_str());
        return kExitUsage;
    }
    if ((level & 1u) == 0) {
        return unlock_usage("--unlock: a security level is odd (its seed's "
                            "sub-function; the key's is the next)");
    }
    if (!plugin_declares(rx, level, c.level_count)) {
        std::fprintf(stderr,
                     "havne-cli: --unlock: the plugin declares no level %02X "
                     "for '%s' (plugins check lists its levels)\n",
                     level, ecu.c_str());
        return kExitUsage;
    }
    args.unlock_rx = rx;
    args.unlock_level = level;
    return -1;
}

bool arm_unlock(Output& out, ecuio_session_t* session, const StreamArgs& args,
                uint32_t channels, UnlockRun* run)
{
    if (args.unlock == nullptr) {
        return true;
    }
    std::string path = args.security_store != nullptr
                           ? args.security_store
                           : cli::settings_file("security.json");
    if (path.empty()) {
        write_error(out, ECUIO_ERR_INVALID_ARGUMENT,
                    "no settings folder is known: name the security store "
                    "with --security-store");
        return false;
    }
    if (args.security_store == nullptr) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::u8path(cli::settings_home()), ec);
    }
    const ecuio_status_t st =
        ecuio_session_security_store(session, path.c_str(), args.vehicle);
    if (st != ECUIO_OK) {
        write_error(out, st);
        return false;
    }
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_decl_t d{};
        if (ecuio_session_get_module(session, c, &d) == ECUIO_OK &&
            d.rx_id == args.unlock_rx) {
            run->wanted = true;
            run->channel = c;
            run->level = args.unlock_level;
            return true;
        }
    }
    char words[160];
    std::snprintf(words, sizeof words,
                  "--unlock's module (0x%X) isn't one of the session's: the "
                  "URI's profile names the plugin's variant",
                  static_cast<unsigned>(args.unlock_rx));
    write_error(out, ECUIO_ERR_INVALID_ARGUMENT, words);
    return false;
}

void poll_conversation(Output& out, ecuio_session_t* session, uint32_t channels,
                       UnlockRun* run)
{
    for (uint32_t c = 0; c < channels && c < ECUIO_MAX_CHANNELS; ++c) {
        ecuio_channel_conversation_t r{};
        if (ecuio_session_get_channel_conversation(session, c, &r) !=
                ECUIO_OK ||
            r.state == ECUIO_CONVERSATION_NONE) {
            continue;
        }
        if (!run->shown[c] || r.generation != run->generation[c]) {
            run->shown[c] = true;
            run->generation[c] = r.generation;
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::conversation_line(l, r);
            write_line(out, l);
        }
        if (run->wanted && !run->asked && c == run->channel &&
            r.state == ECUIO_CONVERSATION_TALKING) {
            give_word(out, session, run, r);
        }
    }
}

} // namespace cmd
