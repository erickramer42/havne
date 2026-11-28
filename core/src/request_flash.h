#ifndef ECUIO_REQUEST_FLASH_H
#define ECUIO_REQUEST_FLASH_H

#include "request_operation_steps.h"

namespace ecuio_operation {

struct no_sink {
    bool room(uint32_t) const { return true; }
};

inline uint32_t flash_end_code(const session_run& o)
{
    const flash_run& f = o.f;
    if (f.result == flash_end::none) {
        return end_code(o.r.result);
    }
    if (f.result == flash_end::written) {
        return o.recover ? ECUIO_OPERATION_END_RECOVERED
                         : ECUIO_OPERATION_END_WRITTEN;
    }
    if (f.result == flash_end::check_failed) {
        return ECUIO_OPERATION_END_CHECK_FAILED;
    }
    if (f.floor) {
        return ECUIO_OPERATION_END_RECOVERY_NEEDED;
    }
    switch (f.result) {
    case flash_end::refused:
        return ECUIO_OPERATION_END_REFUSED;
    case flash_end::no_answer:
        return ECUIO_OPERATION_END_NO_ANSWER;
    case flash_end::bad_reply:
        return ECUIO_OPERATION_END_BAD_REPLY;
    case flash_end::link_lost:
        return ECUIO_OPERATION_END_LINK_LOST;
    case flash_end::file:
        return ECUIO_OPERATION_END_FILE;
    default:
        return ECUIO_OPERATION_END_CANCELED;
    }
}

inline const char* flash_cause(const flash_run& f)
{
    switch (f.result) {
    case flash_end::refused:
        return "the module refused a request";
    case flash_end::no_answer:
        return "the module stopped answering";
    case flash_end::bad_reply:
        return "an answer wasn't what its request asks for";
    case flash_end::link_lost:
        return "a request could not be sent";
    default:
        return "it was stopped";
    }
}

inline void flash_words_of(const session_run& o, uint32_t end, char* out,
                           size_t cap)
{
    const flash_run& f = o.f;
    if (f.result == flash_end::none) {
        words_of(o, end, out, cap);
    } else if (end == ECUIO_OPERATION_END_WRITTEN && o.journal.failed()) {
        std::snprintf(out, cap,
                      "written and checked, but %.70s: the next flash of this "
                      "module is refused while it says writing",
                      o.journal.words());
    } else if (end == ECUIO_OPERATION_END_RECOVERED && o.journal.failed()) {
        std::snprintf(out, cap,
                      "recovered and checked, but %.70s: the next connect "
                      "holds this module while it says writing",
                      o.journal.words());
    } else if (end == ECUIO_OPERATION_END_WRITTEN ||
               end == ECUIO_OPERATION_END_RECOVERED) {
        std::snprintf(out, cap, "%s", "");
    } else if (end == ECUIO_OPERATION_END_CHECK_FAILED && !f.floor) {
        std::snprintf(out, cap,
                      "nothing was written, and the module's own check "
                      "failed: what it holds isn't the images, so flash it "
                      "without a copy");
    } else if (end == ECUIO_OPERATION_END_CHECK_FAILED) {
        std::snprintf(out, cap,
                      "the module's own check of what was written "
                      "failed: recover the module before it runs");
    } else if (end == ECUIO_OPERATION_END_RECOVERY_NEEDED && f.nrc != 0) {
        std::snprintf(out, cap,
                      "the flash stopped after it began writing (%s, code "
                      "%02X): recover the module before it runs",
                      flash_cause(f), f.nrc);
    } else if (end == ECUIO_OPERATION_END_RECOVERY_NEEDED) {
        std::snprintf(out, cap,
                      "the flash stopped after it began writing (%s): "
                      "recover the module before it runs",
                      flash_cause(f));
    } else if (end == ECUIO_OPERATION_END_FILE) {
        std::snprintf(out, cap, "%.138s: nothing was written",
                      o.journal.words());
    } else {
        std::snprintf(out, cap, "%s: nothing was written", flash_cause(f));
    }
}

inline void flash_words(const session_run& o, uint32_t end, char* out,
                        size_t cap)
{
    char words[sizeof o.view.words];
    flash_words_of(o, end, words, sizeof words);
    if (!o.recover || end == ECUIO_OPERATION_END_RECOVERED || o.f.floor) {
        std::snprintf(out, cap, "%s", words);
        return;
    }
    std::snprintf(out, cap,
                  "%.90s; the module is still part-written: recover it "
                  "again before it runs",
                  words);
}

inline void flash_report(ecuio_session_t* s, const session_run& o, bool written,
                         bool held)
{
    if ((!o.recover && !held) || o.channel >= ECUIO_MAX_CHANNELS) {
        return;
    }
    const std::lock_guard<std::mutex> lock(s->support_mutex);
    ecuio_module_recovery_t& r = s->recovery[o.channel];
    const uint32_t operation = o.operation;
    r = ecuio_module_recovery_t{};
    r.operation = operation;
    r.attempts = o.attempts;
    r.sectors = static_cast<uint16_t>(o.journal_sectors);
    r.flags = o.copy_pinned ? ECUIO_RECOVERY_COPY_PINNED : 0u;
    if (written) {
        r.state = ECUIO_RECOVERY_RECOVERED;
        r.sectors_written = r.sectors;
        return;
    }
    r.state = ECUIO_RECOVERY_NEEDED;
    r.sectors_written =
        static_cast<uint16_t>(o.journal_base + o.f.done_sectors);
    std::snprintf(r.words, sizeof r.words, "%s",
                  "a flash of this module didn't finish: recover it before "
                  "it runs");
}

/* The flash's request offered as the channel's talk, a write */
inline void flash_offer(session_run& o, ecuio_request_engine* e,
                        const uint8_t* pdu, uint32_t len, int64_t now_us)
{
    ecuio_talk_request t;
    t.pdu = pdu;
    t.len = len;
    t.echo = 1;
    t.kind = ecuio_talk_kind::write;
    if (ecuio_engine_offer_talk(e, o.channel, t, now_us) ==
        ecuio_talk_offer::taken) {
        o.out = true;
        o.talk_at = o.channel;
        o.sid = pdu[0];
    } else {
        o.f.asked = false;
        flash_stop(o.f, flash_end::link_lost);
    }
}

/* The journal holds the flash's place: before the floor nothing goes
 * until it does, and a failure there ends the run; past it a failure
 * leaves the journal unfinished, which is what it must say */
inline bool flash_journal_holds(session_run& o)
{
    if (o.journal.flushed() == o.journal.posted()) {
        return false;
    }
    if (!o.journal.failed()) {
        return true;
    }
    if (!o.f.floor) {
        flash_stop(o.f, flash_end::file);
    }
    return false;
}

/* One step of the flash's machine; false once it waits */
inline bool flash_step(session_run& o, ecuio_request_engine* e, int64_t now_us)
{
    flash_run& f = o.f;
    if (o.pause_until_us != 0 && now_us < o.pause_until_us) {
        return false;
    }
    o.pause_until_us = 0;
    if (flash_journal_holds(o)) {
        return false;
    }
    uint32_t len = 0;
    switch (flash_next(f, o.r.wire, &len)) {
    case flash_action::journal: {
        journal_mark m;
        m.state = journal_state::writing;
        m.written = o.journal_base + f.done_sectors;
        m.erased = m.written + (f.at == flash_stage::erase ? 1u : 0u);
        o.journal.post(m);
        flash_journaled(f);
        return true;
    }
    case flash_action::pause:
        o.pause_until_us = now_us + int64_t{kFlashPauseMs} * 1000;
        return false;
    case flash_action::send:
        flash_offer(o, e, o.r.wire, len, now_us);
        return false;
    default:
        return false;
    }
}

/* The flash ended: the journal's last mark, the leave when nothing was
 * left half written, then the end */
inline void flash_finish(ecuio_session_t* s, session_run& o,
                         ecuio_request_engine* e, int64_t now_us)
{
    if (o.out) {
        return;
    }
    const flash_run& f = o.f;
    const bool written = f.result == flash_end::written;
    const bool held = !written && (f.floor || o.recover);
    if (!o.journal_closed && o.journal.is_open() && !held) {
        journal_mark m;
        m.state = written ? journal_state::done : journal_state::ready;
        m.written = o.journal_base + f.done_sectors;
        m.erased = m.written;
        o.journal.post(m);
    }
    o.journal_closed = true;
    if (o.journal.flushed() != o.journal.posted() && !o.journal.failed()) {
        return;
    }
    if (written || !f.floor) {
        if (!own_leave(o, e, now_us)) {
            return;
        }
    } else {
        o.own = own_step::left;
    }
    if (held) {
        e->chans[o.channel].part_written = true;
        s->part_written.fetch_or(1u << o.channel);
    } else if (o.recover) {
        s->part_written.fetch_and(~(1u << o.channel));
    }
    flash_report(s, o, written, held);
    e->operation_hold = false;
    if (ecuio_bound_channel* b = ecuio_bound_channel_of(s, o.channel)) {
        drop_word(*b, o);
    }
    const uint32_t end = flash_end_code(o);
    char words[sizeof o.view.words];
    flash_words(o, end, words, sizeof words);
    o.r.nrc = f.result == flash_end::none ? o.r.nrc : f.nrc;
    end_run(s, o, end, words);
}

/* One pass of a flash that is the worker's */
inline void drive_flash(ecuio_session_t* s, session_run& o,
                        ecuio_request_engine* e, int64_t now_us)
{
    run& r = o.r;
    ecuio_engine_talk_state& t = e->chans[o.channel].talk;
    if (o.cancel.load(std::memory_order_relaxed)) {
        if (r.at == run_stage::flash) {
            flash_cancel(o.f);
        } else {
            cancel(r);
        }
    }
    take_settled(o, e->chans[o.talk_at].talk);
    own_settle(o, e, now_us);
    drop_check(o, e, now_us);
    battery_step(s, o, now_us);
    const bool flash_held = r.at == run_stage::flash && (o.f.floor || o.out);
    if ((o.v.refused_low || o.v.refused_high) && r.at != run_stage::done &&
        !flash_held) {
        o.orphan = o.out && !o.fed;
        stop(r, o.v.refused_high && !o.v.refused_low ? run_end::voltage_high
                                                     : run_end::voltage_low);
    }
    if (o.checking && o.v.first && r.at != run_stage::done) {
        check_step(s, o, e, now_us);
    }
    no_sink none;
    while (!o.checking && r.at != run_stage::done && !o.out && !t.offered &&
           !t.out && t.result.end == ecuio_talk_end::none) {
        if (r.at == run_stage::flash) {
            if (!flash_step(o, e, now_us)) {
                break;
            }
            continue;
        }
        run_request q;
        if (next(r, q, none) != run_action::enter) {
            break;
        }
        enter(s, o, e, now_us);
        if (r.at == run_stage::enter) {
            break;
        }
    }
    if (r.at == run_stage::flash && o.f.at == flash_stage::done && !o.out) {
        stop(r, run_end::complete);
    }
    if (r.at == run_stage::done) {
        flash_finish(s, o, e, now_us);
    }
}

} // namespace ecuio_operation

#endif /* ECUIO_REQUEST_FLASH_H */
