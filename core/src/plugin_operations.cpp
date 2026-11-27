// File/layer: core/src/plugin_operations.cpp | internal, C++17, Qt-free
// Testing: UNIT: PluginOperationTest (test_core), test_c_abi; FUZZ:
//          fuzz_plugin_manifest
//
// The operation tier at load (ADR-0066 section 4, plugin.h): the
// manifest's "operations" read and judged into plans (operation_plan.h),
// and their getters. A plan is data: nothing here sends or runs one.

#include "operation_plan.h"
#include "plugin_loader.h"

#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace ecuio_plugin_loader {

namespace {

using ecuio_operation::ident;
using ecuio_operation::plan;
using ecuio_operation::step;

const Value& required(const Value& in, const char* key, const std::string& what)
{
    const Value* v = in.find(key);
    if (v == nullptr) {
        refuse(in, what + " needs " + q(key));
    }
    return *v;
}

/* v's text as whole bytes of upper-case hex, lo to hi of them */
uint64_t hex_of(const Value& v, size_t lo, size_t hi, const std::string& what)
{
    const std::string& t = text_of(v, 2 * lo, 2 * hi, what);
    uint64_t value = 0;
    bool ok = t.size() % 2 == 0;
    for (const char c : t) {
        const int d = (c >= '0' && c <= '9')   ? c - '0'
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                               : -1;
        ok = ok && d >= 0;
        value = value * 16 + static_cast<uint64_t>(d < 0 ? 0 : d);
    }
    if (!ok) {
        refuse(v,
               what + " " + q(t) + " takes pairs of hex digits, 0-9 and A-F");
    }
    return value;
}

/* An id as the manifest's own: [a-z0-9-], a letter or digit at each end */
const std::string& id_of(const Value& v, const std::string& what)
{
    const std::string& t = text_of(v, 1, 31, what);
    if (!id_ok(t, 31)) {
        refuse(v, what + " " + q(t) +
                      " takes a-z, 0-9 and -, a letter or digit at each end");
    }
    return t;
}

/* The word v holds, one of names; its place plus one */
uint32_t word_of(const Value& v, std::initializer_list<const char*> names,
                 const std::string& what, const char* tail)
{
    const std::string& t = text_of(v, 1, 31, what);
    uint32_t at = 1;
    for (const char* n : names) {
        if (t == n) {
            return at;
        }
        ++at;
    }
    refuse(v, what + " " + q(t) + tail);
}

void read_ident(const Value& list, const conversation& c, plan& out)
{
    expect(list, Type::array, "an operation's ident");
    if (list.items.size() > ECUIO_PLUGIN_MAX_IDENTS) {
        refuse(list, "an operation's ident lists more than " +
                         std::to_string(ECUIO_PLUGIN_MAX_IDENTS));
    }
    for (const Value& v : list.items) {
        expect(v, Type::object, "an identification");
        only_keys(v, {"did", "role"}, "an identification");
        ident d;
        d.did = static_cast<uint32_t>(
            hex_of(required(v, "did", "an identification"), 2, 2,
                   "an identification's did"));
        d.role = word_of(required(v, "role", "an identification"),
                         {"hardware", "software", "base"},
                         "an identification's role",
                         " isn't one of \"hardware\", \"software\" and "
                         "\"base\"");
        for (const ident& known : out.idents) {
            if (known.did == d.did) {
                refuse(required(v, "did", "an identification"),
                       "an identification's did is given twice");
            }
        }
        out.idents.push_back(d);
    }
    bool reads = false;
    for (const ecuio_plugin_service_t& s : c.services) {
        reads = reads || s.sid == 0x22;
    }
    if (!out.idents.empty() && !reads) {
        refuse(list, "an operation's ident needs service 22 among the "
                     "conversation's services");
    }
}

step read_step(const Value& v)
{
    expect(v, Type::object, "a step");
    only_keys(v,
              {"step", "name", "session", "address", "bytes", "address_bytes",
               "size_bytes", "chunk", "dfi"},
              "a step");
    step s;
    /* A kind a later Havne runs is refused by its name, so its plan
     * never half-loads here */
    s.kind = word_of(required(v, "step", "a step"), {"read", "upload"}, "step",
                     " isn't one this Havne runs: \"read\" and \"upload\"");
    s.name = id_of(required(v, "name", "a step"), "a step's name");
    const std::string what = "step " + q(s.name);
    s.session = word_of(required(v, "session", what),
                        {"extended", "programming"}, what + "'s session",
                        " isn't one this Havne reads in: \"extended\" and "
                        "\"programming\"") == 1
                    ? ECUIO_STEP_SESSION_EXTENDED
                    : ECUIO_STEP_SESSION_PROGRAMMING;
    s.address = hex_of(required(v, "address", what), 1, 4, what + "'s address");
    s.bytes = static_cast<uint64_t>(whole_of(required(v, "bytes", what), 1,
                                             ECUIO_DUMP_IMAGE_MAX,
                                             what + "'s bytes"));
    s.address_bytes = static_cast<uint8_t>(whole_of(
        required(v, "address_bytes", what), 1, 4, what + "'s address_bytes"));
    s.size_bytes = static_cast<uint8_t>(whole_of(
        required(v, "size_bytes", what), 1, 4, what + "'s size_bytes"));
    const Value* chunk = v.find("chunk");
    const Value* dfi = v.find("dfi");
    if (s.kind == ECUIO_STEP_READ) {
        if (dfi != nullptr) {
            refuse(*dfi, what + " is a read, which takes no \"dfi\"");
        }
        s.chunk = static_cast<uint32_t>(whole_of(required(v, "chunk", what), 1,
                                                 ecuio_operation::kChunkMax,
                                                 what + "'s chunk"));
    } else {
        if (chunk != nullptr) {
            refuse(*chunk, what + " is an upload, which takes no \"chunk\": "
                                  "the module names its block");
        }
        if (dfi != nullptr && hex_of(*dfi, 1, 1, what + "'s dfi") != 0) {
            refuse(*dfi, what + "'s dfi is \"00\": a transformed upload "
                                "isn't the memory a copy holds");
        }
    }
    if (const char* fault = ecuio_operation::step_fault(s)) {
        refuse(v, what + ": " + fault);
    }
    return s;
}

void read_steps(const Value& list, plan& out)
{
    expect(list, Type::array, "an operation's steps");
    if (list.items.empty() || list.items.size() > ECUIO_PLUGIN_MAX_STEPS) {
        refuse(list, "an operation's steps lists 1 to " +
                         std::to_string(ECUIO_PLUGIN_MAX_STEPS));
    }
    for (const Value& v : list.items) {
        step s = read_step(v);
        for (const step& known : out.steps) {
            if (known.name == s.name) {
                refuse(v, "step " + q(s.name) + " is given twice");
            }
            if (ecuio_operation::overlap(known, s)) {
                refuse(v, "steps " + q(known.name) + " and " + q(s.name) +
                              " overlap in address");
            }
            if (known.session != s.session) {
                refuse(v, "steps " + q(known.name) + " and " + q(s.name) +
                              " are in two sessions: this Havne reads a "
                              "plan's steps in one");
            }
        }
        out.steps.push_back(std::move(s));
    }
    if (ecuio_operation::image_bytes(out) > ECUIO_DUMP_IMAGE_MAX) {
        refuse(list, "an operation's steps add to more than a copy holds: " +
                         std::to_string(ECUIO_DUMP_IMAGE_MAX) + " bytes");
    }
}

/* "ecu" and "level": one of the conversation's ECUs, and a level its
 * security declares there (a dump always needs its unlock) */
void read_target(const Value& op, const conversation& c, plan& out)
{
    const std::string what = "operation " + q(out.id);
    const Value& ecu = required(op, "ecu", what);
    const std::string& id = text_of(ecu, 1, 31, what + "'s ecu");
    bool found = false;
    for (size_t i = 0; i < c.ecus.size() && !found; ++i) {
        if (c.ecus[i].id == id) {
            found = true;
            out.ecu = static_cast<uint32_t>(i);
            out.rx_id = c.ecus[i].rx;
            out.tx_id = c.ecus[i].tx;
        }
    }
    if (!found) {
        refuse(ecu, what + "'s ecu " + q(id) +
                        " isn't one the conversation's ecus lists");
    }
    const Value& level = required(op, "level", what);
    out.level = static_cast<uint8_t>(hex_of(level, 1, 1, what + "'s level"));
    bool declared = false;
    for (const ecuio_plugin_security_level_t& l : c.levels) {
        declared = declared || (l.rx_id == out.rx_id && l.level == out.level);
    }
    if (!declared) {
        refuse(level, what +
                          "'s level isn't one the conversation's security "
                          "declares for ecu " +
                          q(id));
    }
}

/* A programming plan's "leave": "default" when left out, or "reset". An
 * extended plan takes none: its session stays after the run. */
void read_leave(const Value& op, plan& out)
{
    const std::string what = "operation " + q(out.id);
    const Value* leave = op.find("leave");
    if (!ecuio_operation::programming_of(out)) {
        if (leave != nullptr) {
            refuse(*leave, what + "'s steps are in the extended session, "
                                  "which stays after the run: it takes no "
                                  "'leave'");
        }
        return;
    }
    out.leave = leave == nullptr
                    ? ECUIO_OPERATION_LEAVE_DEFAULT
                    : word_of(*leave, {"default", "reset"}, what + "'s leave",
                              " isn't one of \"default\" (10 01) and "
                              "\"reset\" (11 01)");
}

plan read_operation(const Value& op, const conversation& c)
{
    expect(op, Type::object, "an operation");
    only_keys(
        op, {"id", "kind", "title", "ecu", "level", "ident", "steps", "leave"},
        "an operation");
    plan out;
    out.id = id_of(required(op, "id", "an operation"), "an operation's id");
    const std::string what = "operation " + q(out.id);
    (void)word_of(required(op, "kind", what), {"dump"}, what + "'s kind",
                  " isn't one this Havne runs: \"dump\"");
    out.kind = ECUIO_OPERATION_KIND_DUMP;
    out.title =
        plain_of(required(op, "title", what), 1, 63, what + "'s title", false);
    read_target(op, c, out);
    if (const Value* list = op.find("ident")) {
        read_ident(*list, c, out);
    }
    read_steps(required(op, "steps", what), out);
    read_leave(op, out);
    return out;
}

} // namespace

/* "operations": the tier's group, there exactly when "tiers" lists the
 * tier, which needs format 3 and the conversation tier beside it */
void loader::read_operations(const Value& root, ecuio_plugin& p)
{
    const Value* ops = root.find("operations");
    const bool tier = (p.tiers & ECUIO_PLUGIN_TIER_OPERATION) != 0;
    if (ops != nullptr && p.format < 3) {
        refuse(*ops, "\"operations\" is format 3's: its manifest starts with "
                     "\"havne_plugin\": 3");
    }
    if (ops != nullptr && !tier) {
        refuse(*ops, "\"operations\" is the operation tier's, which "
                     "\"tiers\" doesn't list");
    }
    if (!tier) {
        return;
    }
    if (p.conversation == nullptr) {
        refuse(root, "the operation tier needs the conversation tier: a "
                     "plan names one of its ECUs and its unlock");
    }
    if (ops == nullptr) {
        refuse(root, "the operation tier needs \"operations\": its plans");
    }
    expect(*ops, Type::array, "\"operations\"");
    if (ops->items.empty() || ops->items.size() > ECUIO_PLUGIN_MAX_OPERATIONS) {
        refuse(*ops, "\"operations\" lists 1 to " +
                         std::to_string(ECUIO_PLUGIN_MAX_OPERATIONS));
    }
    for (const Value& op : ops->items) {
        plan next = read_operation(op, *p.conversation);
        for (const plan& known : p.operations) {
            if (known.id == next.id) {
                refuse(op, "operation " + q(next.id) + " is given twice");
            }
        }
        p.operations.push_back(std::move(next));
    }
}

} // namespace ecuio_plugin_loader

namespace {

using ecuio_operation::plan;

/* The plan a getter names, or NULL: none without the tier */
const plan* plan_at(const ecuio_plugin_t* plugin, uint32_t operation)
{
    return operation < plugin->operations.size()
               ? &plugin->operations[operation]
               : nullptr;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_plugin_operation_count(const ecuio_plugin_t* plugin,
                                            uint32_t* count)
{
    if (plugin == nullptr || count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = static_cast<uint32_t>(plugin->operations.size());
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_operation(const ecuio_plugin_t* plugin,
                                      uint32_t index,
                                      ecuio_plugin_operation_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, index);
    if (p == nullptr || out->struct_size < sizeof(ecuio_plugin_operation_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_operation_t o{};
    o.struct_size = sizeof o;
    o.kind = p->kind;
    o.op_class = ecuio_operation::class_of(*p);
    o.rx_id = p->rx_id;
    o.tx_id = p->tx_id;
    o.level = p->level;
    o.step_count = static_cast<uint32_t>(p->steps.size());
    o.ident_count = static_cast<uint32_t>(p->idents.size());
    o.unlock_sessions = ecuio_operation::unlock_sessions(*p);
    o.image_bytes = ecuio_operation::image_bytes(*p);
    o.leave = p->leave;
    ecuio_sf::copy_text(o.id, sizeof o.id, p->id);
    ecuio_sf::copy_text(o.title, sizeof o.title, p->title);
    *out = o;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_operation_step(const ecuio_plugin_t* plugin,
                                           uint32_t operation, uint32_t index,
                                           ecuio_plugin_operation_step_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, operation);
    if (p == nullptr || index >= p->steps.size() ||
        out->struct_size < sizeof(ecuio_plugin_operation_step_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_operation::step& from = p->steps[index];
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    s.kind = from.kind;
    s.session = from.session;
    s.address = from.address;
    s.bytes = from.bytes;
    s.chunk = from.chunk;
    s.address_bytes = from.address_bytes;
    s.size_bytes = from.size_bytes;
    s.dfi = from.dfi;
    ecuio_sf::copy_text(s.name, sizeof s.name, from.name);
    *out = s;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_operation_ident(const ecuio_plugin_t* plugin,
                                            uint32_t operation, uint32_t index,
                                            ecuio_plugin_operation_ident_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, operation);
    if (p == nullptr || index >= p->idents.size() ||
        out->struct_size < sizeof(ecuio_plugin_operation_ident_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_operation_ident_t d{};
    d.struct_size = sizeof d;
    d.did = p->idents[index].did;
    d.role = p->idents[index].role;
    *out = d;
    return ECUIO_OK;
}

} // extern "C"
