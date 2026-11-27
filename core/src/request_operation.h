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

#include "conversation_bind.h"
#include "host_awake.h"
#include "operation_session.h"
#include "recorder.h"
#include "request_engine.h"
#include "session.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ecuio_operation {

static_assert(ECUIO_STEP_SESSION_EXTENDED == ECUIO_SESSION_EXTENDED,
              "a step's extended session is the engine's own entry");

inline uint32_t end_code(run_end e)
{
    switch (e) {
    case run_end::canceled:
        return ECUIO_OPERATION_END_CANCELED;
    case run_end::refused:
        return ECUIO_OPERATION_END_REFUSED;
    case run_end::no_answer:
        return ECUIO_OPERATION_END_NO_ANSWER;
    case run_end::bad_reply:
        return ECUIO_OPERATION_END_BAD_REPLY;
    case run_end::differ:
        return ECUIO_OPERATION_END_DIFFER;
    case run_end::identity_changed:
        return ECUIO_OPERATION_END_IDENTITY_CHANGED;
    case run_end::session_lost:
        return ECUIO_OPERATION_END_SESSION_LOST;
    case run_end::unlock_refused:
        return ECUIO_OPERATION_END_UNLOCK_REFUSED;
    case run_end::link_lost:
        return ECUIO_OPERATION_END_LINK_LOST;
    case run_end::engine_running:
        return ECUIO_OPERATION_END_ENGINE_RUNNING;
    case run_end::moving:
        return ECUIO_OPERATION_END_MOVING;
    case run_end::voltage_low:
        return ECUIO_OPERATION_END_VOLTAGE_LOW;
    case run_end::voltage_high:
        return ECUIO_OPERATION_END_VOLTAGE_HIGH;
    default:
        return ECUIO_OPERATION_END_FILE;
    }
}

/* An end in words for a person; a file's are the sink's own */
inline const char* end_words(uint32_t end)
{
    switch (end) {
    case ECUIO_OPERATION_END_SAVED:
        return "";
    case ECUIO_OPERATION_END_CANCELED:
        return "the read was stopped: nothing was kept";
    case ECUIO_OPERATION_END_REFUSED:
        return "the module refused a request: nothing was kept";
    case ECUIO_OPERATION_END_NO_ANSWER:
        return "the module stopped answering: nothing was kept";
    case ECUIO_OPERATION_END_BAD_REPLY:
        return "the module's answer wasn't what its request asks for: "
               "nothing was kept";
    case ECUIO_OPERATION_END_DIFFER:
        return "the two readings differ: nothing was kept";
    case ECUIO_OPERATION_END_IDENTITY_CHANGED:
        return "the module's identification changed during the read: "
               "nothing was kept";
    case ECUIO_OPERATION_END_SESSION_LOST:
        return "the module left its session: nothing was kept";
    case ECUIO_OPERATION_END_UNLOCK_REFUSED:
        return "the module's level stayed locked: nothing was read";
    case ECUIO_OPERATION_END_LINK_LOST:
        return "a request could not be sent: nothing was kept";
    default:
        return "the copy could not be written";
    }
}

/* An end's words with what the checks read */
inline void words_of(const session_run& o, uint32_t end, char* out, size_t cap)
{
    if (end == ECUIO_OPERATION_END_ENGINE_RUNNING) {
        std::snprintf(out, cap,
                      "the engine is running (%u rpm): switch it off by the "
                      "key; nothing was read",
                      static_cast<unsigned>(o.k.rpm));
    } else if (end == ECUIO_OPERATION_END_MOVING) {
        std::snprintf(out, cap,
                      "the vehicle is moving (%u km/h): nothing was read",
                      static_cast<unsigned>(o.k.kmh));
    } else if (end == ECUIO_OPERATION_END_VOLTAGE_LOW && o.v.write &&
               !o.v.has) {
        std::snprintf(out, cap,
                      "no battery reading at the adapter: a write "
                      "needs one; nothing was read");
    } else if (end == ECUIO_OPERATION_END_VOLTAGE_LOW && o.v.write) {
        std::snprintf(out, cap,
                      "the battery reads %u.%u V at the adapter, under the "
                      "%u.%u V a write needs: connect a charger; nothing was "
                      "read",
                      static_cast<unsigned>(o.v.mv / 1000),
                      static_cast<unsigned>(o.v.mv % 1000 / 100),
                      static_cast<unsigned>(kWriteStartFloorMv / 1000),
                      static_cast<unsigned>(kWriteStartFloorMv % 1000 / 100));
    } else if (end == ECUIO_OPERATION_END_VOLTAGE_HIGH) {
        std::snprintf(out, cap,
                      "the battery reads %u.%u V at the adapter: set the "
                      "charger to its power-supply mode (24 V systems aren't "
                      "supported); nothing was read",
                      static_cast<unsigned>(o.v.mv / 1000),
                      static_cast<unsigned>(o.v.mv % 1000 / 100));
    } else if (end == ECUIO_OPERATION_END_VOLTAGE_LOW) {
        std::snprintf(out, cap,
                      "the battery fell to %u.%u V at the adapter: the copy "
                      "was incomplete, nothing was kept",
                      static_cast<unsigned>(o.v.min_mv / 1000),
                      static_cast<unsigned>(o.v.min_mv % 1000 / 100));
    } else {
        std::snprintf(out, cap, "%s", end_words(end));
    }
}

/* The checks' engine module: another channel that hears, never a
 * functional one; UINT32_MAX for none */
inline uint32_t check_engine_of(const ecuio_request_engine* e, uint32_t target)
{
    const uint32_t m = ecuio_engine_rpm_module_of(e);
    return m < e->chan_count && m != target && !e->chans[m].functional &&
                   ecuio_engine_module_hears(e->chans[m])
               ? m
               : UINT32_MAX;
}

/* The phase the machine's place reads as */
inline uint32_t phase_of(const session_run& o)
{
    if (o.checking) {
        return ECUIO_OPERATION_PHASE_PRECONDITIONS;
    }
    switch (o.r.at) {
    case run_stage::enter:
        return ECUIO_OPERATION_PHASE_UNLOCKING;
    case run_stage::done:
        return ECUIO_OPERATION_PHASE_SAVING;
    default:
        return o.r.pass == 0 ? ECUIO_OPERATION_PHASE_READING
                             : ECUIO_OPERATION_PHASE_COMPARING;
    }
}

/* What the run has read, as the view's checks */
inline uint32_t checks_of(const session_run& o)
{
    const volts& v = o.v;
    return o.start_checks | (o.k.has_rpm ? ECUIO_OPERATION_CHECK_RPM : 0u) |
           (o.k.has_kmh ? ECUIO_OPERATION_CHECK_KMH : 0u) |
           (v.has ? ECUIO_OPERATION_CHECK_VOLTS : 0u) |
           (v.from_ecu ? ECUIO_OPERATION_CHECK_VOLTS_FROM_ECU : 0u) |
           (v.low_warn ? ECUIO_OPERATION_CHECK_VOLTS_LOW : 0u) |
           (v.high_warn ? ECUIO_OPERATION_CHECK_VOLTS_HIGH : 0u);
}

/* The view follows the machine: taken under the lock only when it moved.
 * The worker alone writes the view while the run is its own, so it reads
 * its own fields without the lock. */
inline void publish(ecuio_session_t* s, session_run& o)
{
    const uint32_t phase = phase_of(o);
    const bool reading = phase == ECUIO_OPERATION_PHASE_READING ||
                         phase == ECUIO_OPERATION_PHASE_COMPARING;
    const uint32_t pass = reading ? o.r.pass + 1 : 0;
    const size_t steps = o.p->steps.size();
    const uint32_t step =
        o.r.step < steps ? o.r.step : static_cast<uint32_t>(steps - 1);
    ecuio_operation_state_t& v = o.view;
    const uint32_t checks = checks_of(o);
    if (v.phase == phase && v.pass == pass && v.step == step &&
        v.bytes_done == o.r.handed && v.checks == checks &&
        v.volts_mv == o.v.mv && v.volts_min_mv == o.v.min_mv) {
        return;
    }
    const std::lock_guard<std::mutex> lock(s->support_mutex);
    v.phase = phase;
    v.pass = pass;
    v.step = step;
    v.bytes_done = o.r.handed;
    v.checks = checks;
    v.volts_mv = o.v.mv;
    v.volts_min_mv = o.v.min_mv;
    v.rpm = static_cast<uint16_t>(o.k.rpm);
    v.kmh = static_cast<uint16_t>(o.k.kmh);
}

/* A recording's operation line for the run's start (end NONE) or end */
inline void record_line(ecuio_session_t* s, const session_run& o, uint32_t end)
{
    ecuio_record_operation(s->record_link.get(), o.p->id.c_str(), o.p->rx_id,
                           image_bytes(*o.p), end);
}

/* The run's end to the view, and the run back to the caller */
inline void end_run(ecuio_session_t* s, session_run& o, uint32_t end,
                    const char* words)
{
    record_line(s, o, end);
    {
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        ecuio_operation_state_t& v = o.view;
        v.phase = ECUIO_OPERATION_PHASE_DONE;
        v.end = end;
        v.pass = 0;
        v.bytes_done = o.r.handed;
        v.checks = checks_of(o);
        v.volts_mv = o.v.mv;
        v.volts_min_mv = o.v.min_mv;
        v.rpm = static_cast<uint16_t>(o.k.rpm);
        v.kmh = static_cast<uint16_t>(o.k.kmh);
        v.nrc = end == ECUIO_OPERATION_END_REFUSED ||
                        end == ECUIO_OPERATION_END_UNLOCK_REFUSED
                    ? o.r.nrc
                    : uint8_t{0};
        if (end == ECUIO_OPERATION_END_DIFFER) {
            v.step = o.sink->differ_step();
            v.differ_offset = o.sink->differ_offset();
        }
        if (end == ECUIO_OPERATION_END_SAVED) {
            std::memcpy(v.name, o.sink->name(), sizeof v.name);
        }
        const size_t n = (std::min)(std::strlen(words), sizeof v.words - 1);
        std::memcpy(v.words, words, n);
        v.words[n] = '\0';
    }
    if (o.held_awake) {
        o.held_awake = false;
        (void)ecuio_awake_core_hold(s, false);
    }
    o.stage.store(kStageEnded, std::memory_order_release);
}

/* A word owed and never given ends here with nothing counted: the
 * conversation's driver settles its mark as it does any word's end */
inline void drop_word(ecuio_bound_channel& b, session_run& o)
{
    if (!o.marked || o.word_given) {
        return;
    }
    o.word_given = true;
    ecuio_conversation_report& r = b.conversation.report;
    b.word_taken = o.p->level;
    r.level = o.p->level;
    r.unlock = ecuio_unlock_state::ended;
    r.nrc = 0;
    ++r.words_ended;
}

/* The machine's enter: the plan's session entered through the engine's
 * own step, then its level open. A level found open costs no attempt; a
 * locked one is opened only by the word the start saved a mark for, and
 * a word that ends any other way than unlocked ends the run. */
inline void enter(ecuio_session_t* s, session_run& o, ecuio_request_engine* e,
                  int64_t now_us)
{
    ecuio_bound_channel* b = ecuio_bound_channel_of(s, o.channel);
    if (b == nullptr ||
        b->conversation.report.state != ecuio_conversation_state::talking) {
        not_entered(o.r, run_end::unlock_refused);
        return;
    }
    if (o.entering_us == 0) {
        o.entering_us = now_us;
    }
    if (e->chans[o.channel].session_step != ECUIO_SESSION_STATE_ENTERED) {
        if (!ecuio_engine_session_enter(e, o.channel) ||
            now_us - o.entering_us > kEnterMostUs) {
            not_entered(o.r, run_end::session_lost);
        }
        return;
    }
    ecuio_conversation& v = b->conversation;
    if (o.word_given) {
        if (v.report.words_ended == o.ends_at) {
            return; /* the attempt runs */
        }
        const ecuio_unlock_state u = v.report.unlock;
        if (u == ecuio_unlock_state::unlocked) {
            ++o.unlocks;
        }
        if (u == ecuio_unlock_state::unlocked ||
            u == ecuio_unlock_state::already_unlocked) {
            entered(o.r);
        } else {
            not_entered(o.r, run_end::unlock_refused, v.report.nrc);
        }
        return;
    }
    const uint32_t k = ecuio_security::find_level(v.access, o.p->level);
    const bool open = k < v.access.count && v.access.levels[k].unlocked;
    if (!o.marked) {
        if (open) {
            entered(o.r);
        } else {
            not_entered(o.r, run_end::unlock_refused);
        }
        return;
    }
    o.ends_at = v.report.words_ended;
    if (ecuio_conversation_unlock(&v, o.p->level)) {
        b->word_taken = o.p->level;
        o.word_given = true;
    } else {
        drop_word(*b, o);
        not_entered(o.r, run_end::unlock_refused);
    }
}

/* The run's talk, settled: the one-shot is free again. An end that came
 * with no frame is told here: a wait run out, or a write that failed. */
inline void take_settled(session_run& o, ecuio_engine_talk_state& t)
{
    ecuio_talk_result res;
    if (!o.out || !ecuio_engine_talk_take(t, &res)) {
        return;
    }
    o.out = false;
    const bool told = o.fed || o.orphan;
    o.fed = false;
    o.orphan = false;
    if (told || o.r.at == run_stage::done) {
        return;
    }
    if (o.checking) {
        check_silence(o.k);
    } else if (res.end == ecuio_talk_end::unsent) {
        stop(o.r, run_end::link_lost);
    } else {
        silence(o.r);
    }
}

/* The machine's request offered as the channel's talk */
inline void offer(session_run& o, ecuio_request_engine* e, const run_request& q,
                  int64_t now_us)
{
    const bool ident = q.bytes[0] == 0x22;
    ecuio_talk_request t;
    t.pdu = q.bytes;
    t.len = q.size;
    t.echo = ident ? uint8_t{3} : uint8_t{1};
    t.kind = ident ? ecuio_talk_kind::plain : ecuio_talk_kind::read;
    if (ecuio_engine_offer_talk(e, o.channel, t, now_us) ==
        ecuio_talk_offer::taken) {
        o.out = true;
        o.talk_at = o.channel;
        o.sid = q.bytes[0];
    } else {
        stop(o.r, run_end::link_lost); /* the start judged every request */
    }
}

/* The battery asked of the transport at the start, at each step's start
 * and every kVoltsEveryUs; an ask not served in kVoltsLateUs is no
 * reading. With no reading at the start the checks ask the engine
 * module's own. */
inline void battery_step(ecuio_session_t* s, session_run& o, int64_t now_us)
{
    ecuio_battery_slot& b = s->battery;
    volts& v = o.v;
    if (!b.source) {
        if (!v.first) {
            v.first = true;
            o.k.want_volts = true;
        }
        return;
    }
    const bool served = o.volts_asked != 0 && b.served == o.volts_asked;
    if (served ||
        (o.volts_asked != 0 && now_us - o.volts_at_us > kVoltsLateUs)) {
        if (served && b.got) {
            volts_take(v, b.mv, v.first);
        }
        if (!v.first) {
            v.first = true;
            o.k.want_volts = !(served && b.got);
            v.refused_low = v.refused_low || (v.write && !(served && b.got));
        }
        o.volts_asked = 0;
    }
    const bool step_moved = !o.checking && o.r.step != o.volts_step;
    const bool due = o.volts_at_us == 0 || step_moved ||
                     now_us - o.volts_at_us >= kVoltsEveryUs;
    if (o.volts_asked == 0 && due && o.r.at != run_stage::done) {
        o.volts_step = o.r.step;
        o.volts_asked = ++b.want == 0 ? ++b.want : b.want;
        o.volts_at_us = now_us;
    }
}

/* A check's talk still waiting for its turn when the run has ended or the
 * checks' bound has passed is withdrawn, and what is left goes unread */
inline void drop_check(session_run& o, ecuio_request_engine* e, int64_t now_us)
{
    const bool late = now_us - o.began_us > kEnterMostUs;
    if (o.checking && o.out && (late || o.r.at == run_stage::done) &&
        ecuio_talk_withdraw(e->chans[o.talk_at].talk)) {
        o.out = false;
        check_give_up(o.k);
    }
}

/* The checks before the run, each a talk of its own on its module's
 * channel: one at a time, a busy channel asked again at the next pass */
inline void check_step(session_run& o, ecuio_request_engine* e, int64_t now_us)
{
    checks& k = o.k;
    check_request q;
    while (!o.out && k.result == check_end::none && check_next(k, q)) {
        ecuio_talk_request t;
        t.pdu = q.bytes;
        t.len = 2;
        t.echo = 2;
        t.kind = ecuio_talk_kind::check;
        const ecuio_talk_offer a =
            ecuio_engine_offer_talk(e, q.module, t, now_us);
        if (a == ecuio_talk_offer::taken) {
            o.out = true;
            o.talk_at = q.module;
            o.sid = q.bytes[0];
            return;
        }
        if (a == ecuio_talk_offer::busy) {
            return;
        }
        check_silence(k);
    }
    if (o.out) {
        return;
    }
    o.checking = false;
    if (k.has_volts) {
        volts_take(o.v, k.volts_mv, false);
        o.v.from_ecu = true;
    }
    if (k.result == check_end::engine_running) {
        stop(o.r, run_end::engine_running);
    } else if (k.result == check_end::moving) {
        stop(o.r, run_end::moving);
    }
}

/* The machine at its end: the copy sealed and the writer's word waited
 * for, or nothing kept. The hold ends with the last request. */
inline void finish(ecuio_session_t* s, session_run& o, ecuio_request_engine* e,
                   int64_t now_us)
{
    e->operation_hold = false;
    if (o.out) {
        return; /* a talk given up is taken first: the slot is left free */
    }
    if (ecuio_bound_channel* b = ecuio_bound_channel_of(s, o.channel)) {
        drop_word(*b, o);
    }
    if (o.r.result != run_end::complete) {
        if (!o.closed_sink) {
            o.sink->abandon();
            o.closed_sink = true;
        }
        if (o.sink->state() == sink_state::writing) {
            return; /* the end is told once the unfinished file is gone */
        }
        const uint32_t end = end_code(o.r.result);
        char words[sizeof o.view.words];
        words_of(o, end, words, sizeof words);
        end_run(s, o, end,
                end == ECUIO_OPERATION_END_FILE &&
                        o.sink->state() == sink_state::failed
                    ? o.sink->words()
                    : words);
        return;
    }
    if (!o.closed_sink) {
        read_facts facts;
        facts.seconds = static_cast<uint32_t>((now_us - o.began_us) / 1000000);
        facts.unlocks = o.unlocks;
        o.sink->seal(facts);
        o.closed_sink = true;
        return;
    }
    const sink_state w = o.sink->state();
    if (w == sink_state::saved) {
        end_run(s, o, ECUIO_OPERATION_END_SAVED, "");
    } else if (w == sink_state::differ) {
        end_run(s, o, ECUIO_OPERATION_END_DIFFER,
                end_words(ECUIO_OPERATION_END_DIFFER));
    } else if (w != sink_state::writing) {
        end_run(s, o, ECUIO_OPERATION_END_FILE, o.sink->words());
    }
}

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
    if ((o.v.dropped || o.v.refused_low || o.v.refused_high) &&
        r.at != run_stage::done) {
        o.orphan = o.out && !o.fed;
        stop(r, o.v.refused_high && !o.v.refused_low ? run_end::voltage_high
                                                     : run_end::voltage_low);
    }
    if (o.checking && o.v.first && r.at != run_stage::done) {
        check_step(o, e, now_us);
    }
    while (!o.checking && r.at != run_stage::done && !o.out && !t.offered &&
           !t.out && t.result.end == ecuio_talk_end::none) {
        run_request q;
        const run_action a = next(r, q, *o.sink);
        if (a == run_action::send) {
            offer(o, e, q, now_us);
        } else if (a == run_action::enter) {
            enter(s, o, e, now_us);
            if (r.at == run_stage::enter) {
                break; /* the entry or the attempt runs */
            }
        } else {
            break; /* hold: the ring's room first; done */
        }
    }
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
        o->fed || o->orphan) {
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
    if (end == ecuio_talk_end::answered || end == ecuio_talk_end::refused) {
        o->fed = true;
        const bool left =
            end == ecuio_talk_end::refused && len == 3 &&
            (bytes[2] == ECUIO_NRC_NOT_IN_ACTIVE_SESSION ||
             bytes[2] == ECUIO_NRC_SUBFUNCTION_NOT_IN_ACTIVE_SESSION);
        if (left) {
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
        begin(o->r, *o->p);
        check_begin(o->k, check_engine_of(e, o->channel), o->channel, false);
        o->checking = true;
        o->talk_at = o->channel;
        o->v = volts{};
        o->v.write = o->write;
        o->volts_asked = 0;
        o->volts_at_us = 0;
        o->volts_step = 0;
        o->began_us = now_us;
        e->operation_hold = true;
        record_line(s, *o, ECUIO_OPERATION_END_NONE);
        stage = kStageRunning;
        o->stage.store(stage, std::memory_order_release);
    }
    if (stage != kStageRunning) {
        return;
    }
    drive(s, *o, e, now_us);
    if (o->stage.load(std::memory_order_relaxed) == kStageRunning) {
        publish(s, *o);
    }
}

/* The wait until a run next has a step to take, from now (INT64_MAX for
 * none): now for an armed run and a settled talk, never while its talk
 * is out, else the poll */
inline int64_t wait_us(ecuio_session_t* s, const ecuio_request_engine* e)
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
    if (o->out) {
        return e->chans[o->talk_at].talk.result.end != ecuio_talk_end::none
                   ? 0
                   : INT64_MAX;
    }
    return kPollUs;
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
