// File/layer: core/src/request_engine_pack.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// Packing (ADR-0039 Appendix A): what a pack carries, its reply and
// its failures.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest (test_obd2)

#ifndef ECUIO_REQUEST_ENGINE_PACK_H
#define ECUIO_REQUEST_ENGINE_PACK_H

#include "isotp.h"
#include "request_engine_rotation.h"

/* Whether request i of channel c may go in a pack: packing on and not
 * ended, the walk over, a Service 01 PID the bitmaps list that isn't a
 * bitmap, whose own reply showed a length the split will use. */
inline bool ecuio_engine_packable(const ecuio_request_engine* e, uint32_t c,
                                  uint32_t i)
{
    const ecuio_engine_channel& ch = e->chans[c];
    const ecuio_request& r = e->requests[i];
    if (!e->pack || ch.unpacked || ch.walking || r.channel != c ||
        !ecuio_engine_in_rotation(r) || r.len != 2 || r.bytes[0] != 0x01) {
        return false;
    }
    const uint8_t pid = r.bytes[1];
    return pid % 0x20u != 0 && (ch.supported[(pid - 1u) / 8u] &
                                (0x80u >> ((pid - 1u) % 8u))) != 0 &&
           ch.pid_len[pid] != 0 && ch.pid_len[pid] != ECUIO_ENGINE_PID_LONG;
}

/* Whether the reply to request r of channel c is expected to take more
 * than one CAN frame (Known Issue #42's hold): a pack by its records'
 * lengths, a table row by its last answer, and always an identification
 * or diagnostics item (most answers take several frames, and these go
 * out on every channel at once at connect and on recovery) or any
 * one-shot asking for the VIN (09 02, 22 F1 90: 17 characters). A table
 * row with no answer and no wait run out is judged by its defs and, for
 * a Service 01 PID, J1979's length: segmented when either reaches past a
 * Single Frame, so its first ask shares the bus with no one while it is
 * out. A row whose wait ran out before any answer counts as one frame
 * until a recovery or its module's waking (ran_out), as does any other
 * one-shot, and a talk is never judged. */
inline bool ecuio_engine_reply_segmented(const ecuio_request_engine* e,
                                         uint32_t c, int32_t r)
{
    const ecuio_engine_channel& ch = e->chans[c];
    if (r == ECUIO_ENGINE_PACK) {
        return isotp_frames(ch.pack.reply_len) > 1;
    }
    if (r == ECUIO_ENGINE_ONCE) {
        const ecuio_request& q = ch.once;
        const bool vin_09 =
            q.len == 2 && q.bytes[0] == 0x09 && q.bytes[1] == 0x02;
        const bool vin_22 = q.len == 3 && q.bytes[0] == 0x22 &&
                            q.bytes[1] == 0xF1 && q.bytes[2] == 0x90;
        const bool list =
            ch.codes.out && ch.codes.step == ECUIO_CODES_STEP_LIST;
        return !ch.talk.out &&
               (ch.identifying || ch.diagnosing || list || vin_09 || vin_22);
    }
    if (r < 0 || static_cast<uint32_t>(r) >= e->count) {
        return false;
    }
    const ecuio_request& q = e->requests[r];
    if (q.reply_frames != 0) {
        return q.reply_frames > 1;
    }
    const uint32_t pid_bytes = q.len == 2 && q.bytes[0] == 0x01
                                   ? ECUIO_OBD2_PID_BYTES[q.bytes[1]]
                                   : 0u;
    return q.long_defs != 0 || isotp_frames(2u + pid_bytes) > 1;
}

/* Request i, due on channel c, alone or as the first of a pack: the
 * next requests of its class from that class's cursor join it while
 * each may (ecuio_engine_packable) and the reply stays within
 * e->pack_frames CAN frames (0 no limit), up to ECUIO_ENGINE_PACK_MAX;
 * the first that may not stops the pack and goes next. Returns i, or
 * ECUIO_ENGINE_PACK with the pack in ch.pack, its cursor past the last
 * request it took. */
inline int32_t ecuio_engine_gather(ecuio_request_engine* e, uint32_t c,
                                   int32_t i)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (!ecuio_engine_packable(e, c, static_cast<uint32_t>(i))) {
        return i;
    }
    const bool slow = e->requests[i].slow != 0;
    uint32_t& cursor = slow ? ch.slow_cursor : ch.cursor;
    ch.pack_rows[0] = static_cast<uint32_t>(i);
    ch.pack_count = 1;
    /* 41, then each PID's record at the length its own reply showed */
    uint32_t reply = 2u + ch.pid_len[e->requests[i].bytes[1]];
    for (uint32_t k = 0; k < e->count && ch.pack_count < ECUIO_ENGINE_PACK_MAX;
         ++k) {
        const uint32_t j = (cursor + k) % e->count;
        const ecuio_request& r = e->requests[j];
        if (r.channel != c || !ecuio_engine_in_rotation(r) ||
            (r.slow != 0) != slow) {
            continue;
        }
        if (j == static_cast<uint32_t>(i) || !ecuio_engine_packable(e, c, j)) {
            break;
        }
        const uint32_t longer = reply + 1u + ch.pid_len[r.bytes[1]];
        if (e->pack_frames != 0 && isotp_frames(longer) > e->pack_frames) {
            break;
        }
        reply = longer;
        ch.pack_rows[ch.pack_count++] = j;
        cursor = (j + 1) % e->count;
        k = static_cast<uint32_t>(-1); /* the cursor moved: look from it */
    }
    if (ch.pack_count < 2) {
        return i;
    }
    ch.pack = ecuio_request{};
    ch.pack.bytes[0] = 0x01;
    for (uint32_t k = 0; k < ch.pack_count; ++k) {
        ch.pack.bytes[1 + k] = e->requests[ch.pack_rows[k]].bytes[1];
    }
    ch.pack.len = static_cast<uint8_t>(1 + ch.pack_count);
    ch.pack.channel = static_cast<uint8_t>(c);
    ch.pack.slow = slow ? 1 : 0;
    ch.pack.reply_len = static_cast<uint8_t>(reply);
    return ECUIO_ENGINE_PACK;
}

/* A pack on channel c that failed: silence, a negative reply or one
 * record. The ECUIO_ENGINE_PACK_FAILURES-th in a row ends packing on
 * the channel until ecuio_engine_rescan. */
inline void ecuio_engine_pack_failed(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (++ch.pack_failures >= ECUIO_ENGINE_PACK_FAILURES && !ch.unpacked) {
        ch.unpacked = true;
        ++ch.generation;
    }
}

/* A frame on channel c while its pack is out: a positive reply whose
 * first record's PID is one of the pack's answers it, and succeeds when
 * its records, each of a PID the pack asked at the length its own reply
 * showed, fill it exactly and number two or more; a negative reply to
 * Service 01 ends it failed; a pending one extends the wait. Out of step
 * (ecuio_engine_on_frame), a reply that would fail it answers nothing:
 * the request before's, come late, can carry one of the pack's PIDs or
 * name its service, and the pack's own may follow, its wait judging it.
 * Nothing is dropped: the probe alone drops a PID. */
inline ecuio_reply ecuio_engine_pack_reply(ecuio_request_engine* e,
                                           uint32_t c, const uint8_t* data,
                                           uint16_t len, int64_t now_us,
                                           uint8_t* out_nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    const auto asked = [&ch](uint8_t pid) {
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            if (ch.pack.bytes[1 + k] == pid) {
                return true;
            }
        }
        return false;
    };
    if (len >= 3 && data[0] == 0x41u && asked(data[1])) {
        uint32_t records = 0;
        uint32_t at = 1;
        while (at < len && asked(data[at]) &&
               at + 1u + ch.pid_len[data[at]] <= len) {
            at += 1u + ch.pid_len[data[at]];
            ++records;
        }
        const bool filled = at == len && records >= 2;
        if (!filled && !ch.in_step) {
            return ecuio_reply::none;
        }
        ch.outstanding = ECUIO_ENGINE_NONE;
        ch.in_step = true;
        if (filled) {
            ch.pack_failures = 0;
            if (!ch.packed) {
                ch.packed = true;
                ++ch.generation;
            }
        } else {
            ecuio_engine_pack_failed(e, c);
        }
        return ecuio_reply::positive;
    }
    if (len < 3 || data[0] != ECUIO_NEGATIVE_RESPONSE || data[1] != 0x01u ||
        (data[2] != ECUIO_NRC_RESPONSE_PENDING && !ch.in_step)) {
        return ecuio_reply::none;
    }
    if (out_nrc != nullptr) {
        *out_nrc = data[2];
    }
    if (data[2] == ECUIO_NRC_RESPONSE_PENDING) {
        ch.deadline_us = now_us + ECUIO_ENGINE_PENDING_WAIT_US;
        return ecuio_reply::pending;
    }
    ch.outstanding = ECUIO_ENGINE_NONE;
    ecuio_engine_pack_failed(e, c);
    return ecuio_reply::negative;
}

/* A pack's reply, for reply timing: each PID whose record came back is
 * answered, every PID refused by a refusal; a PID the reply left out
 * gets nothing that pass (ADR-0042 section 1) */
inline void ecuio_engine_time_pack(ecuio_request_engine* e, uint32_t c,
                                   ecuio_reply reply, const uint8_t* data,
                                   uint16_t len, int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    if (e->timing == nullptr) {
        return;
    }
    if (reply == ecuio_reply::negative) {
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            ecuio_engine_time_reply(e, c, ch.pack_rows[k], false, true, now_us);
        }
        return;
    }
    if (reply != ecuio_reply::positive) {
        return;
    }
    uint32_t at = 1;
    while (at < len) {
        const uint8_t pid = data[at];
        uint32_t k = 0;
        while (k < ch.pack_count && ch.pack.bytes[1 + k] != pid) {
            ++k;
        }
        if (k == ch.pack_count || at + 1u + ch.pid_len[pid] > len) {
            return;
        }
        ecuio_engine_time_reply(e, c, ch.pack_rows[k], true, true, now_us);
        at += 1u + ch.pid_len[pid];
    }
}

#endif
