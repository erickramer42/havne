// File/layer: core/src/request_engine_rotation.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// The rotation (ADR-0015) and its rate classes (ADR-0039 section 3):
// the next request, a drop, the duplicates, reply timing's hook.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, RequestTimingTest (test_obd2);
//          HARNESS: J2534AdoptionTest (test_j2534_stream)

#ifndef ECUIO_REQUEST_ENGINE_ROTATION_H
#define ECUIO_REQUEST_ENGINE_ROTATION_H

#include "request_engine_state.h"

/* A rotation request's answer or refusal, for reply timing */
inline void ecuio_engine_time_reply(ecuio_request_engine* e, uint32_t c,
                                    uint32_t row, bool positive, bool packed,
                                    int64_t now_us)
{
    if (e->timing == nullptr || row >= e->count ||
        e->timing_of[row] >= ECUIO_TIMING_SPENT) {
        return;
    }
    timing_row& r = e->timing->rows[e->timing_of[row]];
    const int64_t latency = now_us - e->chans[c].sent_us;
    if (positive) {
        timing_answered(r, latency, e->stamp_set ? e->stamp_us : now_us,
                        e->stamp_breaks, packed);
    } else {
        timing_refused(r, latency, packed);
    }
}

/* ADR-0039 section 3: puts every Service 01 request whose PID fast() turns
 * down in the slow class. Other services, and a table never classified,
 * stay fast. */
inline void ecuio_engine_classify(ecuio_request_engine* e,
                                  bool (*fast)(uint8_t pid))
{
    for (uint32_t i = 0; i < e->count; ++i) {
        ecuio_request& r = e->requests[i];
        r.slow_default =
            (r.len == 2 && r.bytes[0] == 0x01 && !fast(r.bytes[1])) ? 1 : 0;
        r.slow = r.slow_default;
    }
}

/* ADR-0039 section 3: pairs of fast PIDs that can read one sensor, count pairs
 * of (keep, demote) bytes, which must outlive the engine. Each walk's end
 * judges them on its channel (ecuio_engine_judge_duplicates). */
inline void ecuio_engine_set_duplicates(ecuio_request_engine* e,
                                        const uint8_t* pairs, uint32_t count)
{
    e->duplicates = pairs;
    e->duplicate_count = count;
}

/* Channel c's next request of one class, from that class's cursor, or
 * ECUIO_ENGINE_NONE when the class has none in the rotation. */
inline int32_t ecuio_engine_next_of(ecuio_request_engine* e, uint32_t c,
                                    bool slow)
{
    ecuio_engine_channel& ch = e->chans[c];
    uint32_t& cursor = slow ? ch.slow_cursor : ch.cursor;
    for (uint32_t k = 0; k < e->count; ++k) {
        const uint32_t i = (cursor + k) % e->count;
        const ecuio_request& r = e->requests[i];
        if (r.channel == c && ecuio_engine_in_rotation(r) &&
            (r.slow != 0) == slow) {
            cursor = (i + 1) % e->count;
            return static_cast<int32_t>(i);
        }
    }
    return ECUIO_ENGINE_NONE;
}

/* The request r names on channel c: a table index, the one-shot or the
 * pack. */
inline const ecuio_request& ecuio_engine_request_of(
    const ecuio_request_engine* e, uint32_t c, int32_t r)
{
    return (r == ECUIO_ENGINE_ONCE)   ? e->chans[c].once
           : (r == ECUIO_ENGINE_PACK) ? e->chans[c].pack
                                      : e->requests[r];
}

/* Takes table entry i out of the rotation, for reason (ECUIO_DROP_*). */
inline void ecuio_engine_drop(ecuio_request_engine* e, uint32_t i,
                              uint8_t reason, uint8_t nrc)
{
    ecuio_request& r = e->requests[i];
    if (r.dropped != 0) {
        return;
    }
    r.dropped = reason;
    r.nrc = nrc;
    ecuio_engine_channel& ch = e->chans[r.channel];
    if (r.parked == 0) {
        --ch.requests; /* a parked one was counted out already */
    }
    if (r.swept != 0) {
        ++ch.sweep_dropped; /* asked in this sweep: the sweep still ends
                               once every row left has been asked */
    }
    ++ch.generation;
}

inline bool ecuio_engine_pid_bit(const uint8_t* bits, uint8_t pid)
{
    return (bits[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) != 0;
}

/* ADR-0039 section 3: for each duplicate pair, the demoted PID's request on
 * channel c is slow when the channel's answered bitmaps list both PIDs,
 * and fast otherwise (the pairs are of fast PIDs), so a car that answers
 * one form keeps it fast and a later walk judges again. */
inline void ecuio_engine_judge_duplicates(ecuio_request_engine* e, uint32_t c)
{
    const ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t p = 0; p < e->duplicate_count; ++p) {
        const uint8_t keep = e->duplicates[2 * p];
        const uint8_t demote = e->duplicates[2 * p + 1];
        const bool both = ecuio_engine_pid_bit(ch.supported, keep) &&
                          ecuio_engine_pid_bit(ch.supported, demote);
        for (uint32_t i = 0; i < e->count; ++i) {
            ecuio_request& r = e->requests[i];
            if (r.channel == c && r.len == 2 && r.bytes[0] == 0x01 &&
                r.bytes[1] == demote) {
                r.slow_default = both ? 1 : 0;
                if (!e->focused) {
                    r.slow = r.slow_default;
                }
            }
        }
    }
}

#endif
