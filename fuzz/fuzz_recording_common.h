// File/layer: fuzz/fuzz_recording_common.h | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (shared helpers)

#pragma once

#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/recording.h>
#include <ecuio/signals.h>

#include "recording_aids.h" /* core/src: pure, the stretches' rules */
#include "recording_check.h" /* core/src: pure, iso15765_id */
#include "recording_keys.h"  /* core/src: the formats, a keyed signal line */
#include "recording_rows.h" /* core/src: pure */
#include "sha256.h"         /* core/src: pure */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h> /* _getpid */
#define ecuio_fuzz_getpid _getpid
#else
#include <unistd.h>
#define ecuio_fuzz_getpid getpid
#endif

namespace rows = ecuio_recording_rows;

namespace {

bool same_bits(double a, double b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

constexpr uint32_t kCondFlags =
    ECUIO_CONDITION_STALE | ECUIO_CONDITION_FIRED | ECUIO_CONDITION_HELD;

/* A key in room of `room` bytes: terminated, 1 to 31 of a-z, 0-9 and _,
 * the rest of the room zero */
bool condition_key(const char* key, size_t room)
{
    const void* end = memchr(key, '\0', room);
    if (end == nullptr) {
        return false;
    }
    const size_t n = static_cast<size_t>(static_cast<const char*>(end) - key);
    if (n == 0 || n > ECUIO_CONDITION_KEY_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char k = key[i];
        if (!((k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') || k == '_')) {
            return false;
        }
    }
    for (size_t i = n; i < room; ++i) {
        if (key[i] != '\0') {
            return false;
        }
    }
    return true;
}

/* A SHA-256 in room of `room` bytes: "" or 64 lowercase hex digits */
bool sha256_text(const char* text, size_t room)
{
    const void* end = memchr(text, '\0', room);
    if (end == nullptr) {
        return false;
    }
    const size_t n = static_cast<size_t>(static_cast<const char*>(end) - text);
    if (n != 0 && n != 64) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* The scratch folder, one per process (libFuzzer's -jobs run several) */
std::string g_folder;

void remove_folder()
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(g_folder), ec);
}

const std::string& folder()
{
    if (g_folder.empty()) {
        const char* tmp = getenv("TEMP");
        if (tmp == nullptr) {
            tmp = getenv("TMPDIR");
        }
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\') {
                c = '/';
            }
        }
        while (base.size() > 1 && base.back() == '/') {
            base.pop_back();
        }
        g_folder = base + "/ecuio-fuzz-recording-" +
                   std::to_string(static_cast<long>(ecuio_fuzz_getpid()));
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(g_folder),
                                            ec);
        atexit(remove_folder);
    }
    return g_folder;
}

void write_file(const char* name, const uint8_t* p, size_t n, bool present)
{
    const std::string path = folder() + "/" + name;
    if (!present) {
        remove(path.c_str());
        return;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        abort();
    }
    if (n > 0 && fwrite(p, 1, n, f) != n) {
        abort();
    }
    fclose(f);
}

/* Two FLOAT defs on 0x123, bytes 0-3 and 4-7: the kind whose decoded value
 * can be inf or a NaN, no sample at the open and none read again. Two, so
 * one can have no sample at all in a second the other is read from */
const ecuio_signal_set_t* float_set()
{
    static ecuio_signal_def_t defs[2];
    static ecuio_signal_set_t set;
    if (set.count == 0) {
        for (uint16_t i = 0; i < 2; ++i) {
            defs[i].id = static_cast<uint16_t>(i + 1);
            defs[i].message_id = 0x123;
            defs[i].kind = ECUIO_VALUE_FLOAT;
            defs[i].byte_order = ECUIO_BYTE_ORDER_BE;
            defs[i].start_byte = static_cast<uint16_t>(i * 4);
            defs[i].bit_length = 32;
            defs[i].scale = 1.0;
            snprintf(defs[i].name, sizeof defs[i].name, "Float %u", i + 1u);
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.defs = defs;
        set.count = 2;
        if (ecuio_validate_signal_set(&set, nullptr) != ECUIO_VALIDATE_OK) {
            abort();
        }
    }
    return &set;
}

} // namespace
