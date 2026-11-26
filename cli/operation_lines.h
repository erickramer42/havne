// File/layer: cli/operation_lines.h | havne-cli, a plugin's operations
//             (ADR-0066 section 4)
// Testing: GOLDEN (CliGoldenTest.PluginsCheckListsAPluginsOperations,
//          tests/golden/cli/plugins-check-operation.jsonl)
//
// What `havne-cli plugins list` and `check` write about each plan of a
// plugin with the operation tier (plugin.h): what a run would read, from
// where, and the services it sends to do it.
#ifndef HAVNE_CLI_OPERATION_LINES_H
#define HAVNE_CLI_OPERATION_LINES_H

#include "jsonl.h"
#include "plugin_lines.h"

#include "ecuio/dump.h"
#include "ecuio/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace cli {

// Room for an operation line at every bound of plugin.h's
constexpr size_t kOperationLineBytes = 32768;

// A word for a number of plugin.h's, or "unknown" for one a later core
// added
inline const char* operation_kind_word(uint32_t kind)
{
    return kind == ECUIO_OPERATION_KIND_DUMP ? "dump" : "unknown";
}

inline const char* operation_class_word(uint32_t op_class)
{
    return op_class == ECUIO_OPERATION_CLASS_READ    ? "read"
           : op_class == ECUIO_OPERATION_CLASS_WRITE ? "write"
                                                     : "unknown";
}

inline const char* step_kind_word(uint32_t kind)
{
    return kind == ECUIO_STEP_READ     ? "read"
           : kind == ECUIO_STEP_UPLOAD ? "upload"
                                       : "unknown";
}

inline const char* step_session_word(uint32_t session)
{
    return session == ECUIO_STEP_SESSION_EXTENDED      ? "extended"
           : session == ECUIO_STEP_SESSION_PROGRAMMING ? "programming"
                                                       : "unknown";
}

inline const char* ident_role_word(uint32_t role)
{
    return role == ECUIO_DUMP_ROLE_HARDWARE   ? "hardware"
           : role == ECUIO_DUMP_ROLE_SOFTWARE ? "software"
           : role == ECUIO_DUMP_ROLE_BASE     ? "base"
                                              : "unknown";
}

// The services a plan's steps send, each once, in the order of their
// numbers: [{"sid", "name"}]
inline void operation_services(Line& l, const ecuio_plugin_t* p,
                               uint32_t operation, uint32_t steps)
{
    bool reads = false;
    bool uploads = false;
    for (uint32_t i = 0; i < steps; ++i) {
        ecuio_plugin_operation_step_t s{};
        s.struct_size = sizeof s;
        if (ecuio_plugin_operation_step(p, operation, i, &s) == ECUIO_OK) {
            reads = reads || s.kind == ECUIO_STEP_READ;
            uploads = uploads || s.kind == ECUIO_STEP_UPLOAD;
        }
    }
    l.ch('[');
    if (reads) {
        l.raw("{\"sid\":\"23\",\"name\":\"read memory by address\"}");
    }
    if (uploads) {
        l.raw(reads ? "," : "");
        l.raw("{\"sid\":\"35\",\"name\":\"request upload\"},"
              "{\"sid\":\"36\",\"name\":\"transfer data\"},"
              "{\"sid\":\"37\",\"name\":\"request transfer exit\"}");
    }
    l.ch(']');
}

// One of a plan's steps: {"step", "name", "session", "address" (as many
// hex digits as the request's address takes), "bytes", "chunk" (a read's
// bytes for each request; null for an upload)}
inline void operation_step(Line& l, const ecuio_plugin_t* p, uint32_t operation,
                           uint32_t i)
{
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    ecuio_plugin_operation_step(p, operation, i, &s);
    if (i > 0) {
        l.ch(',');
    }
    l.raw("{\"step\":");
    const char* kind = step_kind_word(s.kind);
    l.string(kind, std::strlen(kind));
    l.key("name");
    l.string(s.name, std::strlen(s.name));
    l.key("session");
    const char* session = step_session_word(s.session);
    l.string(session, std::strlen(session));
    l.key("address");
    char address[24];
    const int digits =
        2 *
        (s.address_bytes >= 1 && s.address_bytes <= 4 ? s.address_bytes : 4);
    std::snprintf(address, sizeof address, "%0*llX", digits,
                  static_cast<unsigned long long>(s.address));
    l.string(address, std::strlen(address));
    l.key("bytes");
    l.u64(s.bytes);
    l.key("chunk");
    if (s.kind == ECUIO_STEP_READ) {
        l.u64(s.chunk);
    } else {
        l.raw("null");
    }
    l.ch('}');
}

// One of a plan's identifiers: {"did", "role"}
inline void operation_ident(Line& l, const ecuio_plugin_t* p,
                            uint32_t operation, uint32_t i)
{
    ecuio_plugin_operation_ident_t d{};
    d.struct_size = sizeof d;
    ecuio_plugin_operation_ident(p, operation, i, &d);
    if (i > 0) {
        l.ch(',');
    }
    char text[8];
    std::snprintf(text, sizeof text, "%04X", d.did & 0xFFFFu);
    l.raw("{\"did\":");
    l.string(text, std::strlen(text));
    l.key("role");
    const char* role = ident_role_word(d.role);
    l.string(role, std::strlen(role));
    l.ch('}');
}

// {"type":"operation", package, id, kind, class ("read" when every step
// only reads: core derives it, a manifest never states it), title, ecu
// (its id among the conversation's ecus), level, unlock_sessions (the
// unlock attempts one run makes), image_bytes (the steps' bytes, added),
// services [{sid, name}] (what its steps send), ident [{did, role}],
// steps [{step, name, session, address, bytes, chunk}]}, one for each
// plan after the conversation line of a plugin with the operation tier
inline void operation_line(Line& l, uint32_t package, const ecuio_plugin_t* p,
                           uint32_t index, const ecuio_plugin_operation_t& o,
                           uint32_t conversation_ecus)
{
    l.raw("{\"type\":\"operation\"");
    l.key("package");
    l.u64(package);
    l.key("id");
    l.string(o.id, std::strlen(o.id));
    l.key("kind");
    const char* kind = operation_kind_word(o.kind);
    l.string(kind, std::strlen(kind));
    l.key("class");
    const char* op_class = operation_class_word(o.op_class);
    l.string(op_class, std::strlen(op_class));
    l.key("title");
    l.string(o.title, std::strlen(o.title));
    l.key("ecu");
    const ecuio_plugin_conversation_ecu_t e =
        conversation_ecu_at(p, conversation_ecus, o.rx_id);
    l.string(e.id, std::strlen(e.id));
    l.key("level");
    const auto level = static_cast<uint8_t>(o.level);
    l.hex_bytes(&level, 1);
    l.key("unlock_sessions");
    l.u64(o.unlock_sessions);
    l.key("image_bytes");
    l.u64(o.image_bytes);
    l.key("services");
    operation_services(l, p, index, o.step_count);
    l.key("ident");
    l.ch('[');
    for (uint32_t i = 0; i < o.ident_count; ++i) {
        operation_ident(l, p, index, i);
    }
    l.ch(']');
    l.key("steps");
    l.ch('[');
    for (uint32_t i = 0; i < o.step_count; ++i) {
        operation_step(l, p, index, i);
    }
    l.raw("]}");
}

} // namespace cli

#endif // HAVNE_CLI_OPERATION_LINES_H
