// File/layer: fuzz/fuzz_conversation.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_conversation
//
// One module's conversation (conversation.h) driven by the input: the
// table's answers (begin's session, each request's length and bytes, a
// key's length and bytes, a call's fault), the caller's words, the clock
// and the strict simulated ECU's replies (intact, silent, refused with
// any code, pending first, garbage, a write that fails). The harness
// holds what goes on the wire to the conversation's rules by its own
// reading of them, and aborts on any break:
//   - a 27 only as core builds it: a seed for a word given and not
//     refused, never inside a delay the wire has shown; a key only
//     straight after its seed's answer, of its level's length, once for
//     each seed, and never once that seed's key has lapsed;
//   - any other request of the module's only inside the guest's list,
//     the manifest and the channel's bound;
//   - the module never called once a fault has ended it, its end never
//     after a fault and at most once;
//   - no failure count lower than the keys' ends the wire has shown.

#include "conversation.h" /* core/src: the conversation, the engine */
#include "sim_security.h" /* core/src: the simulated ECU's security access */
#include "tier2_ceiling.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace {

constexpr int kOps = 512;
constexpr int kDrain = 200;
constexpr int64_t kMs = 1000;
constexpr uint32_t kRx = 0x7E8;
constexpr uint32_t kTx = 0x7E0;
constexpr int64_t kDelayUs = 10000 * kMs; /* every level's, as below */

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* The bytes left of the input, taken from the front */
struct reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint8_t byte()
    {
        if (n == 0) {
            return 0;
        }
        --n;
        return *p++;
    }
};

struct harness {
    reader in;
    std::unique_ptr<ecuio_request_engine> e =
        std::make_unique<ecuio_request_engine>();
    std::unique_ptr<ecuio_conversation> v =
        std::make_unique<ecuio_conversation>();
    ecuio_sim_security sec;
    bool extended = false;
    int64_t now = 0;
    uint32_t words = 0;    /* words the conversation took */
    uint32_t refused = 0;  /* of those, refused: no seed for them */
    uint32_t seeds = 0;    /* seeds on the wire */
    uint8_t seed_open = 0; /* the level whose seed the last reply gave */
    bool key_sent = false; /* a key went, or lapsed, for that seed */
    int64_t blocked_until[256] = {};
    uint32_t failed[256] = {}; /* keys' ends but 67, since the last 67 */
    int ends = 0;
};

harness* g_h = nullptr;

bool talking()
{
    return g_h->v->report.state == ecuio_conversation_state::talking;
}

/* The conversation's step at the harness's time. A word refused is the
 * step's last end (take_word comes after every other), so a refusal
 * read in the report after words_ended moved is that step's. */
void step()
{
    harness& h = *g_h;
    const uint32_t before = h.v->report.words_ended;
    ecuio_conversation_step(h.v.get(), h.e.get(), h.now);
    if (h.v->report.words_ended != before &&
        h.v->report.unlock >= ecuio_unlock_state::no_level) {
        ++h.refused;
    }
}

ecuio_guest_fault fault_of(uint8_t b)
{
    return b == 253   ? ecuio_guest_fault::budget
           : b == 254 ? ecuio_guest_fault::trap
           : b == 255 ? ecuio_guest_fault::buffer
                      : ecuio_guest_fault::none;
}

ecuio_conversation_call t_begin(void*, uint32_t rx, uint32_t tx)
{
    check(g_h->v->report.state == ecuio_conversation_state::unbegun);
    check(rx == kRx && tx == kTx);
    ecuio_conversation_call c;
    c.fault = fault_of(g_h->in.byte());
    const uint8_t b = g_h->in.byte();
    c.value = b % 4 == 0 ? 0 : b % 4 == 3 ? static_cast<int8_t>(b) : 3;
    return c;
}

ecuio_conversation_call t_next(void*, uint8_t* buffer)
{
    check(talking());
    reader& in = g_h->in;
    ecuio_conversation_call c;
    c.fault = fault_of(in.byte());
    const uint8_t mode = in.byte();
    if (mode % 4 == 0) {
        return c; /* nothing this cycle */
    }
    if (mode % 4 == 3) { /* a length of the input's own, nothing written */
        c.value = static_cast<int16_t>(in.byte() << 8 | in.byte());
        return c;
    }
    const uint32_t len = mode % 4 == 1 ? 3 : 1u + in.byte() % 64;
    static const uint8_t sids[] = {0x22, 0x19, 0x2C, 0x27, 0x10, 0x3E, 0x2A};
    buffer[0] = mode % 4 == 1 ? 0x22 : sids[in.byte() % sizeof sids];
    for (uint32_t k = 1; k < len; ++k) {
        buffer[k] = in.byte();
    }
    c.value = static_cast<int32_t>(len);
    return c;
}

ecuio_conversation_call t_accept(void*, int32_t kind, uint8_t* buffer,
                                 uint32_t len)
{
    check(talking());
    check(kind >= ECUIO_GUEST_REPLY && kind <= ECUIO_GUEST_UNLOCK);
    check(len <= ECUIO_GUEST_BUFFER_BYTES);
    reader& in = g_h->in;
    ecuio_conversation_call c;
    c.fault = fault_of(in.byte());
    if (kind != ECUIO_GUEST_SEED) {
        return c;
    }
    check(len >= 2);
    const uint8_t mode = in.byte();
    const uint32_t n = len - 1;
    uint8_t key[64] = {};
    ecuio_sim_security_key(buffer + 1, n, key);
    if (mode % 4 == 2) {
        key[0] = static_cast<uint8_t>(key[0] ^ 0x5A);
    }
    memcpy(buffer, key, n);
    c.value = mode % 4 == 3 ? static_cast<int32_t>(in.byte() % 70) - 5
                            : static_cast<int32_t>(n);
    if (mode & 0x80) { /* the module's own time, the window's test */
        g_h->now += int64_t{in.byte()} * 20 * kMs;
    }
    return c;
}

void t_end(void*)
{
    check(g_h->v->report.state != ecuio_conversation_state::faulted);
    ++g_h->ends;
}

/* The ECU's answer to q: the simulated ECU's security access, strict */
uint32_t answer(const uint8_t* q, uint32_t len, uint8_t* out)
{
    harness& h = *g_h;
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

/* Whether the manifest below declares q */
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

/* What went on the wire, by the harness's own reading of the rules */
void judge_wire(const uint8_t* q, uint32_t len, bool talk)
{
    harness& h = *g_h;
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

/* What a talk's end on the wire counts, by the harness: a seed's 36 or
 * 37 and a key's every end but 67 start the delay; a key's counts */
void judge_end(const uint8_t* q, uint32_t len, const ecuio_talk_result& r,
               const uint8_t* reply, uint32_t reply_len)
{
    harness& h = *g_h;
    if (q[0] != 0x27 || len < 2) {
        return;
    }
    const bool seed = (q[1] & 1) != 0;
    const uint8_t level = seed ? q[1] : static_cast<uint8_t>(q[1] - 1);
    if (seed) {
        h.key_sent = false;
        h.seed_open = 0;
        if (r.end == ecuio_talk_end::answered && reply_len > 2) {
            bool zero = true;
            for (uint32_t k = 2; k < reply_len; ++k) {
                zero = zero && reply[k] == 0;
            }
            h.seed_open = zero ? 0 : level;
        } else if (r.end == ecuio_talk_end::refused &&
                   (r.nrc == 0x36 || r.nrc == 0x37)) {
            h.blocked_until[level] = h.now + kDelayUs;
        }
        return;
    }
    h.seed_open = 0;
    if (r.end == ecuio_talk_end::answered) {
        h.failed[level] = 0;
    } else {
        ++h.failed[level];
        h.blocked_until[level] = h.now + kDelayUs;
    }
}

/* The talk out, as the wire carried it, until its end is judged */
struct out_talk {
    uint8_t q[64] = {};
    uint32_t len = 0;
    bool open = false;
};
out_talk g_out;

/* A talk's end, once the engine has settled it: judged with the bytes of
 * the frame that settled it, none for any other end */
void settled(const uint8_t* reply, uint32_t len)
{
    if (!g_out.open) {
        return;
    }
    const ecuio_talk_result& r = g_h->e->chans[0].talk.result;
    if (r.end == ecuio_talk_end::none) {
        return;
    }
    g_out.open = false;
    judge_end(g_out.q, g_out.len, r, reply, len);
}

/* A frame on the channel, to the engine and the conversation */
void frame(const uint8_t* b, uint32_t len)
{
    harness& h = *g_h;
    (void)ecuio_engine_on_frame(h.e.get(), 0, b, static_cast<uint16_t>(len),
                                h.now, nullptr);
    settled(b, len);
    ecuio_conversation_frame(h.v.get(), h.e.get(), b, len);
}

/* The reply to q with the fate the input picks */
void reply_to(const uint8_t* q, uint32_t len, bool honest)
{
    harness& h = *g_h;
    const uint8_t f = honest ? 0 : h.in.byte();
    if (f >= 200 && f < 215) { /* silent */
        return;
    }
    if (f >= 235 && f < 245) { /* pending first */
        const uint8_t wait[3] = {0x7F, q[0], 0x78};
        h.now += 2 * kMs;
        frame(wait, 3);
    }
    uint8_t reply[64] = {};
    uint32_t n = answer(q, len, reply);
    if (f >= 215 && f < 235) { /* refused with the input's code */
        reply[0] = 0x7F;
        reply[1] = q[0];
        reply[2] = h.in.byte();
        n = 3;
    } else if (f >= 245) { /* garbage */
        n = 1u + h.in.byte() % 8;
        for (uint32_t k = 0; k < n; ++k) {
            reply[k] = h.in.byte();
        }
    }
    h.now += 4 * kMs;
    frame(reply, n);
}

/* One pass, as a worker's: waits run out, the conversation's step, the
 * channel's next request on the wire and its reply, the step again */
void pass(bool honest)
{
    harness& h = *g_h;
    ecuio_request_engine* e = h.e.get();
    if (ecuio_engine_expire(e, 0, h.now)) {
        settled(nullptr, 0);
    }
    step();
    const ecuio_engine_talk_state& t = e->chans[0].talk;
    const bool key_waits = t.offered && t.kind == ecuio_talk_kind::key;
    const int32_t r = ecuio_engine_due(e, 0, h.now);
    if (t.result.end == ecuio_talk_end::lapsed) { /* at its turn, unsent */
        check(key_waits && !t.out);
        h.key_sent = true; /* no key may follow for that seed */
    }
    settled(nullptr, 0);
    if (r == ECUIO_ENGINE_NONE) {
        h.now += ECUIO_ENGINE_GAP_US;
        step();
        return;
    }
    const ecuio_request& rq = ecuio_engine_request_of(e, 0, r);
    const bool talk = r == ECUIO_ENGINE_ONCE && e->chans[0].talk.out;
    uint32_t len = 0;
    const uint8_t* pdu = ecuio_engine_pdu_of(
        e->chans[0].talk, r == ECUIO_ENGINE_ONCE, rq.bytes, rq.len, &len);
    check(len >= 1 && len <= sizeof g_out.q);
    uint8_t q[sizeof g_out.q] = {};
    memcpy(q, pdu, len);
    judge_wire(q, len, talk);
    if (talk) {
        memcpy(g_out.q, q, len);
        g_out.len = len;
        g_out.open = true;
    }
    const bool fails = !honest && h.in.byte() == 0xFF;
    ecuio_engine_sent(e, 0, r, !fails, h.now);
    settled(nullptr, 0); /* unsent */
    if (!fails) {
        reply_to(q, len, honest);
    }
    h.now += ECUIO_ENGINE_GAP_US;
    step();
}

/* No failure count lower than the keys' ends the wire has shown */
void check_counts()
{
    const ecuio_security::module_access& a = g_h->v->access;
    for (uint32_t k = 0; k < a.count; ++k) {
        check(a.levels[k].failed >= g_h->failed[a.levels[k].level]);
    }
}

ecuio_plugin_service_t service(uint8_t sid, uint8_t sub_count,
                               const uint8_t* subs)
{
    ecuio_plugin_service_t s = {};
    s.struct_size = sizeof s;
    s.sid = sid;
    s.sub_count = sub_count;
    for (uint8_t k = 0; k < sub_count; ++k) {
        s.subs[k] = subs[k];
    }
    return s;
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

/* The manifest's part judge_wire's declared() reads, and the simulated
 * ECU's two levels, the window from the input */
bool make(harness& h)
{
    const uint8_t cfg = h.in.byte();
    if (!ecuio_engine_init(h.e.get(), 1)) {
        return false;
    }
    h.e->pdu_max =
        (cfg & 1) != 0 ? ECUIO_ENGINE_PDU_MAX : ECUIO_ENGINE_PDU_SINGLE;
    const uint8_t row[3] = {0x22, 0x1E, 0x1C};
    check(ecuio_engine_add(h.e.get(), 0, row, 3) ==
          ecuio_engine_add_result::added);
    h.sec.strict = true;
    const uint8_t s19[1] = {0x02};
    const uint8_t s2c[1] = {0x01};
    const uint8_t s27[4] = {0x01, 0x02, 0x03, 0x04};
    const ecuio_plugin_service_t services[4] = {
        service(0x22, 0, nullptr), service(0x19, 1, s19), service(0x2C, 1, s2c),
        service(0x27, 4, s27)};
    const uint32_t window = 50u + 50u * (cfg >> 3); /* 50 ms to 1.6 s */
    const ecuio_plugin_security_level_t levels[2] = {level(0x01, 4, window),
                                                     level(0x03, 16, window)};
    ecuio_conversation_ops ops;
    ops.begin = t_begin;
    ops.next_request = t_next;
    ops.accept = t_accept;
    ops.end = t_end;
    check(ecuio_conversation_init(h.v.get(), ops, 0, kRx, kTx, services, 4,
                                  levels, 2));
    return true;
}

void op(harness& h)
{
    switch (h.in.byte() % 8) {
    case 4: {
        static const uint8_t levels[] = {0x01, 0x03, 0x05};
        const uint8_t b = h.in.byte();
        const uint8_t lvl = b < 240 ? levels[b % 3] : h.in.byte();
        if (ecuio_conversation_unlock(h.v.get(), lvl)) {
            ++h.words;
        }
        break;
    }
    case 5:
        h.now += (int64_t{h.in.byte()} + 1) * 50 * kMs;
        break;
    case 6: { /* a frame nobody asked for */
        uint8_t b[8] = {};
        const uint32_t n = 1u + h.in.byte() % 8;
        for (uint32_t k = 0; k < n; ++k) {
            b[k] = h.in.byte();
        }
        frame(b, n);
        break;
    }
    case 7:
        pass(true);
        break;
    default:
        pass(false);
        break;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    harness h;
    g_h = &h;
    g_out = out_talk{};
    h.in.p = data;
    h.in.n = size;
    if (!make(h)) {
        return 0;
    }
    ecuio_conversation_begin(h.v.get(), h.e.get(), h.now);
    for (int i = 0; i < kOps && h.in.n > 0; ++i) {
        op(h);
        check_counts();
    }
    for (int i = 0; i < kDrain; ++i) {
        pass(true);
        check_counts();
    }
    const ecuio_conversation_state s = h.v->report.state;
    ecuio_conversation_end(h.v.get());
    ecuio_conversation_end(h.v.get());
    const bool ends = s == ecuio_conversation_state::talking ||
                      s == ecuio_conversation_state::declined;
    check(h.ends == (ends ? 1 : 0));
    g_h = nullptr;
    return 0;
}
