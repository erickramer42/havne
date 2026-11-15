// File/layer: cli/cmd_stream.cpp | havne-cli, stream (ADR-0031)
// Testing: UNIT: CliGoldenTest, CliReplayTest, CliRecordTest,
//          CliSignalTest, CliConditionsRunTest, CliExitTest (ctest);
//          CliMarkLinesTest (test_cli); BENCH
//
// Public headers only: what any program linking core can do.

#include "condition_lines.h"
#include "default_pick.h"
#include "log_lines.h"
#include "timing_line.h"
#include "jsonl.h"
#include "line_ring.h"
#include "mark_lines.h"
#include "refused_signals.h"
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

constexpr size_t kRingBytes = size_t{1} << 20;

// The ring's size. HAVNE_CLI_RING_BYTES (tests only) makes it small, so
// a golden run fills it: a replay must then wait, not drop. Never below
// one whole line.
size_t ring_bytes()
{
    char env[32] = {};
#ifdef _WIN32
    const DWORD n =
        GetEnvironmentVariableA("HAVNE_CLI_RING_BYTES", env, sizeof env);
    if (n == 0 || n >= sizeof env) {
        return kRingBytes;
    }
#else
    const char* value = std::getenv("HAVNE_CLI_RING_BYTES");
    if (!value || std::strlen(value) >= sizeof env) {
        return kRingBytes;
    }
    std::memcpy(env, value, std::strlen(value));
#endif
    if (!*env) {
        return kRingBytes;
    }
    const unsigned long long v = std::strtoull(env, nullptr, 10);
    const size_t floor = kLineBytes + 256;
    return v < floor        ? floor
           : v > kRingBytes ? kRingBytes
                            : static_cast<size_t>(v);
}

// What the callbacks share with the main thread
struct Stream {
    cli::LineRing ring;
    bool wait = false;                // a replay: wait for room, never drop
    uint64_t limit = 0;               // --count; 0 = none
    std::atomic<uint64_t> covered{0}; // frames written or dropped, up to limit
    // The worker's own: frames seen, and whether the last one was
    // written (its values follow it, or not)
    uint64_t seen = 0;
    bool last_written = false;
    // --conditions: each condition's key by its index, set before start
    // and never changed after; the events no line was written for
    std::vector<std::string> condition_keys;
    std::vector<uint8_t> condition_link_terms;
    std::atomic<uint64_t> conditions_dropped{0};
};

void on_frame(const ecuio_frame_t* f, void* user)
{
    auto* s = static_cast<Stream*>(user);
    const uint64_t n = ++s->seen;
    if (s->limit && n > s->limit) {
        s->last_written = false;
        return;
    }
    char buf[kLineBytes];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"frame\"");
    l.key("t_us");
    l.u64(f->timestamp_us);
    l.key("id");
    l.hex_id(f->arbitration_id);
    l.key("ch");
    l.u64(f->channel_id);
    l.key("flags");
    l.u64(f->flags);
    l.key("data");
    l.hex_bytes(f->data, f->data_length < ECUIO_FRAME_DATA_MAX
                             ? f->data_length
                             : ECUIO_FRAME_DATA_MAX);
    l.ch('}');
    s->last_written = l.finish() && s->ring.push(cli::LineKind::Frame, l.data(),
                                                 l.size(), s->wait);
    s->covered.store(n, std::memory_order_release);
}

// A frame's condition events (ADR-0063), after its values line: a line
// each, only for a frame that was written; a link change's, which have no
// frame, whenever they come. An event with no line is a change the
// reader never sees, so each is counted for the end line.
void on_conditions(const ecuio_condition_event_t* events, uint32_t count,
                   void* user)
{
    auto* s = static_cast<Stream*>(user);
    const uint64_t lost = cli::write_condition_events(
        events, count, s->condition_keys, s->last_written,
        s->limit != 0 && s->seen > s->limit,
        [s](const char* line, size_t n) {
            return s->ring.push(cli::LineKind::Condition, line, n, s->wait);
        },
        &s->condition_link_terms);
    if (lost != 0) {
        s->conditions_dropped.fetch_add(lost, std::memory_order_relaxed);
    }
}

void on_values(const ecuio_value_batch_t* batch, void* user)
{
    auto* s = static_cast<Stream*>(user);
    if (!s->last_written) {
        return;
    }
    char buf[kLineBytes];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"values\"");
    l.key("t_us");
    l.u64(batch->timestamp_us);
    l.key("id");
    l.hex_id(batch->arbitration_id);
    l.key("values");
    l.ch('[');
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (i) {
            l.ch(',');
        }
        l.ch('{');
        l.key("id");
        l.u64(v.signal_id);
        if (v.kind == ECUIO_VALUE_ENUM) {
            // The raw number, and its label when the set has one
            // (ADR-0051): the label is at most ECUIO_ENUM_LABEL_MAX bytes
            l.key("value");
            l.number(v.value);
            if (v.text) {
                l.key("label");
                l.string(v.text, v.text_len, ECUIO_ENUM_LABEL_MAX);
            }
        } else if (v.text) {
            l.key("text");
            // Room for ,"cut":true}]}\n after a cut text
            const size_t limit = l.room() > 32 ? l.room() - 32 : 0;
            if (l.string(v.text, v.text_len, limit) < v.text_len) {
                l.key("cut");
                l.raw("true");
            }
        } else {
            l.key("value");
            l.number(v.value);
        }
        l.ch('}');
    }
    l.raw("]}");
    if (l.finish()) {
        s->ring.push(cli::LineKind::Values, l.data(), l.size(), s->wait);
    }
}

void read_marks(std::shared_ptr<MarkQueue> q)
{
    cli::MarkLines split;
    const auto push = [&q](const std::string& line) {
        std::lock_guard<std::mutex> g(q->m);
        q->lines.push_back(line);
    };
    char buf[1024];
    for (;;) {
        size_t n = 0;
#ifdef _WIN32
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        if (in == nullptr || in == INVALID_HANDLE_VALUE) {
            break;
        }
        DWORD mode = 0;
        if (GetConsoleMode(in, &mode)) {
            // A console types UTF-16; ReadFile would give its code page's
            // bytes, not UTF-8
            wchar_t w[256];
            DWORD got = 0;
            if (!ReadConsoleW(in, w, 256, &got, nullptr) || got == 0) {
                break;
            }
            const int len =
                WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(got), buf,
                                    sizeof buf, nullptr, nullptr);
            if (len <= 0) {
                continue;
            }
            n = static_cast<size_t>(len);
        } else {
            DWORD got = 0;
            if (!ReadFile(in, buf, sizeof buf, &got, nullptr) || got == 0) {
                break;
            }
            n = got;
        }
#else
        const ssize_t got = ::read(0, buf, sizeof buf);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        n = static_cast<size_t>(got);
#endif
        split.feed(buf, n, push);
    }
    split.finish(push);
}

// The end line's faults: what a replay's ?faults= did (ADR-0041)
void write_faults(cli::Line& l, const ecuio_session_t* session)
{
    ecuio_replay_faults_t f;
    if (ecuio_session_get_replay_faults(session, &f) != ECUIO_OK) {
        return;
    }
    l.key("faults");
    l.ch('{');
    l.key("frames");
    l.u64(f.frames);
    l.key("dropped");
    l.u64(f.dropped);
    l.key("overflowed");
    l.u64(f.overflowed);
    l.key("corrupted");
    l.u64(f.corrupted);
    l.key("truncated");
    l.u64(f.truncated);
    l.key("stalled");
    l.u64(f.stalled);
    l.ch('}');
}

} // namespace

int run_stream(const StreamArgs& args)
{
    Output out;
    auto stream = std::unique_ptr<Stream>(new (std::nothrow) Stream);
    if (!stream || !stream->ring.init(ring_bytes())) {
        std::fprintf(stderr, "havne-cli: out of memory for the output ring\n");
        return kExitFailed;
    }
    std::vector<char> drained;
    try {
        drained.resize(stream->ring.capacity());
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "havne-cli: out of memory for the output ring\n");
        return kExitFailed;
    }
    stream->limit = args.count;

    write_start(out);
    // --conditions, else a replay's own copy
    ecuio_condition_set_t* conditions = nullptr;
    if (args.conditions != nullptr) {
        std::string why;
        ecuio_status_t loaded = ECUIO_OK;
        conditions = load_conditions_file(args.conditions, &loaded, &why);
        if (conditions == nullptr) {
            write_error(out, loaded, why.c_str());
            out.flush();
            return kExitConnect;
        }
    } else {
        conditions = recording_conditions(args.uri);
    }
    const char* conditions_source =
        args.conditions != nullptr ? "file" : "recording";
    const bool judging = conditions != nullptr;
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, args.uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        out.flush();
        ecuio_condition_set_free(conditions);
        return kExitConnect;
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    const bool replay = info.backend == ECUIO_BACKEND_REPLAY;
    stream->wait = replay;
    const bool replay_ends =
        replay && !(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP);
    write_session(out, info);
    ShownConditions shown;
    if (conditions != nullptr) {
        if (!attach_conditions(out, session, conditions, conditions_source,
                               &shown)) {
            out.flush();
            ecuio_session_destroy(session);
            return kExitConnect;
        }
        conditions = nullptr;
        stream->condition_keys = shown.keys;
        stream->condition_link_terms = shown.link_terms;
        ecuio_register_conditions_callback(session, on_conditions,
                                           stream.get());
    }
    // --pick ranked before a recording attaches, so its header names the
    // pick; as in the app, a refused pick leaves the whole set asked
    if (args.pick_ranked) {
        const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
        const std::vector<uint16_t> ids = cli::default_pick(set, {});
        if (!session_asks(session)) {
            std::fprintf(stderr, "havne-cli: the session asks no signal: "
                                 "--pick ranked picks none\n");
        } else if (ids.empty()) {
            std::fprintf(stderr, "havne-cli: the profile ranks no signal: "
                                 "--pick ranked asks every one\n");
        } else if (ecuio_session_set_focus_with(
                       session, ids.data(), static_cast<uint32_t>(ids.size()),
                       0, ECUIO_FOCUS_DEFAULT_PICK) != ECUIO_OK) {
            std::fprintf(stderr, "havne-cli: asking every signal: %s\n",
                         ecuio_last_error_detail());
        } else {
            BigLine b(cli::focus_line_bytes(ids.size()));
            cli::focus_line(b.line, set, ids, 0, true);
            write_line(out, b.line);
        }
    }
    ecuio_recorder_t* recorder = nullptr;
    bool recording_failed = false;
    if (args.record != nullptr &&
        !open_recording(out, session, args.record, args.label, &recorder)) {
        out.flush();
        ecuio_session_destroy(session);
        return kExitFailed;
    }
    // A read that completed at create (the mock's) comes first; the rest
    // as they complete
    const uint32_t channels =
        info.channel_count < ECUIO_SESSION_INFO_MAX_CHANNELS
            ? info.channel_count
            : ECUIO_SESSION_INFO_MAX_CHANNELS;
    uint32_t diag_generations[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    uint32_t identity_generations[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    char vehicle_vin[ECUIO_VIN_TEXT_LEN] = "";
    poll_identity(out, session, channels, identity_generations, vehicle_vin);
    poll_diagnostics(out, session, channels, diag_generations);
    cli::ScanSeen scan_seen[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    cli::SessionSeen session_seen[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    ecuio_channel_support_t scan_reports[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    poll_scan(out, session, channels, scan_seen, session_seen, scan_reports);
    // Judged from the values core decodes, so only with --values
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    std::vector<uint16_t> absent_read(
        (args.values && set != nullptr && set->defs != nullptr) ? set->count
                                                                : 0u);
    std::vector<uint16_t> absent_last;
    std::vector<cli::RefusedSignal> refused_last;
    std::vector<cli::RefusedMore> refused_more_last;
    cli::TimingSnapshots timing_snaps;
    std::vector<ecuio_request_timing_t> timing_rows(ECUIO_PROFILE_MAX_REQUESTS);
    if (args.values) {
        write_signals(out, set, session_signal_file());
    }
    out.flush();

    ecuio_register_frame_callback(session, on_frame, stream.get());
    if (args.values) {
        ecuio_register_values_callback(session, on_values, stream.get());
    }
    install_handlers();
    // --until-scanned with no did= range to scan has nothing to wait for:
    // it ends before it starts
    const bool nothing_to_scan =
        args.until_scanned && !cli::scans_any(scan_reports, channels);
    st = nothing_to_scan ? ECUIO_OK : ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        close_recording(out, &recorder, "the session didn't start",
                        &recording_failed);
        out.flush();
        return kExitConnect;
    }
    std::shared_ptr<MarkQueue> marks;
    if (recorder != nullptr) {
        try {
            marks = std::make_shared<MarkQueue>();
            std::thread(read_marks, marks).detach();
        } catch (const std::exception&) {
            marks.reset(); // no marks, the recording goes on
        }
    }

    // Session time's clock (ADR-0033): --seconds counts a sleep too
    const uint64_t started = ecuio_clock_us();
    cli::SleepWatch sleep_watch(ecuio_clock_slept_us());
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
    const char* reason = nothing_to_scan ? "nothing-to-scan" : nullptr;
    while (!reason) {
        const size_t n = stream->ring.drain(drained.data(), kPoll);
        out.write(drained.data(), n);
        out.flush();
        if (out.failed) {
            break;
        }
        const uint64_t covered =
            stream->covered.load(std::memory_order_acquire);
        if (g_interrupted.load()) {
            reason = "interrupted";
        } else if (args.count && covered >= args.count) {
            reason = "count";
        } else if (replay_ends && replay_done(session, info, covered)) {
            reason = "replay-end";
        } else if (args.seconds &&
                   ecuio_clock_us() - started >= args.seconds * 1000000u) {
            reason = "seconds";
        } else if (args.until_scanned &&
                   cli::scans_complete(scan_reports, channels)) {
            reason = "scanned";
        }

        if (const uint64_t slept = sleep_watch.poll(ecuio_clock_slept_us())) {
            char buf[64];
            cli::Line l(buf, sizeof buf);
            l.raw("{\"type\":\"slept\"");
            l.key("us");
            l.u64(slept);
            l.ch('}');
            write_line(out, l);
        }

        poll_identity(out, session, channels, identity_generations,
                      vehicle_vin);
        poll_diagnostics(out, session, channels, diag_generations);
        poll_scan(out, session, channels, scan_seen, session_seen,
                  scan_reports);
        poll_absent(out, session, absent_read, absent_last);
        if (args.values) {
            poll_refused(out, set, scan_reports, channels, refused_last,
                         refused_more_last);
        }
        poll_timing(out, session, info, timing_snaps, timing_rows, false);
        poll_recording(out, recorder, marks.get(), &recording_failed);
        poll_conditions(out, session, &shown);

        poll_health(out, session, &last_error, &episodes);
        // A replay whose file changed has stopped, short of its last
        // frame: the health line above says so, then the stream ends
        if (!reason && replay && last_error == ECUIO_ERR_RECORDING_CHANGED) {
            reason = "recording-changed";
        }
    }

    // The loop left on a failed output (a closed pipe, a full stdout):
    // nothing more is written, but the recording's close needs its why
    if (reason == nullptr) {
        reason = "output-failed";
    }
    // Close joins the worker, whose last callbacks may still be pushing
    // (a replay waiting for room): keep draining until it returns. With
    // the output gone, nothing drains, so the waits end instead.
    if (out.failed) {
        stream->ring.stop();
    }
    std::atomic<bool> closed{false};
    std::thread closer([&] {
        ecuio_session_close(session);
        closed.store(true);
        stream->ring.wake();
    });
    const auto closing = std::chrono::steady_clock::now();
    while (!closed.load()) {
        const size_t n = stream->ring.drain(drained.data(), kPoll);
        out.write(drained.data(), n);
        if (out.failed) {
            stream->ring.stop();
        }
        // Interrupted and the close still waits (an adapter's teardown):
        // the recording ends whole now, the session going on unrecorded
        if (recorder != nullptr && g_interrupted.load() &&
            std::chrono::steady_clock::now() - closing >= kRecordingCloseWait) {
            close_recording(out, &recorder,
                            "interrupted: the session's close didn't finish",
                            &recording_failed);
        }
    }
    closer.join();
    out.write(drained.data(),
              stream->ring.drain(drained.data(), std::chrono::milliseconds(0)));

    int code = kExitOk;
    poll_recording(out, recorder, marks.get(), &recording_failed);
    char buf[512];
    cli::Line l(buf, sizeof buf);
    if (!out.failed) {
        poll_scan(out, session, channels, scan_seen, session_seen, scan_reports,
                  true);
        poll_timing(out, session, info, timing_snaps, timing_rows, true);
        uint64_t covered = stream->covered.load();
        if (args.count && covered > args.count) {
            covered = args.count;
        }
        l.raw("{\"type\":\"end\"");
        l.key("reason");
        l.string(reason, SIZE_MAX);
        l.key("frames");
        l.u64(covered);
        l.key("dropped_frames");
        l.u64(stream->ring.dropped_frames());
        l.key("dropped_values");
        l.u64(stream->ring.dropped_values());
        if (judging) {
            l.key("conditions_dropped");
            l.u64(stream->conditions_dropped.load());
        }
        if (info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS) {
            write_faults(l, session);
        }
        write_isotp(l, session);
        l.ch('}');
    }
    // The recording's last note is the end line; it closes after its
    // session, as the app's does
    if (recorder != nullptr && l.size() > 0) {
        const std::string end(l.data(), l.size());
        ecuio_recorder_note(recorder, end.c_str());
    }
    ecuio_session_destroy(session);
    const std::string why = std::string("havne-cli: ") + reason;
    close_recording(out, &recorder, why.c_str(), &recording_failed);
    if (!out.failed) {
        write_line(out, l);
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
