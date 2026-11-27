// File/layer: core/src/conversation.h | internal, C++17, Qt-free, every
//             platform; no clock, no file, no thread of its own
// Testing: UNIT: ConversationTest (test_obd2); FUZZ: fuzz_conversation
//
// One module's conversation (ADR-0066 section 2.4): a plugin's code
// talking to one ECU on one channel of a session's request engine,
// through a table of functions in the guest ABI's shape (guest_abi.h).
// The table is the only way in: a module bound to a session fills it
// with calls into the sandbox; a test or a fuzzer fills it with its own.
// Nothing outside core/src can hand core a table.
//
// What a module's code can never cause, by where each rule sits:
//   - a 27: the guest's list refuses it in a request, then the door's
//     kinds (a plain talk is never a 27); core builds every seed and key;
//   - a seed without the caller's word: only ecuio_conversation_unlock
//     reaches the attempts' machine (security_access.h);
//   - two keys for one seed: the machine, and a key's send ends the
//     channel's window;
//   - a 27 inside the delay: the machine's ask;
//   - anything outside tier 2's ceiling: the door, whatever the manifest
//     says;
//   - anything at all after a fault: the module is never called again,
//     not even its end.
//
// A fault (a call's budget, trap or buffer; begin's answer; a request
// outside the rules; a key of the wrong length; a key the door refuses
// inside its window) ends the conversation for that module, ends the
// channel's window and lets its talk go. The session core entered
// stays, with its keep-alive, and the rotation reads on: nothing in the
// engine is undone. A key whose window the session, the link or a late
// step closed is no fault: the attempt ends, nothing counted.
//
// One thread drives it (a session's worker): the calls below are made
// between the engine's own, with the session's time passed in. Its
// driver owns the store: an unlock's mark is written before the word is
// given, so each word's end (words_ended moving), a refusal's too, is a
// settle to save.

#ifndef ECUIO_CONVERSATION_H
#define ECUIO_CONVERSATION_H

#include "ecuio/plugin.h"
#include "guest_abi.h"
#include "request_engine.h"
#include "request_talk.h"
#include "security_access.h"

#include <cstdint>

/* How a call into the module ended */
enum class ecuio_guest_fault : uint8_t {
    none,
    budget, /* it ran out of instructions */
    trap,   /* it trapped */
    buffer, /* the exchange buffer lies outside its memory */
};

/* A call's answer: value is the export's result when fault is none */
struct ecuio_conversation_call {
    int32_t value = 0;
    ecuio_guest_fault fault = ecuio_guest_fault::none;
};

/* The table: the guest ABI's exports, each a copy across the exchange
 * buffer (ECUIO_GUEST_BUFFER_BYTES). next_request writes a request at
 * buffer and answers its length; accept reads len bytes at buffer and,
 * for a seed, writes the key there and answers its length. */
struct ecuio_conversation_ops {
    void* ctx = nullptr;
    ecuio_conversation_call (*begin)(void* ctx, uint32_t rx,
                                     uint32_t tx) = nullptr;
    ecuio_conversation_call (*next_request)(void* ctx,
                                            uint8_t* buffer) = nullptr;
    ecuio_conversation_call (*accept)(void* ctx, int32_t kind, uint8_t* buffer,
                                      uint32_t len) = nullptr;
    void (*end)(void* ctx) = nullptr;
};

/* At most once each this long while the module answers "nothing" */
constexpr int64_t ECUIO_CONVERSATION_POLL_US = 50000;

enum class ecuio_conversation_state : uint8_t {
    unbegun,  /* begin not yet called */
    talking,  /* begin answered 0 or 3 */
    declined, /* begin answered a negative: the module reads as data only */
    faulted,  /* a fault ended it: the module is never called again */
    ended,    /* its end was called */
};

enum class ecuio_conversation_fault : uint8_t {
    none,
    budget,     /* a call ran out of instructions */
    trap,       /* a call trapped */
    buffer,     /* the exchange buffer lies outside the module's memory */
    begin,      /* begin answered neither 0, 3 nor a negative */
    length,     /* a request's length outside 0 to the exchange buffer */
    bound,      /* a request past the channel's bound */
    service,    /* a request outside the guest's list (never 27, 10, 3E
                   or 2A: ecuio_tier2_guest_may_send) */
    undeclared, /* a service or sub-function the manifest doesn't declare */
    door,       /* the engine's door refused the request */
    key,        /* a key not of its level's declared length */
    window,     /* the door refused the key inside its window */
    channel,    /* the engine has no such channel: the module never ran */
};

/* Where the caller's word for a level stands */
enum class ecuio_unlock_state : uint8_t {
    none,         /* no word given */
    waiting,      /* given, taken at the next step */
    seed_offered, /* the attempt runs: its seed is offered or out */
    key_offered,  /* its key is offered or out: its end counts */
    unlocked,     /* the level is open (67, or a seed of zeroes) */
    failed,       /* the key's end was no 67: counted, the delay runs */
    ended,        /* the attempt ended before a key went: nothing counted */
    lost,         /* the session was entered again or left (the loss count
                     rose, or the step left ENTERED): every level is
                     locked */
    /* refusals of the word: no attempt started, nothing sent */
    no_level,         /* the module declares no such level */
    key_too_long,     /* the key's request is past the channel's bound */
    busy,             /* an attempt was under way */
    delayed,          /* the delay runs: left_us of it */
    already_unlocked, /* the level is unlocked in this session */
    refused,          /* the engine took no seed (a functional channel in
                         a session; init refuses a window the door
                         wouldn't take) */
};

struct ecuio_conversation_report {
    ecuio_conversation_state state = ecuio_conversation_state::unbegun;
    uint8_t session = 0; /* what begin asked: 0 the default, 3 the extended */
    ecuio_conversation_fault fault = ecuio_conversation_fault::none;
    uint8_t fault_len = 0;       /* of fault_bytes: a refused request's */
    uint8_t fault_bytes[4] = {}; /* its first bytes */
    /* the level of the word waiting, the attempt running, or the last
     * word's end */
    uint8_t level = 0;
    ecuio_unlock_state unlock = ecuio_unlock_state::none;
    uint8_t nrc = 0;          /* a refused seed's or key's code; 0 for none */
    int64_t left_us = 0;      /* delayed: the delay left at the step */
    uint32_t words_ended = 0; /* each word's end: a settle to save */
    uint32_t requests = 0;    /* the module's requests the door took */
};

/* What the conversation has on the channel's talk */
enum class ecuio_conversation_talk : uint8_t { none, request, seed, key };

struct ecuio_conversation {
    ecuio_conversation_ops ops;
    uint32_t channel = 0;
    uint32_t rx = 0;
    uint32_t tx = 0;
    ecuio_plugin_service_t services[ECUIO_PLUGIN_MAX_SERVICES] = {};
    uint32_t service_count = 0;
    ecuio_security::module_access access;
    ecuio_conversation_report report;
    ecuio_conversation_talk own = ecuio_conversation_talk::none;
    bool kept = false; /* buffer holds the reply that settled own */
    uint32_t kept_len = 0;
    bool word = false; /* an unlock's word waits for the step */
    uint8_t word_level = 0;
    bool was_entered = false; /* the channel's session at the last look */
    uint16_t seen_lost = 0;   /* its session_lost at the last look */
    uint16_t seen_handed = 0; /* and its session_handed */
    int64_t next_poll_us = 0;
    uint8_t buffer[ECUIO_GUEST_BUFFER_BYTES] = {};
};

/* Makes v for the ECU rx/tx on the engine's channel: the services the
 * manifest declares, and its levels whose rx_id is rx. False when a list
 * is past its bound or a level doesn't fit the machine, a PDU (key_bytes
 * past ECUIO_PLUGIN_KEY_MAX) or the door's window (the loader refuses
 * all of them first). */
bool ecuio_conversation_init(ecuio_conversation* v,
                             const ecuio_conversation_ops& ops,
                             uint32_t channel, uint32_t rx, uint32_t tx,
                             const ecuio_plugin_service_t* services,
                             uint32_t service_count,
                             const ecuio_plugin_security_level_t* levels,
                             uint32_t level_count);

/* The module joins at now_us: its begin(rx, tx). 0 the default session,
 * nothing to do; 3 the extended one, entered through the engine's own
 * step (ecuio_engine_session_enter); a negative declines, and the module
 * reads as data only; anything else is a fault. A channel the engine
 * lacks is the fault channel, begin never called. Once only. */
void ecuio_conversation_begin(ecuio_conversation* v, ecuio_request_engine* e,
                              int64_t now_us);

/* The driver's call after the engine took a frame on the channel: when
 * that frame settled the conversation's talk, its bytes are kept for
 * the module (a talk's result carries no reply bytes) */
void ecuio_conversation_frame(ecuio_conversation* v,
                              const ecuio_request_engine* e,
                              const uint8_t* bytes, uint32_t len);

/* Once a pass, at now_us: a settled talk to the module, the key, a lost
 * session, the caller's word, then the module's next request. Faulted,
 * only its talk let go once it settles. */
void ecuio_conversation_step(ecuio_conversation* v, ecuio_request_engine* e,
                             int64_t now_us);

/* The caller's word for level, taken at the next step. False, and
 * nothing kept, while another word waits or the module isn't talking.
 * While an attempt runs the report stays the attempt's; the step judges
 * the word busy. */
bool ecuio_conversation_unlock(ecuio_conversation* v, uint8_t level);

/* When the conversation next has a step to take, from now_us: now while
 * a word or a settled talk waits (a faulted one's to let go), the poll's
 * time while the module has nothing, never while its talk is out or it
 * isn't talking */
int64_t ecuio_conversation_next_us(const ecuio_conversation* v,
                                   const ecuio_request_engine* e,
                                   int64_t now_us);

/* At the close: the module's end, unless it never began or faulted;
 * once only */
void ecuio_conversation_end(ecuio_conversation* v);

#endif /* ECUIO_CONVERSATION_H */
