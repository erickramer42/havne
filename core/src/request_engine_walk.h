// File/layer: core/src/request_engine_walk.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// The supported-PID walk (ADR-0015 section 4) and the channel's
// support report.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, RequestTurnsTest, PidSupportTest
//          (test_obd2)

#ifndef ECUIO_REQUEST_ENGINE_WALK_H
#define ECUIO_REQUEST_ENGINE_WALK_H

#include "request_engine_rotation.h"

/* Ends channel c's walk where it stands, answered or not, and drops each
 * of its Service 01 requests whose PID the answers mark unsupported.
 * PIDs it didn't reach stay: silence isn't an answer. Then judges the
 * duplicate pairs on what the answers listed. */
inline void ecuio_engine_end_walk(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.walking = false;
    ch.walk_ended = true;
    ++ch.generation;
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (r.channel == c && r.dropped == 0 && r.bytes[0] == 0x01 &&
            r.bytes[1] != 0 && ecuio_engine_pid_bit(ch.known, r.bytes[1]) &&
            !ecuio_engine_pid_bit(ch.supported, r.bytes[1])) {
            ecuio_engine_drop(e, i, ECUIO_DROP_BITMAP, 0);
        }
    }
    ecuio_engine_judge_duplicates(e, c);
}

/* A bitmap reply to the walk on channel c: 41 <PID> b0 b1 b2 b3, PID
 * 0x20 * i covering PIDs 0x20 * i + 1 to 0x20 * i + 0x20, the last of
 * them the next bitmap. A clear last bit ends the chain, and with it
 * says every later PID is unsupported. */
inline void ecuio_engine_take_bitmap(ecuio_request_engine* e, uint32_t c,
                                     const uint8_t* data, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.walk_stray = false;
    ch.walk_reasked = false;
    const uint32_t range = ch.walk.bytes[1] / 0x20u;
    for (uint32_t b = 0; b < 4; ++b) {
        ch.known[range * 4 + b] = 0xFF;
        ch.supported[range * 4 + b] = data[2 + b];
    }
    ch.ranges = static_cast<uint8_t>(ch.ranges | (1u << range));
    const bool next = (data[5] & 0x01u) != 0;
    if (!next) {
        for (uint32_t b = (range + 1) * 4; b < sizeof(ch.known); ++b) {
            ch.known[b] = 0xFF;
        }
    }
    if (!next || range == 7) {
        ch.walked = true;
        ecuio_engine_end_walk(e, c);
        return;
    }
    ch.walk.bytes[1] = static_cast<uint8_t>((range + 1) * 0x20u);
    if (ch.walk_timed && now_us >= ch.walk_until_us) {
        ecuio_engine_end_walk(e, c);
    }
}

/* Section 4: turns the probe on and starts the walk on every channel that has
 * Service 01 requests in its rotation and whose chain isn't answered to
 * its end yet. A walk resumes at the first bitmap it hasn't had, so the
 * one at connect and each after a recovery (the driver's call) together
 * ask each bitmap until it's answered. Each channel's budget starts at
 * its first bitmap (ecuio_engine_due), so other channels' turns before
 * it never count. A channel whose one-shot is taken by an identification
 * item or a scanned DID keeps it: the walk waits in ch.walk until that
 * one has settled. */
inline void ecuio_engine_probe(ecuio_request_engine* e)
{
    e->probe = true;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        ch.walk_timed = false; /* a walk under way starts its second again */
        if (ch.walked || ch.walking) {
            continue;
        }
        bool asks_pids = false;
        for (uint32_t i = 0; i < e->count && !asks_pids; ++i) {
            const ecuio_request& r = e->requests[i];
            asks_pids = r.channel == c && r.dropped == 0 && r.bytes[0] == 0x01;
        }
        if (!asks_pids) {
            continue;
        }
        uint32_t range = 0;
        while (range < 8 && (ch.ranges & (1u << range)) != 0) {
            ++range;
        }
        if (range == 8) {
            continue;   /* every bitmap answered, so walked is set */
        }
        ch.walk = ecuio_request{};
        ch.walk.bytes[0] = 0x01;
        ch.walk.bytes[1] = static_cast<uint8_t>(range * 0x20u);
        ch.walk.len = 2;
        ch.walk.channel = static_cast<uint8_t>(c);
        ch.walk_stray = false;
        ch.walk_reasked = false;
        ch.walking = true;
        ch.probed = true;
    }
}

/* The session asked of channel ch: none for one never asked (another
 * channel's conversation entered alone); a close's 10 01 leaves it asked */
inline uint8_t ecuio_engine_session_asked(const ecuio_request_engine* e,
                                          const ecuio_engine_channel& ch)
{
    const bool asked = ch.session_step != ECUIO_SESSION_STATE_NOT_ASKED ||
                       ch.session_entries != 0;
    return asked ? e->session : uint8_t{0};
}

/* What channel c has learned, for ecuio_session_get_channel_support;
 * the driver adds the channel's rx_id. */
inline void ecuio_engine_support(const ecuio_request_engine* e, uint32_t c,
                                 ecuio_channel_support_t* out)
{
    const ecuio_engine_channel& ch = e->chans[c];
    *out = ecuio_channel_support_t{};
    /* PROBED says a walk has ended, answered or not: a first walk under
     * way (a retry may be due) reports none, a resumed one keeps the
     * last one's word. */
    const bool ended = !ch.walking || ch.walk_ended;
    out->flags = ((ch.probed && ended) ? ECUIO_SUPPORT_PROBED : 0u) |
                 (ch.walked ? ECUIO_SUPPORT_COMPLETE : 0u) |
                 (ch.packed ? ECUIO_SUPPORT_PACKED : 0u) |
                 (ch.unpacked ? ECUIO_SUPPORT_UNPACKED : 0u);
    out->generation = ch.generation;
    out->ranges = ch.ranges;
    for (uint32_t b = 0; b < sizeof(out->pid_bitmap); ++b) {
        out->pid_bitmap[b] = ch.supported[b];
    }
    out->pids_supported = ecuio_supported_pid_count(ch.supported);
    out->pids_through = ecuio_pids_through(ch.ranges);
    /* The list takes the refused drops first, then the bitmap's, each
     * in set order: a bitmap leaves out many PIDs on most cars, and its
     * drops would otherwise fill the list before the refusals the front
     * ends name (cli/refused_signals.h) */
    uint32_t listed = 0;
    for (int bitmap = 0; bitmap < 2; ++bitmap) {
        for (uint32_t i = 0; i < e->count; ++i) {
            const ecuio_request& r = e->requests[i];
            if (r.channel != c) {
                continue;
            }
            if (bitmap == 0) {
                ++out->requests;
                out->dropped += r.dropped != 0 ? 1u : 0u;
            }
            if (r.dropped == 0 ||
                (r.dropped == ECUIO_DROP_BITMAP) != (bitmap == 1) ||
                listed >= ECUIO_SUPPORT_MAX_DROPPED) {
                continue;
            }
            ecuio_dropped_request_t& d = out->dropped_list[listed++];
            for (uint8_t k = 0; k < r.len; ++k) {
                d.request[k] = r.bytes[k];
            }
            d.length = r.len;
            d.reason = r.dropped;
            d.nrc = r.nrc;
        }
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        const ecuio_scan_entry& s = e->scan[i];
        if (s.channel != c) {
            continue;
        }
        ++out->scan_total;
        if (s.outcome == ECUIO_SCAN_FOUND) {
            ++out->scan_done;
            ++out->scan_found;
            out->scan_polled = static_cast<uint16_t>(out->scan_polled + s.polled);
        } else if (s.outcome == ECUIO_SCAN_REFUSED) {
            ++out->scan_done;
            ++out->scan_refused;
        }
    }
    if (out->scan_total > 0 && out->scan_done == out->scan_total) {
        out->flags |= ECUIO_SUPPORT_SCAN_COMPLETE;
    }
    const bool extended =
        e->session != 0 && ch.session_step == ECUIO_SESSION_STATE_ENTERED;
    out->session_asked = ecuio_engine_session_asked(e, ch);
    out->session = extended ? e->session : ECUIO_SESSION_DEFAULT;
    out->session_state =
        e->session != 0 ? ch.session_step : ECUIO_SESSION_STATE_NOT_ASKED;
    out->session_nrc = ch.session_nrc;
    out->session_entries = ch.session_entries;
    out->session_lost = ch.session_lost;
    if (extended) {
        out->flags |= ECUIO_SUPPORT_EXTENDED;
    }
}

#endif
