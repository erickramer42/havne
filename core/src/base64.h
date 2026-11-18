// File/layer: core/src/base64.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: Base64Test (test_signals); FUZZ: fuzz_plugin_manifest
//
// RFC 4648's base64 as a packed plugin carries a binary file (ADR-0066
// section 2.8): the standard alphabet, padding required, no white space
// or line break, and the unused bits of a padded group zero, so a
// sequence of bytes has one text and a text one sequence of bytes.

#ifndef ECUIO_BASE64_H
#define ECUIO_BASE64_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* A character's six bits, or -1 for one outside the alphabet ('=' too) */
constexpr int ecuio_base64_value(char c)
{
    return (c >= 'A' && c <= 'Z')   ? c - 'A'
           : (c >= 'a' && c <= 'z') ? c - 'a' + 26
           : (c >= '0' && c <= '9') ? c - '0' + 52
           : c == '+'               ? 62
           : c == '/'               ? 63
                                    : -1;
}

/* How many bytes n characters of text decode to; false when the text
 * isn't whole groups of four with padding only at its end */
inline bool ecuio_base64_size(const char* text, size_t n, size_t* bytes)
{
    if (n % 4 != 0) {
        return false;
    }
    size_t pad = 0;
    if (n > 0 && text[n - 1] == '=') {
        pad = text[n - 2] == '=' ? 2 : 1;
    }
    *bytes = n / 4 * 3 - pad;
    return true;
}

/* text's n characters into out (emptied first). False, with out's
 * content unspecified, for anything but the one text of some bytes: a
 * character outside the alphabet, a length that isn't a multiple of 4,
 * padding anywhere but the end, or unused bits that aren't zero. May
 * throw std::bad_alloc. */
inline bool ecuio_base64_decode(const char* text, size_t n,
                                std::vector<uint8_t>* out)
{
    out->clear();
    size_t bytes = 0;
    if (!ecuio_base64_size(text, n, &bytes)) {
        return false;
    }
    out->reserve(bytes);
    for (size_t i = 0; i < n; i += 4) {
        const bool last = i + 4 == n;
        const int a = ecuio_base64_value(text[i]);
        const int b = ecuio_base64_value(text[i + 1]);
        const bool pad2 = last && text[i + 2] == '=' && text[i + 3] == '=';
        const bool pad1 = last && !pad2 && text[i + 3] == '=';
        const int c = pad2 ? 0 : ecuio_base64_value(text[i + 2]);
        const int d = (pad1 || pad2) ? 0 : ecuio_base64_value(text[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return false;
        }
        if ((pad2 && (b & 0x0F) != 0) || (pad1 && (c & 0x03) != 0)) {
            return false; /* bits no byte takes */
        }
        const uint32_t group = (static_cast<uint32_t>(a) << 18) |
                               (static_cast<uint32_t>(b) << 12) |
                               (static_cast<uint32_t>(c) << 6) |
                               static_cast<uint32_t>(d);
        out->push_back(static_cast<uint8_t>(group >> 16));
        if (!pad2) {
            out->push_back(static_cast<uint8_t>(group >> 8));
        }
        if (!pad1 && !pad2) {
            out->push_back(static_cast<uint8_t>(group));
        }
    }
    return true;
}

#endif /* ECUIO_BASE64_H */
