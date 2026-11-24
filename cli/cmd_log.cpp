// File/layer: cli/cmd_log.cpp | havne-cli, log (ADR-0056 section 7)
// Testing: UNIT: CliLogRunTest, CliGoldenTest, CliUsageTest (ctest);
//          CliLogTest, CliSummaryTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "default_pick.h"
#include "log_lines.h"
#include "jsonl.h"
#include "scan_line.h"
#include "sleep_watch.h"
#include "summary_lines.h"

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
// counts them as it writes them; log writes none), and the summary's
// tally, which the main thread reads once the close has joined the worker
struct LogFrames {
    std::atomic<uint64_t> covered{0};
    cli::SummaryTally* tally = nullptr;
};

void on_log_frame(const ecuio_frame_t*, void* user)
{
    auto* f = static_cast<LogFrames*>(user);
    f->tally->on_frame();
    f->covered.fetch_add(1, std::memory_order_release);
}

void on_log_values(const ecuio_value_batch_t* batch, void* user)
{
    static_cast<cli::SummaryTally*>(user)->on_batch(*batch);
}

// The interval line of each signal in the window, in the set's order,
// then the summary line: built after the close joined the worker and
// before the destroy frees the set
std::vector<std::string> summary_lines(const ecuio_signal_set_t* set,
                                       const std::vector<uint16_t>& picks,
                                       const cli::SummaryTally& tally)
{
    std::vector<std::string> lines;
    const std::vector<ecuio_signal_key_t> keys =
        cli::set_keys(set, session_signal_file());
    const std::vector<cli::SignalTally>& all = tally.tallies();
    for (size_t i = 0; i < all.size(); ++i) {
        if (!cli::in_window(all[i], tally.epoch())) {
            continue;
        }
        BigLine b(cli::interval_line_bytes());
        cli::interval_line(b.line, set->defs[i],
                           i < keys.size() ? keys[i].text : nullptr,
                           sizeof(ecuio_signal_key_t::text), all[i]);
        if (b.line.finish()) {
            lines.emplace_back(b.line.data(), b.line.size());
        }
    }
    BigLine b(cli::summary_line_bytes());
    cli::summary_line(b.line, set, keys, picks, tally);
    if (b.line.finish()) {
        lines.emplace_back(b.line.data(), b.line.size());
    }
    return lines;
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

// What run_log's phases share: the session and its lines, the focus,
// the recording, the loop's clock and its end
struct LogRun {
    ecuio_condition_set_t* conditions = nullptr;
    ecuio_session_t* session = nullptr;
    const ecuio_signal_set_t* set = nullptr;
    std::vector<uint16_t> ids;
    ecuio_session_info_t info{};
    bool replay_ends = false;
    uint32_t channels = 0;
    cli::SessionSeen session_seen[ECUIO_MAX_CHANNELS] = {};
    ShownConditions shown;
    ecuio_recorder_t* recorder = nullptr;
    bool recording_failed = false;
    std::string folder;
    cli::ArmedSeen armed_seen;
    LogFrames frames;
    cli::SummaryTally tally{nullptr}; /* made for the focus before start */
    uint64_t started = 0;
    const char* reason = nullptr;
};

// A phase's answer: kGoOn, or the command's exit code
constexpr int kGoOn = -1;

// The conditions file, the connect, and the names against the set: a typo
// is usage before any line is written
int log_connect(Output& out, const cli::LogArgs& args, LogRun& r)
{
    ecuio_condition_set_t*& conditions = r.conditions;
    ecuio_session_t*& session = r.session;
    std::vector<uint16_t>& ids = r.ids;
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
    std::string detail;
    ecuio_status_t st = create_consented(out, args.uri, &session, &detail,
                                         /*start_first=*/true);
    if (st != ECUIO_OK) {
        ecuio_condition_set_free(conditions);
        write_start(out);
        write_error(out, st, detail.c_str());
        return ended(out, kExitConnect);
    }
    r.set = ecuio_session_signal_set(session);
    const ecuio_signal_set_t* set = r.set;
    std::string why;
    if (!cli::resolve_focus(set, args.names, ids, &why,
                            session_signal_file())) {
        ecuio_condition_set_free(conditions);
        ecuio_session_destroy(session);
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        return kExitUsage;
    }
    return kGoOn;
}

// The start and session lines, and the conditions attached
int log_session_lines(Output& out, const cli::LogArgs& args, LogRun& r)
{
    ecuio_session_t* session = r.session;
    ecuio_condition_set_t* conditions = r.conditions;
    ecuio_session_info_t& info = r.info;
    write_start(out);
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    r.replay_ends = info.backend == ECUIO_BACKEND_REPLAY &&
                    !(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP);
    write_held_session(out, session, info, args.allow_sleep);
    r.channels = module_count(info);
    ShownConditions& shown = r.shown;
    if (conditions != nullptr &&
        !attach_conditions(out, session, conditions, "file", &shown)) {
        out.flush();
        ecuio_session_destroy(session);
        return ended(out, kExitConnect);
    }
    return kGoOn;
}

// The focus: the names, or --pick ranked's default pick, set before the
// recorder attaches, and its line
int log_focus(Output& out, const cli::LogArgs& args, LogRun& r)
{
    ecuio_session_t* session = r.session;
    ecuio_condition_set_t* conditions = r.conditions;
    const ecuio_signal_set_t* set = r.set;
    std::vector<uint16_t>& ids = r.ids;
    // --pick ranked: the names join the set's ranked signals as a default
    // pick; with no pick to make, the names alone are the focus
    uint32_t flags = 0;
    uint32_t min_sweep_ms = args.min_sweep_ms;
    const bool pick_only = session_pick_only(session);
    if (args.pick_ranked) {
        const std::vector<uint16_t> picked =
            cli::default_pick(set, ids, pick_only);
        if (!session_asks(session)) {
            std::fprintf(stderr, "havne-cli: the session asks no signal: "
                                 "--pick ranked adds none\n");
        } else if (picked.empty()) {
            std::fprintf(stderr, "havne-cli: the profile ranks no signal: "
                                 "--pick ranked adds none\n");
        } else {
            if (pick_only && !cli::set_ranks(set)) {
                std::fprintf(stderr, "havne-cli: the profile ranks no signal: "
                                     "--pick ranked adds none\n");
            }
            ids = picked;
            flags = ECUIO_FOCUS_DEFAULT_PICK;
        }
    }
    if (ids.empty() && pick_only && conditions == nullptr) {
        // Nothing is asked without a pick or a condition (core asks an
        // attached condition's signals): refuse before a recording of
        // nothing opens
        static const char kWhy[] = "the profile is asked only what is "
                                   "picked: name signals with --signal or "
                                   "attach conditions with --conditions";
        write_error(out, ECUIO_ERR_INVALID_ARGUMENT, kWhy);
        out.flush();
        std::fprintf(stderr, "havne-cli: %s\n", kWhy);
        ecuio_condition_set_free(conditions);
        ecuio_session_destroy(session);
        return ended(out, kExitConnect);
    }
    if (ids.empty()) {
        min_sweep_ms = 0; // no focus: the whole set, which takes no minimum
    }
    // The focus before the recorder attaches: the header's focus line is
    // the focus at the attach
    const ecuio_status_t st = ecuio_session_set_focus_with(
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
                        flags == ECUIO_FOCUS_DEFAULT_PICK,
                        session_signal_file());
        write_line(out, b.line);
    }
    return kGoOn;
}

// The recording, and the folder the log and end lines name
int log_recording(Output& out, const cli::LogArgs& args, LogRun& r)
{
    ecuio_session_t* session = r.session;
    ecuio_recorder_t*& recorder = r.recorder;
    bool& recording_failed = r.recording_failed;
    if (!open_recording(out, session, args.folder, args.label, &recorder,
                        args.armed)) {
        out.flush();
        ecuio_session_destroy(session);
        return ended(out, kExitFailed);
    }
    // The folder, which the log and end lines name: a path core can't give
    // whole is an error, never an empty or cut folder in the lines. An
    // armed log's is each window's as it opens, none before the first.
    std::string& folder = r.folder;
    cli::ArmedSeen& armed_seen = r.armed_seen;
    if (!args.armed) {
        char buf[4096];
        uint32_t len = 0;
        const ecuio_status_t st =
            ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len);
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
    return kGoOn;
}

// The frame and values callbacks, the summary's tally for the focus, and
// the start
int log_start(Output& out, const cli::LogArgs&, LogRun& r)
{
    ecuio_session_t* session = r.session;
    ecuio_recorder_t*& recorder = r.recorder;
    bool& recording_failed = r.recording_failed;
    LogFrames& frames = r.frames;
    r.tally = cli::SummaryTally(r.set, r.ids);
    frames.tally = &r.tally;
    ecuio_register_frame_callback(session, on_log_frame, &frames);
    ecuio_register_values_callback(session, on_log_values, &r.tally);
    install_handlers();
    const ecuio_status_t st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        close_recording(out, &recorder, "the session didn't start",
                        &recording_failed);
        return ended(out, kExitConnect);
    }
    return kGoOn;
}

// The loop until its end's reason: the lines each poll writes
void log_loop(Output& out, const cli::LogArgs& args, LogRun& r)
{
    ecuio_session_t* session = r.session;
    ecuio_recorder_t* recorder = r.recorder;
    bool& recording_failed = r.recording_failed;
    std::string& folder = r.folder;
    cli::ArmedSeen& armed_seen = r.armed_seen;
    cli::SessionSeen* session_seen = r.session_seen;
    ShownConditions& shown = r.shown;
    const ecuio_session_info_t& info = r.info;
    const uint32_t channels = r.channels;
    const bool replay_ends = r.replay_ends;
    LogFrames& frames = r.frames;
    const char*& reason = r.reason;
    // Session time's clock (ADR-0033): --seconds and the log line's
    // period count a sleep too
    r.started = ecuio_clock_us();
    const uint64_t started = r.started;
    cli::SleepWatch sleep_watch(ecuio_clock_slept_us());
    cli::LogTicker ticker(started, log_every_us());
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
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
}

// The close on a thread of its own, the recording ended first when an
// interrupt finds the close still waiting
void log_close(Output& out, const cli::LogArgs& args, LogRun& r)
{
    ecuio_session_t* session = r.session;
    ecuio_recorder_t*& recorder = r.recorder;
    bool& recording_failed = r.recording_failed;
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
}

// The last lines, the end noted into the recording, the destroy and the
// recording's close, then the end line and the exit code
int log_end(Output& out, const cli::LogArgs& args, LogRun& r)
{
    ecuio_session_t* session = r.session;
    ecuio_recorder_t*& recorder = r.recorder;
    bool& recording_failed = r.recording_failed;
    std::string& folder = r.folder;
    cli::ArmedSeen& armed_seen = r.armed_seen;
    cli::SessionSeen* session_seen = r.session_seen;
    const uint32_t channels = r.channels;
    LogFrames& frames = r.frames;
    const uint64_t started = r.started;
    const char* reason = r.reason;
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
    const std::vector<std::string> summary =
        summary_lines(r.set, r.ids, r.tally);
    ecuio_session_destroy(session);
    const std::string why_closed = std::string("havne-cli: ") + reason;
    close_recording(out, &recorder, why_closed.c_str(), &recording_failed,
                    args.armed);
    if (!out.failed) {
        for (const std::string& text : summary) {
            out.write(text.data(), text.size());
        }
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

} // namespace

// The analysis log from the command line (ADR-0056 section 7): the
// session, the focus on the signals named (core asks those alone, as
// often as the bus allows), a recorder attached before start so the
// folder's header carries the focus, and a log line each period of
// session time with the recording's size; no frame or values lines, the
// folder is the output. After the close, an interval line for each signal
// and a summary line (cli/summary_lines.h) say what the log reached.
// The names are checked against the session's set
// before any line is written, so a typo is usage: exit 2, nothing on
// stdout. Ends at Ctrl+C, --seconds, a failed recording (exit 1) or a
// replay's last frame; a lost link is logged in health lines and waited
// out. Exit codes as stream's.
int run_log(const cli::LogArgs& args)
{
    Output out;
    LogRun r;
    for (int (*phase)(Output&, const cli::LogArgs&, LogRun&) :
         {log_connect, log_session_lines, log_focus, log_recording,
          log_start}) {
        const int code = phase(out, args, r);
        if (code != kGoOn) {
            return code;
        }
    }
    log_loop(out, args, r);
    log_close(out, args, r);
    return log_end(out, args, r);
}

// log's arguments and its profile, then run_log
int cmd_log(int argc, char** argv)
{
    cli::LogArgs la;
    std::string why;
    if (!take_consent_store(argc, argv, &la.consent_store)) {
        usage(stderr);
        return kExitUsage;
    }
    if (!cli::parse_log(argc, argv, la, &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    std::string opened;
    const int rc = take_profile_source(la.signals, la.plugin, &la.uri, &opened);
    if (rc >= 0) {
        return rc;
    }
    const int consent = need_consent(la.uri, la.plugin, la.consent_store);
    if (consent >= 0) {
        return consent;
    }
    return run_log(la);
}

} // namespace cmd
