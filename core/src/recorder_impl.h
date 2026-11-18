// File/layer: core/src/recorder_impl.h | internal, C++17, Qt-free; the
//             recorder's class and the records its files share (ADR-0049
//             sections 1 to 4, armed too: ADR-0063)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)
//
// struct ecuio_recorder and what its files share: the ring's records, the
// caller's items and the constants its fields are sized by. Its member
// functions live in recorder.cpp (the worker's puts),
// recorder_attach.cpp, recorder_control.cpp (the ABI), recorder_writer.cpp
// (the writer thread) and recorder_armed.cpp (an armed recorder's windows);
// recorder_text.h has the text helpers. Only those
// files include it.
#ifndef ECUIO_RECORDER_IMPL_H
#define ECUIO_RECORDER_IMPL_H

#include "recorder.h"
#include "recording_lines.h" /* csv_quoted */

#include "ecuio/signals.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ecuio_rec {

constexpr uint32_t kDefaultNoteLines = 256;

/* Condition events a full ring leaves waiting; changes past them are
 * counted lost */
constexpr uint32_t kCarryEvents = 256;

constexpr uint64_t kTimingEveryUs = 10000000;

/* A record in the ring: this head, then its body, which starts with its
 * host_us. A frame's is frame_body, the used bytes, then each value as
 * value_head and its double or text, then each condition event as
 * event_body; a link transition's link_body; reply timing's timing_body
 * and its rows; carried events' conditions_body and its events. */
struct record_head {
    uint32_t size; /* the whole record, head included */
    uint32_t type;
    uint64_t dropped_before; /* frames the ring had no room for */
};
constexpr uint32_t kRecordFrame = 1;
constexpr uint32_t kRecordLink = 2;
constexpr uint32_t kRecordTiming = 3;
constexpr uint32_t kRecordConditions = 4;

struct link_body {
    uint64_t host_us;
    int32_t state;
    uint32_t episodes;
    uint64_t t_us; /* its session time; 0 for none known */
};

struct timing_body {
    uint64_t host_us;
    ecuio_session_timing_t timing;
    uint32_t rows;
    uint32_t pad;
};

struct frame_body {
    uint64_t host_us;
    uint64_t timestamp_us;
    uint32_t id;
    uint32_t flags;
    uint16_t length;
    uint16_t values;
    uint32_t events;
};

/* One condition event, on the recording's clock and the session's */
struct event_body {
    uint64_t host_us;
    uint64_t t_us;
    double value;
    uint32_t condition;
    uint32_t term;
    uint8_t state;
    uint8_t flags; /* ECUIO_CONDITION_STALE, _FIRED, _HELD */
    uint16_t pad;
    uint32_t pad2;
};
static_assert(sizeof(event_body) == 40, "a ring event is 40 bytes");

/* Carried events; lost counts one run of events past the carry, the
 * first of them at lost_us */
struct conditions_body {
    uint64_t host_us;
    uint32_t events;
    uint32_t pad;
    uint64_t lost;
    uint64_t lost_us;
};

struct value_head {
    uint16_t signal_id;
    uint8_t kind;
    uint8_t pad;
    uint32_t text_len; /* text's bytes follow; else a double */
};

struct def_entry {
    uint16_t id;
    bool text;
    std::string key; /* what signals.csv's signal cell holds */
    std::string unit;
};

/* A focus as a recording names it: the picks in order, its kind word,
 * and the minimum time between sweeps of a session that asks */
struct focus_named {
    std::vector<uint16_t> ids;
    bool pick = false;
    uint32_t min_sweep_ms = 0;

    bool operator==(const focus_named& o) const
    {
        return ids == o.ids && pick == o.pick && min_sweep_ms == o.min_sweep_ms;
    }
};

/* The session's focus; the caller holds focus_mutex (recorder_control.cpp) */
focus_named focus_now(const ecuio_session_t* session);

/* What the caller's threads hand the writer; asleep is an armed writer's
 * own, held with the notes until it knows the window it falls in */
enum class item_kind { begin, note, mark, focus, asleep };
struct item {
    item_kind kind;
    uint64_t host_us;
    std::string text;  /* focus's: its kind */
    std::string bytes; /* begin's: signals.json, empty for none; focus's:
                          its lines, a NUL between two */
    focus_named focus; /* focus's: what it names, for a window's header */
};

/* The words a window's end line gives when its conditions cleared */
constexpr const char* kWindowEnded = "the window ended";

inline bool is_text_kind(uint8_t kind)
{
    return kind == ECUIO_VALUE_TEXT || kind == ECUIO_VALUE_HEX;
}

/* recording_lines.h's, which the CSV import writes with too */
using ecuio_recording_lines::csv_quoted;

} // namespace ecuio_rec

/* The recorder and its members read these by their plain names */
using namespace ecuio_rec;

/* One per recording, its members grouped by who uses them: the padding
 * the analyzer would save is some 40 bytes */
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct ecuio_recorder {
    std::string folder; /* UTF-8, absolute, '/' separated */
    FILE* frames = nullptr;
    FILE* signals = nullptr;
    FILE* events = nullptr;
    FILE* markers = nullptr;
    FILE* latency = nullptr;

    /* The ring: the worker advances head, the writer tail; both only
     * grow, so head - tail is what's queued */
    std::unique_ptr<uint8_t[]> ring;
    uint32_t cap = 0;
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> records_dropped{0};

    /* The worker's alone: frames dropped since the last record, a link
     * transition still to go in, the next timing snapshot's session time */
    uint64_t pending_dropped = 0;
    bool link_pending = false;
    link_body pending_link{};
    uint64_t timing_next_us = kTimingEveryUs;

    /* The recording's set of conditions (NULL: none) and its attach, fixed
     * at attach; held states and the carry are the worker's, then close's */
    std::shared_ptr<const ecuio_condition_record> conditions;
    uint32_t conditions_generation = 0;
    bool held_taken = false;
    uint32_t held_count = 0;
    event_body held[ECUIO_CONDITION_MAX_CONDITIONS] = {};
    event_body carry[kCarryEvents] = {};
    uint32_t carry_start = 0;
    uint32_t carry_count = 0;
    uint64_t carry_lost = 0;
    uint64_t carry_lost_us = 0;
    std::atomic<uint64_t> conditions_lost{0};
    /* The worker's: the host_us of the last frame or link line put into
     * the ring, no event put after it placed before it */
    uint64_t put_host_us = 0;

    /* The signal file decoding the session to come, set before attach
     * (ecuio_recorder_signal_file): the set it must be, its bytes and
     * their SHA-256 */
    const ecuio_signal_set_t* signals_set = nullptr;
    std::string signals_json;
    std::string signals_sha;
    std::string signals_plugin; /* "<id>,<version>,<variant>" or "" */
    /* The file's keys in its set's order (it names a computed signal),
     * empty with no file: the set's own keys then */
    std::vector<ecuio_signal_key_t> signal_keys;

    /* Fixed at attach; zero_us is this recording's, which the link's
     * stops being once a later recorder attaches */
    std::shared_ptr<ecuio_record_link> link;
    uint64_t zero_us = 0;
    std::vector<def_entry> defs; /* sorted by id */
    std::vector<uint32_t> rx_ids; /* each module's reply ID */
    uint32_t channels = 0;
    std::chrono::system_clock::time_point start_wall{};
    std::atomic<bool> attached{false};
    std::atomic<bool> detached{false};
    /* Detached, and the writer has taken every record put (DRAINED) */
    std::atomic<bool> drained{false};

    std::mutex note_m;
    std::deque<item> items;
    uint32_t note_cap = kDefaultNoteLines;
    std::atomic<uint64_t> notes_dropped{0};

    /* The focus the header or the last focus line named: set at attach,
     * then read and written under the session's focus_mutex */
    focus_named named_focus;

    std::thread writer;
    std::mutex wake_m;
    std::condition_variable wake_cv;
    bool stop = false; /* under wake_m */

    std::atomic<uint32_t> state{ECUIO_RECORDING_ACTIVE};
    mutable std::mutex reason_m;
    std::string reason;
    std::atomic<uint64_t> frame_rows{0};
    std::atomic<uint64_t> value_rows{0};

    /* The test seams (internal.h) */
    std::atomic<bool> hold{false};
    std::atomic<bool> fail_next{false};
    std::atomic<uint64_t> syncs{0};
    std::atomic<uint64_t> wakes{0};
    std::atomic<uint64_t> slept_added{0};

    /* The machine's slept count at attach, written before attached; the
     * writer's own after that */
    uint64_t slept_at_attach = 0;
    bool sleep_armed = false;
    uint64_t slept_seen = 0;

    /* The writer's own */
    std::vector<uint8_t> scratch;
    std::string frame_out, value_out, event_out, marker_out, latency_out;
    std::string cond_text;
    /* Each request's answers, refusals and timeouts at its last row, so
     * latency.csv gets only the rows that changed */
    uint64_t timing_totals[ECUIO_PROFILE_MAX_REQUESTS] = {};
    /* The files' times count from base_us (a window's first record; 0 for
     * a recording of every frame), the wall clock's from wall_base */
    uint64_t base_us = 0;
    std::chrono::system_clock::time_point wall_base{};

    /* get_folder's copy, under folder_m: an armed writer moves it */
    mutable std::mutex folder_m;
    std::string published;

    /* An armed recorder (ADR-0063 section 7). armed, root and label_tail
     * are set at open; the pieces of a window's header, the copies and
     * the states at attach, before attached is stored; the rest is the
     * writer's. */
    bool armed = false;
    std::string root;
    std::string label_tail;    /* " <label>" or "" */
    std::string armed_core;    /* core to frame flags, conditions among them */
    std::string armed_signals; /* the signal and label lines */
    std::string armed_caller;  /* the caller's lines */
    std::string window_json;   /* signals.json, "" for none */
    uint64_t longest_before = 0;
    /* The second the last window's name took, and the next n in it */
    std::time_t name_second = 0;
    int name_next = 1;
    int32_t win_link_state = ECUIO_OK; /* at the tail */
    uint32_t win_link_episodes = 0;
    focus_named win_focus; /* at the tail */

    /* A window as the scan has seen it, in session time: from start_us to
     * end_us once no record condition is ON (open_end false) */
    struct window {
        uint64_t start_us = 0;
        uint64_t end_us = 0;
        bool open_end = true;
        bool opened = false;
        uint32_t trigger = 0;
    };
    std::deque<window> windows_q;
    /* The scan reads every record ahead of the tail for its events: scan
     * the ring position read to, newest_frame_us the newest session time
     * of a frame or a link change it read. A record's place is that newest
     * time where it stands (tail_pos the tail's), as the evaluator judges
     * a frame stamped earlier than the one before at that one's time; a
     * link change's events, which follow its record, take its place. */
    uint64_t scan = 0;
    uint64_t newest_frame_us = 0;
    bool scanned_frame = false;
    uint64_t tail_pos = 0;
    /* The open window's last record written, on the recording's clock */
    uint64_t win_last_host = 0;
    std::vector<uint8_t> scan_on; /* a record condition ON and fresh */
    uint32_t scan_active = 0;
    /* Each condition as the tail has passed it, for a window's held lines */
    struct tail_state {
        bool on = false;
        bool stale = false;
        double value = 0;
        uint32_t term = 0;
    };
    std::vector<tail_state> tail_cond;
    /* The newest record a full ring let go before its time */
    uint64_t early_upto_us = 0;
    bool early_any = false;
    /* Notes, marks, focus and asleep lines, held until the tail reaches
     * their time */
    std::deque<item> pending;
    bool in_window = false;
    std::atomic<bool> window_open{false};
    uint64_t win_frames0 = 0, win_values0 = 0, win_notes0 = 0;
    uint64_t win_dropped = 0;
    std::atomic<uint32_t> windows_opened{0};
    std::atomic<uint32_t> prerolls_short{0};
    std::atomic<uint64_t> frames_passed{0};

    uint64_t rel(uint64_t host_us) const;

    void fail_text(const std::string& why);

    bool failed() const;

    void fail(int err);

    void write(FILE* f, std::string& out);

    void copy_out(uint64_t pos, void* dst, size_t n) const;

    void copy_in(uint64_t pos, const void* src, size_t n);

    const def_entry* def(uint16_t id) const;

    void event_line(uint64_t host_us, const char* kind,
                    const std::string& text);

    void drop_line(uint64_t host_us, uint64_t dropped);

    void add_marker(uint64_t host_us, const std::string& label);

    void take_event(const event_body& e);

    void lost_line(uint64_t host_us, uint64_t lost);

    void take_conditions(const uint8_t* body, size_t size);

    void take_link(const uint8_t* body, size_t size);

    void take_timing(const uint8_t* body, size_t size);

    void take_frame(const uint8_t* body, size_t size);

    bool drain_ring();

    bool drain_items();

    void begin_rows();

    void write_item(const item& it);

    void write_outs();

    /* ---- The armed writer (ADR-0063 section 7) ----
     * The scan reads each record ahead of the tail and keeps the windows
     * its events make; the tail then writes a record inside a window, and
     * lets one go only once no event yet unread can reach back to it:
     * older than the longest before behind the newest frame read, or the
     * ring three quarters full. Windows and release are in session time,
     * as every condition rule is, so a replay at any speed gives the same
     * windows; release is by time, never by count, since a frame's events
     * can lie back in time (a deadline settled at a gap's end), never
     * before the frame judged before it. The files keep the recording's
     * clock, and notes and marks go by it. */

    void take_items();

    template <typename F>
    static void each_event(uint32_t type, const uint8_t* body, size_t size,
                           F f);

    void scan_event(const event_body& e, uint64_t record_us);

    void follow(const record_head& rh, const uint8_t* body, size_t size);

    void write_record(const record_head& rh, const uint8_t* body, size_t size);

    uint64_t first_frame_t_us(uint64_t at) const;

    std::string window_header(const window& w);

    void open_window(window& w, uint64_t at, uint64_t host_us);

    void close_window(uint64_t end_us, const std::string& why);

    void flush_items(uint64_t limit);
    void take_focus_items(uint64_t limit);

    static uint64_t place_of(const record_head& rh, const uint8_t* body,
                             size_t size, uint64_t pos);

    bool pressed() const;

    bool advance_tail(bool final);

    bool armed_pass(bool final);

    void flush_all();

    void sync_all();

    void check_sleep();

    uint64_t host_us_of(uint64_t clock_us) const;

    void run();

    void close_files();

    ~ecuio_recorder();
};

#endif /* ECUIO_RECORDER_IMPL_H */
