// File/layer: core/src/conversation_bind.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: ConversationSessionTest (test_core)
//
// The bind of conversation_bind.h: the table of functions over a guest's
// instance, the making at start and the freeing at close.

#include "conversation_bind.h"

#include "access_cut.h"
#include "consent_gate.h"
#include "error_detail.h"
#include "guest_abi.h"
#include "session.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>

std::atomic<bool> g_ecuio_conversation_door{false};

extern "C" void ecuio_test_conversation_binding(int on)
{
    g_ecuio_conversation_door.store(on != 0, std::memory_order_relaxed);
}

namespace {

ecuio_conversation_call call_of(ecuio_bound_channel* b, uint32_t index,
                                const uint32_t* args, uint32_t count)
{
    const ecuio_wasm::call_result r =
        ecuio_wasm::call_instance(b->instance, index, args, count, b->budget);
    ecuio_conversation_call c;
    c.value = static_cast<int32_t>(r.value);
    c.fault = r.end == ecuio_wasm::call_end::returned ? ecuio_guest_fault::none
              : r.end == ecuio_wasm::call_end::budget
                  ? ecuio_guest_fault::budget
                  : ecuio_guest_fault::trap;
    return c;
}

/* n bytes between the guest's buffer and ours: false when the buffer no
 * longer lies inside its memory */
bool copy_guest(ecuio_bound_channel* b, uint8_t* ours, uint32_t n, bool in)
{
    uint8_t* theirs = ecuio_wasm::instance_memory(b->instance, b->buffer_at,
                                                  ECUIO_GUEST_BUFFER_BYTES);
    if (theirs == nullptr) {
        return false;
    }
    if (n > 0) {
        std::memcpy(in ? theirs : ours, in ? ours : theirs, n);
    }
    return true;
}

ecuio_conversation_call op_begin(void* ctx, uint32_t rx, uint32_t tx)
{
    const uint32_t args[2] = {rx, tx};
    return call_of(static_cast<ecuio_bound_channel*>(ctx),
                   ECUIO_GUEST_EXPORT_BEGIN, args, 2);
}

/* A length is judged against the buffer before a byte is read */
ecuio_conversation_call op_next_request(void* ctx, uint8_t* buffer)
{
    auto* b = static_cast<ecuio_bound_channel*>(ctx);
    ecuio_conversation_call c =
        call_of(b, ECUIO_GUEST_EXPORT_NEXT_REQUEST, nullptr, 0);
    if (c.fault == ecuio_guest_fault::none && c.value > 0 &&
        static_cast<uint32_t>(c.value) <= ECUIO_GUEST_BUFFER_BYTES &&
        !copy_guest(b, buffer, static_cast<uint32_t>(c.value), false)) {
        c.fault = ecuio_guest_fault::buffer;
    }
    return c;
}

ecuio_conversation_call op_accept(void* ctx, int32_t kind, uint8_t* buffer,
                                  uint32_t len)
{
    auto* b = static_cast<ecuio_bound_channel*>(ctx);
    ecuio_conversation_call c;
    if (len > ECUIO_GUEST_BUFFER_BYTES || !copy_guest(b, buffer, len, true)) {
        c.fault = ecuio_guest_fault::buffer;
        return c;
    }
    const uint32_t args[2] = {static_cast<uint32_t>(kind), len};
    c = call_of(b, ECUIO_GUEST_EXPORT_ACCEPT, args, 2);
    if (kind == ECUIO_GUEST_SEED && c.fault == ecuio_guest_fault::none &&
        c.value > 0 &&
        static_cast<uint32_t>(c.value) <= ECUIO_GUEST_BUFFER_BYTES &&
        !copy_guest(b, buffer, static_cast<uint32_t>(c.value), false)) {
        c.fault = ecuio_guest_fault::buffer;
    }
    return c;
}

void op_end(void* ctx)
{
    (void)call_of(static_cast<ecuio_bound_channel*>(ctx),
                  ECUIO_GUEST_EXPORT_END, nullptr, 0);
}

bool starts_with(const std::string& s, const char* prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

/* The part's ECU on session channel c by both IDs, or NULL */
const ecuio_conversation_part_ecu* ecu_on(const ecuio_conversation_part& p,
                                          uint32_t rx, uint32_t tx)
{
    for (const ecuio_conversation_part_ecu& e : p.ecus) {
        if (e.rx == rx && e.tx == tx) {
            return &e;
        }
    }
    return nullptr;
}

/* Why a part that's there binds nothing, or NULL when it may bind */
const char* not_in_use(const ecuio_session_t* s)
{
    if (starts_with(s->uri, "mock://") || starts_with(s->uri, "replay://")) {
        return "the simulated source asks nothing";
    }
    if (!s->conversation_consent) {
        /* A part with no publisher can't be consented to (format 2) */
        return s->conversation->has_publisher ? "no consent given"
                                              : "not enabled";
    }
    return nullptr;
}

/* The instance for b on channel c, its ABI and buffer checked */
ecuio_status_t make_one(ecuio_session_talk& t, ecuio_bound_channel& b,
                        uint32_t c, uint32_t rx, uint32_t tx)
{
    const ecuio_conversation_part& p = *t.part;
    char why[ecuio_wasm::REFUSAL_MAX] = {};
    const ecuio_status_t made =
        ecuio_wasm::make_instance(t.module, &b.instance, why);
    if (made != ECUIO_OK) {
        if (made == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("plugin %s's module can't be run: %s",
                             p.plugin_id.c_str(), why);
        }
        return made;
    }
    b.budget = p.budget;
    const ecuio_conversation_call abi =
        call_of(&b, ECUIO_GUEST_EXPORT_ABI, nullptr, 0);
    if (abi.fault != ecuio_guest_fault::none || abi.value != ECUIO_GUEST_ABI) {
        ecuio_detail_set("plugin %s's module answers no guest ABI this core "
                         "speaks (ecuio_abi() must answer %d)",
                         p.plugin_id.c_str(), ECUIO_GUEST_ABI);
        return ECUIO_ERR_LOAD_FAILED;
    }
    const ecuio_conversation_call at =
        call_of(&b, ECUIO_GUEST_EXPORT_BUFFER, nullptr, 0);
    b.buffer_at = static_cast<uint32_t>(at.value);
    if (at.fault != ecuio_guest_fault::none ||
        ecuio_wasm::instance_memory(b.instance, b.buffer_at,
                                    ECUIO_GUEST_BUFFER_BYTES) == nullptr) {
        ecuio_detail_set("plugin %s's module names an exchange buffer that "
                         "doesn't lie inside its memory",
                         p.plugin_id.c_str());
        return ECUIO_ERR_LOAD_FAILED;
    }
    ecuio_conversation_ops ops;
    ops.ctx = &b;
    ops.begin = op_begin;
    ops.next_request = op_next_request;
    ops.accept = op_accept;
    ops.end = op_end;
    if (!ecuio_conversation_init(
            &b.conversation, ops, c, rx, tx, p.services.data(),
            static_cast<uint32_t>(p.services.size()), p.levels.data(),
            static_cast<uint32_t>(p.levels.size()))) {
        ecuio_detail_set("plugin %s's conversation part doesn't fit this "
                         "session's channel %X",
                         p.plugin_id.c_str(), rx);
        return ECUIO_ERR_LOAD_FAILED;
    }
    return ECUIO_OK;
}

/* Every bound channel's instance, and its box on the store */
ecuio_status_t make_all(ecuio_session_t* s, ecuio_session_talk& t)
{
    uint32_t k = 0;
    for (uint32_t c = 0; c < s->module_rx.size(); ++c) {
        const uint32_t rx = s->module_rx[c];
        const uint32_t tx = s->module_tx[c];
        if (ecu_on(*t.part, rx, tx) == nullptr) {
            continue;
        }
        const ecuio_status_t st = make_one(t, t.bound[k], c, rx, tx);
        ++t.count; /* freed at unbind whatever it holds */
        if (st != ECUIO_OK) {
            return st;
        }
        if (t.store) {
            std::string why;
            const ecuio_security::store_io io =
                t.store->attach(&t.bound[k].box, &why);
            if (io != ecuio_security::store_io::ok) {
                if (io == ecuio_security::store_io::refused) {
                    ecuio_detail_set("%s", why.c_str());
                    return ECUIO_ERR_LOAD_FAILED;
                }
                return ECUIO_ERR_OUT_OF_MEMORY;
            }
            t.bound[k].box_attached = true;
        }
        ++k;
    }
    return ECUIO_OK;
}

} // namespace

ecuio_security::store_ref ecuio_session_talk::release() noexcept
{
    for (uint32_t k = 0; k < count; ++k) {
        if (bound[k].box_attached) {
            store->detach(&bound[k].box);
            bound[k].box_attached = false;
        }
        ecuio_wasm::free_instance(bound[k].instance);
        bound[k].instance = nullptr;
    }
    ecuio_wasm::free_module(module);
    module = nullptr;
    return std::move(store);
}

void ecuio_session_talk_free(ecuio_session_talk* t) noexcept
{
    delete t;
}

bool ecuio_session_may_talk(const ecuio_session_t* session,
                            const uint32_t* rx, const uint32_t* tx,
                            uint32_t count)
{
    if (session->conversation == nullptr || !session->conversation_consent ||
        starts_with(session->uri, "mock://") ||
        starts_with(session->uri, "replay://")) {
        return false;
    }
    for (uint32_t c = 0; c < count; ++c) {
        if (ecu_on(*session->conversation, rx[c], tx[c]) != nullptr) {
            return true;
        }
    }
    return false;
}

bool ecuio_session_may_talk(const ecuio_session_t* session)
{
    return ecuio_session_may_talk(
        session, session->module_rx.data(), session->module_tx.data(),
        static_cast<uint32_t>(session->module_rx.size()));
}

bool ecuio_session_wants_consent(const ecuio_session_t* session, uint32_t c)
{
    return session->conversation != nullptr &&
           session->conversation->has_publisher &&
           !session->conversation_consent && c < session->module_rx.size() &&
           !starts_with(session->uri, "mock://") &&
           !starts_with(session->uri, "replay://") &&
           ecu_on(*session->conversation, session->module_rx[c],
                  session->module_tx[c]) != nullptr;
}

bool ecuio_consent_log_line(const ecuio_session_t* session, char* out,
                            size_t cap)
{
    out[0] = '\0';
    if (session->conversation == nullptr) {
        return false;
    }
    const ecuio_conversation_part& p = *session->conversation;
    if (session->consent_applied) {
        char prefix[17];
        for (int i = 0; i < 8; ++i) {
            snprintf(prefix + 2 * i, 3, "%02x",
                     session->consent_fingerprint[i]);
        }
        snprintf(out, cap, "consent applied  %s %s  %s", p.plugin_id.c_str(),
                 p.plugin_version.c_str(), prefix);
        return true;
    }
    for (uint32_t c = 0; c < session->module_rx.size(); ++c) {
        if (ecuio_session_wants_consent(session, c)) {
            snprintf(out, cap, "consent withheld  %s %s", p.plugin_id.c_str(),
                     p.plugin_version.c_str());
            return true;
        }
    }
    return false;
}

bool ecuio_session_talks_on(const ecuio_session_t* session, uint32_t c)
{
    return c < session->module_rx.size() &&
           ecuio_session_may_talk(session, &session->module_rx[c],
                                  &session->module_tx[c], 1);
}

bool ecuio_session_cuts_access(const ecuio_session_t* session,
                               const ecuio_frame_t* frame)
{
    if (!ecuio_access_cut(frame->data, frame->data_length)) {
        return false;
    }
    const uint32_t id = frame->arbitration_id;
    for (uint32_t c = 0; c < session->module_rx.size(); ++c) {
        if ((session->module_rx[c] == id || session->module_tx[c] == id) &&
            ecuio_session_talks_on(session, c)) {
            return true;
        }
    }
    return false;
}

ecuio_status_t ecuio_conversation_bind(ecuio_session_t* s)
{
    if (s->conversation == nullptr || s->talk != nullptr) {
        return ECUIO_OK;
    }
    const ecuio_conversation_part& p = *s->conversation;
    uint32_t matched = 0;
    for (uint32_t c = 0; c < s->module_rx.size(); ++c) {
        matched += ecu_on(p, s->module_rx[c], s->module_tx[c]) != nullptr;
    }
    const char* idle = matched == 0 ? "no channel of the session is one of "
                                      "its ECUs"
                                    : not_in_use(s);
    if (idle != nullptr) {
        ecuio_detail_set("plugin %s's conversation part isn't in use: %s",
                         p.plugin_id.c_str(), idle);
        return ECUIO_OK;
    }
    std::unique_ptr<ecuio_session_talk> t(new (std::nothrow)
                                              ecuio_session_talk);
    if (t == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    t->bound.reset(new (std::nothrow) ecuio_bound_channel[matched]);
    if (t->bound == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    t->part = s->conversation;
    t->store = std::move(s->conversation_store); /* the bind holds it now */
    char why[ecuio_wasm::REFUSAL_MAX] = {};
    const ecuio_status_t loaded = ecuio_wasm::load_module(
        p.code.data(), p.code.size(), ECUIO_GUEST_EXPORTS,
        ECUIO_GUEST_EXPORT_COUNT, &t->module, why);
    if (loaded != ECUIO_OK) {
        if (loaded == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("plugin %s's module can't be run: %s",
                             p.plugin_id.c_str(), why);
        }
        s->conversation_store = t->release();
        return loaded == ECUIO_ERR_INVALID_ARGUMENT ? ECUIO_ERR_LOAD_FAILED
                                                    : loaded;
    }
    const ecuio_status_t made = make_all(s, *t);
    if (made != ECUIO_OK) {
        s->conversation_store = t->release(); /* the next start's again */
        return made;
    }
    t->kept = static_cast<bool>(t->store);
    const std::lock_guard<std::mutex> lock(s->conversation_mutex);
    s->talk.reset(t.release());
    return ECUIO_OK;
}

void ecuio_conversation_unbind(ecuio_session_t* s) noexcept
{
    const std::lock_guard<std::mutex> lock(s->conversation_mutex);
    if (s->talk != nullptr) {
        (void)s->talk->release();
    }
}

ecuio_bound_channel* ecuio_bound_channel_of(ecuio_session_t* s, uint32_t c)
{
    ecuio_session_talk* t = s->talk.get();
    for (uint32_t k = 0; t != nullptr && k < t->count; ++k) {
        if (t->bound[k].conversation.channel == c) {
            return &t->bound[k];
        }
    }
    return nullptr;
}

bool ecuio_conversation_report_of(const ecuio_session_t* s, uint32_t c,
                                  ecuio_conversation_report* out,
                                  uint32_t* generation)
{
    ecuio_bound_channel* b =
        ecuio_bound_channel_of(const_cast<ecuio_session_t*>(s), c);
    if (b == nullptr) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(s->support_mutex);
    *out = b->published;
    *generation = b->generation;
    return true;
}

ecuio_status_t ecuio_conversation_give_word(ecuio_session_t* s, uint32_t c,
                                            uint8_t level, uint64_t vehicle,
                                            int64_t utc)
{
    ecuio_bound_channel* b = ecuio_bound_channel_of(s, c);
    if (b == nullptr) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (s->talk->store) {
        std::string why;
        const ecuio_security::store_key key{vehicle, b->conversation.rx, level};
        const ecuio_security::store_io io =
            s->talk->store->began(key, utc, nullptr, &why);
        if (io == ecuio_security::store_io::out_of_memory) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        if (io != ecuio_security::store_io::ok) {
            ecuio_detail_set("%s", why.c_str());
            return ECUIO_ERR_LOAD_FAILED;
        }
    }
    b->vehicle = vehicle;
    b->word_level.store(level, std::memory_order_relaxed);
    b->word_seq.fetch_add(1, std::memory_order_release);
    return ECUIO_OK;
}
