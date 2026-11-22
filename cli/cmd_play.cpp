// File/layer: cli/cmd_play.cpp | havne-cli, play: Linux only (ADR-0032 section 8)
// Testing: UNIT: CliUsageTest (ctest); BENCH: by hand on vcan0
//
// Public headers only: what any program linking core can do.

#include "jsonl.h"
#include "log_lines.h"

#include "commands.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/socketcan.h"

#if HAVNE_CLI_PLAY
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cmd {

#if HAVNE_CLI_PLAY

namespace {

// play (ADR-0032 section 8): a session's frames written to a virtual CAN
// interface as they arrive, so the mock's car (or a recording) drives on
// a bus a socketcan:// session, or any SocketCAN tool, can listen to
struct Player {
    int fd = -1;
    bool fd_frames = false;   // the interface takes CAN FD (MTU >= 72)
    uint64_t limit = 0;       // --count; 0 = none
    uint64_t seen = 0;        // the worker's own
    std::atomic<uint64_t> covered{0};
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> skipped{0};
};

// The CAN FD length that holds n bytes: 0 to 8, then 12, 16, 20, 24,
// 32, 48, 64, the rest zero padding as an FD controller sends it; 0 for
// more than a frame holds
size_t fd_length(size_t n)
{
    static const size_t lengths[] = {12, 16, 20, 24, 32, 48, 64};
    if (n <= 8) {
        return n;
    }
    for (size_t len : lengths) {
        if (n <= len) {
            return len;
        }
    }
    return 0;
}

// On the worker: one non-blocking write, never a wait
void on_play(const ecuio_frame_t* f, void* user)
{
    auto* p = static_cast<Player*>(user);
    const uint64_t n = ++p->seen;
    if (p->limit && n > p->limit) {
        return;
    }
    const size_t len = f->data_length;
    const bool fd = (f->flags & ECUIO_FRAME_FLAG_FD) != 0 || len > 8;
    canid_t id = f->arbitration_id;
    if (f->flags & ECUIO_FRAME_FLAG_EXTENDED) {
        id = (id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    } else {
        id &= CAN_SFF_MASK;
    }
    bool ok = false;
    // An error report isn't a frame to send, and a message longer than one
    // frame needs ISO-TP framing, which play doesn't do
    if (!(f->flags & ECUIO_FRAME_FLAG_ERROR) && (len <= 8 || fd_length(len) != 0) &&
        (!fd || p->fd_frames)) {
        if (fd) {
            struct canfd_frame cf;
            std::memset(&cf, 0, sizeof cf);
            cf.can_id = id;
            cf.len = static_cast<uint8_t>(fd_length(len));
            cf.flags = static_cast<uint8_t>(
                ((f->flags & ECUIO_FRAME_FLAG_BITRATE_SWITCH) ? CANFD_BRS : 0) |
                ((f->flags & ECUIO_FRAME_FLAG_FD_ESI) ? CANFD_ESI : 0));
            std::memcpy(cf.data, f->data, len);
            ok = write(p->fd, &cf, sizeof cf) == static_cast<ssize_t>(sizeof cf);
        } else {
            struct can_frame cf;
            std::memset(&cf, 0, sizeof cf);
            cf.can_id =
                id | ((f->flags & ECUIO_FRAME_FLAG_REMOTE) ? CAN_RTR_FLAG : 0);
            cf.len = static_cast<uint8_t>(len);
            std::memcpy(cf.data, f->data, len);
            ok = write(p->fd, &cf, sizeof cf) == static_cast<ssize_t>(sizeof cf);
        }
    }
    (ok ? p->sent : p->skipped).fetch_add(1);
    p->covered.store(n, std::memory_order_release);
}

} // namespace

int run_play(const PlayArgs& args)
{
    Output out;
    write_start(out);

    // Only a vcan interface, up: a frame from a simulation or a
    // recording never goes onto a real bus
    const std::vector<ecuio_can_interface_t> list = can_interfaces();
    const ecuio_can_interface_t* target = nullptr;
    for (const ecuio_can_interface_t& c : list) {
        if (std::strncmp(c.name, args.to, sizeof c.name) == 0) {
            target = &c;
        }
    }
    char why[200];
    why[0] = '\0';
    if (target == nullptr) {
        std::snprintf(why, sizeof why, "no CAN interface is named %s", args.to);
    } else if (std::strcmp(target->kind, "vcan") != 0) {
        std::snprintf(why, sizeof why,
                      "play writes only to a virtual CAN interface (vcan), "
                      "and %s is a %s",
                      args.to, target->kind);
    } else if (!(target->flags & ECUIO_CAN_UP)) {
        std::snprintf(why, sizeof why, "%s is down: sudo ip link set %s up",
                      args.to, args.to);
    }
    if (why[0] != '\0') {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why);
        return ended(out, kExitConnect);
    }

    Player player;
    player.limit = args.count;
    player.fd_frames = (target->flags & ECUIO_CAN_FD) != 0;
    player.fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    const int on = 1;
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex(args.to));
    if (player.fd < 0 ||
        (player.fd_frames && setsockopt(player.fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                                        &on, sizeof on) != 0) ||
        bind(player.fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        const std::string reason = std::generic_category().message(errno);
        std::snprintf(why, sizeof why, "%s: %s", args.to, reason.c_str());
        write_error(out, ECUIO_ERR_LOAD_FAILED, why);
        if (player.fd >= 0) {
            close(player.fd);
        }
        return ended(out, kExitConnect);
    }
    if (!player.fd_frames) {
        std::fprintf(stderr,
                     "havne-cli: %s takes classic frames only (MTU 16), so "
                     "frames over 8 bytes are skipped: sudo ip link set %s "
                     "down, then sudo ip link set %s mtu 72 up\n",
                     args.to, args.to, args.to);
    }

    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, args.uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        close(player.fd);
        return ended(out, kExitConnect);
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    const bool replay = info.backend == ECUIO_BACKEND_REPLAY;
    const bool replay_ends =
        replay && !(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP);
    write_session(out, session, info);
    {
        char buf[256];
        cli::Line l(buf, sizeof buf);
        l.raw("{\"type\":\"play\"");
        l.key("to");
        l.string(args.to, SIZE_MAX);
        l.key("fd");
        l.raw(player.fd_frames ? "true" : "false");
        l.ch('}');
        write_line(out, l);
    }
    out.flush();

    ecuio_register_frame_callback(session, on_play, &player);
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        close(player.fd);
        return ended(out, kExitConnect);
    }

    const uint64_t started = ecuio_clock_us();
    const char* reason = nullptr;
    while (!reason) {
        std::this_thread::sleep_for(kPoll);
        const uint64_t covered = player.covered.load(std::memory_order_acquire);
        if (g_interrupted.load()) {
            reason = "interrupted";
        } else if (args.count && covered >= args.count) {
            reason = "count";
        } else if (replay_ends && replay_done(session, info, covered)) {
            reason = "replay-end";
        } else if (args.seconds &&
                   ecuio_clock_us() - started >= args.seconds * 1000000u) {
            reason = "seconds";
        }
        int32_t error = ECUIO_OK;
        uint32_t episodes = 0;
        ecuio_session_get_error_state(session, &error, &episodes);
        if (!reason && replay && error == ECUIO_ERR_RECORDING_CHANGED) {
            reason = "recording-changed"; /* stopped short of its end */
        }
    }
    ecuio_session_close(session);
    ecuio_session_destroy(session);
    close(player.fd);

    uint64_t covered = player.covered.load();
    if (args.count && covered > args.count) {
        covered = args.count;
    }
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string(reason, SIZE_MAX);
    l.key("frames");
    l.u64(covered);
    l.key("sent");
    l.u64(player.sent.load());
    l.key("skipped");
    l.u64(player.skipped.load());
    l.ch('}');
    write_line(out, l);
    out.flush();
    return ended(out, kExitOk);
}
#endif

// play's arguments, then run_play; off Linux its one refusal
int cmd_play([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#if HAVNE_CLI_PLAY
    PlayArgs play;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--to" && i + 1 < argc) {
            play.to = argv[++i];
        } else if ((a == "--count" || a == "--seconds") && i + 1 < argc) {
            const bool is_count = a == "--count";
            if (!(is_count ? parse_count(argv[++i], play.count)
                           : parse_run_seconds(argv[++i], play.seconds))) {
                std::fprintf(stderr, "havne-cli: %s\n",
                             is_count ? "--count takes a whole number above 0"
                                      : cli::run_seconds_why());
                return kExitUsage;
            }
        } else if (a.rfind("--", 0) == 0 || play.uri) {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        } else {
            play.uri = argv[i];
        }
    }
    if (!play.uri || !play.to) {
        std::fprintf(stderr,
                     "havne-cli: play needs a URI and --to <interface>\n");
        usage(stderr);
        return kExitUsage;
    }
    return run_play(play);
#else
    std::fprintf(stderr, "havne-cli: play needs Linux, where a CAN "
                         "interface is a network device\n");
    return kExitUsage;
#endif
}

} // namespace cmd
