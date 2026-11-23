// File/layer: core/src/conversation_registry.h | internal, C++17, Qt-free,
//             every platform
// Testing: UNIT: ConversationRegistryTest (test_core)
//
// A plugin's conversation part beside its registered sets (ADR-0066
// section 2.4): nothing else maps a set back to the plugin that carries
// it. ecuio_plugin_register keeps an immutable copy of the part under
// each variant's set pointer; ecuio_session_create takes it with its
// profile hold, before attach, and holds it for the session's life;
// ecuio_plugin_free forgets the sets once no session holds one, and a
// session's copy outlives that. A set registered under a caller's name
// alone, never through ecuio_plugin_register, has no part.

#ifndef ECUIO_CONVERSATION_REGISTRY_H
#define ECUIO_CONVERSATION_REGISTRY_H

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/signals.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* An ECU the module talks to: the manifest's id and both IDs */
struct ecuio_conversation_part_ecu {
    std::string id;
    uint32_t rx = 0;
    uint32_t tx = 0;
};

/* A plugin's conversation part as its loader judged it, never changed
 * once made */
struct ecuio_conversation_part {
    std::string plugin_id;
    std::string plugin_version;
    std::vector<uint8_t> code;
    uint8_t sha256[32] = {};
    uint32_t budget = 0;
    std::vector<ecuio_conversation_part_ecu> ecus;
    std::vector<ecuio_plugin_service_t> services;
    std::vector<ecuio_plugin_security_level_t> levels;
};

using ecuio_conversation_part_ref =
    std::shared_ptr<const ecuio_conversation_part>;

/* Keeps part under set (a set kept already takes the new part).
 * ECUIO_ERR_OUT_OF_MEMORY with the table unchanged; *added says whether
 * the set is new to the table, so a caller undoing its own keep forgets
 * only a set it added. */
ecuio_status_t ecuio_conversation_keep(const ecuio_signal_set_t* set,
                                       ecuio_conversation_part_ref part,
                                       bool* added) noexcept;

/* Drops set's part from the table; a holder's copy lives on. NULL, or a
 * set not kept, is a no-op. */
void ecuio_conversation_forget(const ecuio_signal_set_t* set) noexcept;

/* set's part, NULL for none: a copy of the table's reference, which
 * allocates nothing */
ecuio_conversation_part_ref
ecuio_conversation_of(const ecuio_signal_set_t* set) noexcept;

/* The sets the table keeps: the tests' count */
size_t ecuio_conversation_kept() noexcept;

#endif /* ECUIO_CONVERSATION_REGISTRY_H */
