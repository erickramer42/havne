// File/layer: core/src/request_engine_scan.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// The DID scan (ADR-0020) and the link's return (rescan).
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, UdsGenericTest (test_obd2)

#ifndef ECUIO_REQUEST_ENGINE_SCAN_H
#define ECUIO_REQUEST_ENGINE_SCAN_H

#include "request_engine_table.h"

/* Adds did to channel's scan (ADR-0020): asked once, 22 <DID>, outside
 * the rotation. The same DID on the same channel is kept once. False
 * when the scan is full or the channel is past the table. */
inline bool ecuio_engine_scan_add(ecuio_request_engine* e, uint32_t channel,
                                  uint16_t did)
{
    if (channel >= e->chan_count) {
        return false;
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        if (e->scan[i].channel == channel && e->scan[i].did == did) {
            return true;
        }
    }
    if (e->scan_count >= ECUIO_ENGINE_SCAN_MAX) {
        return false;
    }
    ecuio_scan_entry& s = e->scan[e->scan_count++];
    s = ecuio_scan_entry{};
    s.did = did;
    s.channel = static_cast<uint8_t>(channel);
    ++e->chans[channel].scan_pending;
    return true;
}

/* The next unasked scan entry of channel c, from its cursor round the
 * list; -1 when there is none. */
inline int32_t ecuio_engine_scan_next(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t k = 0; k < e->scan_count; ++k) {
        const uint32_t i = (ch.scan_cursor + k) % e->scan_count;
        if (e->scan[i].channel == c && e->scan[i].outcome == ECUIO_SCAN_UNASKED) {
            ch.scan_cursor = (i + 1) % e->scan_count;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* A found DID joins channel c's rotation, room permitting */
inline void ecuio_engine_scan_poll(ecuio_request_engine* e, uint32_t c,
                                   ecuio_scan_entry& s)
{
    const uint8_t req[3] = {0x22, static_cast<uint8_t>(s.did >> 8),
                            static_cast<uint8_t>(s.did)};
    const ecuio_engine_add_result r = ecuio_engine_add(e, c, req, 3);
    s.polled = (r == ecuio_engine_add_result::added ||
                r == ecuio_engine_add_result::duplicate) ? 1 : 0;
}

/* Channel c's scan has asked every entry: the ones whose wait ran out go
 * once more, since a reply lost on a busy bus or adapter isn't the
 * module's silence. A channel that answered no entry at all is silent,
 * and isn't asked again; an entry asked again stands as it settles. */
inline void ecuio_engine_scan_retry(ecuio_request_engine* e, uint32_t c)
{
    bool answered = false;
    uint32_t lost = 0;
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        const ecuio_scan_entry& s = e->scan[i];
        if (s.channel != c) {
            continue;
        }
        answered = answered || s.outcome == ECUIO_SCAN_FOUND ||
                   s.outcome == ECUIO_SCAN_REFUSED;
        lost += (s.outcome == ECUIO_SCAN_UNANSWERED && s.retried == 0) ? 1 : 0;
    }
    if (!answered || lost == 0) {
        return;
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        if (s.channel == c && s.outcome == ECUIO_SCAN_UNANSWERED &&
            s.retried == 0) {
            s.outcome = ECUIO_SCAN_UNASKED;
            s.retried = 1;
        }
    }
    e->chans[c].scan_pending += lost;
}

/* Settles the scan entry channel c has out: found (and in the rotation
 * while the table has room), refused with its code, or unanswered. */
inline void ecuio_engine_scan_settle(ecuio_request_engine* e, uint32_t c,
                                     uint8_t outcome, uint8_t nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_scan_entry& s = e->scan[ch.scan_index];
    ch.scanning = false;
    s.outcome = outcome;
    s.nrc = nrc;
    if (outcome == ECUIO_SCAN_FOUND) {
        ecuio_engine_scan_poll(e, c, s);
    }
    if (ch.scan_pending > 0) {
        --ch.scan_pending;
        if (ch.scan_pending == 0) {
            ecuio_engine_scan_retry(e, c);
        }
    }
    ++ch.generation;
}

/* A positive reply on channel c naming a scanned DID whose wait ended
 * answers it, however late: a long reply can take longer than the wait
 * (the test car's 263-byte DA9E through the VX), and the
 * adapter hands it over only whole. The DID is found and polled, so the
 * scan completes; one waiting to be asked again needn't be, unless the
 * one-shot holds it (a send that failed), which its own ask settles.
 * Called only for a reply that answers nothing else. */
inline bool ecuio_engine_scan_late(ecuio_request_engine* e, uint32_t c,
                                   const uint8_t* data, uint16_t len)
{
    if (len < 3 || data[0] != 0x62) {
        return false;
    }
    const uint16_t did = static_cast<uint16_t>((data[1] << 8) | data[2]);
    ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        const bool held = ch.scanning && ch.scan_index == i;
        const bool waiting =
            s.outcome == ECUIO_SCAN_UNASKED && s.retried && !held;
        if (s.channel == c && s.did == did &&
            (s.outcome == ECUIO_SCAN_UNANSWERED || waiting)) {
            s.outcome = ECUIO_SCAN_FOUND;
            ecuio_engine_scan_poll(e, c, s);
            if (waiting && ch.scan_pending > 0) {
                --ch.scan_pending;
                if (ch.scan_pending == 0) {
                    ecuio_engine_scan_retry(e, c);
                }
            }
            ++ch.generation;
            return true;
        }
    }
    return false;
}

/* Asks every unanswered scan entry, identification item and diagnostics
 * item again: the driver's call when the link returns to ok, as the walk
 * resumes. Silence wasn't an answer. A channel mid-clear keeps its
 * report as the caller saved it until the clear ends. */
inline void ecuio_engine_rescan(ecuio_request_engine* e)
{
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        s.retried = 0; /* the link's return starts a round of its own */
        if (s.outcome == ECUIO_SCAN_UNANSWERED) {
            s.outcome = ECUIO_SCAN_UNASKED;
            ++e->chans[s.channel].scan_pending;
        }
    }
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        ch.check_due = false; /* the link is back: no check is owed */
        if (ch.unpacked) {
            ch.unpacked = false;
            ch.pack_failures = 0;
            ++ch.generation;
        }
        /* ADR-0060: a link that went quiet may have outlasted S3, and an
         * unanswered 10 03 is asked again; a refusal stands */
        if (e->session != 0 &&
            (ch.session_step == ECUIO_SESSION_STATE_ENTERED ||
             ch.session_step == ECUIO_SESSION_STATE_UNANSWERED)) {
            if (ch.session_step == ECUIO_SESSION_STATE_ENTERED) {
                ecuio_engine_count16(ch.session_lost);
            }
            ch.session_step = ECUIO_SESSION_STATE_ENTERING;
            ++ch.generation;
        }
        for (uint32_t i = 0; i < ch.ident_count; ++i) {
            if (ch.ident[i].outcome == ECUIO_IDENTITY_UNANSWERED) {
                ch.ident[i].outcome = ECUIO_IDENTITY_UNASKED;
                ++ch.ident_pending;
            }
        }
        for (uint32_t i = 0;
             i < ch.diag_count && ch.clear_step == ECUIO_CLEAR_STEP_NONE; ++i) {
            if (ch.diag_items[i].outcome == ECUIO_DIAG_UNANSWERED) {
                ch.diag_items[i].outcome = ECUIO_DIAG_UNASKED;
                ++ch.diag_pending;
                ch.diag.flags &= ~ECUIO_DIAG_COMPLETE;
            }
        }
    }
}

#endif
