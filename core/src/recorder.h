// File/layer: core/src/recorder.h | internal, C++17, Qt-free; what the
//             session and its recorder share (ADR-0049 section 1)
// Testing: HARNESS: RecorderTest (test_core), CoreTest's no-allocation
//          case with a recorder attached; UNIT: ConditionRecordingTest,
//          ArmedRecorderTest (test_core)
//
// A session and its recorder each outlive the other in some teardowns,
// so neither reaches into the other's memory: both hold a link, and the
// worker reaches the recorder only through it. The session makes its
// link at create and keeps it; a recorder attaches before or after
// start (ADR-0056 section 3), and another once it has gone. `active`
// is the recorder while it takes frames; every use of it is pinned
// (`pins` raised before the load and lowered after the use), and
// detaching clears it and waits for `pins` to reach zero, so the
// recorder is never freed under a worker still writing to it. `m`
// orders attaches and the two sides' detaches, and a set of conditions'
// attach, which holds it through its check and handover so a recording's
// set never changes under it.
#ifndef ECUIO_RECORDER_H
#define ECUIO_RECORDER_H

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ecuio_condition_eval;

struct ecuio_record_link {
    std::atomic<ecuio_recorder_t*> active{nullptr};
    std::atomic<uint32_t> pins{0};
    /* ecuio_clock_us at attach: host_us counts from it. Set at each
     * attach before active is stored (release), read after active is
     * loaded (acquire), so a worker that finds a recorder finds its
     * zero. */
    std::atomic<uint64_t> zero_us{0};
    std::mutex m;
};

/* What a recording keeps of a set of conditions (ADR-0063 section 8):
 * made by ecuio_session_set_conditions outside every lock, once per
 * attach, and never changed after, so a recorder takes it by pointer
 * under the session's locks */
struct ecuio_condition_record {
    struct entry {
        std::string key;
        std::string name;
        bool mark = false;
        /* An armed recorder's windows (ADR-0063 section 7): record is an
         * enabled condition with record, before_us and after_us its own */
        bool record = false;
        uint32_t before_us = 0;
        uint32_t after_us = 0;
    };
    std::string bytes;             /* conditions.json */
    std::string sha256;            /* the bytes', lowercase hex */
    std::vector<entry> conditions; /* the set's order */
};

/* The recordings' clock: ecuio_clock_us, or the test clock
 * (ecuio_recorder_test_set_clock, internal.h) */
uint64_t ecuio_record_clock_us();

/* The recorder taking frames, pinned; NULL (and unpinned) when none */
inline ecuio_recorder_t* ecuio_record_pin(ecuio_record_link& link)
{
    link.pins.fetch_add(1, std::memory_order_seq_cst);
    ecuio_recorder_t* r = link.active.load(std::memory_order_seq_cst);
    if (r == nullptr) {
        link.pins.fetch_sub(1, std::memory_order_release);
    }
    return r;
}

inline void ecuio_record_unpin(ecuio_record_link& link)
{
    link.pins.fetch_sub(1, std::memory_order_release);
}

/* Either side: the recorder stops taking frames, once no worker holds
 * it. Safe from any thread, any number of times. With only, nothing
 * happens unless only is the recorder attached (or none is): a
 * recorder's own close never detaches the one that attached after it. */
void ecuio_record_detach(ecuio_record_link& link,
                         ecuio_recorder_t* only = nullptr);

/* On the worker, pinned, before the frame is judged by conditions: at
 * the recorder's first frame judged by its own set, each condition then
 * ON is kept to go into that frame's record as a held event. Never
 * allocates. */
void ecuio_recorder_hold_states(ecuio_recorder_t* r,
                                const ecuio_condition_eval& conditions);

/* On the worker, pinned: one frame, the values it decoded to and the
 * condition events it gave (generation's), as one record or none. Events
 * of another set than the recording's are left out; a frame that doesn't
 * fit is dropped and counted, its events carried to go in ahead of the
 * next record. access_cut flags it ECUIO_FRAME_FLAG_ACCESS_CUT, which the
 * writer cuts. Never blocks, never allocates. */
void ecuio_recorder_put_frame(ecuio_recorder_t* r, uint64_t host_us,
                              const ecuio_frame_t* frame,
                              const ecuio_value_t* values, uint32_t count,
                              const ecuio_condition_event_t* events = nullptr,
                              uint32_t event_count = 0,
                              uint32_t generation = 0,
                              bool access_cut = false);

/* On the worker, pinned: the condition events a change of the link's
 * state gave at time_us (generation's), after its link record, as one
 * record of events, the held states first (a cond line writes no word
 * for ECUIO_CONDITION_LINK). A full ring carries them, as a frame's
 * events are. Never blocks, never allocates. */
void ecuio_recorder_put_events(ecuio_recorder_t* r, uint64_t host_us,
                               uint64_t time_us,
                               const ecuio_condition_event_t* events,
                               uint32_t event_count, uint32_t generation);

/* On the worker that judges the link, at each transition: its state and
 * episode count at their time, pinned here, and its session time (0 for
 * none known), which places it in an armed recording; link may be NULL.
 * It goes in after the condition events carried before it; one that
 * doesn't fit is carried, the latest state, ahead of the next record that
 * fits, so a full ring loses frames but never the link's state. Never
 * blocks, never allocates. */
void ecuio_record_link_state(ecuio_record_link* link, int32_t state,
                             uint32_t episodes, uint64_t t_us = 0);

/* On a requesting worker, when a module's state changed (ADR-0065 step
 * 5): the modules closed and those with no room, a bit each, and each
 * module's reasons (count of them), pinned here; link may be NULL. One
 * that doesn't fit is carried, the latest states, as a link transition
 * is. Never blocks, never allocates. */
void ecuio_record_modules(ecuio_record_link* link, uint32_t closed,
                          uint32_t no_room, const uint8_t* reasons,
                          uint32_t count);

/* Never names the copy: its name is the contents' checksum, and recordings get shared */
void ecuio_record_operation(ecuio_record_link* link, const char* id,
                            uint32_t rx, uint64_t bytes, uint32_t end);

/* On a requesting worker, as it publishes reply timing: the rows every
 * 10 s of session time and when final, pinned here; link may be NULL.
 * The final one, the worker's last record, waits up to 1 s for room and
 * is dropped and counted only if none comes. Never allocates. */
void ecuio_record_timing(ecuio_record_link* link,
                         const ecuio_session_timing_t& timing,
                         const ecuio_request_timing_t* rows, uint32_t count,
                         bool final);

/* Before attach, on the caller's thread: set is what will decode the
 * session, and bytes (length of them) the signal file it came from,
 * which attach checks against the session's set and the writer copies
 * into the folder as signals.json, its SHA-256 on events.log's header
 * (ADR-0051 section 4); plugin, when not "", a
 * "<id>,<version>,<variant>" to a line, each a plugin line of the
 * header: a variant's one (ADR-0052 section 4), a merged set's one for
 * each part that is a plugin's variant, in merge order (ADR-0065 section
 * 9). ecuio_recorder_signal_file passes a file's; length 0 is a
 * set with no file's bytes, checked and not copied.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL recorder or set, or NULL bytes with
 * a length; ECUIO_ERR_INVALID_ARGUMENT once attached, on a second call,
 * or past ECUIO_SIGNAL_SET_MAX_BYTES; ECUIO_ERR_OUT_OF_MEMORY. */
ecuio_status_t ecuio_recorder_take_signals(ecuio_recorder_t* recorder,
                                           const ecuio_signal_set_t* set,
                                           const char* bytes, size_t length,
                                           const char* plugin = "");

/* On the caller's thread, after a focus has been stored, and at an
 * attach: the recorder taking the session's frames, if one is, writes
 * the focus as activity lines of kind pick or focus, as the header names
 * it ("N signals: ...", ", at least M ms between sweeps" after the count
 * when a session that asks has a minimum, "more: ..." past a line's
 * room, "none: every signal asked" when it ended), only when the picks,
 * the kind or the minimum differ from what the header or its last line
 * named. Takes the link's mutex to pin the recorder, then holds the pin
 * across focus_mutex and then the recorder's note_m (one at a time,
 * never nested); a detach waits for the pins, so neither of those may
 * be held around one. */
void ecuio_record_focus(ecuio_session_t* session);

#endif /* ECUIO_RECORDER_H */
