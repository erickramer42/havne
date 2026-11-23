// File/layer: core/src/guest_abi.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: PluginConversationTest (test_signals)
//
// The guest ABI (ADR-0066 section 2.2): what a conversation plugin's
// module exports, by name and type, in the order core calls them by.
// The loader checks a module against this list when it reads a package
// (wasm_check.h's check_module), so one that lacks an export or has it
// at another type never loads; the memory's shape is that check's own
// rule. Bytes cross only through the guest's exchange buffer, which the
// guest places in its own memory and names through ecuio_buffer.
//
//   ecuio_abi     () -> i32          ECUIO_GUEST_ABI, or the module is
//                                    refused where it is bound
//   ecuio_buffer  () -> i32          the offset of the exchange buffer,
//                                    ECUIO_GUEST_BUFFER_BYTES long
//   begin         (rx, tx) -> i32    a module joins: the session it
//                                    needs (0 the default, 3 the
//                                    extended), negative to decline it
//   next_request  () -> i32          a request written at the buffer:
//                                    its length, 0 for none this cycle
//   accept        (kind, len) -> i32 len bytes are at the buffer: a
//                                    reply, a timeout, a seed or an
//                                    unlock's result; for a seed the
//                                    key's length, written at the buffer
//   end           () -> ()           the session closes

#ifndef ECUIO_GUEST_ABI_H
#define ECUIO_GUEST_ABI_H

#include "wasm_check.h"

#include <cstddef>
#include <cstdint>

constexpr int32_t ECUIO_GUEST_ABI = 1;
constexpr uint32_t ECUIO_GUEST_BUFFER_BYTES = 4096;

/* What accept's kind says is at the buffer (conversation.h hands each) */
enum ecuio_guest_accept : int32_t {
    ECUIO_GUEST_REPLY = 1,   /* the reply to the module's request: its
                                positive or negative reply, whole */
    ECUIO_GUEST_TIMEOUT = 2, /* no reply: its wait ran out or its write
                                failed; len 0 */
    ECUIO_GUEST_SEED = 3,    /* the level, then the seed: the key is
                                written at the buffer, its length the
                                answer */
    ECUIO_GUEST_UNLOCK = 4,  /* an unlock's end: the level, the result
                                below, the module's code (0 for none) */
};

/* An unlock's result, ECUIO_GUEST_UNLOCK's second byte */
enum ecuio_guest_unlock : uint8_t {
    ECUIO_GUEST_UNLOCKED = 0, /* 67, or a seed of zeroes: the level is open */
    ECUIO_GUEST_REFUSED = 1,  /* the key was refused, unanswered or its
                                 write failed, or the session was lost
                                 with the key on its way: one failed
                                 attempt, the delay runs */
    ECUIO_GUEST_ENDED = 2,    /* the attempt ended before any key reached
                                 the bus: nothing counted */
};

/* An export's place in ECUIO_GUEST_EXPORTS */
enum ecuio_guest_export : uint32_t {
    ECUIO_GUEST_EXPORT_ABI = 0,
    ECUIO_GUEST_EXPORT_BUFFER,
    ECUIO_GUEST_EXPORT_BEGIN,
    ECUIO_GUEST_EXPORT_NEXT_REQUEST,
    ECUIO_GUEST_EXPORT_ACCEPT,
    ECUIO_GUEST_EXPORT_END,
    ECUIO_GUEST_EXPORT_COUNT
};

constexpr ecuio_wasm::wanted_export ECUIO_GUEST_EXPORTS[] = {
    {"ecuio_abi", ":i"},    {"ecuio_buffer", ":i"}, {"begin", "ii:i"},
    {"next_request", ":i"}, {"accept", "ii:i"},     {"end", ":"},
};
static_assert(sizeof ECUIO_GUEST_EXPORTS / sizeof ECUIO_GUEST_EXPORTS[0] ==
                  ECUIO_GUEST_EXPORT_COUNT,
              "an export added to the guest ABI takes its place in the enum");

#endif /* ECUIO_GUEST_ABI_H */
