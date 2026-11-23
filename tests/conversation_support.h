// File/layer: tests/conversation_support.h | test_obd2, Qt-free
// Testing: UNIT: what ConversationTest's files share: a module's table
//          scripted by the case (Guest), and a bench (Bench) that steps
//          the engine, the simulated ECU's security access and one
//          module's conversation together with the time passed in

#ifndef ECUIO_TESTS_CONVERSATION_SUPPORT_H
#define ECUIO_TESTS_CONVERSATION_SUPPORT_H

#include "engine_talk_support.h"

#include "../core/src/conversation.h"

#include <deque>
#include <functional>
#include <memory>

namespace conversation_run {

using engine_talk_test::Bytes;
using engine_talk_test::Ecu;
using engine_talk_test::kMs;

constexpr uint32_t kRx = 0x7E8;
constexpr uint32_t kTx = 0x7E0;
inline const Bytes kRow = engine_talk_test::kRow;     /* the rotation's read */
inline const Bytes kGated = engine_talk_test::kGated; /* D210, once unlocked */

/* A Guest's key_len for a key as long as its seed */
constexpr int32_t kSeedsOwn = INT32_MIN;

/* A module's code, scripted: what each export answers, and what it was
 * handed */
struct Guest {
    uint32_t rx = kRx; /* the IDs begin expects */
    uint32_t tx = kTx;
    int32_t begin_answer = 3;
    ecuio_guest_fault begin_fault = ecuio_guest_fault::none;
    std::deque<Bytes> requests; /* next_request's, one a call; none: 0 */
    bool raw = false;           /* answer raw_length once, nothing written */
    int32_t raw_length = 0;
    ecuio_guest_fault next_fault = ecuio_guest_fault::none; /* once */
    int32_t fault_kind = 0; /* accept of this kind faults with accept_fault */
    ecuio_guest_fault accept_fault = ecuio_guest_fault::none;
    int32_t key_len = kSeedsOwn;   /* the key's length to answer */
    bool wrong_key = false;        /* the key's first byte changed */
    std::function<void()> on_seed; /* the module's time with the seed */

    struct Accepted {
        int32_t kind;
        Bytes bytes;
    };
    std::vector<Accepted> accepted;
    int begins = 0;
    int polls = 0;
    int ends = 0;

    /* What accept of kind was handed, in order */
    std::vector<Bytes> of_kind(int32_t kind) const
    {
        std::vector<Bytes> out;
        for (const Accepted& a : accepted) {
            if (a.kind == kind) {
                out.push_back(a.bytes);
            }
        }
        return out;
    }
};

inline ecuio_conversation_call guest_begin(void* ctx, uint32_t rx, uint32_t tx)
{
    Guest* g = static_cast<Guest*>(ctx);
    ++g->begins;
    EXPECT_EQ(rx, g->rx);
    EXPECT_EQ(tx, g->tx);
    ecuio_conversation_call c;
    c.value = g->begin_answer;
    c.fault = g->begin_fault;
    return c;
}

inline ecuio_conversation_call guest_next(void* ctx, uint8_t* buffer)
{
    Guest* g = static_cast<Guest*>(ctx);
    ++g->polls;
    ecuio_conversation_call c;
    if (g->next_fault != ecuio_guest_fault::none) {
        c.fault = g->next_fault;
        g->next_fault = ecuio_guest_fault::none;
        return c;
    }
    if (g->raw) {
        g->raw = false;
        c.value = g->raw_length;
        return c;
    }
    if (g->requests.empty()) {
        return c;
    }
    const Bytes q = g->requests.front();
    g->requests.pop_front();
    std::memcpy(buffer, q.data(), q.size());
    c.value = static_cast<int32_t>(q.size());
    return c;
}

inline ecuio_conversation_call guest_accept(void* ctx, int32_t kind,
                                            uint8_t* buffer, uint32_t len)
{
    Guest* g = static_cast<Guest*>(ctx);
    g->accepted.push_back({kind, Bytes(buffer, buffer + len)});
    ecuio_conversation_call c;
    if (kind == g->fault_kind && g->accept_fault != ecuio_guest_fault::none) {
        c.fault = g->accept_fault;
        return c;
    }
    if (kind != ECUIO_GUEST_SEED) {
        return c;
    }
    if (g->on_seed) {
        g->on_seed();
    }
    const uint32_t n = len - 1;
    uint8_t key[64] = {};
    ecuio_sim_security_key(buffer + 1, n, key);
    if (g->wrong_key) {
        key[0] = static_cast<uint8_t>(key[0] ^ 0xFF);
    }
    std::memcpy(buffer, key, n);
    c.value = g->key_len != kSeedsOwn ? g->key_len : static_cast<int32_t>(n);
    return c;
}

inline void guest_end(void* ctx)
{
    ++static_cast<Guest*>(ctx)->ends;
}

inline ecuio_conversation_ops ops_of(Guest* g)
{
    ecuio_conversation_ops ops;
    ops.ctx = g;
    ops.begin = guest_begin;
    ops.next_request = guest_next;
    ops.accept = guest_accept;
    ops.end = guest_end;
    return ops;
}

inline ecuio_plugin_service_t service(uint8_t sid,
                                      std::initializer_list<uint8_t> subs)
{
    ecuio_plugin_service_t s = {};
    s.struct_size = sizeof s;
    s.sid = sid;
    for (uint8_t sub : subs) {
        s.subs[s.sub_count++] = sub;
    }
    return s;
}

inline ecuio_plugin_security_level_t level(uint8_t lvl, uint8_t bytes,
                                           uint32_t rx = kRx)
{
    ecuio_plugin_security_level_t l = {};
    l.struct_size = sizeof l;
    l.rx_id = rx;
    l.level = lvl;
    l.seed_bytes = bytes;
    l.key_bytes = bytes;
    l.delay_ms = 10000;
    l.key_window_ms = 1000;
    return l;
}

/* The manifest's part a case takes unless it says otherwise: 22 whole,
 * 19 02 and 2C 01 by sub-function, 27 with the simulated ECU's levels
 * (01 of 4 bytes, 03 of 16) */
inline std::vector<ecuio_plugin_service_t> default_services()
{
    return {service(0x22, {}), service(0x19, {0x02}), service(0x2C, {0x01}),
            service(0x27, {0x01, 0x02, 0x03, 0x04})};
}

inline std::vector<ecuio_plugin_security_level_t> default_levels()
{
    return {level(0x01, 4), level(0x03, 16)};
}

/* The engine (one channel, kRow in its rotation, ISO-TP whole), the
 * strict simulated ECU and one module's conversation on channel 0 */
struct Bench {
    std::unique_ptr<ecuio_request_engine> e =
        std::make_unique<ecuio_request_engine>();
    std::unique_ptr<ecuio_conversation> v =
        std::make_unique<ecuio_conversation>();
    Guest guest;
    Ecu ecu;
    int64_t now = 0;
    std::vector<Bytes> sent; /* every request on the wire, in order */
    Bytes lose_on;    /* answered 7F <sid> 7F once: the session is lost */
    Bytes pending_on; /* answered 7F <sid> 78 first, then its reply */
    Bytes fail_on;    /* its write fails */
    std::vector<ecuio_plugin_service_t> services = default_services();
    std::vector<ecuio_plugin_security_level_t> levels = default_levels();

    /* Made and begun; with begin 3 until the session is entered */
    void start()
    {
        ASSERT_NO_FATAL_FAILURE(engine_talk_test::engine(e.get()));
        ecu.now = &now;
        ecu.security.strict = true;
        ASSERT_TRUE(ecuio_conversation_init(
            v.get(), ops_of(&guest), 0, kRx, kTx, services.data(),
            static_cast<uint32_t>(services.size()), levels.data(),
            static_cast<uint32_t>(levels.size())));
        ecuio_conversation_begin(v.get(), e.get(), now);
        if (guest.begin_answer != ECUIO_SESSION_EXTENDED ||
            v->report.state != ecuio_conversation_state::talking) {
            return;
        }
        for (int i = 0;
             i < 6 && e->chans[0].session_step != ECUIO_SESSION_STATE_ENTERED;
             ++i) {
            pass();
        }
        ASSERT_EQ(e->chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
        passes(2); /* past the entry: a 7F now is a lost session */
        sent.clear();
        ecu.heard.clear();
    }

    /* The ECU's answer to q, with the case's faults */
    Bytes answer(const Bytes& q)
    {
        if (!lose_on.empty() && q == lose_on) {
            lose_on.clear();
            ecu.heard.push_back(q);
            return {0x7F, q[0], 0x7F};
        }
        return ecu(q);
    }

    /* A frame on channel 0, to the engine and the conversation */
    void frame(const Bytes& b)
    {
        (void)ecuio_engine_on_frame(e.get(), 0, b.data(),
                                    static_cast<uint16_t>(b.size()), now,
                                    nullptr);
        ecuio_conversation_frame(v.get(), e.get(), b.data(),
                                 static_cast<uint32_t>(b.size()));
    }

    /* One pass, as a worker's: the conversation's step, channel 0's next
     * request sent and answered, the step again. The bytes sent, or none. */
    Bytes pass()
    {
        ecuio_conversation_step(v.get(), e.get(), now);
        const int32_t r = ecuio_engine_due(e.get(), 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            now += ECUIO_ENGINE_GAP_US;
            ecuio_conversation_step(v.get(), e.get(), now);
            return {};
        }
        const Bytes q = engine_talk_test::wire(e.get(), 0, r);
        const bool fails = !fail_on.empty() && q == fail_on;
        ecuio_engine_sent(e.get(), 0, r, !fails, now);
        if (fails) {
            fail_on.clear();
        } else {
            sent.push_back(q);
            reply_to(q);
        }
        now += ECUIO_ENGINE_GAP_US;
        ecuio_conversation_step(v.get(), e.get(), now);
        return q;
    }

    void reply_to(const Bytes& q)
    {
        if (!pending_on.empty() && q == pending_on) {
            pending_on.clear();
            now += 2 * kMs;
            frame({0x7F, q[0], 0x78});
        }
        const Bytes reply = answer(q);
        if (reply.empty()) {
            now += ECUIO_ENGINE_WAIT_US;
            EXPECT_TRUE(ecuio_engine_expire(e.get(), 0, now));
        } else {
            now += 4 * kMs;
            frame(reply);
        }
    }

    /* Steps until the module's next request is offered, at most a second
     * on: the poll is paced while it had nothing */
    void until_offered()
    {
        for (int i = 0; i < 100 && !e->chans[0].talk.offered; ++i) {
            now += 10 * kMs;
            ecuio_conversation_step(v.get(), e.get(), now);
        }
        ASSERT_TRUE(e->chans[0].talk.offered);
    }

    void passes(int n)
    {
        for (int i = 0; i < n; ++i) {
            pass();
        }
    }

    /* Passes until a fault ends the conversation, at most n */
    void until_faulted(int n = 20)
    {
        for (int i = 0;
             i < n && v->report.state == ecuio_conversation_state::talking;
             ++i) {
            pass();
        }
    }

    /* Passes until the word's attempt has ended, at most n */
    void until_word_ended(int n = 40)
    {
        const uint32_t before = v->report.words_ended;
        for (int i = 0; i < n && v->report.words_ended == before; ++i) {
            pass();
        }
    }

    const ecuio_security::level_state& lvl(uint8_t l) const
    {
        return v->access.levels[ecuio_security::find_level(v->access, l)];
    }
};

/* How often q went on the wire */
inline size_t heard(const Bench& b, const Bytes& q)
{
    return engine_talk_test::count_of(b.sent, q);
}

} // namespace conversation_run

#endif /* ECUIO_TESTS_CONVERSATION_SUPPORT_H */
