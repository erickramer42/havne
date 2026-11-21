// File/layer: core/src/request_engine_reply.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// What became of a request: sent, expired or answered by a frame.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, EngineTalkTest, RequestTurnsTest
//          (test_obd2); HARNESS: every transport's suites

#ifndef ECUIO_REQUEST_ENGINE_REPLY_H
#define ECUIO_REQUEST_ENGINE_REPLY_H

#include "request_engine_pack.h"
#include "request_engine_scan.h"
#include "request_engine_walk.h"
#include "request_engine_diag.h"
#include "request_engine_session.h"

/* After the driver's write of request r on channel c, at now_us: a sent
 * request waits for its reply until the wait ends; one that wasn't sent
 * leaves nothing outstanding, and a one-shot stays due. The wait and the
 * latency run from now, the write's return; the floor from start_us, the
 * write's start (now when the driver gives none), so the write's own
 * time isn't added to it. A sent table row, and each row of a sent pack,
 * is stamped in asked_us. */
inline void ecuio_engine_sent(ecuio_request_engine* e, uint32_t c, int32_t r,
                              bool sent, int64_t now_us, int64_t start_us = -1)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (r == ECUIO_ENGINE_ONCE && ch.talk.out) {
        ecuio_talk_sent(ch.talk, ch.key, sent, now_us); /* never twice */
    }
    ch.outstanding = sent ? r : ECUIO_ENGINE_NONE;
    ch.deadline_us = now_us + ECUIO_ENGINE_WAIT_US;
    if (sent) {
        ch.sent_us = now_us;
        ch.last_sent_us = now_us;
        if (r >= 0 && static_cast<uint32_t>(r) < e->count) {
            e->asked_us[r] = now_us;
        }
        for (uint32_t k = 0; r == ECUIO_ENGINE_PACK && k < ch.pack_count;
             ++k) {
            e->asked_us[ch.pack_rows[k]] = now_us;
        }
    }
    const int64_t from =
        (start_us < 0 || start_us > now_us) ? now_us : start_us;
    ch.next_send_us = from + e->gap_us;
}

/* Whether a reply may still come on any channel: a request out, or one
 * whose wait ended with its reply not yet in (out of step). While none
 * may, nothing the driver could read answers a request. */
inline bool ecuio_engine_awaiting(const ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].outstanding != ECUIO_ENGINE_NONE ||
            !e->chans[c].in_step) {
            return true;
        }
    }
    return false;
}

/* Ends channel c's request once its wait has run out: true when one
 * timed out, which is that request's outcome. Its reply may still come,
 * so the channel is out of step until a positive reply. A bitmap that
 * times out ends the walk, unless a reply naming another PID came
 * while it waited: then it's asked once more, while the budget lasts.
 * A scanned DID, an identification item, a diagnostics item or a
 * clear's request that times out is unanswered. A table row that times
 * out never answered counts one frame from then (reply_segmented), until
 * an answer, a late one too (ecuio_engine_late_frames), teaches it, or a
 * recovery or its module's waking takes it back (ran_out). */
inline bool ecuio_engine_expire(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding == ECUIO_ENGINE_NONE || now_us < ch.deadline_us) {
        return false;
    }
    const int32_t out = ch.outstanding;
    const bool once = out == ECUIO_ENGINE_ONCE;
    const bool pack = out == ECUIO_ENGINE_PACK;
    ch.outstanding = ECUIO_ENGINE_NONE;
    ch.in_step = false;
    if (!once) {
        e->answering &= ~(1u << c); /* a table's request: the ECU's silence */
    }
    const auto timed_out = [e](uint32_t row) {
        if (e->timing != nullptr && row < e->count &&
            e->timing_of[row] < ECUIO_TIMING_SPENT) {
            ++e->timing->rows[e->timing_of[row]].timeouts;
        }
    };
    if (out >= 0) {
        timed_out(static_cast<uint32_t>(out));
        ch.check_from = static_cast<uint32_t>(out) + 1; /* a check's next row */
        if (static_cast<uint32_t>(out) < e->count &&
            e->requests[out].reply_frames == 0) {
            e->requests[out].reply_frames = 1;
            e->requests[out].ran_out = 1;
        }
    } else if (pack) {
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            timed_out(ch.pack_rows[k]);
        }
    }
    if (pack) {
        ecuio_engine_pack_failed(e, c);
    } else if (once && ch.sessioning) {
        ecuio_engine_session_settle(e, c, ECUIO_DIAG_UNANSWERED, 0, now_us);
    } else if (once && ch.clearing) {
        ecuio_engine_clear_settle(e, c, ECUIO_DIAG_UNANSWERED, 0, nullptr, 0);
    } else if (once && ch.identifying) {
        ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_UNANSWERED, 0,
                                  nullptr, 0);
    } else if (once && ch.diagnosing) {
        ecuio_engine_diag_settle(e, c, ECUIO_DIAG_UNANSWERED, 0, nullptr, 0);
    } else if (once && ch.scanning) {
        ecuio_engine_scan_settle(e, c, ECUIO_SCAN_UNANSWERED, 0);
    } else if (once && ch.talk.out) {
        ecuio_talk_settle(ch.talk, ch.key, ecuio_talk_end::unanswered, 0,
                          now_us);
    } else if (once) {
        if (ch.walk_stray && !ch.walk_reasked &&
            (!ch.walk_timed || now_us < ch.walk_until_us)) {
            ch.walk_reasked = true; /* due asks the same bitmap again */
        } else {
            ecuio_engine_end_walk(e, c);
        }
    }
    ch.walk_stray = false;
    return true;
}

/* A PID's own reply to request r: its length, for packing */
inline void ecuio_engine_learn_pid_len(ecuio_engine_channel& ch,
                                       const ecuio_request& r, uint16_t len)
{
    const uint32_t bytes = len - 2u;
    const uint32_t table = ECUIO_OBD2_PID_BYTES[r.bytes[1]];
    ch.pid_len[r.bytes[1]] = bytes == 0       ? 0
                             : bytes <= table ? static_cast<uint8_t>(bytes)
                                              : ECUIO_ENGINE_PID_LONG;
}

/* The one-shot channel c had out, answered by the frame: the item it
 * held is settled, or the walk takes the bitmap */
inline void ecuio_engine_once_answered(ecuio_request_engine* e, uint32_t c,
                                       const uint8_t* data, uint16_t len,
                                       int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.clearing) {
        ecuio_engine_clear_settle(e, c, ECUIO_DIAG_ANSWERED, 0, data, len);
    } else if (ch.identifying) {
        ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_ANSWERED, 0, data, len);
    } else if (ch.diagnosing) {
        ecuio_engine_diag_settle(e, c, ECUIO_DIAG_ANSWERED, 0, data, len);
    } else if (ch.scanning) {
        ecuio_engine_scan_settle(e, c, ECUIO_SCAN_FOUND, 0, len);
    } else if (ch.talk.out) {
        ecuio_talk_settle(ch.talk, ch.key, ecuio_talk_end::answered, 0, now_us);
    } else if (len >= ECUIO_BITMAP_REPLY_LEN) {
        ecuio_engine_take_bitmap(e, c, data, now_us);
    } else {
        ecuio_engine_end_walk(e, c);
    }
}

/* The positive reply to request r, out on channel c: it completes the
 * request and puts the channel back in step. Out of step a Service 01
 * PID's reply teaches r only at J1979's length for it alone, as
 * ecuio_engine_late_frames: the match reads the PID alone, so a late
 * pack's reply that starts with r's PID would give r the pack's. */
inline ecuio_reply ecuio_engine_take_positive(ecuio_request_engine* e,
                                              uint32_t c, int32_t out,
                                              const ecuio_request& r,
                                              const uint8_t* data, uint16_t len,
                                              int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    const bool pid = r.len == 2 && r.bytes[0] == 0x01;
    const bool learns =
        out >= 0 &&
        (ch.in_step || !pid || len == 2u + ECUIO_OBD2_PID_BYTES[r.bytes[1]]);
    ch.outstanding = ECUIO_ENGINE_NONE;
    ch.in_step = true;
    if (out >= 0) {
        ecuio_engine_time_reply(e, c, static_cast<uint32_t>(out), true, false,
                                now_us);
    }
    if (learns) {
        ecuio_engine_row_frames(e->requests[out],
                                ecuio_engine_reply_frames(len));
    }
    if (learns && pid) {
        ecuio_engine_learn_pid_len(ch, r, len);
    }
    if (out == ECUIO_ENGINE_ONCE && ch.sessioning) {
        ecuio_engine_session_settle(e, c, ECUIO_DIAG_ANSWERED, 0, now_us);
        return ecuio_reply::positive;
    }
    ch.session_fresh = false;
    if (ecuio_engine_same_request(ch.session_refused, r)) {
        ch.session_refused = ecuio_request{}; /* the loss was real */
    }
    if (out == ECUIO_ENGINE_ONCE) {
        ecuio_engine_once_answered(e, c, data, len, now_us);
    }
    return ecuio_reply::positive;
}

/* A positive reply that came after its row's wait ran out, on channel c,
 * teaches that row its frames as an answer in time does: a DID's by its
 * key, a Service 01 PID's only at J1979's length for it alone, which a
 * pack's reply never has (reply_segmented) */
inline void ecuio_engine_late_frames(ecuio_request_engine* e, uint32_t c,
                                     const uint8_t* data, uint16_t len)
{
    const bool did = len >= 3 && data[0] == 0x62;
    const bool pid = len >= 2 && data[0] == 0x41 &&
                     ECUIO_OBD2_PID_BYTES[data[1]] != 0 &&
                     len == 2u + ECUIO_OBD2_PID_BYTES[data[1]];
    if (!did && !pid) {
        return;
    }
    const uint8_t req[3] = {static_cast<uint8_t>(data[0] - 0x40), data[1],
                            did ? data[2] : uint8_t{0}};
    const int32_t row = ecuio_engine_find(e, c, req, did ? 3 : 2);
    if (row >= 0) {
        ecuio_engine_row_frames(e->requests[row],
                                ecuio_engine_reply_frames(len));
    }
}

/* While the walk's bitmap is out, a reply of its service that isn't the
 * bitmap's: another PID's answer, so a silence may be asked once more */
inline void ecuio_engine_mark_stray(ecuio_engine_channel& ch, int32_t out,
                                    const ecuio_request& r, const uint8_t* data,
                                    uint16_t len)
{
    if (out == ECUIO_ENGINE_ONCE && ch.walking && !ch.identifying &&
        !ch.diagnosing && !ch.scanning && !ch.clearing && !ch.sessioning &&
        !ch.talk.out && len >= 2 &&
        data[0] == static_cast<uint8_t>(r.bytes[0] + 0x40)) {
        ch.walk_stray = true; /* another PID's answer, not the bitmap's */
    }
}

/* While the probe is on and channel c in step, what a refusal of
 * request out (service, code) drops: its request, or for 0x11 every
 * request of its service on the channel; a talk drops nothing */
inline void ecuio_engine_drop_refused(ecuio_request_engine* e, uint32_t c,
                                      int32_t out, uint8_t service,
                                      uint8_t code, bool talk)
{
    const ecuio_engine_channel& ch = e->chans[c];
    const uint8_t reason =
        (e->probe && ch.in_step && !talk) ? ecuio_engine_drop_reason(code) : 0;
    if (reason == ECUIO_DROP_SERVICE) {
        for (uint32_t i = 0; i < e->count; ++i) {
            if (e->requests[i].channel == c &&
                e->requests[i].bytes[0] == service) {
                ecuio_engine_drop(e, i, reason, code);
            }
        }
    } else if (reason != 0 && out != ECUIO_ENGINE_ONCE) {
        ecuio_engine_drop(e, static_cast<uint32_t>(out), reason, code);
    }
}

/* The one-shot channel c had out, refused with code: the item it held
 * is settled, or the walk ends */
inline void ecuio_engine_once_refused(ecuio_request_engine* e, uint32_t c,
                                      uint8_t code, bool talk, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.clearing) {
        ecuio_engine_clear_settle(e, c, ECUIO_DIAG_REFUSED, code, nullptr, 0);
    } else if (ch.identifying) {
        ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_REFUSED, code, nullptr,
                                  0);
    } else if (ch.diagnosing) {
        ecuio_engine_diag_settle(e, c, ECUIO_DIAG_REFUSED, code, nullptr, 0);
    } else if (ch.scanning) {
        ecuio_engine_scan_settle(e, c, ECUIO_SCAN_REFUSED, code);
    } else if (talk) {
        ecuio_talk_settle(ch.talk, ch.key, ecuio_talk_end::refused, code,
                          now_us);
    } else {
        ecuio_engine_end_walk(e, c);
    }
}

/* The negative reply (code, not pending) to request r, out on channel
 * c: the session's own, a loss that enters the session again, or a
 * refusal that completes the request */
inline ecuio_reply ecuio_engine_take_negative(ecuio_request_engine* e,
                                              uint32_t c, int32_t out,
                                              const ecuio_request& r,
                                              uint8_t code, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    /* Out of step a negative reply may be another row's */
    if (out >= 0 && ch.in_step) {
        ecuio_engine_row_frames(e->requests[out], 1); /* 7F <sid> <code> */
    }
    if (out == ECUIO_ENGINE_ONCE && ch.sessioning) {
        ch.outstanding = ECUIO_ENGINE_NONE;
        ecuio_engine_session_settle(e, c, ECUIO_DIAG_REFUSED, code, now_us);
        return ecuio_reply::negative;
    }
    if (ch.in_step && ecuio_engine_session_reenter(e, c, out, code, now_us)) {
        return ecuio_reply::negative;
    }
    ch.outstanding = ECUIO_ENGINE_NONE;
    if (out >= 0) {
        ecuio_engine_time_reply(e, c, static_cast<uint32_t>(out), false, false,
                                now_us);
    }
    const uint8_t service = r.bytes[0];
    const bool talk = out == ECUIO_ENGINE_ONCE && ch.talk.out;
    ecuio_engine_drop_refused(e, c, out, service, code, talk);
    if (out == ECUIO_ENGINE_ONCE) {
        ecuio_engine_once_refused(e, c, code, talk, now_us);
    }
    return ecuio_reply::negative;
}

/* A frame received on channel c, its payload after the CAN ID (at most
 * the first six bytes are read, except that an identification reply's
 * data becomes the item's text). A positive or negative reply completes
 * the outstanding request; a pending one keeps it outstanding and
 * extends its wait to the pending wait from now. A bitmap carries on the
 * walk; while the probe is on, a negative reply drops what its code says
 * (ecuio_engine_drop_reason): its request, or for 0x11 every request of
 * its service on the channel. A negative reply names the service but not
 * the key, so one that came late, after its request timed out, would
 * name the request now outstanding: it drops only while the channel is
 * in step. An ECU answers in order, so a positive reply, which carries
 * the key, puts the channel back in step. A scanned DID is settled by
 * either reply, in step or not: its answer counts once, and a late
 * negative reply can at worst count the DID out now as refused, while
 * the reply that was really its own still decodes through its def. An
 * identification item, a diagnostics item and a clear's request are
 * settled the same way;
 * a one-byte request (03, 07, 0A) has no key, so its service's reply
 * alone answers it. *out_nrc (when not NULL) gets the code of a negative
 * or pending reply. */
inline ecuio_reply ecuio_engine_take_frame(ecuio_request_engine* e, uint32_t c,
                                           const uint8_t* data, uint16_t len,
                                           int64_t now_us, uint8_t* out_nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding == ECUIO_ENGINE_NONE || len == 0) {
        ecuio_engine_late_frames(e, c, data, len);
        (void)ecuio_engine_scan_late(e, c, data, len);
        return ecuio_reply::none;
    }
    const int32_t out = ch.outstanding;
    if (out == ECUIO_ENGINE_PACK) {
        if (data[0] != 0x41) {
            /* a 41 reply is the pack's own; another a late row's */
            ecuio_engine_late_frames(e, c, data, len);
        }
        if (ecuio_engine_scan_late(e, c, data, len)) {
            return ecuio_reply::none;
        }
        const ecuio_reply reply =
            ecuio_engine_pack_reply(e, c, data, len, now_us, out_nrc);
        ecuio_engine_time_pack(e, c, reply, data, len, now_us);
        return reply;
    }
    const ecuio_request& r = ecuio_engine_request_of(e, c, out);
    if (len >= r.len && data[0] == static_cast<uint8_t>(r.bytes[0] + 0x40) &&
        (r.len < 2 || data[1] == r.bytes[1]) &&
        (r.len < 3 || data[2] == r.bytes[2])) {
        return ecuio_engine_take_positive(e, c, out, r, data, len, now_us);
    }
    ecuio_engine_mark_stray(ch, out, r, data, len);
    ecuio_engine_late_frames(e, c, data, len);
    if (ecuio_engine_scan_late(e, c, data, len)) {
        return ecuio_reply::none;
    }
    if (len < 3 || data[0] != ECUIO_NEGATIVE_RESPONSE ||
        data[1] != r.bytes[0]) {
        return ecuio_reply::none;
    }
    const uint8_t code = data[2];
    if (out_nrc != nullptr) {
        *out_nrc = code;
    }
    if (code == ECUIO_NRC_RESPONSE_PENDING) {
        ch.deadline_us = now_us + ECUIO_ENGINE_PENDING_WAIT_US;
        return ecuio_reply::pending;
    }
    return ecuio_engine_take_negative(e, c, out, r, code, now_us);
}

/* The driver's call for a frame received on channel c:
 * ecuio_engine_take_frame, and the channel's ECU is answering, whatever
 * the frame answered. */
inline ecuio_reply ecuio_engine_on_frame(ecuio_request_engine* e, uint32_t c,
                                         const uint8_t* data, uint16_t len,
                                         int64_t now_us, uint8_t* out_nrc)
{
    e->answering |= 1u << c;
    return ecuio_engine_take_frame(e, c, data, len, now_us, out_nrc);
}

#endif
