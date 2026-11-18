// File/layer: tests/plugin_conversation_support.h | test helper: a format 2
//             plugin with the conversation tier, in memory
// Testing: used by test_plugin_conversation.cpp and
//          test_plugin_conversation_rules.cpp (test_signals)
//
// A conversation plugin's parts as text a case can change one at a time
// (ADR-0066, "havne_plugin" 2): the least module the guest ABI takes
// (wasm_builder.h writes it), its base64 and SHA-256, the "conversation"
// group and the manifest around it; the plugin loaded packed or as a
// folder in memory, and a table of changes each refused in its words.

#ifndef ECUIO_TESTS_PLUGIN_CONVERSATION_SUPPORT_H
#define ECUIO_TESTS_PLUGIN_CONVERSATION_SUPPORT_H

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include "wasm_builder.h"

#include "sha256.h" /* core/src */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace conversation_test {

using wasm_builder::bytes;

/* The least guest: the ABI's six exports, each returning a constant, and
 * one page of memory */
inline wasm_builder::module guest()
{
    using namespace wasm_builder;
    module m;
    m.memory_max = 1;
    m.funcs.push_back(plain_func("ecuio_abi", {I32_CONST, 1}));
    m.funcs.push_back(plain_func("ecuio_buffer", {I32_CONST, 0x80, 0x08}));
    func two;
    two.params = {I32, I32};
    two.results = {I32};
    two.body = {I32_CONST, 0};
    two.exported = "begin";
    m.funcs.push_back(two);
    m.funcs.push_back(plain_func("next_request", {I32_CONST, 0}));
    two.exported = "accept";
    m.funcs.push_back(two);
    m.funcs.push_back(plain_func("end", {}, false));
    return m;
}

/* RFC 4648's base64, padded */
inline std::string base64(const bytes& in)
{
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const size_t left = in.size() - i;
        const uint32_t group = (uint32_t{in[i]} << 16) |
                               (left > 1 ? uint32_t{in[i + 1]} << 8 : 0u) |
                               (left > 2 ? uint32_t{in[i + 2]} : 0u);
        out += digits[group >> 18];
        out += digits[(group >> 12) & 0x3F];
        out += left > 1 ? digits[(group >> 6) & 0x3F] : '=';
        out += left > 2 ? digits[group & 0x3F] : '=';
    }
    return out;
}

inline std::string sha_of(const bytes& module)
{
    return ecuio_sha256::hex_of(module.data(), module.size());
}

/* The "conversation" group: a key whose text is empty is left out */
struct Group {
    std::string code; /* filled by Made::json() unless a case sets it */
    std::string budget = "250000";
    std::string ecus = R"(["tcm", "pcm"])";
    std::string services = R"([{"sid": "10", "sub": ["03"]}, {"sid": "22"}, )"
                           R"({"sid": "27", "sub": ["01", "02", "03", "04"]}, )"
                           R"({"sid": "3E", "sub": ["00"]}])";
    std::string security =
        R"([{"ecu": "pcm", "level": "01", "seed_bytes": 4, "key_bytes": 4}, )"
        R"({"ecu": "tcm", "level": "03", "seed_bytes": 16, "key_bytes": 16, )"
        R"("delay_ms": 30000, "key_window_ms": 250}])";
    std::string extra; /* more keys, each after a comma */

    std::string json() const
    {
        std::string out;
        const auto key = [&out](const char* name, const std::string& text) {
            if (!text.empty()) {
                out += std::string(out.empty() ? "{\"" : ", \"") + name +
                       "\": " + text;
            }
        };
        key("code", code);
        key("budget", budget);
        key("ecus", ecus);
        key("services", services);
        key("security", security);
        return (out.empty() ? "{" : out) + extra + "}";
    }
};

inline const char* const kData =
    R"({"havne_signals": 1, "signals": [{"key": "oil", "name": "Oil", )"
    R"("ecu": "pcm", "read": {"service": "22", "number": "F40D"}, )"
    R"("field": {"byte": 0}}]})";

/* A plugin with the tier, each part a case's to change: the manifest's
 * own keys, the group, the module and how the package carries it */
struct Made {
    std::string format = "2";
    std::string tiers = R"(["data", "conversation"])";
    std::string ecus = R"([{"id": "pcm", "rx": "7E8"}, {"id": "tcm", )"
                       R"("rx": "7E9"}, {"id": "abs", "rx": "7EA"}])";
    Group group;
    bool has_group = true; /* the manifest has "conversation" */
    bool has_code = true;  /* and the group has "code" */
    bytes module = guest().build();
    std::string sha;    /* the code's sha256, when not the module's own */
    std::string binary; /* guest.wasm's "files" entry, when not the module's */
    bool has_binary = true;  /* a packed package's "files" has guest.wasm */
    std::string files_extra; /* more of "files", each after a comma */
    std::string variant_files = R"(["a.json"])"; /* the one variant's files */

    std::string conversation() const
    {
        Group g = group;
        if (g.code.empty() && has_code) {
            g.code = R"({"file": "guest.wasm", "sha256": ")" +
                     (sha.empty() ? sha_of(module) : sha) + "\"}";
        }
        return g.json();
    }

    /* The manifest: a packed plugin's with its files, or a folder's */
    std::string json(bool packed = true) const
    {
        std::string out =
            "{\"havne_plugin\": " + format +
            R"(, "id": "talker", "name": "Talker", "version": "1.2.0", )"
            R"("requires": {"core": "0.1.0"}, "source": {"name": "t"}, )"
            R"("tiers": )" +
            tiers + ", \"ecus\": " + ecus +
            R"(, "variants": [{"id": "base", "files": )" + variant_files + "}]";
        if (packed) {
            out += std::string(", \"files\": {\"a.json\": ") + kData;
            if (has_binary) {
                out += ", \"guest.wasm\": " +
                       (binary.empty() ? R"({"kind": "wasm", "base64": ")" +
                                             base64(module) + "\"}"
                                       : binary);
            }
            out += files_extra + "}";
        }
        if (has_group) {
            out += ", \"conversation\": " + conversation();
        }
        return out + "}";
    }
};

/* A folder package in memory: manifest.json and the files by path */
struct Folder {
    std::map<std::string, std::string> files;
    std::vector<std::string> asked;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<Folder*>(user);
        self->asked.emplace_back(path);
        auto it = self->files.find(path);
        if (it == self->files.end()) {
            return 1;
        }
        *bytes = it->second.data();
        *length = it->second.size();
        return 0;
    }
};

struct PluginFree {
    void operator()(ecuio_plugin_t* p) const
    {
        EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);
    }
};
using Plugin = std::unique_ptr<ecuio_plugin_t, PluginFree>;

inline Plugin load_packed(const std::string& json,
                          ecuio_status_t* status = nullptr)
{
    ecuio_plugin_t* p = reinterpret_cast<ecuio_plugin_t*>(1);
    const ecuio_status_t s =
        ecuio_plugin_load(json.data(), json.size(), nullptr, nullptr, &p);
    if (status != nullptr) {
        *status = s;
    }
    if (s != ECUIO_OK) {
        EXPECT_EQ(p, nullptr) << "out is NULL on a failure";
        return Plugin(nullptr);
    }
    return Plugin(p);
}

/* m as a folder: its manifest, its data file and its module's bytes */
inline Folder folder_of(const Made& m)
{
    Folder f;
    f.files["manifest.json"] = m.json(false);
    f.files["a.json"] = kData;
    f.files["guest.wasm"] = std::string(m.module.begin(), m.module.end());
    return f;
}

inline Plugin load_folder(Folder& f, ecuio_status_t* status = nullptr)
{
    ecuio_plugin_t* p = nullptr;
    const std::string& manifest = f.files["manifest.json"];
    const ecuio_status_t s = ecuio_plugin_load(manifest.data(), manifest.size(),
                                               &Folder::read, &f, &p);
    if (status != nullptr) {
        *status = s;
    }
    return Plugin(s == ECUIO_OK ? p : nullptr);
}

inline std::string packed_refusal(const Made& m)
{
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_packed(m.json(), &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
    return ecuio_last_error_detail();
}

inline ecuio_plugin_conversation_t conversation_of(const ecuio_plugin_t* p)
{
    ecuio_plugin_conversation_t c;
    std::memset(&c, 0xA5, sizeof c);
    c.struct_size = sizeof c;
    EXPECT_EQ(ecuio_plugin_conversation(p, &c), ECUIO_OK);
    return c;
}

/* The id of the i-th of all_32_ecus' ECUs: the data file's "pcm", an id
 * of the most characters one takes, then "e<i>" */
inline std::string ecu_id(int i)
{
    return i == 0   ? std::string("pcm")
           : i == 1 ? std::string(31, 'x')
                    : "e" + std::to_string(i);
}

/* m with 32 ECUs in its manifest, replies at 0x700 + i and requests at
 * 0x600 + i, all of them the module's, in that order. The group's
 * security levels are left as they were. */
inline void all_32_ecus(Made& m)
{
    std::string ecus = "[";
    std::string ids = "[";
    for (int i = 0; i < 32; ++i) {
        char ecu[96];
        std::snprintf(ecu, sizeof ecu,
                      R"({"id": "%s", "rx": "%X", "tx": "%X"})",
                      ecu_id(i).c_str(), 0x700 + i, 0x600 + i);
        ecus += std::string(i > 0 ? ", " : "") + ecu;
        ids += std::string(i > 0 ? ", " : "") + "\"" + ecu_id(i) + "\"";
    }
    m.ecus = ecus + "]";
    m.group.ecus = ids + "]";
}

/* The index-th ECU of p's conversation, read into a struct filled first */
inline ecuio_plugin_conversation_ecu_t ecu_of(const ecuio_plugin_t* p,
                                              uint32_t index)
{
    ecuio_plugin_conversation_ecu_t e;
    std::memset(&e, 0xA5, sizeof e);
    e.struct_size = sizeof e;
    EXPECT_EQ(ecuio_plugin_conversation_ecu(p, index, &e), ECUIO_OK);
    return e;
}

/* A change to Made's defaults and words its refusal holds */
struct Refusal {
    const char* what;
    std::function<void(Made&)> change;
    const char* words;
};

/* Each row's packed plugin is refused in its words, at a line and column */
inline void expect_refused(const std::vector<Refusal>& rows)
{
    for (const Refusal& r : rows) {
        Made m;
        r.change(m);
        const std::string why = packed_refusal(m);
        EXPECT_NE(why.find(r.words), std::string::npos)
            << r.what << ": " << why;
        EXPECT_EQ(why.compare(0, 5, "line "), 0) << r.what << ": " << why;
    }
}

} // namespace conversation_test

#endif /* ECUIO_TESTS_PLUGIN_CONVERSATION_SUPPORT_H */
