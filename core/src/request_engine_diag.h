// File/layer: core/src/request_engine_diag.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// Diagnostics (ADR-0037) and clears (ADR-0038).
// C++17, Qt-free, no platform headers.
// Testing: UNIT: DiagnosticsTest, ClearTest (test_obd2); HARNESS: each
//          transport's diagnostics and clear cases

#ifndef ECUIO_REQUEST_ENGINE_DIAG_H
#define ECUIO_REQUEST_ENGINE_DIAG_H

#include "request_engine_rotation.h"
#include "request_engine_services.h"

/* Lists one more diagnostics item on channel ch, unasked; false when
 * the list is full. */
inline bool ecuio_engine_diag_push(ecuio_engine_channel& ch, uint8_t b0,
                                   uint8_t b1, uint8_t b2, uint8_t len)
{
    if (ch.diag_count >= ECUIO_DIAG_MAX_ITEMS) {
        return false;
    }
    ecuio_diag_item& it = ch.diag_items[ch.diag_count++];
    it = ecuio_diag_item{};
    it.request[0] = b0;
    it.request[1] = b1;
    it.request[2] = b2;
    it.len = len;
    ++ch.diag_pending;
    ch.diag.flags &= ~ECUIO_DIAG_COMPLETE;
    return true;
}

/* The diag_set's first def that reads Service 01 PID pid, or nullptr */
inline const ecuio_signal_def_t*
ecuio_engine_diag_def(const ecuio_request_engine* e, uint8_t pid,
                      uint32_t from = 0)
{
    const ecuio_signal_set_t* set = e->diag_set;
    if (set == nullptr || set->defs == nullptr) {
        return nullptr;
    }
    for (uint32_t i = from; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            d.mux_key == (0x4100u | pid)) {
            return &d;
        }
    }
    return nullptr;
}

/* The freeze frame's values, once one is stored: 02 <pid> 00 for each
 * PID its bitmap lists that the diag_set decodes, up to the values the
 * report holds */
inline void ecuio_engine_diag_push_values(const ecuio_request_engine* e,
                                          ecuio_engine_channel& ch)
{
    uint32_t asked = 0;
    for (uint8_t pid = 0x03; pid < 0x20 && asked < ECUIO_DIAG_MAX_FREEZE;
         ++pid) {
        if (ecuio_engine_pid_bit(ch.freeze_bits, pid) &&
            ecuio_engine_diag_def(e, pid) != nullptr &&
            ecuio_engine_diag_push(ch, 0x02, pid, 0x00, 3)) {
            ++asked;
        }
    }
}

/* A freeze-frame value's reply, 42 <pid> 00 <data>, read as the Service
 * 01 reply 41 <pid> <data> it mirrors, through each diag_set def of the
 * PID (ADR-0037 section 4) */
inline void ecuio_engine_diag_take_value(const ecuio_request_engine* e,
                                         ecuio_channel_diagnostics_t& d,
                                         const uint8_t* data, uint16_t len)
{
    uint8_t payload[16] = {0x41, data[1]};
    uint16_t n = 2;
    for (uint16_t i = 3; i < len && n < sizeof(payload); ++i) {
        payload[n++] = data[i];
    }
    for (const ecuio_signal_def_t* def = ecuio_engine_diag_def(e, data[1]);
         def != nullptr && d.freeze_count < ECUIO_DIAG_MAX_FREEZE;
         def = ecuio_engine_diag_def(
             e, data[1], static_cast<uint32_t>(def - e->diag_set->defs) + 1)) {
        double v = 0;
        if (ecuio_decode_signal(def, payload, n, &v) == 0) {
            ecuio_freeze_value_t& f = d.freeze_values[d.freeze_count++];
            f = ecuio_freeze_value_t{};
            f.def_id = def->id;
            f.value = v;
        }
    }
}

/* Service 01's items settled into report d: readiness (01 01) or the
 * drive cycle's (01 41) */
inline void ecuio_engine_diag_take_status(ecuio_channel_diagnostics_t& d,
                                          const ecuio_diag_item& it,
                                          uint8_t outcome, uint8_t nrc,
                                          const uint8_t* data, uint16_t len)
{
    const bool answered = outcome == ECUIO_DIAG_ANSWERED;
    if (it.request[1] == 0x41) {
        d.cycle = outcome;
        if (answered && len >= 6) {
            ecuio_diag_take_cycle(data + 2, &d);
        }
    } else {
        d.readiness = outcome;
        d.readiness_nrc = nrc;
        if (answered && len >= 6) {
            ecuio_diag_take_readiness(data + 2, &d);
        }
    }
}

/* A code list's item (03, 07, 0A) settled into report d */
inline void ecuio_engine_diag_take_list(ecuio_channel_diagnostics_t& d,
                                        const ecuio_diag_item& it,
                                        uint8_t outcome, uint8_t nrc,
                                        const uint8_t* data, uint16_t len)
{
    const bool answered = outcome == ECUIO_DIAG_ANSWERED;
    ecuio_dtc_list_t& l =
        d.lists[it.request[0] == 0x03   ? ECUIO_DIAG_STORED
                : it.request[0] == 0x07 ? ECUIO_DIAG_PENDING
                                        : ECUIO_DIAG_PERMANENT];
    l.outcome = outcome;
    l.nrc = nrc;
    if (answered) {
        ecuio_diag_take_codes(data, len, &l);
    }
}

/* A freeze frame's item (02) settled into channel c's report: its PIDs,
 * its code or a value; the answers list what comes next */
inline void ecuio_engine_diag_take_freeze(ecuio_request_engine* e, uint32_t c,
                                          const ecuio_diag_item& it,
                                          uint8_t outcome, uint8_t nrc,
                                          const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_channel_diagnostics_t& d = ch.diag;
    const bool answered = outcome == ECUIO_DIAG_ANSWERED;
    if (it.request[1] == 0x00) {
        /* The freeze frame's PIDs: its code next, if listed, else
         * straight to its values */
        d.freeze = outcome;
        d.freeze_nrc = nrc;
        if (answered && len >= 7) {
            for (uint32_t b = 0; b < 4; ++b) {
                ch.freeze_bits[b] = data[3 + b];
            }
            if (ecuio_engine_pid_bit(ch.freeze_bits, 0x02)) {
                ecuio_engine_diag_push(ch, 0x02, 0x02, 0x00, 3);
            } else {
                ecuio_engine_diag_push_values(e, ch);
            }
        }
    } else if (it.request[1] == 0x02) {
        /* 00 00: no freeze frame is stored, so no values to ask */
        if (answered && len >= 5) {
            d.freeze_dtc = static_cast<uint16_t>((data[3] << 8) | data[4]);
            if (d.freeze_dtc != 0) {
                d.flags |= ECUIO_DIAG_FREEZE_FRAME;
                ecuio_engine_diag_push_values(e, ch);
            }
        }
    } else if (answered && len >= 3) {
        ecuio_engine_diag_take_value(e, d, data, len);
    }
}

/* Lists the first items on a channel holding none, and marks its
 * report asked */
inline void ecuio_engine_diag_list(ecuio_engine_channel& ch)
{
    ch.diag.flags |= ECUIO_DIAG_ASKED;
    for (const auto& q : ECUIO_DIAG_FIRST) {
        ecuio_engine_diag_push(ch, q[0], q[1], q[2], q[3]);
    }
}

/* Starts channel ch's read over: its report empty but for the last
 * clear's outcome, its generation moved on */
inline void ecuio_engine_diag_restart(ecuio_engine_channel& ch)
{
    const uint32_t generation = ch.diag.generation;
    const uint8_t clear = ch.diag.clear;
    const uint8_t clear_nrc = ch.diag.clear_nrc;
    const uint32_t clear_rpm = ch.diag.clear_rpm;
    const uint32_t clear_kmh = ch.diag.clear_kmh;
    const uint32_t read_flags =
        ch.diag.flags & (ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH);
    ch.diag = ecuio_channel_diagnostics_t{};
    ch.diag.generation = generation + 1;
    ch.diag.clear = clear;
    ch.diag.clear_nrc = clear_nrc;
    ch.diag.clear_rpm = clear_rpm;
    ch.diag.clear_kmh = clear_kmh;
    ch.diag.flags = read_flags;
    for (uint8_t& b : ch.freeze_bits) {
        b = 0;
    }
    ch.diag_count = 0;
    ch.diag_pending = 0;
    ecuio_engine_diag_list(ch);
}

/* Whether any target waits on the clear request's judgment */
inline bool ecuio_engine_judge_waits(const ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].clear_step == ECUIO_CLEAR_STEP_WAIT) {
            return true;
        }
    }
    return false;
}

/* The judgment with no target left waiting asks nothing more; a request
 * of it already out settles into nothing (ecuio_engine_judge_rpm) */
inline void ecuio_engine_judge_drop(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count && !ecuio_engine_judge_waits(e);
         ++c) {
        if (!e->chans[c].clearing) {
            e->chans[c].judge_step = ECUIO_CLEAR_STEP_NONE;
        }
    }
}

/* A quiet step's item on channel ch settled, the report changed or
 * not (same): a clear accepted meanwhile, nothing of it sent yet, ends
 * ECUIO_CLEAR_REPORT_CHANGED when the report changed (the user agreed to
 * the one shown); a Read again pressed meanwhile starts the read over */
inline void ecuio_engine_diag_step_settled(ecuio_request_engine* e,
                                           ecuio_engine_channel& ch, bool same)
{
    ch.diag_step = false;
    if (!same && (ch.clear_step == ECUIO_CLEAR_STEP_WAIT ||
                  ch.clear_step == ECUIO_CLEAR_STEP_CLEAR)) {
        ch.clear_step = ECUIO_CLEAR_STEP_NONE;
        ch.diag.flags &= ~ECUIO_DIAG_CLEARING;
        ch.diag.clear = ECUIO_CLEAR_REPORT_CHANGED;
        ecuio_engine_judge_drop(e);
    }
    if (ch.diag_again) {
        ch.diag_again = false;
        ecuio_engine_diag_restart(ch);
    }
}

/* Settles the item channel c has out: answered, with the reply (the
 * whole frame), refused with its code, or unanswered; the report takes
 * what it says, and the freeze frame's answers list what comes next */
inline void ecuio_engine_diag_settle(ecuio_request_engine* e, uint32_t c,
                                     uint8_t outcome, uint8_t nrc,
                                     const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_diag_item& it = ch.diag_items[ch.diag_index];
    ecuio_channel_diagnostics_t& d = ch.diag;
    /* a quiet module's step unanswered again changes no report */
    const bool same =
        it.outcome == ECUIO_DIAG_UNANSWERED && outcome == ECUIO_DIAG_UNANSWERED;
    const bool step = ch.diag_step;
    ch.diagnosing = false;
    it.outcome = outcome;
    it.nrc = nrc;
    if (ch.diag_pending > 0) {
        --ch.diag_pending;
    }
    switch (it.request[0]) {
    case 0x01:
        ecuio_engine_diag_take_status(d, it, outcome, nrc, data, len);
        break;
    case 0x03:
    case 0x07:
    case 0x0A:
        ecuio_engine_diag_take_list(d, it, outcome, nrc, data, len);
        break;
    case 0x02:
        ecuio_engine_diag_take_freeze(e, c, it, outcome, nrc, data, len);
        break;
    default:
        break;
    }
    if (ch.diag_pending == 0) {
        d.flags |= ECUIO_DIAG_COMPLETE;
    }
    d.generation += same ? 0u : 1u;
    if (step) {
        ecuio_engine_diag_step_settled(e, ch, same);
    }
}

/* Diagnostics (ADR-0037): gives every channel the list to ask, through
 * the one-shot slot after identification and ahead of the scan; set
 * decodes the freeze frame (the obd2 set). Idempotent: a channel
 * already holding the list keeps its answers. */
inline void ecuio_engine_diagnose(ecuio_request_engine* e,
                                  const ecuio_signal_set_t* set)
{
    e->diag_set = set;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].diag_count == 0) {
            ecuio_engine_diag_list(e->chans[c]);
        }
    }
}

/* Asks the whole list again on every channel whose last read has
 * settled ("Read again"): its report starts over, its generation moves
 * on. A channel mid-read, mid-clear, or never asked, is left as it is.
 * A quiet step's item out is no read: that channel starts over at the
 * item's settle, so its reply settles no slot of the new list. */
inline void ecuio_engine_diagnose_channel_again(ecuio_request_engine* e,
                                                uint32_t c)
{
    if (c >= e->chan_count) {
        return;
    }
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.diag_count == 0 || ch.diag_pending > 0 ||
        ecuio_engine_clear_on(ch)) {
        return;
    }
    if (ch.diagnosing) {
        ch.diag_again = ch.diag_again || ch.diag_step;
        return;
    }
    ecuio_engine_diag_restart(ch);
}

/* bit c: channel c */
inline void ecuio_engine_diagnose_channels_again(ecuio_request_engine* e,
                                                 uint32_t channels)
{
    for (uint32_t c = 0; c < e->chan_count && c < 32; ++c) {
        if (((channels >> c) & 1u) != 0) {
            ecuio_engine_diagnose_channel_again(e, c);
        }
    }
}

inline void ecuio_engine_diagnose_again(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_diagnose_channel_again(e, c);
    }
}

/* Whether module ch's report holds a positive answer to 01 00 or 03: an
 * emissions module, which alone a clear may clear (ADR-0038) */
inline bool ecuio_engine_diag_emissions(const ecuio_engine_channel& ch)
{
    for (uint32_t i = 0; i < ch.diag_count; ++i) {
        const ecuio_diag_item& it = ch.diag_items[i];
        const bool asked =
            it.request[0] == 0x03 ||
            (it.request[0] == 0x01 && it.len == 2 && it.request[1] == 0x00);
        if (asked && it.outcome == ECUIO_DIAG_ANSWERED) {
            return true;
        }
    }
    return false;
}

/* Whether a clear request runs: any module with a clear under way */
inline bool ecuio_engine_clear_running(const ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (ecuio_engine_clear_on(e->chans[c])) {
            return true;
        }
    }
    return false;
}

/* The engine module, whose rpm a clear judges by: rpm_module when set,
 * else the lowest channel a row of the table asks PID 0C of (a pack's
 * too); UINT32_MAX for none */
inline uint32_t ecuio_engine_rpm_module_of(const ecuio_request_engine* e)
{
    if (e->rpm_module != UINT32_MAX) {
        return e->rpm_module;
    }
    uint32_t found = UINT32_MAX;
    for (const ecuio_request& r : e->requests) {
        for (uint8_t k = 1; r.bytes[0] == 0x01 && k < r.len; ++k) {
            if (r.bytes[k] == 0x0C && r.channel < found) {
                found = r.channel;
            }
        }
    }
    return found;
}

/* Target c's gate (ADR-0038): its report the one the caller saved (read
 * to the end, at generation, nothing running), else
 * ECUIO_CLEAR_REPORT_CHANGED; an emissions module, else
 * ECUIO_CLEAR_NOT_EMISSIONS. Past it, the target waits on the request's
 * judgment. False for a channel past the count, one that reads no
 * diagnostics, one already clearing, or a gate not passed. A quiet
 * step's item out is no read, unless a Read again waits on it: the clear
 * goes once it settles, or ends REPORT_CHANGED if it changed the report
 * (ecuio_engine_diag_step_settled). */
inline bool ecuio_engine_clear_gate(ecuio_request_engine* e, uint32_t c,
                                    uint32_t generation)
{
    if (c >= e->chan_count) {
        return false;
    }
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_channel_diagnostics_t& d = ch.diag;
    if (ch.diag_count == 0 || ecuio_engine_clear_on(ch)) {
        return false;
    }
    d.clear_nrc = 0;
    d.clear_rpm = 0;
    d.clear_kmh = 0;
    d.flags &= ~(ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH);
    ++d.generation;
    const bool reading = ch.diagnosing && (!ch.diag_step || ch.diag_again);
    if ((d.flags & ECUIO_DIAG_COMPLETE) == 0 || reading ||
        ch.diag_pending > 0 || d.generation != generation + 1) {
        d.clear = ECUIO_CLEAR_REPORT_CHANGED;
        return false;
    }
    if (!ecuio_engine_diag_emissions(ch)) {
        d.clear = ECUIO_CLEAR_NOT_EMISSIONS;
        return false;
    }
    d.clear = ECUIO_CLEAR_NONE;
    d.flags |= ECUIO_DIAG_CLEARING;
    ch.clear_step = ECUIO_CLEAR_STEP_WAIT;
    return true;
}

/* Each target waiting on the judgment takes a reading: value into
 * clear_rpm or clear_kmh, as flag (ECUIO_DIAG_CLEAR_RPM, _KMH) says; its
 * generation moves with the step that settles (judge_end, judge_moved) */
inline void ecuio_engine_judge_note(ecuio_request_engine* e, uint32_t flag,
                                    uint32_t value)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.clear_step != ECUIO_CLEAR_STEP_WAIT) {
            continue;
        }
        if (flag == ECUIO_DIAG_CLEAR_RPM) {
            ch.diag.clear_rpm = value;
        } else {
            ch.diag.clear_kmh = value;
        }
        ch.diag.flags |= flag;
    }
}

/* The judgment's step settled with more to ask: each waiting target's
 * generation moves on, as a target's own step's did */
inline void ecuio_engine_judge_moved(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].clear_step == ECUIO_CLEAR_STEP_WAIT) {
            ++e->chans[c].diag.generation;
        }
    }
}

/* The judgment's end: every waiting target sends its 04 next
 * (ECUIO_CLEAR_NONE) or ends as outcome, with nrc, nothing sent */
inline void ecuio_engine_judge_end(ecuio_request_engine* e, uint8_t outcome,
                                   uint8_t nrc)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.clear_step != ECUIO_CLEAR_STEP_WAIT) {
            continue;
        }
        if (outcome == ECUIO_CLEAR_NONE) {
            ch.clear_step = ECUIO_CLEAR_STEP_CLEAR;
        } else {
            ch.clear_step = ECUIO_CLEAR_STEP_NONE;
            ch.diag.flags &= ~ECUIO_DIAG_CLEARING;
            ch.diag.clear = outcome;
            ch.diag.clear_nrc = nrc;
        }
        ++ch.diag.generation;
    }
}

/* The judgment's vehicle speed from the next module not yet asked 01
 * 0D: the engine module, then each waiting target in channel order;
 * with none left, no speed (nrc the last refusal's); nothing once no
 * target waits */
inline void ecuio_engine_judge_speed(ecuio_request_engine* e, uint8_t nrc)
{
    if (!ecuio_engine_judge_waits(e)) {
        return;
    }
    const uint32_t m = ecuio_engine_rpm_module_of(e);
    for (uint32_t k = 0; k <= e->chan_count; ++k) {
        const uint32_t c = k == 0 ? m : k - 1;
        if (c >= e->chan_count || ((e->speed_asked >> c) & 1u) != 0 ||
            !ecuio_engine_module_hears(e->chans[c]) ||
            (k > 0 && e->chans[c].clear_step != ECUIO_CLEAR_STEP_WAIT)) {
            continue;
        }
        e->speed_asked |= 1u << c;
        e->chans[c].judge_step = ECUIO_CLEAR_STEP_VEHICLE;
        return;
    }
    const bool moving_ok = (e->clear_flags & ECUIO_CLEAR_ALLOW_MOVING) != 0;
    ecuio_engine_judge_end(
        e, moving_ok ? ECUIO_CLEAR_NONE : ECUIO_CLEAR_NO_VEHICLE_SPEED,
        moving_ok ? 0 : nrc);
}

/* The judgment's engine speed read (got, raw in 0.25 rpm counts) or not
 * (nrc): 0 rpm sends every 04; under ECUIO_CLEAR_ALLOW_RUNNING it is
 * only recorded, and the vehicle speed is judged next; nothing once no
 * target waits */
inline void ecuio_engine_judge_rpm(ecuio_request_engine* e, bool got,
                                   uint32_t raw, uint8_t nrc)
{
    if (!ecuio_engine_judge_waits(e)) {
        return;
    }
    if (got) {
        ecuio_engine_judge_note(e, ECUIO_DIAG_CLEAR_RPM, (raw + 3) / 4);
    }
    if ((e->clear_flags & ECUIO_CLEAR_ALLOW_RUNNING) != 0) {
        ecuio_engine_judge_moved(e);
        ecuio_engine_judge_speed(e, 0);
    } else if (got && raw == 0) {
        ecuio_engine_judge_end(e, ECUIO_CLEAR_NONE, 0);
    } else if (got) {
        ecuio_engine_judge_end(e, ECUIO_CLEAR_ENGINE_RUNNING, 0);
    } else {
        ecuio_engine_judge_end(e, ECUIO_CLEAR_NO_ENGINE_SPEED, nrc);
    }
}

/* A clear request (ADR-0038): each of count targets past its gate
 * (ecuio_engine_clear_gate) waits on one judgment of the engine and
 * vehicle speed, flags (ECUIO_CLEAR_ALLOW_*) for them all, then sends
 * its 04. The engine module asks 01 0C; with none open there is no rpm.
 * False when a request runs, or no target passed its gate. */
inline bool ecuio_engine_clear_request(ecuio_request_engine* e,
                                       const ecuio_clear_target_t* targets,
                                       uint32_t count, uint32_t flags)
{
    if (ecuio_engine_clear_running(e)) {
        return false;
    }
    bool any = false;
    for (uint32_t k = 0; k < count; ++k) {
        any = ecuio_engine_clear_gate(e, targets[k].channel,
                                      targets[k].generation) ||
              any;
    }
    if (!any) {
        return false;
    }
    e->clear_flags = flags;
    e->speed_asked = 0;
    const uint32_t m = ecuio_engine_rpm_module_of(e);
    if (m < e->chan_count && ecuio_engine_module_hears(e->chans[m])) {
        e->chans[m].judge_step = ECUIO_CLEAR_STEP_SPEED;
    } else {
        ecuio_engine_judge_rpm(e, false, 0, 0);
    }
    return true;
}

/* Clears channel c's trouble codes: a clear request of one target */
inline bool ecuio_engine_clear(ecuio_request_engine* e, uint32_t c,
                               uint32_t generation, uint32_t flags = 0)
{
    const ecuio_clear_target_t target = {c, generation, {0, 0}};
    return ecuio_engine_clear_request(e, &target, 1, flags);
}

/* Settles the judgment's request module c had out: the engine speed's
 * answer (ecuio_engine_judge_rpm), or the vehicle speed's, whose 0 km/h
 * (or anything, under ECUIO_CLEAR_ALLOW_MOVING) sends every 04; a
 * vehicle speed refused or unanswered asks the next module */
inline void ecuio_engine_judge_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    const uint8_t step = ch.judge_step;
    ch.judge_step = ECUIO_CLEAR_STEP_NONE;
    if (step == ECUIO_CLEAR_STEP_SPEED) {
        const bool got = outcome == ECUIO_DIAG_ANSWERED && len >= 4;
        const uint32_t raw = got ? (uint32_t{data[2]} << 8) | data[3] : 0;
        ecuio_engine_judge_rpm(e, got, raw, nrc);
        return;
    }
    if (outcome != ECUIO_DIAG_ANSWERED || len < 3) {
        ecuio_engine_judge_speed(e, nrc);
        return;
    }
    ecuio_engine_judge_note(e, ECUIO_DIAG_CLEAR_KMH, data[2]);
    const bool moving_ok = (e->clear_flags & ECUIO_CLEAR_ALLOW_MOVING) != 0;
    ecuio_engine_judge_end(
        e, moving_ok || data[2] == 0 ? ECUIO_CLEAR_NONE : ECUIO_CLEAR_MOVING,
        0);
}

inline void ecuio_engine_codes_start(ecuio_engine_channel& ch, uint8_t mask,
                                     uint32_t read_id)
{
    const uint32_t generation = ch.codes.report.generation + 1;
    ch.codes.report = ecuio_module_codes_t{};
    ch.codes.report.generation = generation;
    ch.codes.report.read_id = read_id;
    ch.codes.report.mask = mask;
    ch.codes.report.flags = ECUIO_MODULE_CODES_READING;
    ch.codes.step = ECUIO_CODES_STEP_COUNT;
    ch.codes.out = false;
}

/* Taken when the read under way ends */
inline void ecuio_engine_codes_again(ecuio_engine_channel& ch, uint8_t mask,
                                     uint32_t read_id)
{
    ch.codes.again = true;
    ch.codes.again_mask = mask;
    ch.codes.again_id = read_id;
}

inline void ecuio_engine_read_codes(ecuio_request_engine* e, uint32_t c,
                                    uint8_t mask, uint32_t read_id)
{
    if (c >= e->chan_count) {
        return;
    }
    ecuio_engine_channel& ch = e->chans[c];
    mask = mask != 0 ? mask : uint8_t{ECUIO_MODULE_CODES_MASK_DEFAULT};
    if (ch.codes.step != ECUIO_CODES_STEP_NONE) {
        ecuio_engine_codes_again(ch, mask, read_id);
    } else {
        ecuio_engine_codes_start(ch, mask, read_id);
        ch.codes.again = false;
    }
}

inline void ecuio_engine_codes_again_after_clear(ecuio_engine_channel& ch)
{
    const ecuio_module_codes_t& r = ch.codes.report;
    if (r.read_id != 0 && ch.codes.step == ECUIO_CODES_STEP_NONE &&
        !ch.codes.again) {
        ecuio_engine_codes_again(ch, r.mask, r.read_id);
    }
}

/* A taken again starts here, so the read before it was COMPLETE for one
 * publish at least */
inline bool ecuio_engine_take_codes(ecuio_engine_channel& ch, uint32_t c)
{
    if (ch.codes.step == ECUIO_CODES_STEP_NONE) {
        if (!ch.codes.again) {
            return false;
        }
        ch.codes.again = false;
        ecuio_engine_codes_start(ch, ch.codes.again_mask, ch.codes.again_id);
    }
    ch.once = ecuio_request{};
    ch.once.bytes[0] = ECUIO_CODES_SERVICE;
    ch.once.bytes[1] = ch.codes.step == ECUIO_CODES_STEP_COUNT ? 0x01 : 0x02;
    ch.once.bytes[2] = ch.codes.report.mask;
    ch.once.len = 3;
    ch.once.channel = static_cast<uint8_t>(c);
    ch.codes.out = true;
    ch.scan_turn = false;
    return true;
}

inline void ecuio_engine_codes_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len,
                                      int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_module_codes_t& r = ch.codes.report;
    ch.codes.out = false;
    const bool answered = outcome == ECUIO_DIAG_ANSWERED && data != nullptr;
    if (ch.codes.step == ECUIO_CODES_STEP_COUNT) {
        r.count_outcome = outcome;
        r.count_nrc = nrc;
        if (answered) {
            (void)ecuio_diag_take_uds_count(data, len, &r);
        }
        ch.codes.step = ECUIO_CODES_STEP_LIST;
    } else {
        r.list_outcome = outcome;
        r.list_nrc = nrc;
        if (answered) {
            ecuio_diag_take_uds_list(data, len, &r);
        }
        ch.codes.step = ECUIO_CODES_STEP_NONE;
        r.flags = (r.flags & ~ECUIO_MODULE_CODES_READING) |
                  ECUIO_MODULE_CODES_COMPLETE;
        r.settled_us = now_us;
    }
    ++r.generation;
}

/* Module c's read can't open it: COMPLETE, what is left unasked, a read
 * waiting behind it ended with it */
inline void ecuio_engine_codes_not_opened(ecuio_engine_channel& ch)
{
    if (ch.codes.out || !ecuio_engine_codes_asks(ch)) {
        return;
    }
    if (ch.codes.again) {
        ch.codes.again = false;
        ecuio_engine_codes_start(ch, ch.codes.again_mask, ch.codes.again_id);
    }
    ecuio_module_codes_t& r = ch.codes.report;
    ch.codes.step = ECUIO_CODES_STEP_NONE;
    r.flags = (r.flags & ~ECUIO_MODULE_CODES_READING) |
              ECUIO_MODULE_CODES_COMPLETE | ECUIO_MODULE_CODES_NOT_OPENED;
    ++r.generation;
}

/* Settles the request channel c's clear has out: answered with the
 * reply, refused with its code, or unanswered (ECUIO_DIAG_*). The
 * judgment's request settles the judgment; 04's answer ends the target,
 * and a 44 or no answer reads the report again. */
inline void ecuio_engine_clear_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_channel_diagnostics_t& d = ch.diag;
    ch.clearing = false;
    if (ch.judge_step != ECUIO_CLEAR_STEP_NONE) {
        ecuio_engine_judge_settle(e, c, outcome, nrc, data, len);
        return;
    }
    bool read = false;
    if (outcome == ECUIO_DIAG_ANSWERED) {
        d.clear = ECUIO_CLEAR_CLEARED;
        read = true;
    } else if (outcome == ECUIO_DIAG_REFUSED) {
        d.clear = ECUIO_CLEAR_REFUSED;
        d.clear_nrc = nrc;
    } else {
        d.clear = ECUIO_CLEAR_UNANSWERED;
        read = true;
    }
    ch.clear_step = ECUIO_CLEAR_STEP_NONE;
    d.flags &= ~ECUIO_DIAG_CLEARING;
    ++d.generation;
    if (read) {
        ecuio_engine_diag_restart(ch);
        ecuio_engine_codes_again_after_clear(ch);
    }
}

/* The next unasked diagnostics item of channel c, in list order; -1
 * when none. */
inline int32_t ecuio_engine_diag_next(const ecuio_request_engine* e, uint32_t c)
{
    const ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < ch.diag_count; ++i) {
        if (ch.diag_items[i].outcome == ECUIO_DIAG_UNASKED) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* Settles each unasked Service 01 item of channel c whose PID the walk's
 * bitmaps state unsupported, unasked: an ECU that says it lacks PID 41
 * would only let the request wait out its time */
inline void ecuio_engine_diag_skip_unsupported(ecuio_request_engine* e,
                                               uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < ch.diag_count; ++i) {
        const ecuio_diag_item& it = ch.diag_items[i];
        if (it.outcome == ECUIO_DIAG_UNASKED && it.request[0] == 0x01 &&
            it.request[1] != 0 &&
            ecuio_engine_pid_bit(ch.known, it.request[1]) &&
            !ecuio_engine_pid_bit(ch.supported, it.request[1])) {
            ch.diag_index = i;
            ecuio_engine_diag_settle(e, c, ECUIO_DIAG_UNSUPPORTED, 0, nullptr,
                                     0);
        }
    }
}

/* What channel c's ECU has said of its diagnostics, for the session's
 * report; the driver adds the channel's rx_id. Complete once every item
 * listed has an outcome. */
inline void ecuio_engine_diagnostics(const ecuio_request_engine* e, uint32_t c,
                                     ecuio_channel_diagnostics_t* out)
{
    *out = e->chans[c].diag;
}

#endif
