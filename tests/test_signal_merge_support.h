// File/layer: tests/test_signal_merge_support.h | C++17, Qt-free, every platform
// Testing: UNIT: the helpers of test_signal_merge*.cpp (test_signals)

#pragma once

#include "test_signal_file_support.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace {

/* A number read by DID; name "N <key>" unless given, more goes into it */
inline std::string num(const std::string& key, const std::string& did,
                       const std::string& more = "",
                       const std::string& name = "")
{
    return "{\"key\":\"" + key + "\",\"name\":\"" +
           (name.empty() ? "N " + key : name) +
           "\",\"read\":{\"service\":\"22\",\"number\":\"" + did +
           "\"},\"field\":{\"byte\":0}" + more + "}";
}

/* A computed signal named "C <key>" */
inline std::string calc(const std::string& key, const std::string& expr,
                        const std::string& more = "")
{
    return "{\"key\":\"" + key + "\",\"name\":\"C " + key +
           "\",\"kind\":\"computed\",\"expr\":\"" + expr + "\"" + more + "}";
}

const char* const kPcm = R"([{"id":"pcm","rx":"7E8"}])";

/* A signal file named name: head is any member more, ending in a comma;
 * ecus "" for a file that lists none */
inline std::string doc(const std::string& name, const std::string& signals,
                       const std::string& head = "", unsigned format = 3,
                       const std::string& ecus = kPcm)
{
    return "{\"havne_signals\":" + std::to_string(format) + ",\"name\":\"" +
           name + "\",\"source\":{\"name\":\"src " + name + "\"}," + head +
           (ecus.empty() ? "" : "\"ecus\":" + ecus + ",") + "\"signals\":[" +
           signals + "]}";
}

/* A file read at a set's bounds, as a variant's copy or a big part is */
inline File load_set(const std::string& json)
{
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load_with(json.data(), json.size(),
                                          ECUIO_SIGNAL_FILE_LOAD_SET, &f),
              ECUIO_OK)
        << ecuio_last_error_detail();
    return File(f);
}

struct Merged {
    File file;
    ecuio_status_t status = ECUIO_OK;
    std::string words;
};

inline Merged merge(std::initializer_list<const ecuio_signal_file_t*> parts,
                    uint32_t flags = 0)
{
    const std::vector<const ecuio_signal_file_t*> list(parts);
    ecuio_merge_options_t options{};
    options.struct_size = sizeof options;
    options.flags = flags;
    ecuio_signal_file_t* out = reinterpret_cast<ecuio_signal_file_t*>(1);
    Merged m;
    m.status =
        ecuio_signal_file_merge(list.data(), static_cast<uint32_t>(list.size()),
                                flags != 0 ? &options : nullptr, &out);
    m.words = ecuio_last_error_detail();
    if (m.status != ECUIO_OK) {
        EXPECT_EQ(out, nullptr) << "out is NULL on a failure";
        out = nullptr;
    }
    m.file.reset(out);
    return m;
}

/* The file's own signals' keys, in the set's order */
inline std::vector<std::string> keys_of(const ecuio_signal_file_t* f)
{
    std::vector<std::string> out;
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        const std::string key = ecuio_signal_file_key(f, &set->defs[i]);
        if (!key.empty()) {
            out.push_back(key);
        }
    }
    return out;
}

inline std::vector<std::string> replaced_keys(const ecuio_signal_file_t* f,
                                              const char* key)
{
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key);
    EXPECT_NE(def, nullptr) << key;
    uint32_t count = 0;
    std::vector<ecuio_signal_key_t> keys(32);
    EXPECT_EQ(ecuio_signal_file_replaced_keys(f, def, keys.data(), 32, &count),
              ECUIO_OK);
    std::vector<std::string> out;
    for (uint32_t i = 0; i < count && i < 32; ++i) {
        out.emplace_back(keys[i].text);
    }
    return out;
}

inline std::vector<ecuio_signal_note_t> notes_of(const ecuio_signal_file_t* f)
{
    std::vector<ecuio_signal_note_t> out;
    for (uint32_t i = 0; i < ecuio_signal_file_note_count(f); ++i) {
        ecuio_signal_note_t n;
        std::memset(&n, 0xA5, sizeof n);
        n.struct_size = sizeof n;
        EXPECT_EQ(ecuio_signal_file_note(f, i, &n), ECUIO_OK);
        EXPECT_NE(std::memchr(n.text, '\0', sizeof n.text), nullptr);
        for (uint32_t r : n.reserved) {
            EXPECT_EQ(r, 0u);
        }
        if (n.kind != ECUIO_NOTE_BESIDE_BASE) {
            EXPECT_EQ(n.base_def, UINT32_MAX) << n.text;
        }
        out.push_back(n);
    }
    return out;
}

/* The sentences of the file's notes, each "<kind>: <text>" */
inline std::vector<std::string> sentences(const ecuio_signal_file_t* f)
{
    std::vector<std::string> out;
    for (const ecuio_signal_note_t& n : notes_of(f)) {
        out.push_back(std::to_string(n.kind) + ": " + n.text);
    }
    return out;
}

/* The def's place in the file's set */
inline uint32_t place_of(const ecuio_signal_file_t* f, const char* key)
{
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key);
    EXPECT_NE(def, nullptr) << key;
    return def != nullptr
               ? static_cast<uint32_t>(def - ecuio_signal_file_set(f)->defs)
               : UINT32_MAX;
}

using Words = std::vector<std::string>;

} // namespace
