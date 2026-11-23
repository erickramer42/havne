// File/layer: core/src/conversation_bind.h | internal, C++17, Qt-free,
//             every platform
// Testing: UNIT: ConversationSessionTest (test_core)
//
// A plugin's module bound to its session (ADR-0066 section 2.4): the
// part create took (conversation_registry.h) made into one loaded module
// and one instance for each bound channel at start, each instance driving
// one conversation (conversation.h) on the transport's worker.
//
//   - Bound: a session channel whose rx and tx both equal one of the
//     part's ECUs (a functional channel never does), on a requesting
//     transport (mock:// and replay:// ask nothing), and only when
//     ecuio_plugin_code_may_run says so (consent_gate.h).
//   - Made at start on the caller's thread, before the worker: the
//     module loaded once, an instance for each bound channel, its
//     ecuio_abi() 1 and its buffer inside its memory. A refusal is
//     start's status with its words, nothing left bound.
//   - After start every call into a guest is the worker's, between two
//     passes of its engine; close frees the instances once it joined.
//   - The caller reaches a conversation only through atomics the worker
//     takes (a word), and the reports it publishes under the session's
//     support mutex; the worker takes no lock of the store's and waits
//     for nothing (security_store_owner.h's settle box).

#ifndef ECUIO_CONVERSATION_BIND_H
#define ECUIO_CONVERSATION_BIND_H

#include "conversation.h"
#include "conversation_registry.h"
#include "security_store_owner.h"
#include "wasm_sandbox.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

/* One bound channel: its instance, its conversation and what crosses
 * threads to it */
struct ecuio_bound_channel {
    ecuio_wasm::instance* instance = nullptr;
    uint32_t buffer_at = 0; /* the guest's exchange buffer */
    uint32_t budget = 0;
    ecuio_conversation conversation;
    /* The caller's word: word_level written, then word_seq moved on */
    std::atomic<uint8_t> word_level{0};
    std::atomic<uint32_t> word_seq{0};
    uint32_t word_seen = 0; /* the worker's */
    uint32_t ends_seen = 0; /* the report's words_ended last settled */
    uint64_t vehicle = 0;   /* the store's key for this module */
    ecuio_security::settle_box box;
    /* The worker's last publish, under the session's support_mutex: the
     * report, and each level's count and delay left at published_at */
    ecuio_conversation_report published;
    uint32_t generation = 0;
    struct level_view {
        uint8_t level = 0;
        uint32_t failed = 0;
        int64_t delay_left_us = 0;
    };
    level_view levels[ECUIO_PLUGIN_MAX_LEVELS];
    uint32_t level_count = 0;
    std::chrono::steady_clock::time_point published_at;
    /* The worker's, from its start: the channel's bound for one request,
     * and whether a functional channel has a session (no seed then) */
    std::atomic<uint32_t> talk_bound{0};
    std::atomic<bool> functional{false};
    /* The store's counts for each level (the machine's order), handed to
     * the worker at the first unlock: the fields, then restore_seq */
    uint32_t restore_failed[ECUIO_PLUGIN_MAX_LEVELS] = {};
    int64_t restore_last_utc[ECUIO_PLUGIN_MAX_LEVELS] = {};
    int64_t restore_utc = 0;
    std::atomic<uint32_t> restore_seq{0};
    uint32_t restore_seen = 0; /* the worker's */
    /* The unlock caller's: the words given, and the store's key known */
    uint32_t words_given = 0;
    bool vehicle_known = false;
    /* The worker's: the level of the word it last took, the 33 drops put
     * back once, the box on the store, and at its close whether the
     * module's end came while the channel's session stood (before the
     * engine's 10 01) */
    uint8_t word_taken = 0;
    bool rejoined = false;
    bool box_attached = false;
    bool ended_in_session = false;
};

/* A session's binding: NULL in the session when nothing is bound */
struct ecuio_session_talk {
    ecuio_conversation_part_ref part;
    ecuio_wasm::loaded* module = nullptr;
    std::unique_ptr<ecuio_bound_channel[]> bound;
    uint32_t count = 0;
    ecuio_security::store_ref store; /* the session's, taken at the bind;
                                        empty: the counts are the session's */
    bool began = false;              /* the worker's */
    bool kept = false;               /* a store was bound: fixed at the bind */
    /* The settles saved, the instances and the module freed, the store
     * handed back; the reports stay for the getters until destroy */
    ecuio_security::store_ref release() noexcept;
    ~ecuio_session_talk() { (void)release(); }
};

/* Whether session's part names one of the channels rx/tx (count of
 * them, the transport's own, which its module tables copy) by both IDs,
 * on a requesting transport, with the consent create found: the session
 * then asks as one with requests from attach on */
bool ecuio_session_may_talk(const ecuio_session_t* session,
                            const uint32_t* rx, const uint32_t* tx,
                            uint32_t count);
/* The same over the session's module tables, once attach has set them */
bool ecuio_session_may_talk(const ecuio_session_t* session);
/* Whether the part talks on module channel c alone: the channels the
 * bind makes, by the same rule */
bool ecuio_session_talks_on(const ecuio_session_t* session, uint32_t c);

/* At start, before the worker: binds what section 1 says, or nothing.
 * ECUIO_ERR_LOAD_FAILED or ECUIO_ERR_OUT_OF_MEMORY with nothing bound;
 * OK with the detail saying why nothing is in use when the part is
 * there and nothing binds. Once only: a second call changes nothing. */
ecuio_status_t ecuio_conversation_bind(ecuio_session_t* session);

/* At close, after the worker has joined: the settles saved, the
 * instances and the module freed, the last reports kept */
void ecuio_conversation_unbind(ecuio_session_t* session) noexcept;

/* The bound channel for session channel c, or NULL: the worker's, or a
 * caller's under conversation_mutex */
ecuio_bound_channel* ecuio_bound_channel_of(ecuio_session_t* session,
                                            uint32_t c);

/* The report the worker last published for channel c, and its
 * generation: false for a channel nothing is bound to */
bool ecuio_conversation_report_of(const ecuio_session_t* session, uint32_t c,
                                  ecuio_conversation_report* out,
                                  uint32_t* generation);

/* Gives channel c's conversation the word for level, after the store's
 * mark under (vehicle, the channel's rx, level) at UTC second utc when a
 * store is bound: ECUIO_ERR_INVALID_ARGUMENT for a channel nothing is
 * bound to, ECUIO_ERR_LOAD_FAILED with the store's words when the mark
 * can't be saved, ECUIO_ERR_OUT_OF_MEMORY; nothing given then */
ecuio_status_t ecuio_conversation_give_word(ecuio_session_t* session,
                                            uint32_t c, uint8_t level,
                                            uint64_t vehicle, int64_t utc);

#endif /* ECUIO_CONVERSATION_BIND_H */
