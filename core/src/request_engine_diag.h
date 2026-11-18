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
    ch.diagnosing = false;
    it.outcome = outcome;
    it.nrc = nrc;
    if (ch.diag_pending > 0) {
        --ch.diag_pending;
    }
    const bool answered = outcome == ECUIO_DIAG_ANSWERED;
    switch (it.request[0]) {
    case 0x01:
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
        break;
    case 0x03:
    case 0x07:
    case 0x0A: {
        ecuio_dtc_list_t& l =
            d.lists[it.request[0] == 0x03   ? ECUIO_DIAG_STORED
                    : it.request[0] == 0x07 ? ECUIO_DIAG_PENDING
                                            : ECUIO_DIAG_PERMANENT];
        l.outcome = outcome;
        l.nrc = nrc;
        if (answered) {
            ecuio_diag_take_codes(data, len, &l);
        }
        break;
    }
    case 0x02:
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
        break;
    default:
        break;
    }
    if (ch.diag_pending == 0) {
        d.flags |= ECUIO_DIAG_COMPLETE;
    }
    ++d.generation;
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

/* Asks the whole list again on every channel whose last read has
 * settled ("Read again"): its report starts over, its generation moves
 * on. A channel mid-read, mid-clear, or never asked, is left as it is. */
inline void ecuio_engine_diagnose_again(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.diag_count == 0 || ch.diagnosing || ch.diag_pending > 0 ||
            ch.clear_step != ECUIO_CLEAR_STEP_NONE) {
            continue;
        }
        ecuio_engine_diag_restart(ch);
    }
}

/* Clears channel c's trouble codes (ADR-0038) when its report is the
 * one the caller saved: read to the end, at generation, with no clear
 * running. The clear then asks the engine speed through the one-shot,
 * and sends 04 only at 0 rpm, or under flags' ECUIO_CLEAR_ALLOW_RUNNING
 * asks the vehicle speed next (ecuio_engine_clear_settle). Any other
 * report records ECUIO_CLEAR_REPORT_CHANGED and sends nothing. False
 * then, and for a channel past the count, one that reads no
 * diagnostics, or one already clearing (which carries on). */
inline bool ecuio_engine_clear(ecuio_request_engine* e, uint32_t c,
                               uint32_t generation, uint32_t flags = 0)
{
    if (c >= e->chan_count) {
        return false;
    }
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_channel_diagnostics_t& d = ch.diag;
    if (ch.diag_count == 0 || ch.clear_step != ECUIO_CLEAR_STEP_NONE) {
        return false;
    }
    d.clear_nrc = 0;
    d.clear_rpm = 0;
    d.clear_kmh = 0;
    d.flags &= ~(ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH);
    ++d.generation;
    if ((d.flags & ECUIO_DIAG_COMPLETE) == 0 || ch.diagnosing ||
        ch.diag_pending > 0 || d.generation != generation + 1) {
        d.clear = ECUIO_CLEAR_REPORT_CHANGED;
        return false;
    }
    d.clear = ECUIO_CLEAR_NONE;
    d.flags |= ECUIO_DIAG_CLEARING;
    ch.clear_step = ECUIO_CLEAR_STEP_SPEED;
    ch.clear_flags = flags;
    return true;
}

/* Settles the request channel c's clear has out: answered with the
 * reply, refused with its code, or unanswered (ECUIO_DIAG_*). The engine
 * speed's answer sends 04 next at 0 rpm and ends the clear otherwise;
 * under ECUIO_CLEAR_ALLOW_RUNNING it is only recorded, any answer or
 * none, and the vehicle speed is asked, whose 0 km/h (or anything,
 * under ECUIO_CLEAR_ALLOW_MOVING) sends 04. 04's answer ends it, and a
 * 44 or no answer reads the report again. */
inline void ecuio_engine_clear_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_channel_diagnostics_t& d = ch.diag;
    ch.clearing = false;
    bool done = true;
    bool read = false;
    const bool running_ok = (ch.clear_flags & ECUIO_CLEAR_ALLOW_RUNNING) != 0;
    const bool moving_ok = (ch.clear_flags & ECUIO_CLEAR_ALLOW_MOVING) != 0;
    if (ch.clear_step == ECUIO_CLEAR_STEP_SPEED) {
        const bool got = outcome == ECUIO_DIAG_ANSWERED && len >= 4;
        const uint32_t raw = got ? (uint32_t{data[2]} << 8) | data[3] : 0;
        if (got) {
            d.clear_rpm = (raw + 3) / 4; /* 0.25 rpm a count */
            d.flags |= ECUIO_DIAG_CLEAR_RPM;
        }
        if (running_ok) {
            ch.clear_step = ECUIO_CLEAR_STEP_VEHICLE;
            done = false;
        } else if (got && raw == 0) {
            ch.clear_step = ECUIO_CLEAR_STEP_CLEAR;
            done = false;
        } else if (got) {
            d.clear = ECUIO_CLEAR_ENGINE_RUNNING;
        } else {
            d.clear = ECUIO_CLEAR_NO_ENGINE_SPEED;
            d.clear_nrc = nrc;
        }
    } else if (ch.clear_step == ECUIO_CLEAR_STEP_VEHICLE) {
        const bool got = outcome == ECUIO_DIAG_ANSWERED && len >= 3;
        if (got) {
            d.clear_kmh = data[2]; /* 1 km/h a count */
            d.flags |= ECUIO_DIAG_CLEAR_KMH;
        }
        if (moving_ok || (got && data[2] == 0)) {
            ch.clear_step = ECUIO_CLEAR_STEP_CLEAR;
            done = false;
        } else if (got) {
            d.clear = ECUIO_CLEAR_MOVING;
        } else {
            d.clear = ECUIO_CLEAR_NO_VEHICLE_SPEED;
            d.clear_nrc = nrc;
        }
    } else if (outcome == ECUIO_DIAG_ANSWERED) {
        d.clear = ECUIO_CLEAR_CLEARED;
        read = true;
    } else if (outcome == ECUIO_DIAG_REFUSED) {
        d.clear = ECUIO_CLEAR_REFUSED;
        d.clear_nrc = nrc;
    } else {
        d.clear = ECUIO_CLEAR_UNANSWERED;
        read = true;
    }
    if (done) {
        ch.clear_step = ECUIO_CLEAR_STEP_NONE;
        d.flags &= ~ECUIO_DIAG_CLEARING;
    }
    ++d.generation;
    if (read) {
        ecuio_engine_diag_restart(ch);
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
