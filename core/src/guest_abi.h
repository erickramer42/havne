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
