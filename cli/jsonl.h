// File/layer: cli/jsonl.h | havne-cli, JSON-lines formatting (ADR-0031 §3)
// Testing: UNIT (tests/test_cli.cpp, CliLineTest)
//
// Builds one JSON line into a caller's fixed buffer: no allocation, so
// the frame and values callbacks call it on core's worker. A field that
// doesn't fit sets `full` and writes nothing more; the caller checks it
// once, at the end of the line.

#ifndef HAVNE_CLI_JSONL_H
#define HAVNE_CLI_JSONL_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cli {

class Line
{
public:
    Line(char* buf, size_t cap) : m_buf(buf), m_cap(cap) {}

    const char* data() const { return m_buf; }
    size_t size() const { return m_len; }
    size_t room() const { return m_cap - m_len; }
    bool full() const { return m_full; }

    void raw(const char* s, size_t n)
    {
        if (m_full || n > room()) {
            m_full = true;
            return;
        }
        std::memcpy(m_buf + m_len, s, n);
        m_len += n;
    }
    void raw(const char* s) { raw(s, std::strlen(s)); }
    void ch(char c) { raw(&c, 1); }

    // `"key":`, after a comma unless it's the object's first key
    void key(const char* k)
    {
        if (m_len > 0 && m_buf[m_len - 1] != '{' && m_buf[m_len - 1] != '[') {
            ch(',');
        }
        ch('"');
        raw(k);
        raw("\":", 2);
    }

    void u64(uint64_t v)
    {
        char tmp[24];
        const int n = std::snprintf(tmp, sizeof tmp, "%llu",
                                    static_cast<unsigned long long>(v));
        raw(tmp, static_cast<size_t>(n));
    }

    // A CAN ID or a flags word as frames.csv writes them: "0x7E8"
    void hex_id(uint32_t v)
    {
        char tmp[16];
        const int n = std::snprintf(tmp, sizeof tmp, "\"0x%X\"",
                                    static_cast<unsigned>(v));
        raw(tmp, static_cast<size_t>(n));
    }

    // Bytes as uppercase hex pairs with a space between, frames.csv's form
    void hex_bytes(const uint8_t* p, size_t n)
    {
        static const char digits[] = "0123456789ABCDEF";
        if (m_full || 2 + (n ? 3 * n - 1 : 0) > room()) {
            m_full = true;
            return;
        }
        m_buf[m_len++] = '"';
        for (size_t i = 0; i < n; ++i) {
            if (i) {
                m_buf[m_len++] = ' ';
            }
            m_buf[m_len++] = digits[p[i] >> 4];
            m_buf[m_len++] = digits[p[i] & 0x0F];
        }
        m_buf[m_len++] = '"';
    }

    // A decimal that reads back as the same double: 15 significant
    // digits when they do (most values, and the shortest form for any
    // reading with a few decimals), else 16 or 17. JSON has no NaN or
    // infinity, so those are null. The CLI never sets a locale, so the
    // point is '.'.
    void number(double v)
    {
        if (!std::isfinite(v)) {
            raw("null", 4);
            return;
        }
        char tmp[32];
        int n = 0;
        for (int digits = 15; digits <= 17; ++digits) {
            n = std::snprintf(tmp, sizeof tmp, "%.*g", digits, v);
            if (std::strtod(tmp, nullptr) == v) {
                break;
            }
        }
        raw(tmp, static_cast<size_t>(n));
    }

    // A JSON string from up to `n` bytes of UTF-8, stopping at a NUL.
    // Quotes, backslashes and control characters are escaped; a byte
    // that isn't part of a well-formed UTF-8 sequence becomes U+FFFD.
    // Returns the input bytes taken: fewer than the string's when
    // `limit` (output bytes, the quotes included) cut it, at a whole
    // character. Nothing is written if not even "" fits.
    size_t string(const char* s, size_t n, size_t limit = SIZE_MAX)
    {
        const size_t end = limit < room() ? m_len + limit : m_cap;
        if (m_full || end - m_len < 2) {
            m_full = true;
            return 0;
        }
        m_buf[m_len++] = '"';
        size_t i = 0;
        while (i < n && s[i] != '\0') {
            char esc[8];
            const char* out = esc;
            size_t out_n = 0;
            size_t take = 1;
            const auto c = static_cast<unsigned char>(s[i]);
            if (c == '"' || c == '\\') {
                esc[0] = '\\';
                esc[1] = static_cast<char>(c);
                out_n = 2;
            } else if (c < 0x20) {
                out_n = control_escape(c, esc);
            } else if (c < 0x80) {
                out = s + i;
                out_n = 1;
            } else {
                take = utf8_sequence(
                    reinterpret_cast<const unsigned char*>(s) + i, n - i);
                if (take) {
                    out = s + i;
                    out_n = take;
                } else {
                    out = "\xEF\xBF\xBD"; // U+FFFD
                    out_n = 3;
                    take = 1;
                }
            }
            if (out_n > end - m_len - 1) {
                break; // room for the closing quote stays
            }
            std::memcpy(m_buf + m_len, out, out_n);
            m_len += out_n;
            i += take;
        }
        m_buf[m_len++] = '"';
        return i;
    }

    // The line's end. False if anything didn't fit: drop the line.
    bool finish()
    {
        ch('\n');
        return !m_full;
    }

    // The length of the well-formed UTF-8 sequence at p (2 to 4 bytes),
    // or 0: a stray continuation byte, an overlong form, a surrogate, a
    // code point past U+10FFFF, or a sequence cut short
    static size_t utf8_sequence(const unsigned char* p, size_t n)
    {
        const unsigned char c = p[0];
        size_t len;
        unsigned char lo = 0x80, hi = 0xBF; // the second byte's range
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0) {
                lo = 0xA0;
            } else if (c == 0xED) {
                hi = 0x9F;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0) {
                lo = 0x90;
            } else if (c == 0xF4) {
                hi = 0x8F;
            }
        } else {
            return 0;
        }
        if (n < len || p[1] < lo || p[1] > hi) {
            return 0;
        }
        for (size_t k = 2; k < len; ++k) {
            if (p[k] < 0x80 || p[k] > 0xBF) {
                return 0;
            }
        }
        return len;
    }

private:
    static size_t control_escape(unsigned char c, char* esc)
    {
        esc[0] = '\\';
        switch (c) {
        case '\n':
            esc[1] = 'n';
            return 2;
        case '\r':
            esc[1] = 'r';
            return 2;
        case '\t':
            esc[1] = 't';
            return 2;
        case '\b':
            esc[1] = 'b';
            return 2;
        case '\f':
            esc[1] = 'f';
            return 2;
        default:
            std::snprintf(esc, 8, "\\u%04X", c);
            return 6;
        }
    }

    char* m_buf;
    size_t m_cap;
    size_t m_len = 0;
    bool m_full = false;
};

} // namespace cli

#endif // HAVNE_CLI_JSONL_H
