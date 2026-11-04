// File/layer: fuzz/fuzz_request_bus.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: a requesting session's state across many requests: the
//          request engine (core/src/request_engine.h) and core's ISO-TP
//          (core/src/isotp.h) driven as the SocketCAN worker drives them
//          (ADR-0040), on a simulated bus and clock, against one ECU per
//          channel answering through tests/fake_ecu.h and core's ISO-TP
//          sender. The input picks the profile (obd2 on one or two
//          channels, packing on or off; uds-generic with DIDs polled and
//          scanned, 11-bit or 29-bit) and then, request by request, each
//          reply's fate (intact, silent, response pending first, a
//          negative code, garbage, on the other channel's ID, a wrong
//          sequence number, a stall past N_Cr, cut short, a frame twice,
//          a Single Frame longer than its frame), stray frames, the
//          clock's steps, and Read again, rescans, clears and re-probes
//          between. After every step: each request is one the engine may
//          send (04 only while clearing), a Single Frame; no worker would
//          spin (nothing out, nothing due, yet next_event_us 0); a reply
//          on one channel changes no other channel's request; every
//          report stays within its caps and its generation never goes
//          back; ISO-TP's slots stay within bounds. Then an honest drain:
//          whatever came before, intact replies bring every channel's
//          walk, identification, diagnostics and DID scan to an end
//          within simulated minutes. Two runs agree.

#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>

#include "fake_ecu.h"       /* tests/: the fakes' ECU, header-only */
#include "isotp.h"          /* core/src: pure */
#include "obd2_rates.h"     /* core/src: the obd2 set's rate classes */
#include "request_engine.h" /* core/src: the engine, with the time passed in */
#include "uds_generic.h"    /* core/src: the uds-generic builder */
#include "uri_query.h"      /* core/src: the query split */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace {

constexpr uint32_t kMaxChannels = 2;
constexpr int kSteps = 1024;
/* The honest drain's budget, in simulated time, and its step */
constexpr int64_t kDrainUs = 300LL * 1000000;
constexpr int64_t kDrainStepUs = 5000;
/* The drain checks the reports every this many steps, and at its end */
constexpr int kDrainCheckEvery = 20;

/* The bytes left of the input, taken from the front */
struct reader {
    const uint8_t* p;
    size_t n;
    bool more() const { return n > 0; }
    uint8_t byte()
    {
        if (n == 0) {
            return 0;
        }
        --n;
        return *p++;
    }
};

enum class fate : uint8_t {
    intact,
    silent,
    pending_first, /* 7F <sid> 78, then the reply */
    negative,      /* 7F <sid> <any code> instead */
    garbage,       /* a Single Frame of arbitrary bytes instead */
    crosstalk,     /* the reply on the other channel's ID */
    bad_sequence,  /* a segmented reply's last frame numbered wrong */
    stalled,       /* its last frame past N_Cr */
    cut_short,     /* its last frame never sent */
    doubled,       /* a Consecutive Frame sent twice */
    overlong_sf,   /* a Single Frame saying more bytes than it holds */
};

fate fate_of(uint8_t b, bool honest)
{
    if (honest) {
        return fate::intact;
    }
    switch (b % 16) {
    case 0:
        return fate::silent;
    case 1:
        return fate::pending_first;
    case 2:
        return fate::negative;
    case 3:
        return fate::garbage;
    case 4:
        return fate::crosstalk;
    case 5:
        return fate::bad_sequence;
    case 6:
        return fate::stalled;
    case 7:
        return fate::cut_short;
    case 8:
        return fate::doubled;
    case 9:
        return fate::overlong_sf;
    default:
        return fate::intact;
    }
}

/* One ECU: it hears requests on tx, answers on rx through core's
 * sender, and waits for the tester's Flow Control there */
struct ecu {
    uint32_t rx = 0;
    uint32_t tx = 0;
    isotp_tx sender;
    uint8_t reply[fake_ecu::kReplyMax] = {};
    size_t reply_len = 0;
    fate f = fate::intact;
    uint8_t garbage[8] = {};
    bool queued = false;  /* a reply waiting to start */
    int64_t start_us = 0; /* when it starts (late for pending_first) */
    bool held = false;    /* stalled: the last frame put off once */
};

struct bus {
    ecuio_request_engine engine;
    isotp_rx rx;
    ecu ecus[kMaxChannels];
    uint32_t chans = 0;
    uint32_t rx_ids[kMaxChannels] = {};
    uint32_t tx_ids[kMaxChannels] = {};
    uint32_t next_chan = 0;
    int64_t now = 0;
    /* the last support, identity and diagnostics generations seen */
    uint32_t support_gen[kMaxChannels] = {};
    uint32_t identity_gen[kMaxChannels] = {};
    uint32_t diag_gen[kMaxChannels] = {};
};

struct digest {
    uint32_t requests = 0;
    uint32_t messages = 0;
    uint32_t message_bytes = 0;
    uint32_t timeouts = 0;
    uint32_t generations = 0;
    bool operator==(const digest& o) const
    {
        return requests == o.requests && messages == o.messages &&
               message_bytes == o.message_bytes && timeouts == o.timeouts &&
               generations == o.generations;
    }
};

/* Large (every channel's tables, 32 KB of ISO-TP), so off the stack, as
 * the transports keep them */
bus g_bus;

/* The uds-generic sessions the profiles build, once: the builder
 * allocates, and the set's pointers reach into it */
const char* const kUdsQueries[] = {
    "did=1E1C,F190:text,F180-F18F,F19E-F1A2",
    "did=7E9:1E1C,1E1C,7E9:1E00-1E03,F187-F191",
    "did=18DAF110:F190:text,18DAF110:1E1A-1E1D",
};
constexpr size_t kUdsCount = sizeof kUdsQueries / sizeof kUdsQueries[0];

ecuio_uds_session* uds_session(size_t k)
{
    static std::unique_ptr<ecuio_uds_session> built[kUdsCount];
    if (!built[k]) {
        built[k].reset(new ecuio_uds_session());
        ecuio_uri_query q;
        if (ecuio_uri::split(kUdsQueries[k], &q) !=
                ecuio_uri::split_result::ok ||
            ecuio_uds_build(q, built[k].get()) != ECUIO_OK) {
            abort(); /* the harness's own queries */
        }
    }
    return built[k].get();
}

/* The engine as ecuio_build_requests builds it for the profile, with the
 * probe the worker starts */
void set_up(bus& b, uint8_t profile)
{
    ecuio_request_engine& e = b.engine;
    const uint32_t kind = profile % 5u;
    const bool pack = (profile & 0x80u) != 0;
    if (kind < 2) {
        b.chans = kind + 1;
        b.rx_ids[0] = 0x7E8;
        b.tx_ids[0] = 0x7E0;
        b.rx_ids[1] = 0x7E9;
        b.tx_ids[1] = 0x7E1;
        ecuio_engine_init(&e, b.chans);
        (void)ecuio_engine_derive(&e, ecuio_obd2_signal_set(), b.rx_ids,
                                  b.chans, nullptr);
        ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
        ecuio_engine_set_duplicates(&e, &ECUIO_OBD2_DUPLICATE_PIDS[0][0],
                                    sizeof(ECUIO_OBD2_DUPLICATE_PIDS) /
                                        sizeof(ECUIO_OBD2_DUPLICATE_PIDS[0]));
    } else {
        const ecuio_uds_session& u = *uds_session(kind - 2);
        b.chans = 0;
        for (const ecuio_channel_decl_t& c : u.channels) {
            if (b.chans < kMaxChannels) {
                b.rx_ids[b.chans] = c.rx_id;
                b.tx_ids[b.chans] = c.tx_id;
                ++b.chans;
            }
        }
        ecuio_engine_init(&e, b.chans);
        const ecuio_signal_set_t head = ecuio_uds_polled_set(u);
        (void)ecuio_engine_derive(&e, &head, b.rx_ids, b.chans, nullptr);
        for (const ecuio_uds_scan_target& s : u.scan) {
            uint32_t c = 0;
            for (uint32_t i = 0; i < b.chans; ++i) {
                if (b.rx_ids[i] == s.rx_id) {
                    c = i;
                }
            }
            (void)ecuio_engine_scan_add(&e, c, s.did);
        }
        e.scan_first = (profile & 0x40u) != 0;
    }
    e.pack = pack;
    ecuio_engine_identify(&e);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    ecuio_engine_probe(&e, 0);
    for (uint32_t c = 0; c < b.chans; ++c) {
        b.ecus[c].rx = b.rx_ids[c];
        b.ecus[c].tx = b.tx_ids[c];
    }
}

void check_slots(const isotp_rx& r)
{
    for (const isotp_rx_slot& s : r.slots) {
        if (s.active && (s.have > s.expected || s.expected < 8 ||
                         s.expected > ECUIO_ISOTP_MAX || s.next_seq > 15)) {
            abort();
        }
    }
}

/* Every report within its caps, its generation never going back */
void check_reports(bus& b, digest* d)
{
    for (uint32_t c = 0; c < b.chans; ++c) {
        ecuio_channel_support_t sup;
        ecuio_engine_support(&b.engine, c, &sup);
        if (sup.generation < b.support_gen[c] ||
            sup.scan_done > sup.scan_total ||
            static_cast<uint32_t>(sup.scan_found) + sup.scan_refused >
                sup.scan_done) {
            abort();
        }
        d->generations += sup.generation != b.support_gen[c];
        b.support_gen[c] = sup.generation;

        ecuio_channel_identity_t id;
        ecuio_engine_identity(&b.engine, c, &id);
        if (id.generation < b.identity_gen[c] ||
            id.items > ECUIO_IDENTITY_MAX_ITEMS) {
            abort();
        }
        b.identity_gen[c] = id.generation;
        for (uint32_t i = 0; i < id.items; ++i) {
            const ecuio_identity_item_t& it = id.list[i];
            if (it.outcome > ECUIO_IDENTITY_UNANSWERED ||
                memchr(it.text, '\0', sizeof it.text) == nullptr) {
                abort();
            }
        }

        ecuio_channel_diagnostics_t dg;
        ecuio_engine_diagnostics(&b.engine, c, &dg);
        if (dg.generation < b.diag_gen[c] ||
            dg.monitor_count > ECUIO_DIAG_MAX_MONITORS ||
            dg.freeze_count > ECUIO_DIAG_MAX_FREEZE) {
            abort();
        }
        b.diag_gen[c] = dg.generation;
        for (uint32_t k = 0; k < ECUIO_DIAG_LISTS; ++k) {
            const ecuio_dtc_list_t& l = dg.lists[k];
            if (l.outcome > ECUIO_DIAG_UNANSWERED ||
                l.held > ECUIO_DIAG_MAX_DTCS || l.held > l.reported) {
                abort();
            }
        }
        const ecuio_engine_channel& ch = b.engine.chans[c];
        if (ch.diag_pending > ch.diag_count ||
            ch.ident_pending > ch.ident_count) {
            abort();
        }
    }
}

/* A whole message the tester's receiver put together on channel c, to
 * the engine: no other channel's request or report may change */
void to_engine(bus& b, uint32_t c, const isotp_message& m, digest* d)
{
    if (m.len == 0 || m.len > ECUIO_ISOTP_MAX || m.id != b.rx_ids[c]) {
        abort();
    }
    int32_t out[kMaxChannels];
    int64_t deadline[kMaxChannels];
    uint32_t gen[kMaxChannels];
    for (uint32_t k = 0; k < b.chans; ++k) {
        out[k] = b.engine.chans[k].outstanding;
        deadline[k] = b.engine.chans[k].deadline_us;
        gen[k] = b.engine.chans[k].generation;
    }
    (void)ecuio_engine_on_frame(&b.engine, c, m.data,
                                static_cast<uint16_t>(m.len), b.now, nullptr);
    for (uint32_t k = 0; k < b.chans; ++k) {
        if (k != c && (b.engine.chans[k].outstanding != out[k] ||
                       b.engine.chans[k].deadline_us != deadline[k] ||
                       b.engine.chans[k].generation != gen[k])) {
            abort();
        }
    }
    ++d->messages;
    d->message_bytes += m.len;
}

/* A frame on the bus, heard by the tester and by every ECU */
void deliver(bus& b, const isotp_can& f, digest* d)
{
    for (uint32_t c = 0; c < b.chans; ++c) {
        if (f.id != b.rx_ids[c]) {
            continue;
        }
        isotp_message m;
        const isotp_result res = isotp_take(b.rx, f, b.now, &m);
        if (res == isotp_result::first) {
            /* the worker's Flow Control, at once; the ECU waiting on that
             * ID takes it */
            const isotp_can fc = isotp_flow_control(
                isotp_flow_control_id(b.rx_ids[c], b.tx_ids[c]));
            for (uint32_t k = 0; k < b.chans; ++k) {
                if (b.ecus[k].tx == fc.id) {
                    (void)isotp_tx_flow(b.ecus[k].sender, fc, b.now);
                }
            }
        } else if (res == isotp_result::complete) {
            to_engine(b, c, m, d);
        }
        break;
    }
    check_slots(b.rx);
}

/* The ECU's reply under way: every frame due at now */
void pump(bus& b, ecu& u, digest* d)
{
    if (u.queued && b.now >= u.start_us) {
        u.queued = false;
        isotp_can f;
        if (u.f == fate::garbage) {
            f.id = u.rx;
            f.len = 8;
            memcpy(f.data, u.garbage, 8);
            deliver(b, f, d);
            return;
        }
        if (u.reply_len == 0 || u.reply_len > ECUIO_ISOTP_MAX ||
            !isotp_tx_start(u.sender, u.rx, u.reply,
                            static_cast<uint16_t>(u.reply_len), b.now, &f)) {
            return; /* nothing to send, or past ISO-TP's longest */
        }
        if (u.f == fate::overlong_sf && f.data[0] < 7) {
            f.data[0] = 7; /* says more than it holds */
            f.len = static_cast<uint8_t>(1 + u.reply_len);
        }
        if (u.f == fate::crosstalk && b.chans > 1) {
            f.id = (u.rx == b.rx_ids[0]) ? b.rx_ids[1] : b.rx_ids[0];
        }
        u.held = false;
        deliver(b, f, d);
    }
    while (u.sender.state != isotp_tx_state::idle) {
        const bool last = u.sender.state == isotp_tx_state::sending &&
                          u.sender.len - u.sender.sent <= 7;
        if (last && u.f == fate::stalled && !u.held) {
            u.held = true;
            u.sender.due_us = b.now + ECUIO_ISOTP_N_CR_US + 10000;
        }
        isotp_can f;
        const uint8_t seq = u.sender.seq;
        const isotp_tx_step st = isotp_tx_next(u.sender, b.now, &f);
        if (st != isotp_tx_step::frame) {
            return; /* later, or the tester's Flow Control never came */
        }
        const bool was_last = u.sender.state == isotp_tx_state::idle;
        if (was_last && u.f == fate::cut_short) {
            return;
        }
        if (was_last && u.f == fate::bad_sequence) {
            f.data[0] = static_cast<uint8_t>(0x20 | ((seq + 1) & 0x0F));
        }
        if (u.f == fate::crosstalk && b.chans > 1) {
            f.id = (u.rx == b.rx_ids[0]) ? b.rx_ids[1] : b.rx_ids[0];
        }
        deliver(b, f, d);
        if (u.f == fate::doubled && seq == 1) {
            deliver(b, f, d);
        }
    }
}

/* The request the tester sent on channel c reaches its ECU, which
 * answers by the fate the input gives */
void ecu_hears(bus& b, uint32_t c, const ecuio_request& q, reader& in,
               bool honest, digest* d)
{
    ecu& u = b.ecus[c];
    u.f = fate_of(in.byte(), honest);
    u.reply_len = 0;
    u.queued = false;
    u.sender = isotp_tx{};
    size_t n = 0;
    if (!fake_ecu::answer(q.bytes, q.len, -1, u.reply, &n)) {
        return; /* the fake answers nothing */
    }
    u.reply_len = n;
    u.start_us = b.now + 1000;
    switch (u.f) {
    case fate::silent:
        return;
    case fate::pending_first: {
        /* 7F <sid> 78 now, the reply 200 ms later */
        isotp_can p;
        p.id = u.rx;
        p.len = 8;
        memset(p.data, ECUIO_ISOTP_PAD, 8);
        p.data[0] = 3;
        p.data[1] = 0x7F;
        p.data[2] = q.bytes[0];
        p.data[3] = 0x78;
        deliver(b, p, d);
        u.start_us = b.now + 200000;
        break;
    }
    case fate::negative:
        u.reply[0] = 0x7F;
        u.reply[1] = q.bytes[0];
        u.reply[2] = in.byte();
        u.reply_len = 3;
        break;
    case fate::garbage:
        for (uint8_t& x : u.garbage) {
            x = in.byte();
        }
        break;
    default:
        break;
    }
    u.queued = true;
}

/* The worker's turn: timeouts, then one request out if one is due; the
 * checks on what goes out and on the wait that follows */
void tester_turn(bus& b, reader& in, bool honest, digest* d)
{
    ecuio_request_engine& e = b.engine;
    for (uint32_t c = 0; c < b.chans; ++c) {
        if (ecuio_engine_expire(&e, c, b.now)) {
            ++d->timeouts;
        }
    }
    uint32_t c = 0;
    int32_t r = ECUIO_ENGINE_NONE;
    for (uint32_t k = 0; k < b.chans && r == ECUIO_ENGINE_NONE; ++k) {
        c = (b.next_chan + k) % b.chans;
        r = ecuio_engine_due(&e, c, b.now);
    }
    if (r != ECUIO_ENGINE_NONE) {
        b.next_chan = (c + 1) % b.chans;
        const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
        if (q.len == 0 || q.len > 7) {
            abort(); /* every request here is a Single Frame */
        }
        if (q.bytes[0] == 0x04 ? !e.chans[c].clearing
                               : !ecuio_engine_service_allowed(q.bytes[0])) {
            abort();
        }
        isotp_tx sender;
        isotp_can f;
        if (!isotp_tx_start(sender, b.tx_ids[c], q.bytes,
                            static_cast<uint16_t>(q.len), b.now, &f) ||
            sender.state != isotp_tx_state::idle) {
            abort();
        }
        ecuio_engine_sent(&e, c, r, true, b.now);
        ++d->requests;
        ecu_hears(b, c, q, in, honest, d);
    }
    /* A worker sleeps until next_event_us: 0 with nothing out and
     * nothing due would spin it */
    bool out = false;
    for (uint32_t k = 0; k < b.chans; ++k) {
        out = out || e.chans[k].outstanding != ECUIO_ENGINE_NONE;
    }
    if (!out && r == ECUIO_ENGINE_NONE &&
        ecuio_engine_next_event_us(&e, b.now) == 0) {
        abort(); /* every channel was asked above, and none had one */
    }
}

/* A frame from no ECU of the session, or on one's ID */
void stray(bus& b, reader& in, digest* d)
{
    isotp_can f;
    const uint8_t which = in.byte();
    f.id = (which & 1) && b.chans > 0 ? b.rx_ids[(which >> 1) % b.chans]
                                      : 0x700u + in.byte();
    f.len = static_cast<uint8_t>(in.byte() % 9);
    for (uint8_t i = 0; i < f.len; ++i) {
        f.data[i] = in.byte();
    }
    deliver(b, f, d);
}

void operate(bus& b, uint8_t op)
{
    ecuio_request_engine& e = b.engine;
    switch (op % 4) {
    case 0:
        ecuio_engine_diagnose_again(&e);
        break;
    case 1:
        ecuio_engine_rescan(&e);
        break;
    case 2: {
        /* the channel's report generation, or one off it, with any of
         * the caller's words */
        const uint32_t c = (op >> 2) % (b.chans ? b.chans : 1);
        ecuio_channel_diagnostics_t dg;
        ecuio_engine_diagnostics(&e, c, &dg);
        (void)ecuio_engine_clear(&e, c, dg.generation + (op >> 4) % 3 - 1,
                                 (op >> 6) & 3u);
        break;
    }
    default:
        /* the link back after a failure: probe and rescan again */
        ecuio_engine_probe(&e, b.now);
        ecuio_engine_rescan(&e);
        break;
    }
}

/* One step of simulated time: the clock, the tester, the ECUs, ISO-TP's
 * expiry, the checks */
void step(bus& b, reader& in, bool honest, int64_t dt, digest* d,
          bool reports = true)
{
    b.now += dt;
    tester_turn(b, in, honest, d);
    for (uint32_t c = 0; c < b.chans; ++c) {
        pump(b, b.ecus[c], d);
    }
    (void)isotp_expire(b.rx, b.now);
    for (const isotp_rx_slot& s : b.rx.slots) {
        if (s.active && b.now - s.last_us > ECUIO_ISOTP_N_CR_US) {
            abort();
        }
    }
    if (reports) {
        check_reports(b, d);
    }
}

/* Whether channel c has nothing left but its rotation */
bool settled(const ecuio_request_engine& e, uint32_t c)
{
    const ecuio_engine_channel& ch = e.chans[c];
    return !ch.walking && !ch.identifying && ch.ident_pending == 0 &&
           !ch.diagnosing && ch.diag_pending == 0 && !ch.scanning &&
           ch.scan_pending == 0 && !ch.clearing &&
           ch.clear_step == ECUIO_CLEAR_STEP_NONE;
}

/* Intact replies until every channel has nothing left but its rotation,
 * within the budget */
void drain(bus& b, digest* d)
{
    reader none{nullptr, 0};
    const int64_t until = b.now + kDrainUs;
    bool done = false;
    for (int i = 1; !done && b.now < until; ++i) {
        step(b, none, true, kDrainStepUs, d, i % kDrainCheckEvery == 0);
        done = true;
        for (uint32_t c = 0; c < b.chans; ++c) {
            done = done && settled(b.engine, c);
        }
    }
    if (!done) {
        abort();
    }
    check_reports(b, d);
}

void run(const uint8_t* data, size_t size, digest* d)
{
    if (size == 0) {
        return;
    }
    fake_ecu::reset_diagnostics();
    bus& b = g_bus;
    b.rx = isotp_rx{};
    b.rx.timed = true;
    for (ecu& u : b.ecus) {
        u = ecu{};
    }
    b.next_chan = 0;
    b.now = 0;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        b.support_gen[c] = b.identity_gen[c] = b.diag_gen[c] = 0;
    }
    set_up(b, data[0]);
    reader in{data + 1, size - 1};
    for (int i = 0; i < kSteps && in.more(); ++i) {
        const uint8_t op = in.byte();
        if ((op & 0x0F) == 0x0F) {
            stray(b, in, d);
            continue;
        }
        if ((op & 0x0F) == 0x0E) {
            operate(b, in.byte());
            continue;
        }
        /* 0 to 51 ms, or up to 5.1 s */
        int64_t dt = static_cast<int64_t>(in.byte()) * 200;
        if (op & 0x10) {
            dt *= 100;
        }
        step(b, in, false, dt, d);
    }
    /* The honest drain: whatever came before, intact replies end every
     * one-shot within the budget; then, after a Read again (rescan and
     * diagnose again, which re-ask what went unanswered), every
     * identification item is answered or refused and every report
     * complete */
    drain(b, d);
    ecuio_engine_rescan(&b.engine);
    ecuio_engine_diagnose_again(&b.engine);
    drain(b, d);
    for (uint32_t c = 0; c < b.chans; ++c) {
        ecuio_channel_identity_t id;
        ecuio_engine_identity(&b.engine, c, &id);
        for (uint32_t i = 0; i < id.items; ++i) {
            if (id.list[i].outcome != ECUIO_IDENTITY_ANSWERED &&
                id.list[i].outcome != ECUIO_IDENTITY_REFUSED) {
                abort();
            }
        }
        ecuio_channel_diagnostics_t dg;
        ecuio_engine_diagnostics(&b.engine, c, &dg);
        if ((dg.flags & ECUIO_DIAG_COMPLETE) == 0) {
            abort();
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::unique_ptr<uint8_t[]> copy(new uint8_t[size ? size : 1]);
    if (size > 0) {
        memcpy(copy.get(), data, size);
    }
    digest a, b;
    run(copy.get(), size, &a);
    run(copy.get(), size, &b);
    if (!(a == b)) {
        abort();
    }
    return 0;
}
