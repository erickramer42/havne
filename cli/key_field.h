// File/layer: cli/key_field.h | havne-cli, a signal's key on a line
// Testing: UNIT: CliLogTest, CliViewTest (test_cli)
//
// The key a signal is kept under (ADR-0065 section 9) as havne-cli's
// lines give it: a "key" field after each name, so two signals of one
// name are told apart, and a set's keys for the lines and the lookups
// that go by them. Public headers only.

#ifndef HAVNE_CLI_KEY_FIELD_H
#define HAVNE_CLI_KEY_FIELD_H

#include "jsonl.h"

#include "ecuio/signals.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cli {

// Room for a key field: its name, quotes and a key's 31 bytes
inline constexpr size_t kKeyFieldBytes = 48;

// ,"key":"<key>", or ,"key":null for a signal with none (a recording
// older than format 4 keeps its signals by name)
inline void key_field(Line& l, const char* key, size_t size)
{
    l.key("key");
    if (key == nullptr || key[0] == '\0') {
        l.raw("null");
    } else {
        l.string(key, size);
    }
}

// The key each def of set is kept under, in the set's order
// (ecuio_signal_set_keys; file the session's signal file or NULL, which
// gives a computed signal its author's key). Empty for no set, or when
// core refuses the set.
inline std::vector<ecuio_signal_key_t> set_keys(const ecuio_signal_set_t* set,
                                                const ecuio_signal_file_t* file)
{
    std::vector<ecuio_signal_key_t> keys;
    if (set == nullptr || set->defs == nullptr || set->count == 0) {
        return keys;
    }
    keys.resize(set->count);
    uint32_t count = 0;
    if (ecuio_signal_set_keys(set, file, keys.data(), set->count, &count) !=
        ECUIO_OK) {
        keys.clear();
    }
    return keys;
}

} // namespace cli

#endif // HAVNE_CLI_KEY_FIELD_H
