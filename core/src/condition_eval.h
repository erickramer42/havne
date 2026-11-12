// File/layer: core/src/condition_eval.h | internal, C++17, Qt-free; a
//             set of conditions judged on a frame's values (ADR-0063
//             sections 2 and 3)
// Testing: UNIT: ConditionEvalTest (test_signals), ConditionSessionTest
//          (test_core); FUZZ: fuzz_condition_file
//
// The evaluator: a condition set bound to a signal set once (make, on the
// caller's thread), then judged frame by frame on the frames' own time
// (judge, on the worker, right after the frame's decode and its computed
// stage). Every rule runs on the times it is given and nothing else, so
// the same frames give the same events at the same times, live, replayed
// or read back. judge allocates nothing. Its clock never goes back: a
// frame stamped earlier than the one before it (two channels' frames can
// arrive a little out of order) is judged at that one's time, so a sample
// only ever moves a deadline later and events never go back in time.
//
// A term keeps its truth (hysteresis: `above` with `clear` turns true
// above the level and false at or below clear) and reads its signal's
// newest sample. A sample older than the term's age bound can't be
// judged. A condition joins its terms three ways (true, false, not
// judged), turns ON once they have held `for` and OFF once they have
// failed `clear_for`, and while it can't be judged keeps its state and is
// STALE.
//
// A hold's end and a sample's age bound are deadlines. judge settles each
// deadline earlier than the frame's time before the frame's values, and
// stamps the change with the deadline's time. A hold ending exactly at
// the frame's time is settled after the frame's values, if they didn't
// break it; a sample exactly at its bound is still fresh.
//
// The cost of a sample: each watched signal keeps the levels and clears
// of its terms sorted, and a term's truth can change only when a sample
// lands on or crosses one of them, so a new sample visits the points
// between the sample before it and itself and no other term.

#ifndef ECUIO_CONDITION_EVAL_H
#define ECUIO_CONDITION_EVAL_H

#include "ecuio/conditions.h"
#include "ecuio/profile.h"

#include <cstdint>
#include <memory>
#include <vector>

struct ecuio_condition_eval {
    static constexpr uint32_t kNone = UINT32_MAX;
    /* The most events one frame gives one condition: a hold's end, an
     * age bound, and the frame's own judgment. Changes at one time are
     * one event. */
    static constexpr uint32_t kEventsPerCondition = 3;
    /* Twice the most watched signals (ECUIO_CONDITION_MAX_CONDITIONS x
     * ECUIO_CONDITION_MAX_TERMS), so a probe is short */
    static constexpr uint32_t kHashSlots = 512;
    static constexpr uint16_t kEmpty = UINT16_MAX;

    struct term {
        double level = 0.0;
        double clear = 0.0;
        uint64_t max_age_us = 0;
        uint32_t condition = 0;
        uint16_t signal = 0;    /* its signal, in `signals` */
        uint16_t next = kEmpty; /* the next term reading that signal */
        uint8_t test = 0;       /* ECUIO_TERM_ABOVE and kin */
        uint8_t has_clear = 0;
        uint8_t truth = 0;  /* kept across a stale stretch */
        uint8_t judged = 0; /* its signal's sample is within its bound */
    };
    /* A signal some term reads: its newest sample, its terms, and their
     * levels and clears in order */
    struct signal {
        double value = 0.0;
        uint64_t time_us = 0; /* the frame time the sample came at */
        uint32_t first_point = 0;
        uint32_t point_count = 0;
        uint16_t id = 0;
        uint16_t first_term = kEmpty;
        uint16_t unjudged = 0; /* its terms with judged clear */
        uint8_t sampled = 0;
    };
    struct point {
        double at = 0.0;
        uint16_t term = 0;
    };
    struct condition {
        uint64_t hold_end_us = 0;
        /* No later than its earliest deadline; UINT64_MAX with none. A
         * new sample only moves a deadline later, so it isn't updated
         * for one. */
        uint64_t check_us = UINT64_MAX;
        uint64_t since_us = 0;
        uint64_t fired = 0;
        uint64_t event_frame = 0; /* the judge call last_event is of */
        double value = 0.0;
        uint32_t first_term = 0;
        uint32_t term_count = 0;
        uint32_t for_us = 0;
        uint32_t clear_for_us = 0;
        uint32_t hold_term = 0; /* the term that started the hold */
        uint32_t changed_term = 0;
        uint32_t why = ECUIO_CONDITION_WHY_NONE;
        uint32_t last_event = 0;
        uint8_t any = 0;
        uint8_t state = ECUIO_CONDITION_OFF;
        uint8_t stale = 1;
        uint8_t hold = 0; /* 0 none, 1 toward ON, 2 toward OFF */
        uint8_t available = 0;
        uint8_t dirty = 0;
    };

    std::vector<term> terms;
    std::vector<signal> signals;
    std::vector<point> points;
    std::vector<condition> conditions; /* one per condition of the set */
    /* A def id's signal, by open addressing; kEmpty for none */
    uint16_t hash[kHashSlots];
    /* One judge call's events, in time order then the set's */
    std::vector<ecuio_condition_event_t> events;
    uint32_t event_count = 0;
    std::vector<uint32_t> dirty;
    uint32_t dirty_count = 0;
    /* No later than any condition's check_us */
    uint64_t next_check_us = UINT64_MAX;
    uint64_t frame = 0;
    uint64_t newest_us = 0; /* the latest time judge was given */
    uint32_t generation = 0;
    /* Events that found the buffer full: the bound above broken. Never
     * counts in a correct build; fuzz_condition_file holds it to 0. */
    uint64_t dropped = 0;

    /* The set bound to signal_set (and file, for key: references), as
     * ecuio_condition_set_resolve binds it; a condition with a link or a
     * check term is unavailable. Every state starts OFF and stale.
     * Throws bad_alloc. */
    static std::unique_ptr<ecuio_condition_eval>
    make(const ecuio_condition_set_t* set, const ecuio_signal_set_t* signal_set,
         const ecuio_signal_file_t* file, uint32_t generation);

    /* Judges one frame: its time and the values it decoded. Returns the
     * events, in `events`, valid until the next call. Allocates
     * nothing. */
    uint32_t judge(uint64_t time_us, const ecuio_value_t* values,
                   uint32_t count);

    /* Condition index's state as it stands */
    ecuio_condition_state_t state_of(uint32_t index) const;

    /* The bytes the evaluator holds: its own and its vectors' */
    size_t bytes() const;

private:
    enum class joined : uint8_t { no, yes, unjudged };
    joined join(const condition& c) const;
    void feed(uint64_t now_us, const ecuio_value_t& v);
    void sample(uint16_t index, uint64_t now_us);
    void rejoin(uint32_t index, uint64_t at_us, uint32_t decider);
    void settle(uint64_t now_us, bool hold_at_now);
    void settle_one(uint32_t index, uint64_t now_us, bool hold_at_now);
    void changed(uint32_t index, uint64_t at_us, uint32_t decider, bool fired);
    void watch(uint32_t index, uint64_t deadline_us);
};

#endif /* ECUIO_CONDITION_EVAL_H */
