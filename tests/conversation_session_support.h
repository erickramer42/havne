// File/layer: tests/conversation_session_support.h | test_core, Qt-free
// Testing: UNIT: what ConversationSessionTest's files share: a guest
//          module that answers the simulated ECU's made-up challenge, a
//          conversation plugin carrying it, and a bench that drives a
//          bound session's worker half (request_conversation.h) on an
//          engine of its own with the time passed in
//
// The session is created over the plugin's set as mock://, then named a
// requesting transport's so the bind takes it: the bench is its worker.

#ifndef ECUIO_TESTS_CONVERSATION_SESSION_SUPPORT_H
#define ECUIO_TESTS_CONVERSATION_SESSION_SUPPORT_H

#include "engine_talk_support.h"
#include "plugin_conversation_support.h"

#include "../core/src/request_conversation.h"

#include <ecuio/internal.h>
#include <ecuio/plugin.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>

namespace conversation_session {

using engine_talk_test::Bytes;
using engine_talk_test::Ecu;

/* The guest's exchange buffer, and its scratch for a key */
constexpr uint8_t kBufferLeb[2] = {0x80, 0x08}; /* 1024 */

/* accept(kind, len): 0 but for a seed ([level][seed], n = len - 1),
 * whose key is the sim's transform (sim_security.h), xored with flip:
 * key[i] = seed[n-1-i] ^ "HAVNE"[i % 5] ^ flip, written at the buffer */
inline wasm_builder::func accept_func(uint8_t flip)
{
    using namespace wasm_builder;
    func f;
    f.params = {I32, I32};
    f.results = {I32};
    f.exported = "accept";
    f.i32_locals = 2; /* 2: n, 3: i */
    // clang-format off
    f.body = {
        0x20, 0, I32_CONST, 3, 0x47, 0x04, VOID_BLOCK, I32_CONST, 0, 0x0F,
        0x0B, /* kind != 3: 0 */
        I32_CONST, 0xB8, 0x17, I32_CONST, 0xC8, 0x00, 0x3A, 0, 0, /* 'H' */
        I32_CONST, 0xB9, 0x17, I32_CONST, 0xC1, 0x00, 0x3A, 0, 0, /* 'A' */
        I32_CONST, 0xBA, 0x17, I32_CONST, 0xD6, 0x00, 0x3A, 0, 0, /* 'V' */
        I32_CONST, 0xBB, 0x17, I32_CONST, 0xCE, 0x00, 0x3A, 0, 0, /* 'N' */
        I32_CONST, 0xBC, 0x17, I32_CONST, 0xC5, 0x00, 0x3A, 0, 0, /* 'E' */
        0x20, 1, I32_CONST, 1, I32_SUB, 0x21, 2, I32_CONST, 0, 0x21, 3,
        /* scratch[i] = seed[n-1-i] ^ word[i % 5] ^ flip */
        0x02, VOID_BLOCK, 0x03, VOID_BLOCK, 0x20, 3, 0x20, 2, 0x4F, 0x0D, 1,
        I32_CONST, 0x80, 0x10, 0x20, 3, I32_ADD,          /* 2048 + i */
        I32_CONST, 0x80, 0x08, 0x20, 2, I32_ADD, 0x20, 3, I32_SUB, 0x2D, 0,
        0,                                                 /* seed byte */
        I32_CONST, 0xB8, 0x17, 0x20, 3, I32_CONST, 5, 0x70, I32_ADD, 0x2D, 0,
        0, 0x73,                                           /* ^ word */
        I32_CONST, static_cast<uint8_t>(flip & 0x3F), 0x73, 0x3A, 0, 0,
        0x20, 3, I32_CONST, 1, I32_ADD, 0x21, 3, 0x0C, 0, 0x0B, 0x0B,
        /* buffer[i] = scratch[i] */
        I32_CONST, 0, 0x21, 3, 0x02, VOID_BLOCK, 0x03, VOID_BLOCK, 0x20, 3,
        0x20, 2, 0x4F, 0x0D, 1, I32_CONST, 0x80, 0x08, 0x20, 3, I32_ADD,
        I32_CONST, 0x80, 0x10, 0x20, 3, I32_ADD, 0x2D, 0, 0, 0x3A, 0, 0,
        0x20, 3, I32_CONST, 1, I32_ADD, 0x21, 3, 0x0C, 0, 0x0B, 0x0B,
        0x20, 2};
    // clang-format on
    return f;
}

/* What a case changes in the guest */
struct GuestShape {
    uint8_t abi = 1;
    bool buffer_inside = true;
    uint8_t begin = 3; /* begin's answer */
    uint8_t flip = 0;  /* a wrong key when not 0 (below 0x40) */
    bool begin_spins = false; /* begin counts 20,000 down first */
    bool long_request = false; /* next_request writes AB, answers 5,000 */
    bool end_marks = false;    /* end stores 1 at kEndMark */
};

/* Where an end_marks guest's end leaves its 1 */
constexpr uint32_t kEndMark = 3500;

/* The guest ABI's six exports: begin answers shape.begin, next_request
 * nothing, accept the key of each seed */
inline wasm_builder::module unlocking_guest(const GuestShape& shape = {})
{
    using namespace wasm_builder;
    module m;
    m.memory_max = 1;
    m.funcs.push_back(plain_func("ecuio_abi", {I32_CONST, shape.abi}));
    m.funcs.push_back(
        shape.buffer_inside
            ? plain_func("ecuio_buffer",
                         {I32_CONST, kBufferLeb[0], kBufferLeb[1]})
            : plain_func("ecuio_buffer",
                         {I32_CONST, 0x80, 0xF0, 0x03})); /* 63488 */
    func two;
    two.params = {I32, I32};
    two.results = {I32};
    two.body = {I32_CONST, shape.begin};
    if (shape.begin_spins) {
        /* local 2 from 20,000 down to 0, about 100,000 units of work */
        two.i32_locals = 1;
        two.body = {I32_CONST, 0xA0, 0x9C, 0x01, 0x21, 2, 0x03, VOID_BLOCK,
                    0x20, 2, I32_CONST, 1, I32_SUB, 0x22, 2, 0x0D, 0, 0x0B,
                    I32_CONST, shape.begin};
    }
    two.exported = "begin";
    m.funcs.push_back(two);
    m.funcs.push_back(
        shape.long_request
            ? plain_func("next_request", {I32_CONST, kBufferLeb[0],
                                          kBufferLeb[1], I32_CONST, 0xAB, 0x01,
                                          0x3A, 0, 0, I32_CONST, 0x88, 0x27})
            : plain_func("next_request", {I32_CONST, 0}));
    m.funcs.push_back(accept_func(shape.flip));
    m.funcs.push_back(
        shape.end_marks
            ? plain_func("end", {I32_CONST, 0xAC, 0x1B, I32_CONST, 1, 0x3A, 0, 0},
                         false)
            : plain_func("end", {}, false));
    return m;
}

/* A plugin whose one ECU is 7E8/7E0 at level 01 (the sim's 4 bytes) */
inline conversation_test::Made made(const GuestShape& shape = {},
                                    const char* budget = "250000")
{
    conversation_test::Made m;
    m.module = unlocking_guest(shape).build();
    m.group.budget = budget;
    m.ecus = R"([{"id": "pcm", "rx": "7E8"}])";
    m.group.ecus = R"(["pcm"])";
    m.group.security =
        R"([{"ecu": "pcm", "level": "01", "seed_bytes": 4, "key_bytes": 4, )"
        R"("delay_ms": 1000, "key_window_ms": 1000}])";
    return m;
}

struct SessionFree {
    void operator()(ecuio_session_t* s) const { ecuio_session_destroy(s); }
};
using Session = std::unique_ptr<ecuio_session_t, SessionFree>;

/* The door, opened for a case's life */
struct DoorOpen {
    DoorOpen() { ecuio_test_conversation_binding(1); }
    ~DoorOpen() { ecuio_test_conversation_binding(0); }
    DoorOpen(const DoorOpen&) = delete;
    DoorOpen& operator=(const DoorOpen&) = delete;
};

/* The plugin registered and a session over its set, named a requesting
 * transport's (uri) so the bind takes it */
struct Bound {
    conversation_test::Plugin plugin;
    Session session;
};

inline void make_session(Bound* out, const conversation_test::Made& m,
                         const char* uri = "elm327://bench")
{
    out->plugin = conversation_test::load_packed(m.json());
    ASSERT_NE(out->plugin, nullptr) << ecuio_last_error_detail();
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(out->plugin.get(), 0, name, sizeof name),
              ECUIO_OK);
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(
                  (std::string("mock://x?profile=") + name).c_str(), &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    out->session.reset(s);
    s->uri = uri;
    ASSERT_FALSE(s->module_rx.empty());
    ASSERT_EQ(s->module_rx[0], 0x7E8u);
    ASSERT_EQ(s->module_tx[0], 0x7E0u);
}

/* The worker's half on channel 0 of an engine of its own, against the
 * simulated ECU's security access, with a row for the gated DID that
 * answers 7F 22 33 until a level is unlocked */
struct Bench {
    ecuio_session_t* s = nullptr;
    std::unique_ptr<ecuio_request_engine> e =
        std::make_unique<ecuio_request_engine>();
    Ecu ecu;
    int64_t now = 0;
    std::vector<Bytes> sent;
    uint32_t hook_channel = 0; /* the channel the frame hook is told of */

    void start(ecuio_session_t* session)
    {
        s = session;
        ASSERT_NO_FATAL_FAILURE(engine_talk_test::engine(e.get()));
        ASSERT_EQ(
            ecuio_engine_add(e.get(), 0, engine_talk_test::kGated.data(), 3),
            ecuio_engine_add_result::added);
        e->probe = true;
        ecu.now = &now;
        ecu.security.strict = true;
        ecuio_conversations_start(s, e.get());
    }

    Bytes answer(const Bytes& q)
    {
        if (q == engine_talk_test::kGated && ecu.security.unlocked == 0) {
            ecu.heard.push_back(q);
            return {0x7F, 0x22, 0x33};
        }
        return ecu(q);
    }

    void frame(const Bytes& b)
    {
        (void)ecuio_engine_on_frame(e.get(), 0, b.data(),
                                    static_cast<uint16_t>(b.size()), now,
                                    nullptr);
        ecuio_conversations_frame(s, e.get(), hook_channel, b.data(),
                                  static_cast<uint32_t>(b.size()));
    }

    /* One pass as a worker's: the step, channel 0's request answered */
    void pass()
    {
        ecuio_step_conversations(s, e.get(), now);
        const int32_t r = ecuio_engine_due(e.get(), 0, now);
        if (r != ECUIO_ENGINE_NONE) {
            const Bytes q = engine_talk_test::wire(e.get(), 0, r);
            ecuio_engine_sent(e.get(), 0, r, true, now);
            sent.push_back(q);
            const Bytes reply = answer(q);
            if (reply.empty()) {
                now += ECUIO_ENGINE_WAIT_US;
                (void)ecuio_engine_expire(e.get(), 0, now);
            } else {
                now += 4 * engine_talk_test::kMs;
                frame(reply);
            }
        }
        now += ECUIO_ENGINE_GAP_US;
        ecuio_step_conversations(s, e.get(), now);
    }

    void passes(int n)
    {
        for (int i = 0; i < n; ++i) {
            pass();
        }
    }

    ecuio_conversation_report report(uint32_t* generation = nullptr) const
    {
        ecuio_conversation_report r;
        uint32_t g = 0;
        EXPECT_TRUE(ecuio_conversation_report_of(s, 0, &r, &g));
        if (generation != nullptr) {
            *generation = g;
        }
        return r;
    }

    /* Passes until the word's attempt has ended, at most n */
    void until_word_ended(uint32_t before, int n = 60)
    {
        for (int i = 0; i < n && report().words_ended == before; ++i) {
            pass();
        }
    }

    size_t heard(const Bytes& q) const
    {
        size_t k = 0;
        for (const Bytes& b : sent) {
            k += b == q;
        }
        return k;
    }
};

/* made() in format 3, naming its publisher; passive: its variant's one
 * signal waits on a need nothing meets, so a session over it asks nothing */
inline conversation_test::Made made3(bool passive = false)
{
    conversation_test::Made m = made();
    m.format = "3";
    m.publisher = R"({"name": "Made-up Tools", "contact": "c", )"
                  R"("license": "MIT", "terms": "Use at your own risk."})";
    if (passive) {
        m.data = R"({"havne_signals": 3, "needs": ["x"], "signals": )"
                 R"([{"key": "c", "name": "C", "kind": "computed", )"
                 R"("expr": "x"}]})";
    }
    return m;
}

/* p's fingerprint, its 32 bytes */
inline std::string print_of(const ecuio_plugin_t* p)
{
    ecuio_plugin_fingerprint_t f{};
    f.struct_size = sizeof f;
    EXPECT_EQ(ecuio_plugin_fingerprint(p, &f), ECUIO_OK);
    return std::string(reinterpret_cast<const char*>(f.fingerprint), 32);
}

/* p's record given in a store of the case's own (file in the test
 * folder, gone after) and applied to session s */
inline void give_and_apply(const char* file, const ecuio_plugin_t* p,
                           ecuio_session_t* s)
{
    const std::string path = testing::TempDir() + file;
    std::remove(path.c_str());
    ecuio_consent_store_t* store = nullptr;
    ASSERT_EQ(ecuio_consent_store_open(path.c_str(), &store), ECUIO_OK);
    EXPECT_EQ(ecuio_consent_store_give(store, p, 1795132800), ECUIO_OK)
        << ecuio_last_error_detail();
    const std::string print = print_of(p);
    EXPECT_EQ(ecuio_session_apply_consent(
                  s, store, reinterpret_cast<const uint8_t*>(print.data())),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_consent_store_close(store);
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
}

inline uint32_t wants_consent(const ecuio_session_t* s)
{
    uint32_t out = 7;
    EXPECT_EQ(ecuio_session_wants_consent(s, &out), ECUIO_OK);
    return out;
}

/* The first 16 hex digits of p's fingerprint, as a refusal and the
 * session logs show it */
inline std::string print_prefix_of(const ecuio_plugin_t* p)
{
    static const char digits[] = "0123456789abcdef";
    const std::string print = print_of(p);
    std::string out;
    for (size_t i = 0; i < 8; ++i) {
        const auto b = static_cast<uint8_t>(print[i]);
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

} // namespace conversation_session

#endif /* ECUIO_TESTS_CONVERSATION_SESSION_SUPPORT_H */
