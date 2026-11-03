// File/layer: core/src/replay_transport.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest.Replay* (the reader's refusals with their line
//          numbers, either line ending, a whole-message payload, pacing at
//          speed 1 and 10, late frames catching up with no drift, stamps
//          rebased and never decreasing, the backward-step fallback, loop
//          and end of file, an early stop's error state (a bad row, a
//          file gone, fewer rows; rows added played no further), the
//          query);
//          an early stop in a recording's link line
//          (RecorderTest.AReplayThatStopsEarlySaysSoInALinkLine); the
//          source's frame flags line passed on
//          (RecorderTest.AReplayPassesOnItsSourcesFrameFlagsLine);
//          BridgeTest.ReplayOfAMockRecordingDecodesWhatWasRecorded (the
//          round trip through core's recorder), .ReplayThatStopsEarlySaysSo;
//          CoreTest.ReplayOfTheVehicleFixtureDecodesTheSheetsValues;
//          CoreTest.ReplayFaults* (?faults= and ?seed=: each kind's
//          effect, its count and the same faults from the same seed),
//          BridgeTest.ReplayFaults* (the bridge and the recorder under
//          them); CoreTest.ReplayStretch* (?from= and ?to=: the rows
//          played, their stamps, a loop of the stretch, the refusals)
//
// replay:// (ADR-0023): a recording's frames.csv (ADR-0017) played back
// through the frame API. Attach reads the whole file once and refuses a
// bad row with its line number, but drops a last row with no newline and
// counts it (a write cut short, ADR-0049 section 7); the worker reads it again a row at a time
// and sends each frame when the recording's own session time, divided by
// the speed, has passed. Passive: no requests and no link health, except
// that a file changed since attach stops playback with
// ECUIO_ERR_RECORDING_CHANGED in the error state. ?faults= (ADR-0041)
// drops, corrupts, cuts short or holds back frames as they play, drawn
// from ?seed= so the same URI plays the same faults. ?from= and ?to=
// (recording_stretch.h) play a stretch alone, its first frame at 0.

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "error_detail.h"
#include "frames_csv.h"
#include "recording_stretch.h"
#include "replay_faults.h"
#include "session.h"
#include "uri_query.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ecuio_frames_csv;

/* speed is kept in thousandths: 0.1 to 100 */
constexpr uint32_t kSpeedMin = 100;
constexpr uint32_t kSpeedMax = 100000;
constexpr uint32_t kSpeedDefault = 1000;

/* What the source recording's header says of its frames' flags
 * (ADR-0049 section 2), so a recording of this replay says the same:
 * the line in the events.log beside a frames.csv, read up to the
 * header's blank line. Only core's two words are taken; anything else,
 * a file not named frames.csv (its folder's events.log isn't its own),
 * or no such line is an older recording's, whose flag says nothing. */
std::string source_frame_flags(const std::string& frames_csv)
{
    const char* const older = "from an older recording";
    const size_t slash = frames_csv.find_last_of("/\\");
    if (slash == std::string::npos ||
        frames_csv.compare(slash + 1, std::string::npos, "frames.csv") != 0) {
        return older;
    }
    native_path events;
    if (!events.set(frames_csv.substr(0, slash + 1) + "events.log")) {
        return older;
    }
    FILE* f = events.open();
    if (f == nullptr) {
        return older;
    }
    std::vector<char> buf(256);
    bool too_long = false;
    std::string found = older;
    static const char kFirst[] = "ecuio session recording, format ";
    static const char kKey[] = "frame flags  ";
    if (read_line(f, buf, &too_long) && !too_long &&
        std::strncmp(buf.data(), kFirst, sizeof kFirst - 1) == 0) {
        for (int n = 0; n < 1000 && read_line(f, buf, &too_long); ++n) {
            if (too_long) {
                break;
            }
            if (buf[0] == '\0') {
                break;
            }
            if (std::strncmp(buf.data(), kKey, sizeof kKey - 1) == 0) {
                const char* v = buf.data() + sizeof kKey - 1;
                if (std::strcmp(v, "complete") == 0) {
                    found = v;
                }
                break;
            }
        }
    }
    std::fclose(f);
    return found;
}

/* What attach learns of the file for the worker: the rows before the
 * stretch, the last row a pass reads, the frames it plays and the session
 * time of its last (its first is 0) */
struct file_plan {
    uint64_t skip = 0;
    uint64_t rows = 0;
    uint64_t frames = 0;
    uint64_t span_us = 0;
};

/* The whole file, checked: every row parses, a torn last one aside (left
 * out and counted in *torn), and the stretch holds a frame. Reads through
 * the worker's own line buffer and frame, which attach has made. */
ecuio_status_t check_file(const native_path& path, std::vector<char>& buf,
                          ecuio_frame_t* frame,
                          const ecuio_stretch::stretch& stretch,
                          file_plan* plan, uint32_t* torn)
{
    FILE* f = path.open();
    if (f == nullptr) {
        ecuio_detail_set("couldn't open the recording's frames.csv");
        return ECUIO_ERR_LOAD_FAILED;
    }
    if (!read_header(f, buf)) {
        std::fclose(f);
        return ECUIO_ERR_LOAD_FAILED;
    }
    row prev, cur;
    uint64_t n = 0, span = 0, line_no = 1;
    uint64_t first_us = 0, last_us = 0;
    bool too_long = false;
    bool cut = false;
    ecuio_stretch::cursor in(stretch);
    file_plan p;
    while (read_line(f, buf, &too_long, &cut)) {
        ++line_no;
        if (cut) {
            *torn = 1;
            break;
        }
        const char* why = nullptr;
        if (too_long) {
            why = "the line is longer than any frame's row";
        } else if (!parse_row(buf.data(), &cur, frame, &why)) {
            /* why is set */
        }
        if (why != nullptr) {
            ecuio_detail_set("frames.csv line %llu: %s",
                             static_cast<unsigned long long>(line_no), why);
            std::fclose(f);
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (n == 0) {
            first_us = cur.host_us;
        }
        last_us = cur.host_us;
        ++n;
        if (in.plays(cur.host_us)) {
            if (p.frames == 0) {
                p.skip = n - 1;
            } else {
                span += step_us(prev, cur);
            }
            prev = cur;
            ++p.frames;
            p.rows = n;
            p.span_us = span;
        }
    }
    std::fclose(f);
    if (n == 0) {
        ecuio_detail_set("the recording's frames.csv holds no frames");
        return ECUIO_ERR_LOAD_FAILED;
    }
    if (p.frames == 0) {
        char why[192];
        ecuio_stretch::describe_empty(stretch, first_us, last_us, why,
                                      sizeof why);
        ecuio_detail_set("%s", why);
        return ECUIO_ERR_LOAD_FAILED;
    }
    *plan = p;
    return ECUIO_OK;
}

/* "0.1" to "100", or "max" (0): at most three decimals, no locale */
bool parse_speed(const ecuio_uri::param& p, uint32_t* milli)
{
    if (p.val_len == 3 && std::strncmp(p.val, "max", 3) == 0) {
        *milli = 0;
        return true;
    }
    uint64_t whole = 0, frac = 0;
    size_t i = 0, int_digits = 0, frac_digits = 0;
    for (; i < p.val_len && p.val[i] >= '0' && p.val[i] <= '9';
         ++i, ++int_digits) {
        whole = whole * 10 + static_cast<uint64_t>(p.val[i] - '0');
        if (whole > kSpeedMax) {
            return false;
        }
    }
    if (i < p.val_len && p.val[i] == '.') {
        for (++i; i < p.val_len && p.val[i] >= '0' && p.val[i] <= '9'; ++i) {
            if (++frac_digits > 3) {
                return false;
            }
            frac = frac * 10 + static_cast<uint64_t>(p.val[i] - '0');
        }
        if (frac_digits == 0) {
            return false;
        }
    }
    if (i != p.val_len || int_digits == 0) {
        return false;
    }
    for (size_t k = frac_digits; k < 3; ++k) {
        frac *= 10;
    }
    const uint64_t v = whole * 1000 + frac;
    if (v < kSpeedMin || v > kSpeedMax) {
        return false;
    }
    *milli = static_cast<uint32_t>(v);
    return true;
}

} // namespace

/* Everything the worker uses is made in attach, so the worker allocates
 * nothing: an exception can't end its thread */
struct ecuio_replay_transport {
    ecuio_session_t* session = nullptr;
    native_path path;                     /* the frames.csv */
    std::vector<char> line;               /* kLineMax + 2 */
    std::unique_ptr<ecuio_frame_t> frame; /* 4 KB, off the worker's stack */
    uint64_t skip = 0;                    /* rows before the stretch */
    uint64_t rows = 0;                    /* the last row a pass reads */
    uint32_t speed_milli = kSpeedDefault; /* 0 = max */
    bool loop = false;
    replay_faults faults; /* ?faults=, ?seed= */
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false; /* under mutex */
};

namespace {

/* The file isn't the one attach checked (a row that no longer parses,
 * fewer rows, or a file that won't open again): published through the
 * error state, one episode, so the app can say why playback stopped. The
 * count goes first, so a reader that sees the code sees its episode. */
void stop_early(ecuio_replay_transport* t)
{
    const uint32_t episodes = t->session->error_count.fetch_add(1) + 1;
    t->session->last_error.store(ECUIO_ERR_RECORDING_CHANGED);
    ecuio_record_link_state(t->session->record_link.get(),
                            ECUIO_ERR_RECORDING_CHANGED, episodes);
}

/* Sends the file's frames, paced by their session time over the speed;
 * on loop=1 again from the top, its stamps carried on past the last */
void replay_worker(ecuio_replay_transport* t)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<char>& buf = t->line;
    ecuio_frame_t* frame = t->frame.get();
    ecuio_session_t* s = t->session;
    uint64_t base_us = 0; /* session time at the start of this pass */
    /* ADR-0041: the draws run on across passes; stalls push every later
     * frame back */
    uint32_t draws = t->faults.seed;
    uint64_t stalled_us = 0;
    bool overflow_pending = false;
    for (;;) {
        FILE* f = t->path.open();
        if (f == nullptr || !read_header(f, buf)) {
            if (f != nullptr) {
                std::fclose(f);
            }
            stop_early(t);
            return;
        }
        row prev, cur;
        uint64_t pass_us = 0, n = 0;
        bool first = true, too_long = false, cut = false;
        /* Attach's rows and no more: rows added since (a recording
         * still being written) wait for another session. A row attach
         * read whole that has lost its newline since is a changed file. */
        while (n < t->rows && read_line(f, buf, &too_long, &cut)) {
            ++n;
            const char* why = nullptr;
            *frame = ecuio_frame_t{};
            if (too_long || cut || !parse_row(buf.data(), &cur, frame, &why)) {
                std::fclose(f);
                stop_early(t);
                return;
            }
            if (n <= t->skip) {
                continue; /* before the stretch: read, never sent */
            }
            if (!first) {
                pass_us += step_us(prev, cur);
            }
            first = false;
            prev = cur;
            s->replay_frames_read.fetch_add(1, std::memory_order_relaxed);
            if (t->faults.any) {
                const replay_fault_verdict v =
                    replay_fault_draw(t->faults, draws, frame->data_length);
                if (v.stall_ms != 0) {
                    stalled_us += static_cast<uint64_t>(v.stall_ms) * 1000;
                    s->replay_stalled.fetch_add(1, std::memory_order_relaxed);
                }
                if (v.drop) {
                    (v.overflow ? s->replay_overflowed : s->replay_dropped)
                        .fetch_add(1, std::memory_order_relaxed);
                    overflow_pending = overflow_pending || v.overflow;
                    continue;
                }
                if (v.corrupt) {
                    frame->data[v.corrupt_bit / 8] ^=
                        static_cast<uint8_t>(1u << (v.corrupt_bit % 8));
                    s->replay_corrupted.fetch_add(1, std::memory_order_relaxed);
                }
                if (v.truncate) {
                    std::memset(frame->data + v.truncate_to, 0,
                                frame->data_length - v.truncate_to);
                    frame->data_length = v.truncate_to;
                    s->replay_truncated.fetch_add(1, std::memory_order_relaxed);
                }
                if (overflow_pending) {
                    frame->flags |= ECUIO_FRAME_FLAG_OVERFLOW;
                    overflow_pending = false;
                }
            }
            const uint64_t now_us = base_us + pass_us + stalled_us;
            frame->timestamp_us = now_us;
            {
                std::unique_lock<std::mutex> lock(t->mutex);
                if (t->speed_milli != 0) {
                    const auto due =
                        start + std::chrono::microseconds(now_us * 1000 /
                                                          t->speed_milli);
                    t->wake.wait_until(lock, due, [t] { return t->stop; });
                }
                if (t->stop) {
                    std::fclose(f);
                    return;
                }
            }
            ecuio_session_dispatch_callbacks(t->session, frame);
        }
        std::fclose(f);
        if (n != t->rows) {
            stop_early(t); /* cut short: a reader waiting for the last
                              frame would wait forever */
            return;
        }
        if (!t->loop) {
            return; /* the end of the file: the session stays open, silent */
        }
        base_us += pass_us;
    }
}

ecuio_status_t replay_start(ecuio_session_t* session, void* impl)
{
    (void)session;
    ecuio_replay_transport* t = static_cast<ecuio_replay_transport*>(impl);
    if (t == nullptr) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    if (t->worker.joinable()) {
        return ECUIO_OK; /* already running: idempotent */
    }
    try {
        t->worker = std::thread(replay_worker, t);
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void replay_detach(ecuio_session_t* session, void* impl)
{
    (void)session;
    ecuio_replay_transport* t = static_cast<ecuio_replay_transport*>(impl);
    if (t == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(t->mutex);
        t->stop = true;
    }
    t->wake.notify_all();
    if (t->worker.joinable()) {
        t->worker.join();
    }
    delete t;
}

const ecuio_transport_ops_t kReplayTransportOps = {replay_start, replay_detach};

} // namespace

extern "C" {

const char* const ecuio_replay_query_keys[] = {
    "profile", "did", "speed", "loop", "faults", "seed", "from", "to", nullptr};

ecuio_status_t ecuio_replay_transport_attach(ecuio_session_t* session,
                                             const char* uri,
                                             const ecuio_uri_query* query)
{
    if (session->transport != nullptr) {
        return ECUIO_OK; /* idempotent */
    }
    uint32_t speed = kSpeedDefault;
    bool loop = false;
    replay_faults faults;
    ecuio_stretch::stretch stretch;
    for (size_t i = 0; i < query->count; ++i) { /* duplicates: last wins */
        const ecuio_uri::param& p = query->params[i];
        if (!ecuio_stretch::take(p, &stretch)) {
            ecuio_detail_set("%s", ecuio_stretch::kValueWhy);
            return ECUIO_ERR_BAD_URI;
        }
        if (p.is("speed") && !parse_speed(p, &speed)) {
            ecuio_detail_set("?speed= takes a factor from 0.1 to 100, at most "
                             "three decimals, or max");
            return ECUIO_ERR_BAD_URI;
        }
        if (p.is("loop")) {
            if (p.val_len != 1 || (p.val[0] != '0' && p.val[0] != '1')) {
                ecuio_detail_set("?loop= takes 0 or 1");
                return ECUIO_ERR_BAD_URI;
            }
            loop = p.val[0] == '1';
        }
        if (p.is("faults")) {
            replay_faults f;
            f.seed = faults.seed;
            if (!replay_parse_faults(p.val, p.val_len, &f)) {
                ecuio_detail_set(
                    "?faults= takes <kind>:<percent> joined by ',', each kind "
                    "once: drop, overflow, corrupt, truncate, and "
                    "stall:<percent>:<ms> (1 to 10000 ms); a percent is more "
                    "than 0 and at most 100, with up to three decimals");
                return ECUIO_ERR_BAD_URI;
            }
            faults = f;
        }
        if (p.is("seed") &&
            !replay_parse_seed(p.val, p.val_len, &faults.seed)) {
            ecuio_detail_set("?seed= takes a number from 1 to 4294967295");
            return ECUIO_ERR_BAD_URI;
        }
    }
    if (!ecuio_stretch::ordered(stretch)) {
        ecuio_detail_set("%s", ecuio_stretch::kOrderWhy);
        return ECUIO_ERR_BAD_URI;
    }

    const char* body = uri + std::strlen("replay://");
    const char* q = std::strchr(body, '?');
    const size_t body_len =
        q != nullptr ? static_cast<size_t>(q - body) : std::strlen(body);
    if (body_len == 0) {
        ecuio_detail_set(
            "replay:// needs a recording's folder or its frames.csv");
        return ECUIO_ERR_BAD_URI;
    }

    /* Every allocation from here is inside the try: a bad_alloc becomes
     * OUT_OF_MEMORY, never an exception out of ecuio_session_create */
    file_plan plan;
    uint32_t torn = 0;
    try {
        std::string path(body, body_len);
        if (!is_absolute(path)) {
            ecuio_detail_set("the recording's path must be absolute, like "
#ifdef _WIN32
                             "C:\\path\\to\\recording"
#else
                             "/path/to/recording"
#endif
            );
            return ECUIO_ERR_BAD_URI;
        }
        to_file(path);

        std::unique_ptr<ecuio_replay_transport> impl(
            new ecuio_replay_transport());
        if (!impl->path.set(path)) {
            ecuio_detail_set("couldn't open the recording's frames.csv");
            return ECUIO_ERR_LOAD_FAILED;
        }
        impl->line.resize(kLineMax + 2);
        impl->frame.reset(new ecuio_frame_t());
        const ecuio_status_t checked = check_file(
            impl->path, impl->line, impl->frame.get(), stretch, &plan, &torn);
        if (checked != ECUIO_OK) {
            return checked;
        }
        impl->skip = plan.skip;
        impl->rows = plan.rows;
        /* A pass with no span would never wait: the worker would spin,
         * sending the same instant again and again */
        if (loop && plan.span_us == 0) {
            ecuio_detail_set(stretch.given()
                                 ? "?loop=1 needs a stretch that spans time; "
                                   "this one's frames all share one time"
                                 : "?loop=1 needs a recording that spans "
                                   "time; this one's frames all share one "
                                   "time");
            return ECUIO_ERR_BAD_URI;
        }
        session->source_frame_flags = source_frame_flags(path);
        impl->session = session;
        impl->speed_milli = speed;
        impl->loop = loop;
        impl->faults = faults;
        session->transport =
            new ecuio_transport_t{&kReplayTransportOps, nullptr};
        session->transport->impl = impl.release();
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    session->info.backend = ECUIO_BACKEND_REPLAY;
    session->info.replay_frames = plan.frames > UINT32_MAX
                                      ? UINT32_MAX
                                      : static_cast<uint32_t>(plan.frames);
    const uint64_t span_ms = plan.span_us / 1000;
    session->info.replay_span_ms =
        span_ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(span_ms);
    session->info.replay_speed_milli = speed;
    session->info.replay_rows_torn = torn;
    if (loop) {
        session->info.flags |= ECUIO_SESSION_INFO_REPLAY_LOOP;
    }
    if (faults.any) {
        session->info.flags |= ECUIO_SESSION_INFO_REPLAY_FAULTS;
    }
    return ECUIO_OK;
}

} // extern "C"
