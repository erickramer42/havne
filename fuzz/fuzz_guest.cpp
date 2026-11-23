// File/layer: fuzz/fuzz_guest.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_guest
//
// A plugin's WebAssembly module, any bytes, bound to a session's engine
// ECU channel (conversation_bind.h) and stepped as a worker steps it
// (request_conversation.h) against the strict simulated ECU's security
// access (sim_security.h), the caller's word through the public unlock
// call. The first four bytes pick the adapter's bound, the budget, the
// key window, the word's level and pass, and the replies' fates; the
// rest is the module. The harness aborts on any break of:
//   - the bind: OK with the channel bound, or LOAD_FAILED or out of
//     memory with nothing held;
//   - the wire: a 27 only as core builds it (a seed for a word given and
//     not refused, never inside a delay the wire has shown; a key
//     straight after its seed's answer, of its level's length, once for
//     each seed), any other request of the module's inside the guest's
//     list, the manifest and the channel's bound;
//   - the report: a fault, a decline or an end stays;
//   - the runtime: every byte the bind took given back at the unbind.

#include "conversation_bind.h"     /* core/src: the bind */
#include "request_conversation.h"  /* core/src: the worker's steps */
#include "session.h"               /* core/src: the session's parts */
#include "sim_security.h"          /* core/src: the simulated ECU */
#include "tier2_ceiling.h"
#include "wasm_sandbox.h"

#include "ecuio/core.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace {

constexpr int kPasses = 300;
constexpr int64_t kMs = 1000;
constexpr uint32_t kRx = 0x7E8; /* the mock's engine ECU, channel 0 */
constexpr uint32_t kTx = 0x7E0;
constexpr int64_t kDelayUs = 10000 * kMs; /* every level's, as below */
constexpr uint32_t kBudgets[8] = {1,     16,     256,    4096,
                                  65536, 100000, 200000, 250000};
constexpr uint8_t kLevels[4] = {0x01, 0x03, 0x05, 0x00}; /* 05: undeclared */

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* The session the bind takes: a mock's, made once, named a requesting
 * transport's so the bind goes on */
ecuio_session_t* session()
{
    static ecuio_session_t* s = [] {
        ecuio_session_t* made = nullptr;
        check(ecuio_session_create("mock://fuzz?diagnose=0", &made) ==
              ECUIO_OK);
        check(!made->module_rx.empty() && made->module_rx[0] == kRx &&
              made->module_tx[0] == kTx);
        made->uri = "elm327://fuzz";
        made->conversation_consent = true;
        return made;
    }();
    return s;
}

ecuio_plugin_service_t service(uint8_t sid, uint8_t sub_count,
                               const uint8_t* subs)
{
    ecuio_plugin_service_t v = {};
    v.struct_size = sizeof v;
    v.sid = sid;
    v.sub_count = sub_count;
    for (uint8_t k = 0; k < sub_count; ++k) {
        v.subs[k] = subs[k];
    }
    return v;
}

ecuio_plugin_security_level_t level(uint8_t lvl, uint8_t bytes,
                                    uint32_t window_ms)
{
    ecuio_plugin_security_level_t l = {};
    l.struct_size = sizeof l;
    l.rx_id = kRx;
    l.level = lvl;
    l.seed_bytes = bytes;
    l.key_bytes = bytes;
    l.delay_ms = static_cast<uint32_t>(kDelayUs / kMs);
    l.key_window_ms = window_ms;
    return l;
}

/* The manifest's part over the module's bytes: the services declared()
 * reads and the simulated ECU's two levels */
std::shared_ptr<ecuio_conversation_part>
part_of(const uint8_t* module, size_t n, uint32_t budget, uint32_t window)
{
    auto p = std::make_shared<ecuio_conversation_part>();
    p->plugin_id = "fuzz";
    p->plugin_version = "1.0.0";
    p->code.assign(module, module + n);
    p->budget = budget;
    p->ecus.push_back({"pcm", kRx, kTx});
    const uint8_t s19[1] = {0x02};
    const uint8_t s2c[1] = {0x01};
    const uint8_t s27[4] = {0x01, 0x02, 0x03, 0x04};
    p->services = {service(0x22, 0, nullptr), service(0x19, 1, s19),
                   service(0x2C, 1, s2c), service(0x27, 4, s27)};
    p->levels = {level(0x01, 4, window), level(0x03, 16, window)};
    return p;
}

bool declared(const uint8_t* q, uint32_t len)
{
    switch (q[0]) {
    case 0x22:
        return true;
    case 0x19:
        return len >= 2 && q[1] == 0x02;
    case 0x2C:
        return len >= 2 && q[1] == 0x01;
    default:
        return false;
    }
}

struct harness {
    ecuio_session_t* s = nullptr;
    std::unique_ptr<ecuio_request_engine> e =
        std::make_unique<ecuio_request_engine>();
    ecuio_sim_security sec;
    bool extended = false;
    int64_t now = 0;
    uint32_t fate = 0;     /* the replies' fates, a generator's state */
    uint32_t words = 0;    /* words the conversation took */
    uint32_t refused = 0;  /* of those, refused: no seed for them */
    uint32_t seeds = 0;    /* seeds on the wire */
    uint8_t seed_open = 0; /* the level whose seed the last reply gave */
    bool key_sent = false; /* a key went, or lapsed, for that seed */
    int64_t blocked_until[256] = {};
    uint8_t q[ECUIO_ENGINE_PDU_MAX] = {}; /* the talk out, as sent */
    uint32_t q_len = 0;
    bool q_open = false;

    ecuio_conversation& conversation()
    {
        return s->talk->bound[0].conversation;
    }

    uint8_t next_fate()
    {
        fate = fate * 1103515245u + 12345u;
        return static_cast<uint8_t>(fate >> 16);
    }
};

/* The steps at the harness's time, a word the step refused counted (as
 * fuzz_conversation.cpp reads the report) and an ended state kept */
void step(harness& h)
{
    const ecuio_conversation_report before = h.conversation().report;
    ecuio_step_conversations(h.s, h.e.get(), h.now);
    const ecuio_conversation_report& r = h.conversation().report;
    if (r.words_ended != before.words_ended &&
        r.unlock >= ecuio_unlock_state::no_level) {
        ++h.refused;
    }
    if (before.state == ecuio_conversation_state::faulted ||
        before.state == ecuio_conversation_state::declined ||
        before.state == ecuio_conversation_state::ended) {
        check(r.state == before.state);
    }
}

/* What went on the wire, by the harness's own reading of the rules */
void judge_wire(harness& h, const uint8_t* q, uint32_t len, bool talk)
{
    check(len >= 1);
    if (q[0] == 0x27) {
        check(talk && len >= 2);
        if ((q[1] & 1) != 0) {
            check(len == 2 && ++h.seeds <= h.words - h.refused);
            check(h.now >= h.blocked_until[q[1]]);
            return;
        }
        check(h.seed_open != 0 && q[1] == h.seed_open + 1 && !h.key_sent);
        check(len == 2u + (h.seed_open == 0x01 ? 4u : 16u));
        h.key_sent = true;
        return;
    }
    h.seed_open = 0;
    if (talk) {
        check(ecuio_tier2_guest_may_send(q[0], len >= 2 ? q[1] : 0));
        check(declared(q, len));
        check(len <= ecuio_engine_talk_bound(h.e.get(), 0));
    }
}

/* A talk's end once the engine settled it: a seed's answer opens its
 * key, a seed's 36 or 37 and a key's every end but 67 start the delay */
void settled(harness& h, const uint8_t* reply, uint32_t reply_len)
{
    const ecuio_talk_result& r = h.e->chans[0].talk.result;
    if (!h.q_open || r.end == ecuio_talk_end::none) {
        return;
    }
    h.q_open = false;
    if (h.q[0] != 0x27 || h.q_len < 2) {
        return;
    }
    const bool seed = (h.q[1] & 1) != 0;
    const uint8_t lvl = seed ? h.q[1] : static_cast<uint8_t>(h.q[1] - 1);
    h.seed_open = 0;
    if (seed) {
        h.key_sent = false;
        if (r.end == ecuio_talk_end::answered && reply_len > 2) {
            bool zero = true;
            for (uint32_t k = 2; k < reply_len; ++k) {
                zero = zero && reply[k] == 0;
            }
            h.seed_open = zero ? 0 : lvl;
        } else if (r.end == ecuio_talk_end::refused &&
                   (r.nrc == 0x36 || r.nrc == 0x37)) {
            h.blocked_until[lvl] = h.now + kDelayUs;
        }
    } else if (r.end != ecuio_talk_end::answered) {
        h.blocked_until[lvl] = h.now + kDelayUs;
    }
}

uint32_t answer(harness& h, const uint8_t* q, uint32_t len, uint8_t* out)
{
    if (q[0] == 0x27) {
        return ecuio_sim_security_answer(h.sec, h.extended, kDelayUs, h.now, q,
                                         len, out);
    }
    ecuio_sim_security_between(h.sec);
    if (len == 2 && q[0] == 0x10 && (q[1] == 0x01 || q[1] == 0x03)) {
        h.extended = q[1] == 0x03;
        ecuio_sim_security_lock(h.sec);
        const uint8_t ok[6] = {0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
        memcpy(out, ok, 6);
        return 6;
    }
    if (len >= 3 && q[0] == 0x22) {
        const uint8_t ok[5] = {0x62, q[1], q[2], 0x12, 0x34};
        memcpy(out, ok, 5);
        return 5;
    }
    out[0] = q[0] == 0x3E ? 0x7E : 0x7F;
    out[1] = q[0] == 0x3E ? 0x00 : q[0];
    out[2] = 0x11;
    return q[0] == 0x3E ? 2 : 3;
}

/* A frame on the channel, to the engine and the module */
void frame(harness& h, const uint8_t* b, uint32_t len)
{
    (void)ecuio_engine_on_frame(h.e.get(), 0, b, static_cast<uint16_t>(len),
                                h.now, nullptr);
    settled(h, b, len);
    ecuio_conversations_frame(h.s, h.e.get(), 0, b, len);
}

/* The reply to q with the fate the generator draws */
void reply_to(harness& h, const uint8_t* q, uint32_t len)
{
    const uint8_t f = h.next_fate();
    if (f >= 200 && f < 215) { /* silent */
        return;
    }
    uint8_t reply[64] = {};
    uint32_t n = answer(h, q, len, reply);
    if (f >= 215 && f < 235) { /* refused with a drawn code */
        reply[0] = 0x7F;
        reply[1] = q[0];
        reply[2] = h.next_fate();
        n = 3;
    } else if (f >= 245) { /* garbage */
        n = 1u + h.next_fate() % 8;
        for (uint32_t k = 0; k < n; ++k) {
            reply[k] = h.next_fate();
        }
    }
    h.now += 4 * kMs;
    frame(h, reply, n);
}

/* One pass, as a worker's: waits run out, the steps, the channel's next
 * request on the wire and its reply, the steps again */
void pass(harness& h)
{
    ecuio_request_engine* e = h.e.get();
    if (ecuio_engine_expire(e, 0, h.now)) {
        settled(h, nullptr, 0);
    }
    step(h);
    const ecuio_engine_talk_state& t = e->chans[0].talk;
    const bool key_waits = t.offered && t.kind == ecuio_talk_kind::key;
    const int32_t r = ecuio_engine_due(e, 0, h.now);
    if (t.result.end == ecuio_talk_end::lapsed) { /* at its turn, unsent */
        check(key_waits && !t.out);
        h.key_sent = true;
    }
    settled(h, nullptr, 0);
    if (r == ECUIO_ENGINE_NONE) {
        h.now += ECUIO_ENGINE_GAP_US;
        step(h);
        return;
    }
    const ecuio_request& rq = ecuio_engine_request_of(e, 0, r);
    const bool talk = r == ECUIO_ENGINE_ONCE && e->chans[0].talk.out;
    uint32_t len = 0;
    const uint8_t* pdu = ecuio_engine_pdu_of(
        e->chans[0].talk, r == ECUIO_ENGINE_ONCE, rq.bytes, rq.len, &len);
    check(len >= 1 && len <= sizeof h.q);
    uint8_t q[sizeof h.q] = {};
    memcpy(q, pdu, len);
    judge_wire(h, q, len, talk);
    if (talk) {
        memcpy(h.q, q, len);
        h.q_len = len;
        h.q_open = true;
    }
    const bool fails = h.next_fate() == 0xFF;
    ecuio_engine_sent(e, 0, r, !fails, h.now);
    settled(h, nullptr, 0); /* unsent */
    if (!fails) {
        reply_to(h, q, len);
    }
    h.now += ECUIO_ENGINE_GAP_US;
    step(h);
}

/* The caller's word, as a host gives it: the report, then the unlock */
void word(harness& h, uint8_t lvl)
{
    ecuio_channel_conversation_t c = {};
    check(ecuio_session_get_channel_conversation(h.s, 0, &c) == ECUIO_OK);
    uint32_t wait = 0;
    const ecuio_status_t st =
        ecuio_session_security_unlock(h.s, 0, lvl, c.generation, &wait);
    if (st == ECUIO_OK) {
        ++h.words;
    } else if (st == ECUIO_ERR_TOO_SOON) {
        check(wait > 0); /* a session with no store has no identity wait */
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 4) {
        return 0;
    }
    harness h;
    h.s = session();
    const uint8_t cfg = data[0];
    const uint8_t when = data[1];
    h.fate = static_cast<uint32_t>(data[2]) | (data[3] << 8);
    const size_t base = ecuio_wasm::runtime_bytes_in_use();
    h.s->conversation =
        part_of(data + 4, size - 4, kBudgets[(cfg >> 1) & 7],
                50u + 250u * (cfg >> 4)); /* 50 ms to 3.8 s */
    const ecuio_status_t bound = ecuio_conversation_bind(h.s);
    if (bound != ECUIO_OK) {
        check(bound == ECUIO_ERR_LOAD_FAILED ||
              bound == ECUIO_ERR_OUT_OF_MEMORY);
        check(h.s->talk == nullptr);
        check(ecuio_wasm::runtime_bytes_in_use() == base);
        h.s->conversation.reset();
        return 0;
    }
    check(h.s->talk != nullptr && h.s->talk->count == 1);
    check(ecuio_engine_init(h.e.get(), 1));
    h.e->pdu_max =
        (cfg & 1) != 0 ? ECUIO_ENGINE_PDU_MAX : ECUIO_ENGINE_PDU_SINGLE;
    const uint8_t row[3] = {0x22, 0x1E, 0x1C};
    check(ecuio_engine_add(h.e.get(), 0, row, 3) ==
          ecuio_engine_add_result::added);
    h.sec.strict = true;
    ecuio_conversations_start(h.s, h.e.get());
    const int word_at = when >> 2; /* the pass, 0 to 63 */
    for (int i = 0; i < kPasses; ++i) {
        if (i == word_at || i == word_at + 160) {
            word(h, kLevels[when & 3]);
        }
        pass(h);
    }
    const ecuio_conversation_state s = h.conversation().report.state;
    ecuio_conversations_end(h.s, h.e.get());
    const ecuio_conversation_state after = h.conversation().report.state;
    check(after == (s == ecuio_conversation_state::faulted
                        ? ecuio_conversation_state::faulted
                        : ecuio_conversation_state::ended));
    ecuio_conversation_unbind(h.s);
    h.s->talk.reset();
    h.s->conversation.reset();
    check(ecuio_wasm::runtime_bytes_in_use() == base);
    return 0;
}
