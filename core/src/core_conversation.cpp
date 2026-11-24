// File/layer: core/src/core_conversation.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: CoreConversationTest (test_core); HARNESS: the
//          transports' conversation cases
//
// The three public calls of a bound module (core.h, ADR-0066 section
// 2.4): the report's copy, the security store with its fallback label,
// and the unlock with its refusals in words. The store's key for a
// module is core's own read (the write tier's V1): its serial (22 F18C),
// else the car's VIN, else the label trimmed and case folded, each
// hashed with its source.

#include "conversation_bind.h"
#include "error_detail.h"
#include "request_engine_ident.h"
#include "session.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

static_assert(static_cast<uint8_t>(ecuio_unlock_state::refused) ==
                      ECUIO_UNLOCK_REFUSED &&
                  static_cast<uint8_t>(ecuio_unlock_state::delayed) ==
                      ECUIO_UNLOCK_DELAYED &&
                  static_cast<uint8_t>(ecuio_unlock_state::unlocked) ==
                      ECUIO_UNLOCK_UNLOCKED,
              "the report's unlock codes are the machine's, in order");
static_assert(static_cast<uint8_t>(ecuio_conversation_fault::channel) ==
                      ECUIO_CONVERSATION_FAULT_CHANNEL &&
                  static_cast<uint8_t>(ecuio_conversation_fault::door) ==
                      ECUIO_CONVERSATION_FAULT_DOOR,
              "the report's fault codes are the conversation's, in order");

namespace {

uint8_t state_code(ecuio_conversation_state s)
{
    switch (s) {
    case ecuio_conversation_state::unbegun:
        return ECUIO_CONVERSATION_UNBEGUN;
    case ecuio_conversation_state::talking:
        return ECUIO_CONVERSATION_TALKING;
    case ecuio_conversation_state::declined:
        return ECUIO_CONVERSATION_DECLINED;
    case ecuio_conversation_state::faulted:
        return ECUIO_CONVERSATION_FAULTED;
    default:
        return ECUIO_CONVERSATION_ENDED;
    }
}

/* The identity's text and source, or why there is none yet */
enum class identity_is : uint8_t { found, unsettled, none };

/* An item's answer as text, NULL while unasked; "" when it settled
 * without one */
const char* item_text(const ecuio_channel_identity_t& id, uint8_t a, uint8_t b,
                      uint8_t c)
{
    for (uint32_t i = 0; i < id.items && i < ECUIO_IDENTITY_MAX_ITEMS; ++i) {
        const ecuio_identity_item_t& it = id.list[i];
        if (it.length == 3 && it.request[0] == a && it.request[1] == b &&
            it.request[2] == c) {
            if (it.outcome == ECUIO_IDENTITY_UNASKED) {
                return nullptr;
            }
            return it.outcome == ECUIO_IDENTITY_ANSWERED ? it.text : "";
        }
    }
    return ""; /* not asked at all: settled with none */
}

/* Whether the session's VIN can still come: a 09 02 or F190 unasked on
 * some channel */
bool vin_may_come(const ecuio_session_t* s)
{
    for (uint32_t c = 0; c < s->support_count; ++c) {
        const ecuio_channel_identity_t& id = s->identity[c];
        for (uint32_t i = 0; i < id.items && i < ECUIO_IDENTITY_MAX_ITEMS;
             ++i) {
            const ecuio_identity_item_t& it = id.list[i];
            const bool vin = (it.length == 2 && it.request[0] == 0x09 &&
                              it.request[1] == 0x02) ||
                             (it.length == 3 && it.request[0] == 0x22 &&
                              it.request[1] == 0xF1 && it.request[2] == 0x90);
            if (vin && it.outcome == ECUIO_IDENTITY_UNASKED) {
                return true;
            }
        }
    }
    return false;
}

/* The label trimmed of ASCII spaces at both ends, ASCII letters folded */
std::string folded(const std::string& label)
{
    size_t from = 0;
    size_t to = label.size();
    while (from < to && (label[from] == ' ' || label[from] == '\t')) {
        ++from;
    }
    while (to > from && (label[to - 1] == ' ' || label[to - 1] == '\t')) {
        --to;
    }
    std::string out = label.substr(from, to - from);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

/* Channel c's module's identity for the store, under support_mutex */
identity_is identity_of(const ecuio_session_t* s, uint32_t c, std::string* out)
{
    const char* serial = item_text(s->identity[c], 0x22, 0xF1, 0x8C);
    if (serial == nullptr) {
        return identity_is::unsettled;
    }
    if (serial[0] != '\0') {
        *out = std::string("serial:") + serial;
        return identity_is::found;
    }
    for (uint32_t k = 0; k < s->support_count; ++k) {
        const ecuio_ident_vin v = ecuio_identity_vin_of(s->identity[k]);
        if (v.in) {
            *out = std::string("vin:") + v.text;
            return identity_is::found;
        }
    }
    if (vin_may_come(s)) {
        return identity_is::unsettled;
    }
    const std::string label = folded(s->security_label);
    if (label.empty()) {
        return identity_is::none;
    }
    *out = "label:" + label;
    return identity_is::found;
}

int64_t utc_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/* The delay level's view says is left now, in us; 0 for none */
int64_t delay_left_us(const ecuio_bound_channel& b, uint8_t level)
{
    for (uint32_t k = 0; k < b.level_count; ++k) {
        if (b.levels[k].level == level) {
            const int64_t since =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - b.published_at)
                    .count();
            const int64_t left = b.levels[k].delay_left_us - since;
            return left > 0 ? left : 0;
        }
    }
    return 0;
}

ecuio_status_t too_soon(uint32_t* out_wait_ms, int64_t left_us)
{
    const int64_t ms = (left_us + 999) / 1000;
    if (out_wait_ms != nullptr) {
        *out_wait_ms = static_cast<uint32_t>(ms > UINT32_MAX ? UINT32_MAX : ms);
    }
    ecuio_detail_set("the module's delay after a refused key runs %lld ms more",
                     static_cast<long long>(ms));
    return ECUIO_ERR_TOO_SOON;
}

/* The store's key for the module at the first unlock: its counts handed
 * to the worker; TOO_SOON while its identity is read, or while the
 * stored delay runs */
ecuio_status_t first_unlock(ecuio_session_t* s, ecuio_bound_channel& b,
                            uint32_t c, uint8_t level, uint32_t* out_wait_ms)
{
    std::string identity;
    identity_is found = identity_is::none;
    {
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        found = identity_of(s, c, &identity);
    }
    if (found == identity_is::unsettled) {
        ecuio_detail_set("the module's identity is still being read: ask "
                         "again shortly");
        return ECUIO_ERR_TOO_SOON;
    }
    if (found == identity_is::none) {
        ecuio_detail_set("the security store names a module by its serial, "
                         "the car's VIN or a label: this module gave no "
                         "serial, the car no VIN, and no label was given");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    b.vehicle = ecuio_security::store_vehicle_of(identity);
    b.vehicle_known = true;
    const int64_t utc = utc_now();
    const ecuio_security::module_access& a = b.conversation.access;
    for (uint32_t k = 0; k < a.count && k < ECUIO_PLUGIN_MAX_LEVELS; ++k) {
        const ecuio_security::store_entry e = s->talk->store->counts(
            {b.vehicle, b.conversation.rx, a.levels[k].level});
        b.restore_failed[k] = e.failed;
        b.restore_last_utc[k] = e.last_utc;
        if (a.levels[k].level == level) {
            const int64_t left = ecuio_security::stored_delay_left_us(
                a.levels[k].delay_us, e.last_utc, utc);
            if (left > 0) {
                b.restore_utc = utc;
                b.restore_seq.fetch_add(1, std::memory_order_release);
                return too_soon(out_wait_ms, left);
            }
        }
    }
    b.restore_utc = utc;
    b.restore_seq.fetch_add(1, std::memory_order_release);
    return ECUIO_OK;
}

/* The refusals checked before anything is marked or sent */
ecuio_status_t judge(ecuio_session_t* s, ecuio_bound_channel& b, uint8_t level,
                     uint32_t generation, uint32_t* out_wait_ms)
{
    ecuio_conversation_report r;
    uint32_t now_generation = 0;
    int64_t left = 0;
    {
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        r = b.published;
        now_generation = b.generation;
        left = delay_left_us(b, level);
    }
    if (generation != now_generation) {
        return ECUIO_ERR_REPORT_CHANGED;
    }
    if (r.state != ecuio_conversation_state::talking) {
        ecuio_detail_set("the module's code isn't talking on this channel");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_security::module_access& a = b.conversation.access;
    const uint32_t k = ecuio_security::find_level(a, level);
    if (k >= a.count) {
        ecuio_detail_set("the plugin declares no security level %02X for "
                         "this module",
                         level);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (2u + a.levels[k].key_bytes > b.talk_bound.load()) {
        ecuio_detail_set("level %02X's key of %u bytes doesn't fit one "
                         "request on this adapter (%u bytes)",
                         level, a.levels[k].key_bytes, b.talk_bound.load());
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (b.functional.load()) {
        ecuio_detail_set("security access isn't asked while the functional "
                         "address has a session of its own (uds-generic's "
                         "did=7E7: with session=03): leave one of them out");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (b.words_given != r.words_ended) {
        ecuio_detail_set("an unlock is under way on this module");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (left > 0) {
        return too_soon(out_wait_ms, left);
    }
    return ECUIO_OK;
}

} // namespace

extern "C" {

ecuio_status_t
ecuio_session_get_channel_conversation(const ecuio_session_t* session,
                                       uint32_t channel,
                                       ecuio_channel_conversation_t* out)
{
    if (session == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (channel >= session->module_rx.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out = ecuio_channel_conversation_t{};
    out->rx_id = session->module_rx[channel];
    const std::lock_guard<std::mutex> calls(session->conversation_mutex);
    ecuio_bound_channel* b =
        ecuio_bound_channel_of(const_cast<ecuio_session_t*>(session), channel);
    if (b == nullptr) {
        return ECUIO_OK;
    }
    const std::lock_guard<std::mutex> lock(session->support_mutex);
    const ecuio_conversation_report& r = b->published;
    out->flags = session->talk->kept ? 0u : ECUIO_CONVERSATION_NOT_KEPT;
    out->generation = b->generation;
    out->state = state_code(r.state);
    out->session = r.session;
    out->fault = static_cast<uint8_t>(r.fault);
    out->fault_len = r.fault_len;
    std::memcpy(out->fault_bytes, r.fault_bytes, sizeof out->fault_bytes);
    out->level = r.level;
    out->unlock = static_cast<uint8_t>(r.unlock);
    out->nrc = r.nrc;
    const int64_t left = delay_left_us(*b, r.level);
    out->delay_left_ms = static_cast<uint32_t>((left + 999) / 1000);
    out->words_ended = r.words_ended;
    out->requests = r.requests;
    for (uint32_t k = 0; k < b->level_count; ++k) {
        if (b->levels[k].level == r.level) {
            out->failed = b->levels[k].failed;
        }
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_security_store(ecuio_session_t* session,
                                            const char* path, const char* label)
{
    ecuio_detail_clear();
    if (session == nullptr || path == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->started) {
        ecuio_detail_set("the security store is named before the session "
                         "starts");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::string why;
        const ecuio_security::store_io io = ecuio_security::store_open(
            path, &session->conversation_store, &why);
        if (io == ecuio_security::store_io::out_of_memory) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        if (io != ecuio_security::store_io::ok) {
            ecuio_detail_set("%s", why.c_str());
            return ECUIO_ERR_LOAD_FAILED;
        }
        session->security_label = label == nullptr ? "" : label;
    } catch (const std::bad_alloc&) {
        session->conversation_store.reset();
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_security_unlock(ecuio_session_t* session,
                                             uint32_t channel, uint8_t level,
                                             uint32_t generation,
                                             uint32_t* out_wait_ms)
{
    ecuio_detail_clear();
    if (out_wait_ms != nullptr) {
        *out_wait_ms = 0;
    }
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    /* the whole call, so a second caller or close's release waits */
    const std::lock_guard<std::mutex> calls(session->conversation_mutex);
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    if (channel >= session->module_rx.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_bound_channel* b = ecuio_bound_channel_of(session, channel);
    if (b == nullptr && ecuio_session_wants_consent(session, channel)) {
        ecuio_detail_set("no consent was given to the plugin whose code "
                         "talks on this channel");
        return ECUIO_ERR_CONSENT_REQUIRED;
    }
    if (b == nullptr) {
        ecuio_detail_set("no plugin's code talks on this channel");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_status_t judged =
        judge(session, *b, level, generation, out_wait_ms);
    if (judged != ECUIO_OK) {
        return judged;
    }
    if (session->talk->kept && !b->vehicle_known) {
        const ecuio_status_t first =
            first_unlock(session, *b, channel, level, out_wait_ms);
        if (first != ECUIO_OK) {
            return first;
        }
    }
    const ecuio_status_t given = ecuio_conversation_give_word(
        session, channel, level, b->vehicle, utc_now());
    if (given == ECUIO_OK) {
        ++b->words_given;
    }
    return given;
}

} // extern "C"
