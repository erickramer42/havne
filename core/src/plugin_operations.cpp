// File/layer: core/src/plugin_operations.cpp | internal, C++17, Qt-free
// Testing: UNIT: PluginOperationTest (test_core), test_c_abi; FUZZ:
//          fuzz_plugin_manifest
//
// The operation tier at load (ADR-0066 section 4, plugin.h): the
// manifest's "operations" read and judged into plans (operation_plan.h),
// and their getters. A plan is data: nothing here sends or runs one.

#include "operation_plan.h"
#include "plugin_loader.h"
#include "sha256.h"

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

/* For a literal: GCC's -Wdangling-reference flags a temporary string */
const Value& required(const Value& in, const char* key, const char* what)
{
    return required(in, key, std::string(what));
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

constexpr uint32_t kKeepaliveMinMs = 500;
constexpr uint32_t kKeepaliveMaxMs = 4500;

/* One of "functional": a suppressed request to the functional address,
 * 10 82 or 3E 80 alone: no reset in a read, and a physical suppressed
 * request would leave core unable to read the target's reply */
void read_functional(const Value& v, ecuio_operation::kernel& k)
{
    expect(v, Type::object, "a functional request");
    only_keys(v, {"request", "every_ms"}, "a functional request");
    const std::string r =
        text_of(required(v, "request", "a functional request"), 5, 5,
                "a functional request's request");
    const Value* every = v.find("every_ms");
    if (r == "10 82" && every == nullptr && k.quiet_session == 0) {
        k.quiet_session = 0x82;
        return;
    }
    if (r == "3E 80" && every != nullptr && k.quiet_keepalive == 0) {
        k.quiet_keepalive = 0x80;
        k.keepalive_ms = static_cast<uint32_t>(whole_of(
            *every, kKeepaliveMinMs, kKeepaliveMaxMs, "a 3E 80's every_ms"));
        return;
    }
    refuse(v, "a functional request is \"10 82\", or \"3E 80\" with its "
              "\"every_ms\", each once: nothing else goes to every module");
}

void read_kernel(const Value& v, ecuio_operation::kernel& k)
{
    expect(v, Type::object, "the kernel");
    only_keys(v, {"ram", "start", "stays", "functional"}, "the kernel");
    const Value& ram = required(v, "ram", "the kernel");
    expect(ram, Type::object, "the kernel's ram");
    only_keys(ram, {"address", "bytes"}, "the kernel's ram");
    k.ram_address = hex_of(required(ram, "address", "the kernel's ram"), 1, 4,
                           "the kernel's ram address");
    k.ram_bytes = static_cast<uint64_t>(
        whole_of(required(ram, "bytes", "the kernel's ram"), 1,
                 static_cast<int64_t>(ecuio_operation::most_of(4)),
                 "the kernel's ram bytes"));
    const Value& start = required(v, "start", "the kernel");
    expect(start, Type::array, "the kernel's start");
    if (start.items.empty() || start.items.size() > ECUIO_PLUGIN_MAX_STARTS) {
        refuse(start, "the kernel's start lists 1 to " +
                          std::to_string(ECUIO_PLUGIN_MAX_STARTS) +
                          " routines");
    }
    for (const Value& id : start.items) {
        k.starts.push_back(
            static_cast<uint16_t>(hex_of(id, 2, 2, "a start routine")));
    }
    const Value& stays = required(v, "stays", "the kernel");
    expect(stays, Type::boolean, "the kernel's stays");
    k.stays = stays.boolean;
    if (const Value* list = v.find("functional")) {
        expect(*list, Type::array, "the kernel's functional");
        for (const Value& f : list->items) {
            read_functional(f, k);
        }
    }
}

int hex_digit(char c)
{
    return (c >= '0' && c <= '9')   ? c - '0'
           : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                    : -1;
}

void read_download(const Value& v, const std::string& what,
                   ecuio_operation::kernel& k)
{
    only_keys(v,
              {"step", "name", "session", "address", "address_bytes",
               "size_bytes", "file", "sha256"},
              "a download");
    k.address = hex_of(required(v, "address", what), 1, 4, what + "'s address");
    k.address_bytes = static_cast<uint8_t>(whole_of(
        required(v, "address_bytes", what), 1, 4, what + "'s address_bytes"));
    k.size_bytes = static_cast<uint8_t>(whole_of(
        required(v, "size_bytes", what), 1, 4, what + "'s size_bytes"));
    k.file = path_of(required(v, "file", what), what + "'s file");
    const Value& sha = required(v, "sha256", what);
    const std::string& t = text_of(sha, 64, 64, what + "'s sha256");
    for (size_t i = 0; i < 32; ++i) {
        const int hi = hex_digit(t[2 * i]);
        const int lo = hex_digit(t[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            refuse(sha, what + "'s sha256 takes 64 of 0-9 and a-f");
        }
        k.sha256[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
}

/* The helper's two steps, the download then its routine, both in the
 * programming session; false for any other step */
bool read_kernel_step(const Value& v, plan& out, uint32_t at)
{
    expect(v, Type::object, "a step");
    const Value* kind = v.find("step");
    const bool named = kind != nullptr && kind->type == Type::string;
    const bool download = named && kind->text == "download";
    const bool routine = named && kind->text == "routine";
    if (!download && !routine) {
        return false;
    }
    const std::string name =
        id_of(required(v, "name", "a step"), "a step's name");
    const std::string what = "step " + q(name);
    if (!out.has_kernel || at != (download ? 0u : 1u)) {
        refuse(v, what + ": a kernel read's download and its routine are its "
                         "first two steps, and only a kernel read's");
    }
    (void)word_of(required(v, "session", what), {"programming"},
                  what + "'s session", " isn't the helper's: \"programming\"");
    ecuio_operation::kernel& k = out.helper;
    if (download) {
        k.download_name = name;
        read_download(v, what, k);
        return true;
    }
    only_keys(v, {"step", "name", "session", "routine"}, "a routine");
    if (name == k.download_name) {
        refuse(v, what + " is given twice");
    }
    k.routine_name = name;
    k.routine = static_cast<uint16_t>(
        hex_of(required(v, "routine", what), 2, 2, what + "'s routine"));
    return true;
}

void read_steps(const Value& list, plan& out)
{
    expect(list, Type::array, "an operation's steps");
    if (list.items.empty() || list.items.size() > ECUIO_PLUGIN_MAX_STEPS) {
        refuse(list, "an operation's steps lists 1 to " +
                         std::to_string(ECUIO_PLUGIN_MAX_STEPS));
    }
    uint32_t at = 0;
    for (const Value& v : list.items) {
        if (read_kernel_step(v, out, at++)) {
            continue;
        }
        if (out.has_kernel && at <= 2) {
            refuse(v, "a kernel read's first two steps are its download and "
                      "its routine");
        }
        step s = read_step(v);
        if (s.name == out.helper.download_name ||
            s.name == out.helper.routine_name) {
            refuse(v, "step " + q(s.name) + " is given twice");
        }
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
    only_keys(op,
              {"id", "kind", "title", "ecu", "level", "ident", "kernel",
               "steps", "leave"},
              "an operation");
    plan out;
    out.id = id_of(required(op, "id", "an operation"), "an operation's id");
    const std::string what = "operation " + q(out.id);
    static_assert(ECUIO_OPERATION_KIND_KERNEL_READ == 2,
                  "the words' order is the kinds'");
    out.kind = word_of(required(op, "kind", what), {"dump", "kernel_read"},
                       what + "'s kind",
                       " isn't one this Havne runs: \"dump\" and "
                       "\"kernel_read\"");
    const Value* kernel = op.find("kernel");
    out.has_kernel = out.kind == ECUIO_OPERATION_KIND_KERNEL_READ;
    if (out.has_kernel) {
        read_kernel(required(op, "kernel", what), out.helper);
    } else if (kernel != nullptr) {
        refuse(*kernel, what + " is a dump, which takes no \"kernel\"");
    }
    out.title =
        plain_of(required(op, "title", what), 1, 63, what + "'s title", false);
    read_target(op, c, out);
    if (const Value* list = op.find("ident")) {
        read_ident(*list, c, out);
    }
    read_steps(required(op, "steps", what), out);
    if (out.has_kernel &&
        (out.helper.routine_name.empty() || out.steps.empty())) {
        refuse(op, what + " is a kernel read: a download, its routine, then "
                          "the ranges the helper reads");
    }
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

constexpr size_t kBinaryMax = size_t{1} << 20;

/* Each kernel read's helper: its bytes, their SHA-256 against the
 * manifest's, and the window they go to */
void loader::read_blobs(const Value& root, ecuio_plugin& p)
{
    for (size_t i = 0; i < p.operations.size(); ++i) {
        plan& op = p.operations[i];
        if (!op.has_kernel) {
            continue;
        }
        const Value& at = root.find("operations")->items[i];
        const Value& file = *at.find("steps")->items[0].find("file");
        const std::string name = "file " + q(op.helper.file);
        std::vector<uint8_t>& blob = op.helper.blob;
        if (packed()) {
            packed_binary(file, op.helper.file, "bin", &blob);
        } else {
            const char* bytes = nullptr;
            size_t length = 0;
            read_folder_file(file, name, op.helper.file, &bytes, &length);
            if (length > kBinaryMax) {
                refuse(file, name + " is over 1 MiB");
            }
            total_ += length;
            if (total_ > ECUIO_PLUGIN_PACKAGE_MAX) {
                refuse(file, "the package is over 16 MiB");
            }
            blob.assign(bytes, bytes + length);
        }
        uint8_t have[32];
        ecuio_sha256::hasher h;
        h.add(blob.data(), blob.size());
        h.finish(have);
        if (std::memcmp(have, op.helper.sha256, sizeof have) != 0) {
            refuse(file, name + " has SHA-256 " + ecuio_sha256::hex(have) +
                             ", and the manifest says " +
                             ecuio_sha256::hex(op.helper.sha256));
        }
        if (const char* fault = ecuio_operation::kernel_fault(op)) {
            refuse(at, "operation " + q(op.id) + ": " + fault);
        }
    }
}

} // namespace ecuio_plugin_loader
