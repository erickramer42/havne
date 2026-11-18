// File/layer: core/src/plugin_conversation.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginConversationTest (test_signals); FUZZ:
//          fuzz_plugin_manifest
//
// The plugin loader's reading of the conversation tier (ADR-0066,
// "havne_plugin" 2): the manifest's "conversation" group and the module
// it names. Everything a plugin's code may later cause core to send is
// judged here, at load: each declared service against tier 2's ceiling
// (tier2_ceiling.h), each security level's pair declared, the module's
// SHA-256 against the manifest's and its shape against the guest ABI
// (guest_abi.h, wasm_check.h). Nothing here starts the runtime or runs
// the module.

#include "plugin_loader.h"

#include "base64.h"
#include "guest_abi.h"
#include "sha256.h"
#include "tier2_ceiling.h"
#include "wasm_check.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_plugin_loader {

namespace {

constexpr int64_t kDelayMsMin = 1000;
constexpr int64_t kDelayMsMax = 3600000;
constexpr uint32_t kDelayMsDefault = 10000;
constexpr int64_t kKeyWindowMsMin = 50;
/* Under ISO 14229-2's S3 (5 s) by a second: a key sent later than that
 * after its seed request would find the session gone */
constexpr int64_t kKeyWindowMsMax = 4000;
constexpr uint32_t kKeyWindowMsDefault = 1000;
constexpr uint8_t kSecurityAccess = 0x27;
/* The longest text MODULE_MAX_BYTES take as base64 */
constexpr size_t kBase64Max = (ecuio_wasm::MODULE_MAX_BYTES + 2) / 3 * 4;

std::string hex2(uint8_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    return {digits[v >> 4], digits[v & 0x0F]};
}

/* A byte as a manifest writes one: two hex digits, the letters in upper
 * case, as a signal file's service */
uint8_t byte_of(const Value& v, const std::string& what)
{
    const std::string& t = text_of(v, 2, 2, what);
    const auto digit = [](char c) {
        return (c >= '0' && c <= '9')   ? c - '0'
               : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                        : -1;
    };
    const int hi = digit(t[0]);
    const int lo = digit(t[1]);
    if (hi < 0 || lo < 0) {
        refuse(v, what + " " + q(t) + " takes two hex digits, 0-9 and A-F");
    }
    return static_cast<uint8_t>(hi * 16 + lo);
}

const Value& needed(const Value& in, const char* key, const std::string& words)
{
    const Value* v = in.find(key);
    if (v == nullptr) {
        refuse(in, words);
    }
    return *v;
}

/* "ecus": the manifest's ECUs the module talks to, by id, each once */
void read_ecus(const Value& root, const Value& conv, conversation& c)
{
    const Value& ecus =
        needed(conv, "ecus",
               "\"conversation\" needs \"ecus\": the manifest's ECUs its "
               "module talks to");
    expect(ecus, Type::array, "the conversation's ecus");
    if (ecus.items.empty() || ecus.items.size() > ECUIO_MAX_CHANNELS) {
        refuse(ecus, "the conversation's ecus lists 1 to 32 of the manifest's");
    }
    const Value* declared = root.find("ecus");
    for (const Value& e : ecus.items) {
        const std::string& id = text_of(e, 1, 31, "a conversation ECU");
        for (const conversation_ecu& known : c.ecus) {
            if (known.id == id) {
                refuse(e, "the conversation's ecus lists " + q(id) + " twice");
            }
        }
        const Value* found = nullptr;
        if (declared != nullptr && declared->type == Type::array) {
            for (const Value& d : declared->items) {
                const Value* did =
                    d.type == Type::object ? d.find("id") : nullptr;
                if (did != nullptr && did->type == Type::string &&
                    did->text == id && d.find("rx") != nullptr) {
                    found = &d;
                }
            }
        }
        if (found == nullptr) {
            refuse(e, "the conversation's ecu " + q(id) +
                          " isn't one \"ecus\" lists");
        }
        conversation_ecu ecu;
        ecu.id = id;
        ecuio_sf::ecu_addresses(*found, &ecu.rx, &ecu.tx);
        c.ecus.push_back(std::move(ecu));
    }
}

/* A service declared by sub-function: each one once, inside the ceiling */
void read_subs(const Value& sub, const std::string& name,
               ecuio_plugin_service_t& out)
{
    expect(sub, Type::array, name + "'s sub");
    if (sub.items.empty() || sub.items.size() > ECUIO_PLUGIN_MAX_SUBS) {
        refuse(sub, name + "'s sub lists 1 to 14 sub-functions");
    }
    for (const Value& b : sub.items) {
        const uint8_t value = byte_of(b, name + "'s sub-function");
        for (uint8_t k = 0; k < out.sub_count; ++k) {
            if (out.subs[k] == value) {
                refuse(b,
                       name + " lists sub-function " + hex2(value) + " twice");
            }
        }
        if (!ecuio_tier2_allowed(out.sid, value)) {
            refuse(b, name + " " + hex2(value) +
                          " isn't a request the conversation tier may send");
        }
        out.subs[out.sub_count++] = value;
    }
}

/* "services": what the module may cause core to send, each inside tier
 * 2's ceiling, by sub-function exactly where the ceiling splits */
void read_services(const Value& conv, conversation& c)
{
    const Value& services =
        needed(conv, "services",
               "\"conversation\" needs \"services\": what its module may "
               "cause to be sent");
    expect(services, Type::array, "the conversation's services");
    if (services.items.empty() ||
        services.items.size() > ECUIO_PLUGIN_MAX_SERVICES) {
        refuse(services, "the conversation's services lists 1 to 16");
    }
    for (const Value& s : services.items) {
        expect(s, Type::object, "a service");
        only_keys(s, {"sid", "sub"}, "a service");
        const Value& sid =
            needed(s, "sid", "a service needs \"sid\": its two hex digits");
        ecuio_plugin_service_t out{};
        out.sid = byte_of(sid, "a service's sid");
        const std::string name = "service " + hex2(out.sid);
        for (const ecuio_plugin_service_t& known : c.services) {
            if (known.sid == out.sid) {
                refuse(sid, name + " is given twice");
            }
        }
        const Value* sub = s.find("sub");
        if (ecuio_tier2_splits(out.sid)) {
            if (sub == nullptr) {
                refuse(s, name + " is declared by sub-function: it needs "
                                 "\"sub\"");
            }
            read_subs(*sub, name, out);
        } else {
            if (!ecuio_tier2_allowed(out.sid, 0)) {
                refuse(sid, name + " isn't one the conversation tier may send");
            }
            if (sub != nullptr) {
                refuse(*sub, name + " takes no \"sub\": it isn't declared by "
                                    "sub-function");
            }
        }
        c.services.push_back(out);
    }
}

/* Whether the services declare 27 with sub-function sub */
bool declares_27(const conversation& c, uint8_t sub)
{
    for (const ecuio_plugin_service_t& s : c.services) {
        for (uint8_t k = 0; s.sid == kSecurityAccess && k < s.sub_count; ++k) {
            if (s.subs[k] == sub) {
                return true;
            }
        }
    }
    return false;
}

/* "security": each level the module computes a key for, on one of the
 * conversation's ECUs, with the lengths and waits core holds it to */
void read_security(const Value& conv, conversation& c)
{
    const Value* security = conv.find("security");
    if (security == nullptr) {
        return;
    }
    expect(*security, Type::array, "the conversation's security");
    if (security->items.size() > ECUIO_PLUGIN_MAX_LEVELS) {
        refuse(*security, "the conversation's security lists more than 32");
    }
    for (const Value& l : security->items) {
        expect(l, Type::object, "a security level");
        only_keys(l,
                  {"ecu", "level", "seed_bytes", "key_bytes", "delay_ms",
                   "key_window_ms"},
                  "a security level");
        const Value& ecu = needed(l, "ecu",
                                  "a security level needs \"ecu\": one of the "
                                  "conversation's");
        const Value& level =
            needed(l, "level",
                   "a security level needs \"level\": its seed "
                   "request's sub-function");
        ecuio_plugin_security_level_t out{};
        const std::string& id = text_of(ecu, 1, 31, "a security level's ecu");
        for (const conversation_ecu& known : c.ecus) {
            if (known.id == id) {
                out.rx_id = known.rx;
            }
        }
        if (out.rx_id == 0) {
            refuse(ecu, "a security level's ecu " + q(id) +
                            " isn't one the conversation's ecus lists");
        }
        out.level = byte_of(level, "a security level's level");
        const std::string what = "security level " + hex2(out.level);
        if ((out.level & 1) == 0 ||
            !ecuio_tier2_allowed(kSecurityAccess, out.level) ||
            !ecuio_tier2_allowed(kSecurityAccess,
                                 static_cast<uint8_t>(out.level + 1))) {
            refuse(level, what + " isn't a seed request's sub-function: odd, "
                                 "01 to 7D, never 5F");
        }
        for (const ecuio_plugin_security_level_t& known : c.levels) {
            if (known.rx_id == out.rx_id && known.level == out.level) {
                refuse(l, what + " of ecu " + q(id) + " is given twice");
            }
        }
        out.seed_bytes = static_cast<uint8_t>(
            whole_of(needed(l, "seed_bytes", what + " needs \"seed_bytes\""), 1,
                     255, what + "'s seed_bytes"));
        out.key_bytes = static_cast<uint8_t>(
            whole_of(needed(l, "key_bytes", what + " needs \"key_bytes\""), 1,
                     ECUIO_PLUGIN_KEY_MAX, what + "'s key_bytes"));
        const Value* delay = l.find("delay_ms");
        out.delay_ms =
            delay == nullptr
                ? kDelayMsDefault
                : static_cast<uint32_t>(whole_of(
                      *delay, kDelayMsMin, kDelayMsMax, what + "'s delay_ms"));
        const Value* window = l.find("key_window_ms");
        out.key_window_ms = window == nullptr
                                ? kKeyWindowMsDefault
                                : static_cast<uint32_t>(whole_of(
                                      *window, kKeyWindowMsMin, kKeyWindowMsMax,
                                      what + "'s key_window_ms"));
        const auto key_sub = static_cast<uint8_t>(out.level + 1);
        if (!declares_27(c, out.level) || !declares_27(c, key_sub)) {
            refuse(l, what + " needs service 27 declared with sub-functions " +
                          hex2(out.level) + " and " + hex2(key_sub));
        }
        c.levels.push_back(out);
    }
}

/* A check's words, with anything but printable ASCII made '?' */
std::string printable(const char* words)
{
    std::string out(words);
    for (char& ch : out) {
        if (ch < 0x20 || ch > 0x7E) {
            ch = '?';
        }
    }
    return out;
}

} // namespace

/* "conversation": the tier's group, there exactly when "tiers" lists the
 * tier; and every binary file of a packed package named by something */
void loader::read_conversation(const Value& root, ecuio_plugin& p)
{
    const Value* conv = root.find("conversation");
    const bool tier = (p.tiers & ECUIO_PLUGIN_TIER_CONVERSATION) != 0;
    if (conv != nullptr && p.format < 2) {
        refuse(*conv, "\"conversation\" is format 2's: its manifest starts "
                      "with \"havne_plugin\": 2");
    }
    if (conv != nullptr && !tier) {
        refuse(*conv, "\"conversation\" is the conversation tier's, which "
                      "\"tiers\" doesn't list");
    }
    if (conv == nullptr && tier) {
        refuse(root, "the conversation tier needs \"conversation\": its "
                     "code, ECUs and services");
    }
    if (conv != nullptr) {
        expect(*conv, Type::object, "\"conversation\"");
        only_keys(*conv, {"code", "budget", "ecus", "services", "security"},
                  "\"conversation\"");
        std::unique_ptr<conversation> c(new conversation());
        if (const Value* budget = conv->find("budget")) {
            c->budget = static_cast<uint32_t>(
                whole_of(*budget, 1, ECUIO_PLUGIN_BUDGET_MAX,
                         "the conversation's budget"));
        }
        read_ecus(root, *conv, *c);
        read_services(*conv, *c);
        read_security(*conv, *c);
        read_code(needed(*conv, "code",
                         "\"conversation\" needs \"code\": its module's file "
                         "and SHA-256"),
                  *c);
        p.conversation = std::move(c);
    }
    for (const binary_file& b : binaries_) {
        if (!b.read) {
            refuse(*b.root, "file " + q(b.path) +
                                " is a binary file nothing in the manifest "
                                "names");
        }
    }
}

/* A packed package's module: the binary file at path, its base64 decoded */
void loader::packed_code(const Value& file, const std::string& path,
                         conversation& c)
{
    const std::string name = "file " + q(path);
    binary_file* found = nullptr;
    for (binary_file& b : binaries_) {
        if (b.path == path) {
            found = &b;
        }
    }
    if (found == nullptr) {
        refuse(file, name + " isn't a binary file in \"files\"");
    }
    found->read = true;
    const Value& entry = *found->root;
    only_keys(entry, {"kind", "base64"}, "a binary file");
    const Value& kind = *entry.find("kind");
    expect(kind, Type::string, "a binary file's kind");
    if (kind.text != "wasm") {
        refuse(kind, "a binary file's kind " + q(kind.text) +
                         " isn't one this Havne reads: only \"wasm\"");
    }
    const Value& text =
        needed(entry, "base64", "a binary file needs \"base64\": its bytes");
    expect(text, Type::string, "a binary file's base64");
    if (text.text.size() > kBase64Max) {
        refuse(text, name + " is over 1 MiB");
    }
    if (!ecuio_base64_decode(text.text.data(), text.text.size(), &c.code)) {
        refuse(text, name + "'s base64 isn't RFC 4648's: A-Z, a-z, 0-9, "
                            "+ and /, padded with =, nothing else");
    }
}

/* A folder's module: its bytes through the host's callback, counted into
 * the package's most */
void loader::folder_code(const Value& file, const std::string& path,
                         conversation& c)
{
    const std::string name = "file " + q(path);
    const char* bytes = nullptr;
    size_t length = 0;
    if (read_(user_, path.c_str(), &bytes, &length) != 0 ||
        (bytes == nullptr && length > 0)) {
        refuse(file, name + " couldn't be read");
    }
    if (length > ecuio_wasm::MODULE_MAX_BYTES) {
        refuse(file, name + " is over 1 MiB");
    }
    total_ += length;
    if (total_ > ECUIO_PLUGIN_PACKAGE_MAX) {
        refuse(file, "the package is over 16 MiB");
    }
    c.code.assign(bytes, bytes + length);
}

/* "code": the module by its path, its bytes from a packed package's
 * binary file or through the host's callback, then its hash and shape */
void loader::read_code(const Value& code, conversation& c)
{
    expect(code, Type::object, "the conversation's code");
    only_keys(code, {"file", "sha256"}, "the conversation's code");
    const Value& file =
        needed(code, "file",
               "the conversation's code needs \"file\": the module's path in "
               "the package");
    const Value& sha =
        needed(code, "sha256",
               "the conversation's code needs \"sha256\": the module's, 64 "
               "hex digits");
    const std::string& path = path_of(file, "the conversation's code");
    const std::string& want = text_of(sha, 64, 64, "the code's sha256");
    for (const char ch : want) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            refuse(sha, "the code's sha256 takes 64 of 0-9 and a-f");
        }
    }
    const std::string name = "file " + q(path);
    if (packed()) {
        packed_code(file, path, c);
    } else {
        folder_code(file, path, c);
    }
    if (c.code.size() > ecuio_wasm::MODULE_MAX_BYTES) {
        refuse(file, name + " is over 1 MiB");
    }

    ecuio_sha256::hasher h;
    h.add(c.code.data(), c.code.size());
    h.finish(c.sha256);
    const std::string have = ecuio_sha256::hex(c.sha256);
    if (have != want) {
        refuse(sha, name + " has SHA-256 " + have + ", and the manifest says " +
                        want);
    }
    const ecuio_wasm::check_result check =
        ecuio_wasm::check_module(c.code.data(), c.code.size(),
                                 ECUIO_GUEST_EXPORTS, ECUIO_GUEST_EXPORT_COUNT);
    if (!check.ok) {
        refuse(file, name + " isn't a module this Havne runs: " +
                         printable(check.why));
    }
}

} // namespace ecuio_plugin_loader
