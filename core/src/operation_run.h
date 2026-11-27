// File/layer: core/src/operation_run.h | internal, C++17, Qt-free, pure
// Testing: UNIT: OperationRunTest (test_obd2); FUZZ: fuzz_operation
//
// A dump's run as a machine with no clock and no wire (ADR-0066 section
// 4): what to ask next, and what a reply, a silence, a cancel and a stop
// do to it. Its caller sends the requests, times the waits and enters the
// sessions; its sink takes the bytes.
//
// The order: the first step's session is entered (and the plan's level
// unlocked there, the caller's work), the plan's identifiers are read,
// then each step's range is read twice back to back, a new session
// entered where a step names another, and the identifiers are read again
// and held to the first reading. Every data byte of both passes is handed
// to the sink once, in order; the sink compares the passes and stops the
// run where they differ.
//
// The rules of the wire (ISO 14229-1):
// - A read is 23 <format> <address> <size> for each chunk; its answer is
//   63 and exactly the bytes asked.
// - An upload is 35 00 <format> <address> <size>, answered 75 <length
//   format> <the most a 76 holds>; then 36 <counter> from 01, wrapping FF
//   to 00, each answered 76 <counter> and a block's bytes; then 37.
// - 7F <service> 78 means wait: the caller goes on waiting, kPendingMostMs
//   in all for one request.
// - A chunk or an identifier unanswered, or answered 21, is asked again,
//   kAsksMost times in all. Nothing of an upload is ever asked twice: a
//   block lost or wrong ends the run after one 37, whose answer is not
//   judged.
// - A reply of any length but the one asked is a fault, never padded or
//   cut.

#ifndef ECUIO_OPERATION_RUN_H
#define ECUIO_OPERATION_RUN_H

#include "operation_plan.h"

#include "ecuio/dump.h"
#include "ecuio/plugin.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ecuio_operation {

constexpr uint32_t kAsksMost = 3;
constexpr uint32_t kPendingMostMs = 60000;
/* 35 00 <format> and four bytes each of address and size */
constexpr size_t kRequestMax = 11;
/* The most a 75 may name: a 76, its counter and its data in one message */
constexpr uint32_t kBlockMost = 4095;

/* How a run ended. complete: every byte was handed over twice and the
 * identifiers held. That is the machine's word alone: its sink compares
 * behind it, so a sink that finds the last bytes differ stops a run that
 * is already complete (stop() takes it), and only the sink's seal makes
 * a copy. */
enum class run_end : uint8_t {
    none,
    complete,
    canceled,
    refused, /* nrc says how */
    no_answer,
    bad_reply,
    differ,
    identity_changed,
    session_lost,   /* 7F .. 33: the unlock is gone */
    unlock_refused, /* the caller's, through not_entered */
    link_lost,      /* the caller's, through stop */
    file,           /* the sink's, through stop */
    engine_running, /* the caller's checks, through stop */
    moving,
    voltage_low,
};

enum class run_action : uint8_t {
    enter, /* enter request.session and unlock the level, then entered() */
    send,  /* send request.bytes, then reply() or silence() */
    hold,  /* the sink has no room for the answer yet: ask again later */
    done,
};

enum class run_stage : uint8_t {
    enter,
    ident_before,
    read,
    upload_open,
    upload_block,
    upload_exit,
    ident_after,
    closing, /* the one 37 after a cancel or a fault in an upload */
    done,
};

struct run_request {
    uint8_t bytes[kRequestMax] = {};
    uint32_t size = 0;
    uint32_t session = 0; /* run_action::enter: ECUIO_STEP_SESSION_* */
};

struct run {
    const plan* p = nullptr;
    run_stage at = run_stage::enter;
    run_end result = run_end::none;
    run_end closing_as = run_end::none; /* the end once the 37 has gone */
    uint8_t nrc = 0;
    uint8_t counter = 0;   /* the next 36's */
    bool asked = false;    /* a request is out */
    bool canceled = false; /* asked to end, while a request was out */
    bool open = false;     /* the module holds an upload open, or may */
    bool before = false;   /* the identifiers' first reading is done */
    uint32_t block = 0;    /* an upload block's data bytes */
    uint32_t asks = 0;     /* of the request that is out or next */
    uint32_t step = 0;
    uint32_t pass = 0;
    uint32_t ident = 0;
    uint32_t session = 0; /* the one entered; 0 before the first */
    uint64_t offset = 0;  /* into the step's range, this pass */
    uint64_t handed = 0;  /* data bytes handed to the sink, both passes */
    uint8_t ident_size[ECUIO_PLUGIN_MAX_IDENTS] = {};
    uint8_t ident_value[ECUIO_PLUGIN_MAX_IDENTS][ECUIO_DUMP_IDENT_MAX] = {};
};

/* A run of p, which outlives it, at its start */
inline void begin(run& r, const plan& p)
{
    r = run{};
    r.p = &p;
}

/* The data bytes a whole run hands over: each range twice */
inline uint64_t total_bytes(const run& r)
{
    return 2 * image_bytes(*r.p);
}

/* Ended at once as e: nothing more is asked. The sink's and the
 * caller's ends come through here, a complete run's too. */
inline void stop(run& r, run_end e, uint8_t nrc = 0)
{
    r.at = run_stage::done;
    r.result = e;
    r.nrc = nrc;
    r.asked = false;
    r.open = false;
}

/* Ended as e after one 37 when an upload is open, else at once; a 37
 * that failed was that one */
inline void close(run& r, run_end e, uint8_t nrc = 0)
{
    if (!r.open || r.at == run_stage::upload_exit) {
        stop(r, e, nrc);
        return;
    }
    r.at = run_stage::closing;
    r.closing_as = e;
    r.nrc = nrc;
    r.asked = false;
    r.open = false;
}

/* The stage the run's place calls for */
inline void settle(run& r)
{
    const plan& p = *r.p;
    r.asks = 0;
    if (r.step == p.steps.size()) {
        if (r.ident < p.idents.size()) {
            r.at = run_stage::ident_after;
        } else {
            stop(r, run_end::complete);
        }
        return;
    }
    const step& s = p.steps[r.step];
    if (s.session != r.session) {
        r.at = run_stage::enter;
        return;
    }
    if (!r.before && r.ident < p.idents.size()) {
        r.at = run_stage::ident_before;
        return;
    }
    if (!r.before) {
        r.before = true;
        r.ident = 0;
    }
    r.at = s.kind == ECUIO_STEP_READ ? run_stage::read : run_stage::upload_open;
}

/* A pass over the step's range is whole: its second pass, or the next
 * step */
inline void pass_done(run& r)
{
    r.offset = 0;
    r.open = false;
    if (r.pass == 0) {
        r.pass = 1;
    } else {
        r.pass = 0;
        ++r.step;
    }
    settle(r);
}

/* The session next() asked for is entered and its level unlocked */
inline void entered(run& r)
{
    if (r.at == run_stage::enter) {
        r.session = r.p->steps[r.step].session;
        settle(r);
    }
}

/* It couldn't be entered or unlocked: the run ends as e */
inline void not_entered(run& r, run_end e, uint8_t nrc = 0)
{
    stop(r, e, nrc);
}

constexpr unsigned kNumberMax = 4;
static_assert(kRequestMax == 3 + 2 * kNumberMax, "a 35 fits its request");

/* n is bounded here so GCC can prove the request's buffer holds it */
inline uint8_t* put_be(uint8_t* out, uint64_t value, unsigned n)
{
    for (unsigned i = n < kNumberMax ? n : kNumberMax; i > 0; --i) {
        *out++ = static_cast<uint8_t>(value >> (8 * (i - 1)));
    }
    return out;
}

/* The data bytes the next 23 or 36 asks for */
inline uint32_t next_bytes(const run& r)
{
    const step& s = r.p->steps[r.step];
    const uint64_t most = r.at == run_stage::read ? s.chunk : r.block;
    return static_cast<uint32_t>((std::min)(most, s.bytes - r.offset));
}

/* The request of the run's stage into out */
inline void write_request(const run& r, run_request& out)
{
    uint8_t* at = out.bytes;
    if (r.at == run_stage::ident_before || r.at == run_stage::ident_after) {
        *at++ = 0x22;
        at = put_be(at, r.p->idents[r.ident].did, 2);
    } else if (r.at == run_stage::upload_block) {
        *at++ = 0x36;
        *at++ = r.counter;
    } else if (r.at == run_stage::upload_exit || r.at == run_stage::closing) {
        *at++ = 0x37;
    } else {
        const step& s = r.p->steps[r.step];
        const bool read = r.at == run_stage::read;
        *at++ = read ? 0x23 : 0x35;
        if (!read) {
            *at++ = s.dfi;
        }
        *at++ = static_cast<uint8_t>((s.size_bytes << 4) | s.address_bytes);
        at = put_be(at, s.address + (read ? r.offset : 0), s.address_bytes);
        at = put_be(at, read ? next_bytes(r) : s.bytes, s.size_bytes);
    }
    out.size = static_cast<uint32_t>(at - out.bytes);
}

/* What to do now. A sink is asked room(n) before a request whose answer
 * brings n data bytes. Call it only while no request is out. */
template <typename Sink> run_action next(run& r, run_request& out, Sink& sink)
{
    out = run_request{};
    if (r.at == run_stage::done) {
        return run_action::done;
    }
    if (r.at == run_stage::enter) {
        out.session = r.p->steps[r.step].session;
        return run_action::enter;
    }
    const bool data =
        r.at == run_stage::read || r.at == run_stage::upload_block;
    if (data && !sink.room(next_bytes(r))) {
        return run_action::hold;
    }
    write_request(r, out);
    r.asked = true;
    ++r.asks;
    return run_action::send;
}

/* The service the request that is out names */
inline uint8_t asked_service(const run& r)
{
    switch (r.at) {
    case run_stage::ident_before:
    case run_stage::ident_after:
        return 0x22;
    case run_stage::read:
        return 0x23;
    case run_stage::upload_open:
        return 0x35;
    case run_stage::upload_block:
        return 0x36;
    default:
        return 0x37;
    }
}

/* Whether the request that is out may be asked again as it is */
inline bool may_ask_again(const run& r)
{
    const bool repeatable = r.at == run_stage::read ||
                            r.at == run_stage::ident_before ||
                            r.at == run_stage::ident_after;
    return repeatable && r.asks < kAsksMost;
}

/* An identifier's answer: 62 <did> and 1 to ECUIO_DUMP_IDENT_MAX bytes */
template <typename Sink>
void take_ident(run& r, const uint8_t* d, size_t n, Sink& sink)
{
    const uint32_t did = r.p->idents[r.ident].did;
    if (n < 4 || n > 3 + ECUIO_DUMP_IDENT_MAX || d[0] != 0x62 ||
        d[1] != static_cast<uint8_t>(did >> 8) ||
        d[2] != static_cast<uint8_t>(did)) {
        stop(r, run_end::bad_reply);
        return;
    }
    const auto size = static_cast<uint8_t>(n - 3);
    if (r.at == run_stage::ident_before) {
        r.ident_size[r.ident] = size;
        std::memcpy(r.ident_value[r.ident], d + 3, size);
        sink.ident(r.ident, d + 3, size);
    } else if (r.ident_size[r.ident] != size ||
               std::memcmp(r.ident_value[r.ident], d + 3, size) != 0) {
        stop(r, run_end::identity_changed);
        return;
    }
    ++r.ident;
    settle(r);
}

/* A positive answer to the request that is out */
template <typename Sink>
void take_positive(run& r, const uint8_t* d, size_t n, Sink& sink)
{
    const step* s = r.step < r.p->steps.size() ? &r.p->steps[r.step] : nullptr;
    switch (r.at) {
    case run_stage::ident_before:
    case run_stage::ident_after:
        take_ident(r, d, n, sink);
        return;
    case run_stage::read:
    case run_stage::upload_block: {
        const uint32_t want = next_bytes(r);
        const size_t head = r.at == run_stage::read ? 1 : 2;
        const bool ours = r.at == run_stage::read
                              ? d[0] == 0x63
                              : n >= 2 && d[0] == 0x76 && d[1] == r.counter;
        if (!ours || n != head + want) {
            close(r, run_end::bad_reply);
            return;
        }
        sink.data(r.step, r.pass, r.offset, d + head, want);
        r.offset += want;
        r.handed += want;
        r.asks = 0;
        ++r.counter; /* a 36's; FF wraps to 00. A read has none */
        if (r.offset == s->bytes) {
            if (r.at == run_stage::read) {
                pass_done(r);
            } else {
                r.at = run_stage::upload_exit;
            }
        }
        return;
    }
    case run_stage::upload_open: {
        const size_t sized = n >= 2 ? d[1] >> 4 : 0;
        uint64_t most = 0;
        for (size_t i = 0; i < sized && 2 + i < n; ++i) {
            most = most << 8 | d[2 + i];
        }
        r.open = true; /* whatever it answered, it may hold one now */
        if (d[0] != 0x75 || sized < 1 || sized > 4 || n != 2 + sized ||
            most < 3 || most > kBlockMost) {
            close(r, run_end::bad_reply);
            return;
        }
        r.block = static_cast<uint32_t>(most - 2);
        r.counter = 1;
        r.asks = 0;
        r.at = run_stage::upload_block;
        return;
    }
    default: /* upload_exit */
        if (d[0] != 0x77) {
            close(r, run_end::bad_reply);
            return;
        }
        pass_done(r);
        return;
    }
}

enum class run_wait : uint8_t {
    go_on,       /* the request is settled: call next() */
    keep_waiting /* a 78: wait on for the same request's answer */
};

/* An answer to the request that is out, waited_ms after it was sent */
template <typename Sink>
run_wait reply(run& r, const uint8_t* d, size_t n, uint32_t waited_ms,
               Sink& sink)
{
    if (!r.asked) {
        return run_wait::go_on;
    }
    if (r.at == run_stage::closing) {
        stop(r, r.closing_as, r.nrc); /* the 37's answer isn't judged */
        return run_wait::go_on;
    }
    const uint8_t sid = asked_service(r);
    const bool negative = n == 3 && d[0] == 0x7F && d[1] == sid;
    if (negative && d[2] == 0x78) {
        if (waited_ms < kPendingMostMs && !r.canceled) {
            return run_wait::keep_waiting;
        }
        r.open = r.open || r.at == run_stage::upload_open;
        close(r, r.canceled ? run_end::canceled : run_end::no_answer);
        return run_wait::go_on;
    }
    r.asked = false;
    if (r.canceled) {
        r.open =
            r.open || (r.at == run_stage::upload_open && n > 0 && d[0] == 0x75);
        close(r, run_end::canceled);
    } else if (n == 0) {
        close(r, run_end::bad_reply);
    } else if (!negative) {
        take_positive(r, d, n, sink);
    } else if (d[2] == 0x33) {
        stop(r, run_end::session_lost, 0x33);
    } else if (d[2] == 0x21 && may_ask_again(r)) {
        /* busy: next() asks the same again */
    } else {
        close(r, run_end::refused, d[2]);
    }
    return run_wait::go_on;
}

/* The wait for the request that is out ran out with no answer */
inline void silence(run& r)
{
    if (!r.asked) {
        return;
    }
    r.asked = false;
    if (r.at == run_stage::closing) {
        stop(r, r.closing_as, r.nrc);
        return;
    }
    /* An unanswered 35 may have opened an upload all the same */
    r.open = r.open || r.at == run_stage::upload_open;
    if (r.canceled) {
        close(r, run_end::canceled);
    } else if (!may_ask_again(r)) {
        close(r, run_end::no_answer);
    }
}

/* The user's cancel: at once when nothing is out, else when the request
 * that is out is settled. An open upload gets its one 37 first. */
inline void cancel(run& r)
{
    if (r.at == run_stage::done || r.at == run_stage::closing) {
        return;
    }
    if (r.asked) {
        r.canceled = true;
    } else {
        close(r, run_end::canceled);
    }
}

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_RUN_H */
