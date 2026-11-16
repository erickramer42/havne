// File/layer: cli/cmd_log.cpp | havne-cli, log (ADR-0056 section 7)
// Testing: UNIT: CliLogRunTest, CliGoldenTest (ctest); CliLogTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "default_pick.h"
#include "log_lines.h"
#include "jsonl.h"
#include "scan_line.h"
#include "sleep_watch.h"

#include "commands.h"
#include "profile_source.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "ecuio/conditions.h"

namespace cmd {

namespace {

// The log line's period (cli::kLogEveryUs); HAVNE_CLI_LOG_EVERY_MS (tests
// only) shortens it
uint64_t log_every_us()
{
    return env_ms("HAVNE_CLI_LOG_EVERY_MS",
                  static_cast<unsigned long>(cli::kLogEveryUs / 1000)) *
           1000ULL;
}

// The recording's size so far: the recorder's files' bytes summed (its
// writer thread adds to them meanwhile; a file not yet made counts 0)
uint64_t folder_bytes(const std::string& folder)
{
    return cli::recording_bytes(folder, [](const std::string& path) {
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(wide(path.c_str()).c_str(),
                                 GetFileExInfoStandard, &a)) {
            return (static_cast<uint64_t>(a.nFileSizeHigh) << 32) |
                   a.nFileSizeLow;
        }
#else
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            return static_cast<uint64_t>(st.st_size);
        }
#endif
        return uint64_t{0};
    });
}

// log's frame callback: the frames seen, for a replay's end (stream
// counts them as it writes them; log writes none)
struct LogFrames {
    std::atomic<uint64_t> covered{0};
};

void on_log_frame(const ecuio_frame_t*, void* user)
{
    static_cast<LogFrames*>(user)->covered.fetch_add(1,
                                                     std::memory_order_release);
}

// What the log and end lines say: session time since start in whole
// seconds, the folder's bytes, the rows signals.csv holds
cli::LogReadout log_readout(uint64_t started, const std::string& folder,
                            ecuio_recorder_t* recorder, bool armed = false)
{
    cli::LogReadout r;
    r.armed = armed;
    r.seconds = (ecuio_clock_us() - started) / 1000000u;
    r.bytes = folder.empty() ? 0 : folder_bytes(folder);
    ecuio_recorder_state_t st;
    if (recorder != nullptr &&
        ecuio_recorder_get_state(recorder, &st) == ECUIO_OK) {
        r.values = st.value_rows;
        r.windows = st.windows;
    }
    return r;
}

// An armed log's recording lines (cli::armed_due): a window's with its
// folder, which becomes the one the log and end lines name, and an armed
// line once none is open
void poll_armed(Output& out, ecuio_recorder_t* recorder, cli::ArmedSeen* seen,
                std::string* folder)
{
    ecuio_recorder_state_t st;
    if (recorder == nullptr ||
        ecuio_recorder_get_state(recorder, &st) != ECUIO_OK) {
        return;
    }
    const cli::ArmedDue due = cli::armed_due(*seen, st.state, st.windows);
    if (due.window) {
        /* A newer window can land between the size and the copy */
        uint32_t len = 0;
        std::vector<char> buf(1);
        while (ecuio_recorder_get_folder(recorder, buf.data(),
                                         static_cast<uint32_t>(buf.size()),
                                         &len) == ECUIO_OK &&
               len >= buf.size()) {
            buf.resize(size_t{len} + 1);
        }
        folder->assign(buf.data(), std::min<size_t>(len, buf.size() - 1));
        BigLine b(cli::armed_window_line_bytes(folder->c_str()));
        cli::armed_window_line(b.line, st.windows, folder->c_str());
        write_line(out, b.line);
    }
    if (due.armed) {
        char buf[96];
        cli::Line l(buf, sizeof buf);
        cli::armed_line(l, st.windows);
        write_line(out, l);
    }
}

// The writer drains on a timer of its own (100 ms when idle), so a readout
// right after the close can count none of a replay's rows: wait, at most
// kRecordingCloseWait, until each frame put is written, dropped, or let
// go outside every window of an armed log
void wait_for_writer(const ecuio_recorder_t* recorder, uint64_t frames)
{
    const auto give_up = std::chrono::steady_clock::now() + kRecordingCloseWait;
    ecuio_recorder_state_t st;
    while (recorder != nullptr &&
           ecuio_recorder_get_state(recorder, &st) == ECUIO_OK &&
           st.state != ECUIO_RECORDING_FAILED &&
           st.frame_rows + st.records_dropped + st.frames_passed < frames &&
           std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

// The analysis log from the command line (ADR-0056 section 7): the
// session, the focus on the signals named (core asks those alone, as
// often as the bus allows), a recorder attached before start so the
// folder's header carries the focus, and a log line each period of
// session time with the recording's size; no frame or values lines, the
// folder is the output. The names are checked against the session's set
// before any line is written, so a typo is usage: exit 2, nothing on
// stdout. Ends at Ctrl+C, --seconds, a failed recording (exit 1) or a
// replay's last frame; a lost link is logged in health lines and waited
// out. Exit codes as stream's.
int run_log(const cli::LogArgs& args)
{
    Output out;
    ecuio_condition_set_t* conditions = nullptr;
    if (args.conditions != nullptr) {
        std::string why;
        ecuio_status_t loaded = ECUIO_OK;
        conditions = load_conditions_file(args.conditions, &loaded, &why);
        if (conditions == nullptr) {
            write_start(out);
            write_error(out, loaded, why.c_str());
            return ended(out, kExitConnect);
        }
    }
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st =
        create_within(out, args.uri, &session, &detail, /*start_first=*/true);
    if (st != ECUIO_OK) {
        ecuio_condition_set_free(conditions);
        write_start(out);
        write_error(out, st, detail.c_str());
        return ended(out, kExitConnect);
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    std::vector<uint16_t> ids;
    std::string why;
    if (!cli::resolve_focus(set, args.names, ids, &why)) {
        ecuio_condition_set_free(conditions);
        ecuio_session_destroy(session);
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        return kExitUsage;
    }
    write_start(out);
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    const bool replay_ends = info.backend == ECUIO_BACKEND_REPLAY &&
                             !(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP);
    write_session(out, session, info);
    const uint32_t channels = module_count(info);
    cli::SessionSeen session_seen[ECUIO_MAX_CHANNELS] = {};
    ShownConditions shown;
    if (conditions != nullptr &&
        !attach_conditions(out, session, conditions, "file", &shown)) {
        out.flush();
        ecuio_session_destroy(session);
        return ended(out, kExitConnect);
    }
    // --pick ranked: the names join the set's ranked signals as a default
    // pick; with no pick to make, the names alone are the focus
    uint32_t flags = 0;
    uint32_t min_sweep_ms = args.min_sweep_ms;
    if (args.pick_ranked) {
        const std::vector<uint16_t> picked = cli::default_pick(set, ids);
        if (!session_asks(session)) {
            std::fprintf(stderr, "havne-cli: the session asks no signal: "
                                 "--pick ranked adds none\n");
        } else if (picked.empty()) {
            std::fprintf(stderr, "havne-cli: the profile ranks no signal: "
                                 "--pick ranked adds none\n");
        } else {
            ids = picked;
            flags = ECUIO_FOCUS_DEFAULT_PICK;
        }
    }
    if (ids.empty()) {
        min_sweep_ms = 0; // no focus: the whole set, which takes no minimum
    }
    // The focus before the recorder attaches: the header's focus line is
    // the focus at the attach
    st = ecuio_session_set_focus_with(
        session, ids.empty() ? nullptr : ids.data(),
        static_cast<uint32_t>(ids.size()), min_sweep_ms, flags);
    if (st != ECUIO_OK) {
        write_error(out, st, ecuio_last_error_detail());
        out.flush();
        ecuio_session_destroy(session);
        return ended(out, kExitConnect);
    }
    {
        BigLine b(cli::focus_line_bytes(ids.size()));
        cli::focus_line(b.line, set, ids, min_sweep_ms,
                        flags == ECUIO_FOCUS_DEFAULT_PICK);
        write_line(out, b.line);
    }
    ecuio_recorder_t* recorder = nullptr;
    bool recording_failed = false;
    if (!open_recording(out, session, args.folder, args.label, &recorder,
                        args.armed)) {
        out.flush();
        ecuio_session_destroy(session);
        return ended(out, kExitFailed);
    }
    // The folder, which the log and end lines name: a path core can't give
    // whole is an error, never an empty or cut folder in the lines. An
    // armed log's is each window's as it opens, none before the first.
    std::string folder;
    cli::ArmedSeen armed_seen;
    if (!args.armed) {
        char buf[4096];
        uint32_t len = 0;
        st = ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len);
        if (st != ECUIO_OK || len >= sizeof buf) {
            write_error(out, st != ECUIO_OK ? st : ECUIO_ERR_INVALID_ARGUMENT,
                        st != ECUIO_OK
                            ? "the recording's folder couldn't be read"
                            : "the recording's folder path is longer than "
                              "4095 bytes");
            out.flush();
            ecuio_session_destroy(session);
            close_recording(out, &recorder,
                            "havne-cli: the folder couldn't be read",
                            &recording_failed);
            return ended(out, kExitFailed);
        }
        folder.assign(buf, len);
    } else {
        /* Said before the start: a fast replay can fire before the first
         * read of the state */
        char buf[96];
        cli::Line l(buf, sizeof buf);
        cli::armed_line(l, 0);
        write_line(out, l);
        armed_seen.armed_said = true;
    }
    out.flush();

    LogFrames frames;
    ecuio_register_frame_callback(session, on_log_frame, &frames);
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        close_recording(out, &recorder, "the session didn't start",
                        &recording_failed);
        return ended(out, kExitConnect);
    }

    // Session time's clock (ADR-0033): --seconds and the log line's
    // period count a sleep too
    const uint64_t started = ecuio_clock_us();
    cli::SleepWatch sleep_watch(ecuio_clock_slept_us());
    cli::LogTicker ticker(started, log_every_us());
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
    const char* reason = nullptr;
    while (!reason) {
        std::this_thread::sleep_for(kPoll);
        const uint64_t now = ecuio_clock_us();
        if (const uint64_t slept = sleep_watch.poll(ecuio_clock_slept_us())) {
            char buf[64];
            cli::Line l(buf, sizeof buf);
            l.raw("{\"type\":\"slept\"");
            l.key("us");
            l.u64(slept);
            l.ch('}');
            write_line(out, l);
        }
        poll_recording(out, recorder, nullptr, &recording_failed);
        if (args.armed) {
            poll_armed(out, recorder, &armed_seen, &folder);
        }
        poll_health(out, session, &last_error, &episodes);
        poll_sessions(out, session, channels, session_seen);
        poll_conditions(out, session, &shown);
        reason = cli::log_end_reason(
            g_interrupted.load(), recording_failed,
            replay_ends &&
                replay_done(session, info,
                            frames.covered.load(std::memory_order_acquire)),
            args.seconds != 0 && now - started >= args.seconds * 1000000u);
        if (ticker.due(now)) {
            char buf[256];
            cli::Line l(buf, sizeof buf);
            cli::log_line(l,
                          log_readout(started, folder, recorder, args.armed));
            write_line(out, l);
        }
        out.flush();
        if (out.failed) {
            break;
        }
    }
    // The loop left on a failed output (a closed pipe, a full stdout):
    // nothing more is written, but the recording's close needs its why
    if (reason == nullptr) {
        reason = "output-failed";
    }

    // The close on a thread of its own, as stream's: interrupted and the
    // close still waiting (an adapter's teardown), the recording ends
    // whole now, the session going on unrecorded
    std::atomic<bool> closed{false};
    std::thread closer([&] {
        ecuio_session_close(session);
        closed.store(true);
    });
    const auto closing = std::chrono::steady_clock::now();
    while (!closed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (recorder != nullptr && g_interrupted.load() &&
            std::chrono::steady_clock::now() - closing >= kRecordingCloseWait) {
            close_recording(out, &recorder,
                            "interrupted: the session's close didn't finish",
                            &recording_failed, args.armed);
        }
    }
    closer.join();

    poll_sessions(out, session, channels, session_seen);
    int code = kExitOk;
    wait_for_writer(recorder, frames.covered.load(std::memory_order_acquire));
    poll_recording(out, recorder, nullptr, &recording_failed);
    if (args.armed) {
        poll_armed(out, recorder, &armed_seen, &folder);
    }
    const cli::LogReadout last =
        log_readout(started, folder, recorder, args.armed);
    BigLine end(cli::log_end_line_bytes(folder.c_str()));
    if (!out.failed) {
        cli::log_end_line(end.line, reason,
                          folder.empty() ? nullptr : folder.c_str(), last);
    }
    // The recording's last note is the end line, without the folder (its
    // path holds the account name, and a recording gets shared); it closes
    // after its session, as the app's does
    if (recorder != nullptr && end.line.size() > 0) {
        char noted_buf[256];
        cli::Line noted(noted_buf, sizeof noted_buf);
        cli::log_end_line(noted, reason, nullptr, last);
        const std::string text(noted.data(), noted.size());
        ecuio_recorder_note(recorder, text.c_str());
    }
    ecuio_session_destroy(session);
    const std::string why_closed = std::string("havne-cli: ") + reason;
    close_recording(out, &recorder, why_closed.c_str(), &recording_failed,
                    args.armed);
    if (!out.failed) {
        write_line(out, end.line);
        out.flush();
    }
    if (!out.ok() || recording_failed) {
        code = kExitFailed;
    }
#ifdef _WIN32
    g_done.store(true);
#endif
    return code;
}

} // namespace cmd
