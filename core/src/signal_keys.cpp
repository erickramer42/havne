// File/layer: core/src/signal_keys.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalKeysTest (test_signals)
//
// A signal's key (ADR-0065 section 9, signals.h): the address key's
// text, the key each def of a set is kept under, and the def a person's
// text names.

#include "ecuio/signals.h"

#include "signal_file_build.h" /* a file's keys by def */
#include "signal_keys.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

bool is_text_kind(uint8_t kind)
{
    return kind == ECUIO_VALUE_TEXT || kind == ECUIO_VALUE_HEX;
}

/* The def's address into out, zeroed past it; false, out "", when the
 * text has no room. A computed def's is "". */
bool address_of(const ecuio_signal_def_t& d, ecuio_signal_key_t* out)
{
    std::memset(out, 0, sizeof *out);
    if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
        return true;
    }
    char text[96];
    int at = std::snprintf(text, sizeof text,
                           d.message_id > 0x7FFu ? "%08lX:" : "%03lX:",
                           static_cast<unsigned long>(d.message_id));
    if (d.mux_length > 0) {
        /* The key's bytes, its first the most significant of them */
        const unsigned bytes = d.mux_length < 4 ? d.mux_length : 4;
        const unsigned long key =
            bytes == 4 ? static_cast<unsigned long>(d.mux_key)
                       : static_cast<unsigned long>(d.mux_key &
                                                    ((1ul << (8 * bytes)) - 1));
        at += std::snprintf(text + at, sizeof text - static_cast<size_t>(at),
                            "%0*lX", static_cast<int>(2 * bytes), key);
        if (d.mux_offset != 0) {
            at +=
                std::snprintf(text + at, sizeof text - static_cast<size_t>(at),
                              "@%u", static_cast<unsigned>(d.mux_offset));
        }
    }
    if (is_text_kind(d.kind)) {
        at += std::snprintf(text + at, sizeof text - static_cast<size_t>(at),
                            ":%u:t%u", static_cast<unsigned>(d.start_byte),
                            static_cast<unsigned>(d.byte_length));
    } else {
        at += std::snprintf(text + at, sizeof text - static_cast<size_t>(at),
                            ":%u.%u:%u", static_cast<unsigned>(d.start_byte),
                            static_cast<unsigned>(d.bit_offset),
                            static_cast<unsigned>(d.bit_length));
    }
    if (at < 0 || static_cast<size_t>(at) > ECUIO_SIGNAL_KEY_MAX) {
        return false;
    }
    std::memcpy(out->text, text, static_cast<size_t>(at));
    return true;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_signal_address_key(const ecuio_signal_def_t* def,
                                        ecuio_signal_key_t* out)
{
    if (def == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    return address_of(*def, out) ? ECUIO_OK : ECUIO_ERR_INVALID_ARGUMENT;
}

ecuio_status_t ecuio_signal_set_keys(const ecuio_signal_set_t* set,
                                     const ecuio_signal_file_t* file,
                                     ecuio_signal_key_t* keys,
                                     uint32_t capacity, uint32_t* count)
{
    if (count != nullptr) {
        *count = 0;
    }
    if (set == nullptr || count == nullptr ||
        (keys == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (file != nullptr && &file->set != set) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const uint32_t n = set->defs != nullptr ? set->count : 0;
    std::vector<ecuio_signal_key_t> made;
    std::vector<uint32_t> order;
    std::vector<const char*> authors; /* by def, NULL with none */
    std::vector<uint8_t> own;         /* by def: its address is its key */
    try {
        made.resize(n);
        order.resize(n);
        authors.assign(n, nullptr);
        own.assign(n, 0);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (file != nullptr) {
        for (const auto& k : file->keys) {
            if (k.second < n && authors[k.second] == nullptr) {
                authors[k.second] = k.first.c_str();
            }
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        (void)address_of(set->defs[i], &made[i]); /* no room: no address */
        order[i] = i;
    }
    /* Defs of one address side by side, the set's order among them: the
     * first keeps the address, the rest have none of their own */
    std::sort(order.begin(), order.end(), [&made](uint32_t a, uint32_t b) {
        const int c = std::strcmp(made[a].text, made[b].text);
        return c != 0 ? c < 0 : a < b;
    });
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t i = order[k];
        own[i] =
            made[i].text[0] != '\0' &&
            (k == 0 || std::strcmp(made[order[k - 1]].text, made[i].text) != 0);
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (own[i] != 0) {
            continue;
        }
        std::memset(&made[i], 0, sizeof made[i]);
        if (authors[i] != nullptr) {
            std::snprintf(made[i].text, sizeof made[i].text, "%s", authors[i]);
        } else {
            std::snprintf(made[i].text, sizeof made[i].text, "id:%u",
                          static_cast<unsigned>(set->defs[i].id));
        }
    }
    *count = n;
    for (uint32_t i = 0; i < n && i < capacity; ++i) {
        keys[i] = made[i];
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_signal_set_find(const ecuio_signal_set_t* set,
                                     const ecuio_signal_key_t* keys,
                                     uint32_t count, const char* text_utf8,
                                     uint32_t* index)
{
    if (set == nullptr || text_utf8 == nullptr || index == nullptr ||
        (keys == nullptr && count > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const uint32_t n = set->defs != nullptr ? set->count : 0;
    return ecuio_keys::find(
        count < n ? count : n,
        [keys](uint32_t i) { return ecuio_keys::view_of(keys[i].text); },
        [set](uint32_t i) { return ecuio_keys::view_of(set->defs[i].name); },
        text_utf8, index);
}

} // extern "C"
