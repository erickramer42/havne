// File/layer: core/src/plugin_rules.h | internal, C++17, Qt-free, header-only
// Testing: UNIT: PluginTest, PluginWrapTest (test_signals)
//
// A manifest's rules that the plugin loader (plugin_manifest.cpp) and the calls
// that write a manifest (plugin_wrap.cpp) share, so what one writes the
// other reads: an id's alphabet, a version's X.Y.Z, a manufacturer
// code, a path's Windows device names, and core's own version as
// requires.core is compared with it.

#ifndef ECUIO_PLUGIN_RULES_H
#define ECUIO_PLUGIN_RULES_H

#include "ecuio/core.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ecuio_plugin_rules {

/* [a-z0-9-], lo to hi bytes, a letter or digit at each end */
inline bool id_ok(const std::string& s, size_t hi)
{
    if (s.empty() || s.size() > hi || s.front() == '-' || s.back() == '-') {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* X.Y.Z, each 0 to 65535 with no leading zero; false otherwise */
inline bool parse_version(const std::string& s, uint32_t out[3])
{
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            return false;
        }
        if (s[i] == '0' && i + 1 < s.size() && s[i + 1] >= '0' &&
            s[i + 1] <= '9') {
            return false;
        }
        uint32_t n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            n = n * 10 + static_cast<uint32_t>(s[i] - '0');
            if (n > 65535) {
                return false;
            }
            ++i;
        }
        out[part] = n;
        if (part < 2) {
            if (i >= s.size() || s[i] != '.') {
                return false;
            }
            ++i;
        }
    }
    return i == s.size();
}

/* ecuio_plugin_test_set_core_version's (internal.h, plugin.cpp): 0 for
 * none, else 1 << 48 and the three parts 16 bits each */
extern std::atomic<uint64_t> g_test_core;

/* The leading X.Y.Z of core's own version (a suffix after it ignored) */
inline void core_version(uint32_t out[3])
{
    const uint64_t test = g_test_core.load(std::memory_order_relaxed);
    if (test != 0) {
        out[0] = static_cast<uint32_t>((test >> 32) & 0xFFFF);
        out[1] = static_cast<uint32_t>((test >> 16) & 0xFFFF);
        out[2] = static_cast<uint32_t>(test & 0xFFFF);
        return;
    }
    const std::string v = ecuio_version_string();
    size_t end = 0;
    int dots = 0;
    while (end < v.size() &&
           ((v[end] >= '0' && v[end] <= '9') || v[end] == '.')) {
        if (v[end] == '.' && ++dots == 3) {
            break;
        }
        ++end;
    }
    if (!parse_version(v.substr(0, end), out)) {
        out[0] = out[1] = out[2] = 0;
    }
}

inline bool older(const uint32_t a[3], const uint32_t b[3])
{
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i];
        }
    }
    return false;
}

/* 3 or 6 of the VIN alphabet (no I, O or Q), as ecuio_vin_info_t.wmi */
inline bool wmi_ok(const std::string& s)
{
    if (s.size() != 3 && s.size() != 6) {
        return false;
    }
    for (char c : s) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter =
            c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q';
        if (!digit && !letter) {
            return false;
        }
    }
    return true;
}

/* names_a_device's judgment of one part */
inline bool device_part(const std::string& part)
{
    if (part.back() == '.' || part.back() == ' ') {
        return true;
    }
    std::string stem = part.substr(0, part.find('.'));
    for (char& c : stem) {
        c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    static const char* const kDevices[] = {"CON", "PRN",    "AUX",
                                           "NUL", "CONIN$", "CONOUT$"};
    for (const char* d : kDevices) {
        if (stem == d) {
            return true;
        }
    }
    return stem.size() == 4 &&
           (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
           stem[3] >= '0' && stem[3] <= '9';
}

/* Whether a part of path p is a Windows device name in any case with any
 * extension (con.json, NUL, com1.txt) or ends in a dot or a space, which
 * Windows drops: read from disk it opens the device or another file. A folder's
 * reads refuse one and a wrap or put never writes one; a packed file's
 * paths are keys in its text, which nothing opens, so its load takes them */
inline bool names_a_device(const std::string& p)
{
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            if (i > start && device_part(p.substr(start, i - start))) {
                return true;
            }
            start = i + 1;
        }
    }
    return false;
}

/* A manifest's "lineage": ECUIO_PLUGIN_LINEAGE_LEN of 0-9 and a-f */
inline bool lineage_ok(const std::string& s, size_t length)
{
    if (s.size() != length) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace ecuio_plugin_rules

#endif /* ECUIO_PLUGIN_RULES_H */
