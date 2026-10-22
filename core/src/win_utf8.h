// File/layer: core/src/win_utf8.h | internal, Win32-only, C++17, Qt-free
// Testing: HARNESS — FakeFixture.NonAsciiDllPathLoadsInProc and
//          J2534SurrogateTest.NonAsciiDllPathLoadsInTheChild load the
//          fake DLL from a directory with a non-ASCII name
//
// Paths cross the C ABI as UTF-8 (j2534.h); Win32 takes them as UTF-16
// through the W APIs. The A APIs would read the same bytes in the ANSI
// code page and mangle any non-ASCII character.

#ifndef QTTUNE_WIN_UTF8_H
#define QTTUNE_WIN_UTF8_H

#ifdef _WIN32

#include <windows.h>

#include <cstddef>

/* UTF-8 to UTF-16, terminator included. False, with out empty, when the
 * input isn't valid UTF-8 or doesn't fit in cap wide characters. */
inline bool qttune_utf8_to_wide(const char* in, wchar_t* out, size_t cap)
{
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1,
                                      out, static_cast<int>(cap));
    if (n <= 0) {
        if (cap > 0) out[0] = L'\0';
        return false;
    }
    return true;
}

/* UTF-16 to UTF-8, terminator included. False, with out empty, when the
 * input has an unpaired surrogate or doesn't fit in cap bytes. */
inline bool qttune_wide_to_utf8(const wchar_t* in, char* out, size_t cap)
{
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in, -1,
                                      out, static_cast<int>(cap),
                                      nullptr, nullptr);
    if (n <= 0) {
        if (cap > 0) out[0] = '\0';
        return false;
    }
    return true;
}

#endif /* _WIN32 */

#endif /* QTTUNE_WIN_UTF8_H */
