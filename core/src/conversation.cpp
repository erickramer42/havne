// File/layer: core/src/conversation.cpp | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: ConversationTest (test_obd2); FUZZ: fuzz_conversation
//
// One module's conversation over the table (conversation.h has the
// rules and why).

#include "conversation.h"

#include <cstring>

namespace {

using ecuio_security::phase;
using conv_fault = ecuio_conversation_fault;
using conv_unlock = ecuio_unlock_state;

ecuio_engine_channel& chan(ecuio_request_engine* e, const ecuio_conversation* v)
{
    return e->chans[v->channel];
}

/* Whether the channel's talk is free for the next offer */
bool slot_free(const ecuio_request_engine* e, const ecuio_conversation* v)
{
    const ecuio_engine_talk_state& t = e->chans[v->channel].talk;
    return !t.offered && !t.out && t.result.end == ecuio_talk_end::none;
}

/* After a fault, the conversation's talk is let go: one offered is
 * withdrawn, one settled taken, one out taken at its settle (the step's
 * call), so the channel's talk is never left busy */
void let_go(ecuio_conversation* v, ecuio_request_engine* e)
{
    if (v->own == ecuio_conversation_talk::none) {
        return; /* and none on a channel the engine lacks */
    }
    ecuio_engine_talk_state& t = chan(e, v).talk;
    ecuio_talk_result res;
    if (!ecuio_talk_withdraw(t) && !ecuio_engine_talk_take(t, &res)) {
        return;
    }
    v->own = ecuio_conversation_talk::none;
    v->kept = false;
    v->kept_len = 0;
}

/* The conversation ends for the module: it is never called again, the
 * channel's window ends, an attempt not yet at its key ends with nothing
 * counted, and its talk is let go. bytes: the refused request's, kept in
 * the report. */
void fault(ecuio_conversation* v, ecuio_request_engine* e, conv_fault f,
           const uint8_t* bytes = nullptr, uint32_t len = 0)
{
    ecuio_conversation_report& r = v->report;
    r.state = ecuio_conversation_state::faulted;
    r.fault = f;
    r.fault_len = static_cast<uint8_t>(len < 4 ? len : 4);
    for (uint32_t k = 0; k < r.fault_len; ++k) {
        r.fault_bytes[k] = bytes[k];
    }
    ecuio_engine_key_window_end(e, v->channel);
    if (v->access.at != phase::idle && ecuio_security::drop(v->access)) {
        r.unlock = conv_unlock::ended;
        ++r.words_ended;
    }
    if (v->word) {
        v->word = false;
        r.unlock = conv_unlock::ended;
        ++r.words_ended;
    }
    let_go(v, e);
}

/* A call's fault, when it has one: true when the module answered */
bool answered(ecuio_conversation* v, ecuio_request_engine* e,
              const ecuio_conversation_call& c)
{
    switch (c.fault) {
    case ecuio_guest_fault::none:
        return true;
    case ecuio_guest_fault::budget:
        fault(v, e, conv_fault::budget);
        break;
    case ecuio_guest_fault::trap:
        fault(v, e, conv_fault::trap);
        break;
    default:
        fault(v, e, conv_fault::buffer);
        break;
    }
    return false;
}

/* accept(kind, len) with len bytes at the buffer */
bool tell(ecuio_conversation* v, ecuio_request_engine* e, int32_t kind,
          uint32_t len, ecuio_conversation_call* out = nullptr)
{
    const ecuio_conversation_call c =
        v->ops.accept(v->ops.ctx, kind, v->buffer, len);
    if (out != nullptr) {
        *out = c;
    }
    return answered(v, e, c);
}

/* The word's attempt has ended as u: reported, the settle counted, and
 * the module told the level, its result and the module's code */
void word_ended(ecuio_conversation* v, ecuio_request_engine* e, conv_unlock u,
                uint8_t result, uint8_t nrc)
{
    ecuio_conversation_report& r = v->report;
    r.level = v->access.levels[v->access.in_flight].level;
    r.unlock = u;
    r.nrc = nrc;
    ++r.words_ended;
    v->buffer[0] = r.level;
    v->buffer[1] = result;
    v->buffer[2] = nrc;
    (void)tell(v, e, ECUIO_GUEST_UNLOCK, 3);
}

/* The module's reply, or its timeout, to the module */
void settle_request(ecuio_conversation* v, ecuio_request_engine* e,
                    const ecuio_talk_result& res)
{
    const bool reply = v->kept && (res.end == ecuio_talk_end::answered ||
                                   res.end == ecuio_talk_end::refused);
    (void)tell(v, e, reply ? ECUIO_GUEST_REPLY : ECUIO_GUEST_TIMEOUT,
               reply ? v->kept_len : 0);
}

/* The seed of n bytes (at buffer + 2) to the module, and its key offered
 * as the channel's talk at now_us */
void offer_key(ecuio_conversation* v, ecuio_request_engine* e, uint32_t n,
               int64_t now_us)
{
    ecuio_conversation_report& r = v->report;
    const ecuio_security::level_state& l =
        v->access.levels[v->access.in_flight];
    v->buffer[0] = l.level;
    std::memmove(v->buffer + 1, v->buffer + 2, n);
    ecuio_conversation_call c;
    if (!tell(v, e, ECUIO_GUEST_SEED, 1 + n, &c)) {
        return;
    }
    if (c.value < 0 ||
        !ecuio_security::key(v->access, static_cast<uint32_t>(c.value))) {
        (void)ecuio_security::drop(v->access);
        r.unlock = conv_unlock::ended;
        ++r.words_ended;
        fault(v, e, conv_fault::key);
        return;
    }
    uint8_t pdu[ECUIO_ENGINE_PDU_MAX] = {ECUIO_TALK_SECURITY_ACCESS,
                                         static_cast<uint8_t>(l.level + 1)};
    std::memcpy(pdu + 2, v->buffer, l.key_bytes); /* init bounds key_bytes */
    ecuio_talk_request q;
    q.pdu = pdu;
    q.len = 2u + l.key_bytes;
    q.echo = 2;
    q.kind = ecuio_talk_kind::key;
    if (ecuio_engine_offer_talk(e, v->channel, q, now_us) !=
        ecuio_talk_offer::taken) {
        (void)ecuio_security::drop(v->access);
        if (!ecuio_key_window_open(chan(e, v).key, now_us)) {
            /* closed by the session, the link or a late step: not the
             * module's doing (its own time never moves now_us) */
            word_ended(v, e, conv_unlock::ended, ECUIO_GUEST_ENDED, 0);
            return;
        }
        r.unlock = conv_unlock::ended;
        ++r.words_ended;
        fault(v, e, conv_fault::window);
        return;
    }
    ecuio_security::key_offered(v->access);
    v->own = ecuio_conversation_talk::key;
    r.unlock = conv_unlock::key_offered;
}

/* The seed's end to the attempts' machine, and on to the module */
void settle_seed(ecuio_conversation* v, ecuio_request_engine* e,
                 const ecuio_talk_result& res, int64_t now_us)
{
    const bool whole =
        res.end == ecuio_talk_end::answered && v->kept && v->kept_len >= 2;
    const uint32_t n = whole ? v->kept_len - 2 : 0;
    switch (ecuio_security::seed(v->access, res.end, res.nrc,
                                 whole ? v->buffer + 2 : nullptr, n, now_us)) {
    case ecuio_security::seed_answer::key:
        offer_key(v, e, n, now_us);
        return;
    case ecuio_security::seed_answer::unlocked:
        ecuio_engine_key_window_end(e, v->channel);
        word_ended(v, e, conv_unlock::unlocked, ECUIO_GUEST_UNLOCKED, 0);
        return;
    case ecuio_security::seed_answer::ended:
        ecuio_engine_key_window_end(e, v->channel);
        word_ended(v, e, conv_unlock::ended, ECUIO_GUEST_ENDED, res.nrc);
        return;
    default: /* the session was lost meanwhile: the attempt is over */
        ecuio_security::seed_after_loss(v->access, res.end, res.nrc, now_us);
        ecuio_engine_key_window_end(e, v->channel);
        return;
    }
}

/* The key's end to the attempts' machine, and on to the module */
void settle_key(ecuio_conversation* v, ecuio_request_engine* e,
                const ecuio_talk_result& res, int64_t now_us)
{
    switch (ecuio_security::key_end(v->access, res.end, now_us)) {
    case ecuio_security::key_answer::unlocked:
        word_ended(v, e, conv_unlock::unlocked, ECUIO_GUEST_UNLOCKED, 0);
        return;
    case ecuio_security::key_answer::failed:
        word_ended(v, e, conv_unlock::failed, ECUIO_GUEST_REFUSED, res.nrc);
        return;
    case ecuio_security::key_answer::not_counted:
        word_ended(v, e, conv_unlock::ended, ECUIO_GUEST_ENDED, 0);
        return;
    default: /* told already, as a lost session */
        return;
    }
}

/* Step 1: the conversation's talk, settled, to whom it concerns */
void take_settled(ecuio_conversation* v, ecuio_request_engine* e,
                  int64_t now_us)
{
    ecuio_talk_result res;
    if (v->own == ecuio_conversation_talk::none ||
        !ecuio_engine_talk_take(chan(e, v).talk, &res)) {
        return;
    }
    const ecuio_conversation_talk own = v->own;
    v->own = ecuio_conversation_talk::none;
    if (own == ecuio_conversation_talk::request) {
        settle_request(v, e, res);
    } else if (own == ecuio_conversation_talk::seed) {
        settle_seed(v, e, res, now_us);
    } else {
        settle_key(v, e, res, now_us);
    }
    v->kept = false;
    v->kept_len = 0;
}

/* Step 2, after the talk's end (which decides wherever there is one): a
 * session entered again, or left, locks every level and ends an attempt
 * under way; a key on its way counts, and a seed not yet out is taken
 * back (else it goes with no attempt to tell its 36 or 37 to). A run's
 * session handed over or returned is a loss; of the loss count only a
 * rise is: the engine gives one back when a
 * refusal was the request's own. The step's leaving is watched too,
 * since an abandoned session counts no loss and the count saturates. */
void check_session(ecuio_conversation* v, ecuio_request_engine* e,
                   int64_t now_us)
{
    const ecuio_engine_channel& ch = chan(e, v);
    const bool entered = ch.session_step == ECUIO_SESSION_STATE_ENTERED;
    const bool lost = ch.session_lost > v->seen_lost ||
                      ch.session_handed != v->seen_handed ||
                      (v->was_entered && !entered);
    v->seen_lost = ch.session_lost;
    v->seen_handed = ch.session_handed;
    v->was_entered = entered;
    if (!lost) {
        return;
    }
    const bool running = v->access.at != phase::idle;
    bool open = false;
    for (uint32_t k = 0; k < v->access.count; ++k) {
        open = open || v->access.levels[k].unlocked;
    }
    const bool counted = ecuio_security::session_lost(v->access, now_us);
    if (v->own == ecuio_conversation_talk::seed &&
        ecuio_talk_withdraw(chan(e, v).talk)) {
        v->own = ecuio_conversation_talk::none; /* its answer has no attempt */
    }
    if (running) {
        word_ended(v, e, counted ? conv_unlock::failed : conv_unlock::ended,
                   counted ? ECUIO_GUEST_REFUSED : ECUIO_GUEST_ENDED, 0);
    } else if (open) {
        v->report.unlock = conv_unlock::lost;
    }
}

/* What ask's refusal reads as */
conv_unlock refusal_of(ecuio_security::ask_answer a)
{
    switch (a) {
    case ecuio_security::ask_answer::no_level:
        return conv_unlock::no_level;
    case ecuio_security::ask_answer::key_too_long:
        return conv_unlock::key_too_long;
    case ecuio_security::ask_answer::busy:
        return conv_unlock::busy;
    case ecuio_security::ask_answer::delayed:
        return conv_unlock::delayed;
    default:
        return conv_unlock::already_unlocked;
    }
}

/* Step 3: the caller's word, once the channel's talk is free: the
 * machine's ask, and on go the seed offered with its level's window */
void take_word(ecuio_conversation* v, ecuio_request_engine* e, int64_t now_us)
{
    ecuio_conversation_report& r = v->report;
    if (!v->word) {
        return;
    }
    if (v->access.at != phase::idle) {
        v->word = false;
        r.level = v->word_level;
        r.unlock = conv_unlock::busy;
        ++r.words_ended;
        return;
    }
    if (v->own != ecuio_conversation_talk::none || !slot_free(e, v)) {
        return; /* the module's own request first */
    }
    v->word = false;
    const ecuio_security::asked a =
        ecuio_security::ask(v->access, v->word_level,
                            ecuio_engine_talk_bound(e, v->channel), now_us);
    if (a.answer != ecuio_security::ask_answer::go) {
        r.unlock = refusal_of(a.answer);
        r.left_us = a.left_us;
        ++r.words_ended;
        return;
    }
    const uint8_t pdu[2] = {ECUIO_TALK_SECURITY_ACCESS, v->word_level};
    ecuio_talk_request q;
    q.pdu = pdu;
    q.len = 2;
    q.echo = 2;
    q.kind = ecuio_talk_kind::seed;
    q.window_us = v->access.levels[a.index].window_us;
    if (ecuio_engine_offer_talk(e, v->channel, q, now_us) !=
        ecuio_talk_offer::taken) {
        (void)ecuio_security::drop(v->access);
        r.unlock = conv_unlock::refused;
        ++r.words_ended;
        return;
    }
    v->own = ecuio_conversation_talk::seed;
    r.unlock = conv_unlock::seed_offered;
}

/* Whether the manifest declares the request of len bytes at buffer: its
 * service, and its sub-function where the service is declared by them */
bool declared(const ecuio_conversation* v, uint32_t len)
{
    for (uint32_t k = 0; k < v->service_count; ++k) {
        const ecuio_plugin_service_t& s = v->services[k];
        if (s.sid != v->buffer[0]) {
            continue;
        }
        if (s.sub_count == 0) {
            return true;
        }
        for (uint32_t j = 0; len >= 2 && j < s.sub_count; ++j) {
            if (s.subs[j] == v->buffer[1]) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/* The rules a module's request meets before the door, in turn: the
 * channel's bound, the guest's list, the manifest's declaration */
conv_fault judge_request(const ecuio_conversation* v,
                         const ecuio_request_engine* e, uint32_t len)
{
    if (len > ecuio_engine_talk_bound(e, v->channel)) {
        return conv_fault::bound;
    }
    const uint8_t sub = len >= 2 ? v->buffer[1] : uint8_t{0};
    if (!ecuio_tier2_guest_may_send(v->buffer[0], sub)) {
        return conv_fault::service;
    }
    return declared(v, len) ? conv_fault::none : conv_fault::undeclared;
}

/* Step 4: with nothing of its own out, no attempt under way and its poll
 * due, the module's next request, offered as a plain talk */
void poll(ecuio_conversation* v, ecuio_request_engine* e, int64_t now_us)
{
    if (v->own != ecuio_conversation_talk::none ||
        v->access.at != phase::idle || v->word || now_us < v->next_poll_us ||
        !slot_free(e, v)) {
        return;
    }
    const ecuio_conversation_call c =
        v->ops.next_request(v->ops.ctx, v->buffer);
    if (!answered(v, e, c)) {
        return;
    }
    if (c.value == 0) {
        v->next_poll_us = now_us + ECUIO_CONVERSATION_POLL_US;
        return;
    }
    if (c.value < 0 || c.value > static_cast<int32_t>(sizeof v->buffer)) {
        fault(v, e, conv_fault::length);
        return;
    }
    const uint32_t len = static_cast<uint32_t>(c.value);
    const conv_fault f = judge_request(v, e, len);
    if (f != conv_fault::none) {
        fault(v, e, f, v->buffer, len);
        return;
    }
    ecuio_talk_request q;
    q.pdu = v->buffer;
    q.len = len;
    q.echo = ecuio_talk_echo_min(v->buffer[0]);
    q.kind = ecuio_talk_kind::plain;
    if (ecuio_engine_offer_talk(e, v->channel, q, now_us) !=
        ecuio_talk_offer::taken) {
        fault(v, e, conv_fault::door, v->buffer, len);
        return;
    }
    v->own = ecuio_conversation_talk::request;
    ++v->report.requests;
}

bool talking(const ecuio_conversation* v)
{
    return v->report.state == ecuio_conversation_state::talking;
}

/* Whether level l fits what core sends for it: its key's request inside
 * one PDU, its window the door's (the loader refuses both first) */
bool fits(const ecuio_plugin_security_level_t& l)
{
    const int64_t window_us = int64_t{l.key_window_ms} * 1000;
    return l.key_bytes <= ECUIO_PLUGIN_KEY_MAX &&
           window_us >= ECUIO_KEY_WINDOW_MIN_US &&
           window_us <= ECUIO_KEY_WINDOW_MAX_US;
}
static_assert(2 + ECUIO_PLUGIN_KEY_MAX <= ECUIO_ENGINE_PDU_MAX,
              "a key's request (27, its sub-function, the key) fits a PDU");

} // namespace

bool ecuio_conversation_init(ecuio_conversation* v,
                             const ecuio_conversation_ops& ops,
                             uint32_t channel, uint32_t rx, uint32_t tx,
                             const ecuio_plugin_service_t* services,
                             uint32_t service_count,
                             const ecuio_plugin_security_level_t* levels,
                             uint32_t level_count)
{
    *v = ecuio_conversation{};
    if (ops.begin == nullptr || ops.next_request == nullptr ||
        ops.accept == nullptr || ops.end == nullptr ||
        service_count > ECUIO_PLUGIN_MAX_SERVICES ||
        level_count > ECUIO_PLUGIN_MAX_LEVELS) {
        return false;
    }
    v->ops = ops;
    v->channel = channel;
    v->rx = rx;
    v->tx = tx;
    for (uint32_t k = 0; k < service_count; ++k) {
        v->services[k] = services[k];
    }
    v->service_count = service_count;
    for (uint32_t k = 0; k < level_count; ++k) {
        const ecuio_plugin_security_level_t& l = levels[k];
        if (l.rx_id != rx) {
            continue;
        }
        if (!fits(l) || !ecuio_security::add_level(
                            v->access, l.level, l.seed_bytes, l.key_bytes,
                            l.delay_ms, l.key_window_ms)) {
            return false;
        }
    }
    return true;
}

void ecuio_conversation_begin(ecuio_conversation* v, ecuio_request_engine* e,
                              int64_t now_us)
{
    ecuio_conversation_report& r = v->report;
    if (r.state != ecuio_conversation_state::unbegun) {
        return;
    }
    if (v->channel >= e->chan_count) {
        r.state = ecuio_conversation_state::faulted; /* the module never ran */
        r.fault = conv_fault::channel;
        return;
    }
    const ecuio_conversation_call c = v->ops.begin(v->ops.ctx, v->rx, v->tx);
    if (!answered(v, e, c)) {
        return;
    }
    if (c.value == 0 || c.value == ECUIO_SESSION_EXTENDED) {
        r.state = ecuio_conversation_state::talking;
        r.session = static_cast<uint8_t>(c.value);
    } else if (c.value < 0) {
        r.state = ecuio_conversation_state::declined;
        return;
    } else {
        fault(v, e, conv_fault::begin);
        return;
    }
    if (c.value == ECUIO_SESSION_EXTENDED) {
        (void)ecuio_engine_session_enter(e, v->channel);
    }
    const ecuio_engine_channel& ch = chan(e, v);
    v->seen_lost = ch.session_lost;
    v->seen_handed = ch.session_handed;
    v->was_entered = ch.session_step == ECUIO_SESSION_STATE_ENTERED;
    v->next_poll_us = now_us;
}

void ecuio_conversation_frame(ecuio_conversation* v,
                              const ecuio_request_engine* e,
                              const uint8_t* bytes, uint32_t len)
{
    if (!talking(v) || v->own == ecuio_conversation_talk::none || v->kept) {
        return;
    }
    const ecuio_talk_end end = e->chans[v->channel].talk.result.end;
    if (end != ecuio_talk_end::answered && end != ecuio_talk_end::refused) {
        return; /* not settled by this frame: none comes without one */
    }
    v->kept_len = len < sizeof v->buffer ? len : sizeof v->buffer;
    std::memcpy(v->buffer, bytes, v->kept_len);
    v->kept = true;
}

void ecuio_conversation_step(ecuio_conversation* v, ecuio_request_engine* e,
                             int64_t now_us)
{
    if (v->report.state == ecuio_conversation_state::faulted) {
        let_go(v, e);
        return;
    }
    if (talking(v)) {
        take_settled(v, e, now_us);
    }
    if (talking(v)) {
        check_session(v, e, now_us);
    }
    if (talking(v)) {
        take_word(v, e, now_us);
    }
    /* under an operation's hold the module is asked for no request */
    if (talking(v) && !e->operation_hold &&
        !e->chans[v->channel].part_written) {
        poll(v, e, now_us);
    }
}

bool ecuio_conversation_unlock(ecuio_conversation* v, uint8_t level)
{
    if (!talking(v) || v->word) {
        return false;
    }
    v->word = true;
    v->word_level = level;
    if (v->access.at != phase::idle) {
        return true; /* the report stays the running attempt's until the step */
    }
    ecuio_conversation_report& r = v->report;
    r.level = level;
    r.unlock = conv_unlock::waiting;
    r.nrc = 0;
    r.left_us = 0;
    return true;
}

int64_t ecuio_conversation_next_us(const ecuio_conversation* v,
                                   const ecuio_request_engine* e,
                                   int64_t now_us)
{
    const bool letting_go =
        v->report.state == ecuio_conversation_state::faulted &&
        v->own != ecuio_conversation_talk::none;
    if (!talking(v) && !letting_go) {
        return INT64_MAX;
    }
    if (v->own != ecuio_conversation_talk::none) { /* a faulted one's too */
        const ecuio_talk_end end = e->chans[v->channel].talk.result.end;
        return end != ecuio_talk_end::none ? now_us : INT64_MAX;
    }
    if (!slot_free(e, v)) {
        return INT64_MAX; /* another's talk: its settle wakes the driver */
    }
    if (v->word) {
        return now_us;
    }
    return v->next_poll_us > now_us ? v->next_poll_us : now_us;
}

void ecuio_conversation_end(ecuio_conversation* v)
{
    const ecuio_conversation_state s = v->report.state;
    if (s == ecuio_conversation_state::talking ||
        s == ecuio_conversation_state::declined) {
        v->ops.end(v->ops.ctx);
    }
    if (s != ecuio_conversation_state::faulted) {
        v->report.state = ecuio_conversation_state::ended;
    }
}
