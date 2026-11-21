// File/layer: cli/cmd_sim.cpp | havne-cli, sim (ADR-0046) and sim --elm327 (ADR-0055)
// Testing: UNIT: CliGoldenTest, CliElm327RunTest, CliUsageTest (ctest);
//          CliSimTest, CliElm327Test (test_cli)
//
// Public headers only: what any program linking core can do.

#include "elm327_listener.h"
#include "jsonl.h"
#include "sim_line.h"
#include "elm327_emulator.h"
#include "elm327_lines.h"
#include "log_lines.h"

#include "commands.h"
#include "profile_source.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/sim.h"

namespace cmd {

namespace {

// How often sim writes its counts, when they changed
constexpr uint64_t kSimStatsEveryUs = 10u * 1000 * 1000;

} // namespace

// The simulated ECU as an ELM327 adapter on a TCP port (ADR-0055), on
// every platform, until Ctrl+C or --seconds: start, elm327, a client
// line as each host connects and leaves, a sim_stats line every 10 s
// while the counts change, end with them
int run_sim_elm327(const SimArgs& args)
{
    install_handlers();
    Output out;
    write_start(out);
    ecuio_sim_t* sim = nullptr;
    const ecuio_status_t st = ecuio_sim_create_unbound(args.uri, 0, &sim);
    if (st != ECUIO_OK) {
        write_error(out, st);
        return ended(out, kExitConnect);
    }
    const auto created = std::chrono::steady_clock::now();
    cli::Elm327Server server;
    std::string why;
    if (!server.open(args.elm327_at, &why)) {
        ecuio_sim_destroy(sim);
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        return ended(out, kExitConnect);
    }
    {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::elm327_line(
            l, args.uri,
            cli::address_text(server.address().ip, server.address().port));
        write_line(out, l);
    }
    out.flush();
    cli::Elm327Emulator emu(sim);
    const uint64_t started = ecuio_clock_us();
    uint64_t next_stats = started + kSimStatsEveryUs;
    ecuio_sim_stats_t last{};
    ecuio_sim_stats_t s{};
    const char* reason = nullptr;
    server.serve(
        emu,
        [created] {
            return static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - created)
                    .count());
        },
        [&] {
            const uint64_t now = ecuio_clock_us();
            if (g_interrupted.load()) {
                reason = "interrupted";
            } else if (args.seconds &&
                       now - started >= args.seconds * 1000000u) {
                reason = "seconds";
            } else if (now >= next_stats) {
                next_stats = now + kSimStatsEveryUs;
                (void)ecuio_sim_get_stats(sim, &s);
                if (std::memcmp(&s, &last, sizeof s) != 0) {
                    last = s;
                    char buf[1024];
                    cli::Line l(buf, sizeof buf);
                    cli::sim_stats_line(l, s);
                    write_line(out, l);
                    out.flush();
                }
            }
            return reason != nullptr || out.failed;
        },
        [&](const char* event, const std::string& peer) {
            char buf[256];
            cli::Line l(buf, sizeof buf);
            cli::client_line(l, event, peer);
            write_line(out, l);
            out.flush();
        });
    (void)ecuio_sim_get_stats(sim, &s);
    ecuio_sim_destroy(sim);
    if (out.failed) {
        return ended(out, kExitOk);
    }
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::sim_end_line(l, reason != nullptr ? reason : "failed", s);
    write_line(out, l);
    out.flush();
    if (reason == nullptr) {
        // The listener's wait failed: the end line says "failed", stderr why
        std::fprintf(stderr, "havne-cli: %s\n",
                     server.failure().empty() ? "the listener stopped"
                                              : server.failure().c_str());
        (void)out.ok();
        return kExitFailed;
    }
    return ended(out, kExitOk);
}

// The simulated ECU (ADR-0046) answering on a virtual bus, or a desk
// bus's controller with --desk-bus, until Ctrl+C, --seconds, the bus
// failing or another node sending on 0x7E8: start, sim, a sim_stats
// line every 10 s
// while the counts change, end with them
int run_sim(const SimArgs& args)
{
    install_handlers();
    Output out;
    write_start(out);
    ecuio_sim_t* sim = nullptr;
    const ecuio_status_t st = ecuio_sim_create_with(
        args.uri, args.on, args.desk_bus ? ECUIO_SIM_ALLOW_DESK_BUS : 0u, &sim);
    if (st != ECUIO_OK) {
        write_error(out, st);
        return ended(out, kExitConnect);
    }
    {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::sim_line(l, args.uri, args.on, args.desk_bus);
        write_line(out, l);
    }
    out.flush();
    const uint64_t started = ecuio_clock_us();
    uint64_t next_stats = started + kSimStatsEveryUs;
    ecuio_sim_stats_t last{};
    ecuio_sim_stats_t s{};
    const char* reason = nullptr;
    while (!reason) {
        std::this_thread::sleep_for(kPoll);
        (void)ecuio_sim_get_stats(sim, &s);
        const uint64_t now = ecuio_clock_us();
        if (g_interrupted.load()) {
            reason = "interrupted";
        } else if (s.flags & ECUIO_SIM_FAILED) {
            reason = "bus-failed";
        } else if (s.flags & ECUIO_SIM_YIELDED) {
            reason = "yielded";
        } else if (args.seconds && now - started >= args.seconds * 1000000u) {
            reason = "seconds";
        } else if (now >= next_stats) {
            next_stats = now + kSimStatsEveryUs;
            if (std::memcmp(&s, &last, sizeof s) != 0) {
                last = s;
                char buf[1024];
                cli::Line l(buf, sizeof buf);
                cli::sim_stats_line(l, s);
                write_line(out, l);
                out.flush();
            }
        }
        if (out.failed) {
            break; /* a closed pipe ends it quietly */
        }
    }
    ecuio_sim_destroy(sim);
    if (out.failed) {
        return ended(out, kExitOk);
    }
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::sim_end_line(l, reason, s);
    write_line(out, l);
    out.flush();
    if (std::strcmp(reason, "bus-failed") == 0) {
        std::fprintf(stderr,
                     "havne-cli: the simulated ECU's bus failed (%s "
                     "went down or away)\n",
                     args.on);
        return kExitFailed;
    }
    if (std::strcmp(reason, "yielded") == 0) {
        std::fprintf(stderr,
                     "havne-cli: another node sent on 0x7E8 on %s, so "
                     "an ECU answers there: the simulated ECU went "
                     "silent\n",
                     args.on);
        return kExitFailed;
    }
    return ended(out, kExitOk);
}

// sim's arguments and its profile, then the bus's sim or the ELM327's
int cmd_sim(int argc, char** argv)
{
    SimArgs sim;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--on" && i + 1 < argc) {
            sim.on = argv[++i];
        } else if (a == "--elm327" && i + 1 < argc) {
            sim.elm327 = argv[++i];
            if (!cli::parse_elm327_address(sim.elm327, &sim.elm327_at)) {
                std::fprintf(stderr, "havne-cli: --elm327 takes tcp:<port> or "
                                     "tcp:<a.b.c.d>:<port>, a port 0-65535\n");
                return kExitUsage;
            }
        } else if (a == "--desk-bus") {
            sim.desk_bus = true;
        } else if (a == "--signals" && i + 1 < argc) {
            sim.signals = argv[++i];
        } else if (a == "--plugin" && i + 1 < argc) {
            sim.plugin = argv[++i];
        } else if (a == "--seconds" && i + 1 < argc) {
            if (!parse_run_seconds(argv[++i], sim.seconds)) {
                std::fprintf(stderr, "havne-cli: %s\n", cli::run_seconds_why());
                return kExitUsage;
            }
        } else if (a.rfind("--", 0) == 0 || sim.uri) {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        } else {
            sim.uri = argv[i];
        }
    }
    if (!sim.uri || (sim.on == nullptr) == (sim.elm327 == nullptr)) {
        std::fprintf(stderr, "havne-cli: sim needs a URI and either --on "
                             "<interface> or --elm327 tcp:<port>\n");
        usage(stderr);
        return kExitUsage;
    }
    if (sim.elm327 != nullptr && sim.desk_bus) {
        std::fprintf(stderr, "havne-cli: --desk-bus is for --on: the "
                             "ELM327 emulator is on no bus\n");
        usage(stderr);
        return kExitUsage;
    }
    if (sim.signals != nullptr && sim.plugin != nullptr) {
        std::fprintf(stderr, "havne-cli: --signals and --plugin each "
                             "give the profile: one at a time\n");
        usage(stderr);
        return kExitUsage;
    }
    if (sim.plugin != nullptr) {
        const int rc = use_plugin(sim.plugin, sim.uri);
        if (rc >= 0) {
            return rc;
        }
    }
    std::string opened;
    if (sim.signals != nullptr) {
        const int rc = use_signal_file(sim.signals, sim.uri, &opened);
        if (rc >= 0) {
            return rc;
        }
        sim.uri = opened.c_str();
    }
    return sim.elm327 != nullptr ? run_sim_elm327(sim) : run_sim(sim);
}

} // namespace cmd
