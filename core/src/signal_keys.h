// File/layer: core/src/signal_keys.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: SignalKeysTest (test_signals)
//
// The one rule for a signal a person names (ADR-0065 section 9), over
// anything that has keys and names: a set's defs (ecuio_signal_set_find)
// and a recording's signals.

#ifndef ECUIO_SIGNAL_KEYS_H
#define ECUIO_SIGNAL_KEYS_H

#include "ecuio/core.h"
#include "ecuio/signals.h"
#include "error_detail.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace ecuio_keys {

/* The most keys a repeated name's refusal lists */
constexpr uint32_t kListed = 8;

/* text among count signals, key_of(i) and name_of(i) each one's as a
 * string_view: the signal with that key, else the one signal with that
 * name. A signal with no key ("") goes by its name alone.
 * ECUIO_ERR_INVALID_ARGUMENT with the detail set when none has it or
 * several are named so; *index is written only on ECUIO_OK. Allocates
 * nothing. */
template <typename KeyOf, typename NameOf>
ecuio_status_t find(uint32_t count, KeyOf key_of, NameOf name_of,
                    const char* text, uint32_t* index)
{
    const std::string_view want(text);
    char shown[97];
    std::snprintf(shown, sizeof shown, "%.96s", text);
    ecuio_text_one_line(shown);
    if (want.empty()) {
        ecuio_detail_set("a signal's key or name is empty");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (key_of(i) == want) {
            *index = i;
            return ECUIO_OK;
        }
    }
    uint32_t named = 0;
    uint32_t first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (name_of(i) == want) {
            if (named == 0) {
                first = i;
            }
            ++named;
        }
    }
    if (named == 1) {
        *index = first;
        return ECUIO_OK;
    }
    if (named == 0) {
        ecuio_detail_set("no signal with the key or name '%s'", shown);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    /* Their keys, the first kListed: each at most ECUIO_SIGNAL_KEY_MAX
     * bytes and a ", " */
    char list[kListed * (ECUIO_SIGNAL_KEY_MAX + 2) + 1] = "";
    size_t at = 0;
    uint32_t listed = 0;
    for (uint32_t i = 0; i < count && listed < kListed; ++i) {
        if (name_of(i) != want) {
            continue;
        }
        const std::string_view key = key_of(i);
        const int n = std::snprintf(list + at, sizeof list - at, "%s%.*s",
                                    listed > 0 ? ", " : "",
                                    static_cast<int>(key.size()), key.data());
        if (n < 0 || static_cast<size_t>(n) >= sizeof list - at) {
            break;
        }
        at += static_cast<size_t>(n);
        ++listed;
    }
    if (named > listed) {
        ecuio_detail_set("%u signals are named '%s': name one by its key "
                         "(%s and %u more)",
                         static_cast<unsigned>(named), shown, list,
                         static_cast<unsigned>(named - listed));
    } else {
        ecuio_detail_set("%u signals are named '%s': name one by its key (%s)",
                         static_cast<unsigned>(named), shown, list);
    }
    return ECUIO_ERR_INVALID_ARGUMENT;
}

/* A fixed text field's bytes up to its NUL */
template <size_t N> inline std::string_view view_of(const char (&text)[N])
{
    return std::string_view(text, strnlen(text, N));
}

} // namespace ecuio_keys

#endif /* ECUIO_SIGNAL_KEYS_H */
