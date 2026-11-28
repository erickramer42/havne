// File/layer: core/src/request_operation.h | internal, C++17, Qt-free,
//             every platform, header-only
// Testing: UNIT: SessionOperationTest (test_core); HARNESS: the transports'
//          operation cases
//
// A requesting worker's half of a run (operation_session.h): the calls
// request_conversation.h makes for it on the worker's thread, beside a
// bound module's conversation. No worker's source names a run.
//
//   ecuio_operation::on_frame  after the engine took a frame: the answer
//                              that settled the run's talk goes to the
//                              machine as it came, so a chunk is copied
//                              once, into the sink's ring
//   ecuio_operation::on_pass   once a pass: an armed run taken, the
//                              cancel, the talk's settle, the sink's
//                              verdict, the machine's next request
//                              offered, the end
//   ecuio_operation::wait_us   bounds the worker's wait
//   ecuio_operation::on_close  at the worker's close
//
// What goes on the wire for a run:
//   - Every request is the machine's (operation_run.h), offered as a
//     talk: an identification read as a plain one, on the guest's own
//     list; a memory read as kind `read`, which the door holds to tier
//     3's read ceiling and which nothing else offers.
//   - While a run is under way the engine holds everything else
//     (ecuio_request_engine::operation_hold): each module's session and
//     its keep-alive go on, and a key.
//   - The session and the unlock are the conversation's: the entry is
//     the engine's own step, and an owed word goes through
//     ecuio_conversation_unlock, as an unlock call's does. A word owed
//     and never given ends uncounted, so its mark is settled.
//
// The worker allocates nothing here and waits for nothing: the sink's
// ring takes a chunk or the machine holds its next request.

#ifndef ECUIO_REQUEST_OPERATION_H
#define ECUIO_REQUEST_OPERATION_H

#include "request_flash.h"

namespace ecuio_operation {

/* One pass of a run that is the worker's */
inline void drive(ecuio_session_t* s, session_run& o, ecuio_request_engine* e,
                  int64_t now_us)
{
    run& r = o.r;
    ecuio_engine_talk_state& t = e->chans[o.channel].talk;
    if (o.cancel.load(std::memory_order_relaxed)) {
        cancel(r);
    }
    take_settled(o, e->chans[o.talk_at].talk);
    own_settle(o, e, now_us);
    /* The writer's word, over the machine's complete too: a difference
     * or a file's failure ends the run before its next request */
    const sink_state w = o.sink->state();
    if (!o.closed_sink && w != sink_state::writing &&
        (r.at != run_stage::done || r.result == run_end::complete)) {
        o.orphan = o.out && !o.fed;
        stop(r, w == sink_state::differ ? run_end::differ : run_end::file);
    }
    drop_check(o, e, now_us);
    battery_step(s, o, now_us);
    if ((o.v.refused_low || o.v.refused_high) && r.at != run_stage::done) {
        o.orphan = o.out && !o.fed;
        stop(r, o.v.refused_high && !o.v.refused_low ? run_end::voltage_high
                                                     : run_end::voltage_low);
    }
    if (o.checking && o.v.first && r.at != run_stage::done) {
        check_step(s, o, e, now_us);
    }
    functional_step(s, o, e, now_us);
    while (!o.checking && r.at != run_stage::done && !o.out && !o.quiet_out &&
           !t.offered && !t.out && t.result.end == ecuio_talk_end::none) {
        run_request q;
        const run_action a = next(r, q, *o.sink);
        if (a == run_action::send && q.functional) {
            aside(s, o, e, q.bytes[0], q.bytes[1]);
            o.quiet_out = true;
        } else if (a == run_action::send) {
            offer(o, e, q, now_us);
        } else if (a == run_action::leave && own_leave(o, e, now_us)) {
            left(r);
        } else if (a == run_action::enter) {
            enter(s, o, e, now_us);
            if (r.at == run_stage::enter) {
                break; /* the entry or the attempt runs */
            }
        } else {
            break; /* the leave runs; hold: the ring's room first; done */
        }
    }
    log_status(s, o);
    if (r.at == run_stage::done) {
        finish(s, o, e, now_us);
    }
}

/* After the engine took a frame on channel c */
inline void on_frame(ecuio_session_t* s, const ecuio_request_engine* e,
                     uint32_t c, const uint8_t* bytes, uint32_t len)
{
    session_run* o = s->operation_live.load(std::memory_order_acquire);
    if (o == nullptr || c != o->talk_at || len == 0 ||
        o->stage.load(std::memory_order_acquire) != kStageRunning || !o->out ||
        o->fed || o->orphan || o->own_out) {
        return;
    }
    const ecuio_engine_channel& ch = e->chans[c];
    const ecuio_talk_end end = ch.talk.result.end;
    if (o->checking) {
        if (end == ecuio_talk_end::answered || end == ecuio_talk_end::refused) {
            o->fed = true;
            check_reply(o->k, bytes, len);
        }
        return;
    }
    if (o->r.at == run_stage::flash) {
        if (end == ecuio_talk_end::answered || end == ecuio_talk_end::refused) {
            o->fed = true;
            (void)flash_reply(o->f, bytes, len);
        } else if (ch.talk.out && len == 3 &&
                   bytes[0] == ECUIO_NEGATIVE_RESPONSE && bytes[1] == o->sid &&
                   bytes[2] == ECUIO_NRC_RESPONSE_PENDING) {
            (void)flash_reply(o->f, bytes, len);
        }
        return;
    }
    if (end == ecuio_talk_end::answered || end == ecuio_talk_end::refused) {
        o->fed = true;
        const bool left =
            end == ecuio_talk_end::refused && len == 3 &&
            (bytes[2] == ECUIO_NRC_NOT_IN_ACTIVE_SESSION ||
             bytes[2] == ECUIO_NRC_SUBFUNCTION_NOT_IN_ACTIVE_SESSION);
        if (left && !may_go_unread(o->r)) {
            stop(o->r, run_end::session_lost, bytes[2]);
        } else {
            (void)reply(o->r, bytes, len, 0, *o->sink);
        }
        return;
    }
    /* A pending answer keeps the talk out: the machine bounds the wait.
     * The engine put the talk's deadline one pending wait after this
     * frame, which is how the frame's time is known here. */
    if (ch.talk.out && len == 3 && bytes[0] == ECUIO_NEGATIVE_RESPONSE &&
        bytes[1] == o->sid && bytes[2] == ECUIO_NRC_RESPONSE_PENDING) {
        const int64_t waited_us =
            ch.deadline_us - ECUIO_ENGINE_PENDING_WAIT_US - ch.sent_us;
        const uint32_t waited_ms =
            waited_us > 0 ? static_cast<uint32_t>(waited_us / 1000) : 0;
        (void)reply(o->r, bytes, len, waited_ms, *o->sink);
    }
}

/* Once a pass, after the conversations' own step */
inline void on_pass(ecuio_session_t* s, ecuio_request_engine* e, int64_t now_us)
{
    session_run* o = s->operation_live.load(std::memory_order_acquire);
    if (o == nullptr) {
        return;
    }
    uint8_t stage = o->stage.load(std::memory_order_acquire);
    if (stage == kStageArmed) {
        begin(o->r, *o->p, e->write_max);
        flash_begin(o->f, *o->p, e->write_max,
                    o->current.empty() ? nullptr : o->current.data());
        o->pause_until_us = 0;
        o->journal_closed = false;
        check_begin(o->k, check_engine_of(e, o->channel), o->channel, false);
        o->checking = true;
        o->talk_at = o->channel;
        o->own = own_step::none;
        o->own_out = false;
        o->own_end = ecuio_talk_result{};
        o->v = volts{};
        o->v.write = o->write;
        o->v.ram_only = o->p->has_kernel;
        o->volts_asked = 0;
        o->volts_at_us = 0;
        o->volts_step = 0;
        o->quiet_out = false;
        o->keep_at_us = 0;
        o->began_us = now_us;
        e->operation_hold = true;
        record_line(s, *o, ECUIO_OPERATION_END_NONE);
        stage = kStageRunning;
        o->stage.store(stage, std::memory_order_release);
    }
    if (stage != kStageRunning) {
        return;
    }
    if (o->p->has_flash) {
        drive_flash(s, *o, e, now_us);
    } else {
        drive(s, *o, e, now_us);
    }
    if (o->stage.load(std::memory_order_relaxed) == kStageRunning) {
        publish(s, *o);
    }
}

/* The wait until a run next has a step to take, from now (INT64_MAX for
 * none): now for an armed run and a settled talk, never while its talk
 * is out, else the poll */
inline int64_t wait_us(ecuio_session_t* s, const ecuio_request_engine* e,
                       int64_t now_us)
{
    const session_run* o = s->operation_live.load(std::memory_order_acquire);
    if (o == nullptr) {
        return INT64_MAX;
    }
    const uint8_t stage = o->stage.load(std::memory_order_acquire);
    if (stage == kStageArmed) {
        return 0;
    }
    if (stage != kStageRunning) {
        return INT64_MAX;
    }
    const int64_t pause = o->pause_until_us == 0 ? INT64_MAX
                          : o->pause_until_us > now_us
                              ? o->pause_until_us - now_us
                              : 0;
    const int64_t keep =
        (std::min)(pause, o->keep_at_us == 0       ? INT64_MAX
                          : o->keep_at_us > now_us ? o->keep_at_us - now_us
                                                   : 0);
    if (o->out) {
        return e->chans[o->talk_at].talk.result.end != ecuio_talk_end::none
                   ? 0
                   : keep;
    }
    return (std::min)(kPollUs, keep);
}

/* At the worker's close: a word owed and never given, an armed run's
 * too, ends uncounted, so the conversations' end settles it. The run's
 * own end is the close's (ecuio_operation_closed), once the worker has
 * joined. */
inline void on_close(ecuio_session_t* s, ecuio_request_engine* e)
{
    session_run* o = s->operation_live.load(std::memory_order_acquire);
    const uint8_t stage =
        o == nullptr ? kStageIdle : o->stage.load(std::memory_order_acquire);
    if (stage != kStageArmed && stage != kStageRunning) {
        return;
    }
    e->operation_hold = false;
    if (ecuio_bound_channel* b = ecuio_bound_channel_of(s, o->channel)) {
        drop_word(*b, *o);
    }
}

} // namespace ecuio_operation

#endif /* ECUIO_REQUEST_OPERATION_H */
