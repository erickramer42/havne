// File/layer: core/src/request_engine_quiet.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// A quiet module (ADR-0065's appendix of 2026-11-21): one that has stopped
// answering is asked one row at a time on a backoff while another module
// answers, so its silence costs the others no wait on every lap.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestQuietTest (test_obd2); MODEL: PacingTest
//          (test_pacing); HARNESS: every transport's suites
//
// The rule, one for every requesting transport, read through
// ecuio_engine_due, ecuio_engine_next_event_us and ecuio_engine_turn_next_us:
//   1. A channel counts its waits that run out in a row (a pack's rows
//      each, a one-shot's too); any frame from it ends the run. It is quiet
//      at a run of ECUIO_ENGINE_QUIET_RUN when no frame has come from it
//      since the start or the last rescan (the module that isn't there);
//      once heard, at a run as long as its rotation (that many at least)
//      with no frame from it for ECUIO_ENGINE_QUIET_SILENCE_US, so rows a
//      module never answers among rows it does, and a module losing
//      replies on a busy bus, are no quiet.
//   2. It is held back only while another channel is live (not quiet, with
//      rows in its rotation or a request out). With none live nothing
//      changes: a session of one module, or a car with every module silent,
//      asks, times out and judges its link as it would.
//   3. Held, its rotation gives one row, never a pack, each time its step
//      has passed (ECUIO_ENGINE_QUIET_STEPS_US from going quiet, the last
//      repeating), and two held channels' rows go
//      ECUIO_ENGINE_QUIET_SPACING_US apart. What isn't held: a session's
//      request, a key or talk, the walk, a clear, the unpicked check, a
//      one-shot already in the slot.
//   4. Held, its unasked identification, diagnostics and scan items settle
//      unanswered unasked, as each would after its own wait, so the
//      reports complete.
//   5. Any frame from it ends the quiet state, as a rescan would for its
//      channel: its rotation is asked again whole, its rows' waits run out
//      are judged by their defs again (ecuio_engine_forget_ran_out) and the
//      items settled unasked are asked again (ecuio_engine_ask_again).
//      ecuio_engine_rescan ends it on every channel.

#ifndef ECUIO_REQUEST_ENGINE_QUIET_H
#define ECUIO_REQUEST_ENGINE_QUIET_H

#include "request_engine_diag.h"
#include "request_engine_scan.h"

/* Rule 1's run for a channel never heard, and the least for one heard;
 * the silence a heard channel's run needs beside it */
constexpr uint32_t ECUIO_ENGINE_QUIET_RUN = 2;
constexpr int64_t ECUIO_ENGINE_QUIET_SILENCE_US = 2000000;

/* Rule 3: the time from going quiet to the first row, and from each row
 * to the next; past the list, its last. The last is the unpicked check's
 * interval (request_setup.h asserts it). */
constexpr int64_t ECUIO_ENGINE_QUIET_STEPS_US[] = {500000, 1000000, 2000000,
                                                   4000000, 5000000};
constexpr uint32_t ECUIO_ENGINE_QUIET_STEP_COUNT =
    sizeof(ECUIO_ENGINE_QUIET_STEPS_US) /
    sizeof(ECUIO_ENGINE_QUIET_STEPS_US[0]);
constexpr int64_t ECUIO_ENGINE_QUIET_CAP_US =
    ECUIO_ENGINE_QUIET_STEPS_US[ECUIO_ENGINE_QUIET_STEP_COUNT - 1];
constexpr int64_t ECUIO_ENGINE_QUIET_SPACING_US = 250000;

/* A wait on channel c ran out at now_us, rows of them (a pack's): its run
 * grows, and at rule 1's run the channel goes quiet, its first row a step
 * away */
inline void ecuio_engine_quiet_expired(ecuio_request_engine* e, uint32_t c,
                                       uint32_t rows, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.quiet_run =
        ch.quiet_run > UINT32_MAX - rows ? UINT32_MAX : ch.quiet_run + rows;
    if (ch.quiet) {
        return;
    }
    const uint32_t run = ch.heard && ch.requests > ECUIO_ENGINE_QUIET_RUN
                             ? ch.requests
                             : ECUIO_ENGINE_QUIET_RUN;
    if (ch.quiet_run < run ||
        (ch.heard && now_us - ch.heard_us < ECUIO_ENGINE_QUIET_SILENCE_US)) {
        return;
    }
    ch.quiet = true;
    ch.quiet_asks = 0;
    ch.quiet_next_us = now_us + ECUIO_ENGINE_QUIET_STEPS_US[0];
}

/* Whether channel ch is live (rule 2): not quiet, with rows in its
 * rotation or a request out */
inline bool ecuio_engine_quiet_live(const ecuio_engine_channel& ch)
{
    return !ch.quiet &&
           (ch.requests > 0 || ch.outstanding != ECUIO_ENGINE_NONE);
}

/* Whether channel ch, one of e's, is held: quiet while another is live */
inline bool ecuio_engine_quiet_held(const ecuio_request_engine* e,
                                    const ecuio_engine_channel& ch)
{
    if (!ch.quiet) {
        return false;
    }
    for (uint32_t k = 0; k < e->chan_count; ++k) {
        if (&e->chans[k] != &ch && ecuio_engine_quiet_live(e->chans[k])) {
            return true;
        }
    }
    return false;
}

/* When held channel ch's next row may go: its step passed, and the
 * spacing since any held channel's last */
inline int64_t ecuio_engine_quiet_due_us(const ecuio_request_engine* e,
                                         const ecuio_engine_channel& ch)
{
    const int64_t spaced = e->quiet_ask_us + ECUIO_ENGINE_QUIET_SPACING_US;
    return ch.quiet_next_us > spaced ? ch.quiet_next_us : spaced;
}

/* Held channel c asked a row at now_us: its next step starts */
inline void ecuio_engine_quiet_asked(ecuio_request_engine* e, uint32_t c,
                                     int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.quiet_asks < ECUIO_ENGINE_QUIET_STEP_COUNT) {
        ++ch.quiet_asks;
    }
    const uint32_t step = ch.quiet_asks < ECUIO_ENGINE_QUIET_STEP_COUNT
                              ? ch.quiet_asks
                              : ECUIO_ENGINE_QUIET_STEP_COUNT - 1;
    ch.quiet_next_us = now_us + ECUIO_ENGINE_QUIET_STEPS_US[step];
    e->quiet_ask_us = now_us;
}

/* Rule 4 on held channel c: its unasked items settle unanswered, a
 * Service 01 item the bitmaps state unsupported as unsupported. Called
 * with nothing in the one-shot. */
inline void ecuio_engine_quiet_settle(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.ident_pending == 0 && ch.diag_pending == 0 && ch.scan_pending == 0) {
        return;
    }
    ch.quiet_settled = true;
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        if (ch.ident[i].outcome == ECUIO_IDENTITY_UNASKED) {
            ch.ident_index = i;
            ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_UNANSWERED, 0,
                                      nullptr, 0);
        }
    }
    ch.ident_pending = 0;
    ecuio_engine_diag_skip_unsupported(e, c);
    for (int32_t i = ecuio_engine_diag_next(e, c); i >= 0;
         i = ecuio_engine_diag_next(e, c)) {
        ch.diag_index = static_cast<uint32_t>(i);
        ecuio_engine_diag_settle(e, c, ECUIO_DIAG_UNANSWERED, 0, nullptr, 0);
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        if (s.channel == c && s.outcome == ECUIO_SCAN_UNASKED) {
            s.outcome = ECUIO_SCAN_UNANSWERED;
        }
    }
    if (ch.scan_pending > 0) {
        ch.scan_pending = 0;
        ++ch.generation;
    }
}

/* A frame from channel c at now_us (rules 1 and 5): heard, its run
 * ended, and a quiet channel back in its rotation, its rows' waits run
 * out judged by their defs again (it was asleep, not leaving them
 * unanswered) and the items settled while it was held asked again */
inline void ecuio_engine_quiet_heard(ecuio_request_engine* e, uint32_t c,
                                     int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.heard = true;
    ch.heard_us = now_us;
    ch.quiet_run = 0;
    if (!ch.quiet) {
        return;
    }
    ch.quiet = false;
    ecuio_engine_forget_ran_out(e, c);
    if (ch.quiet_settled) {
        ch.quiet_settled = false;
        ecuio_engine_ask_again(e, c);
    }
}

#endif
