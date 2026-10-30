// File/layer: core/src/win_utf8.h | internal, Win32-only, C++17, Qt-free
// Testing: HARNESS: FakeFixture.NonAsciiDllPathLoads and
//          J2534SurrogateTest.NonAsciiDllPathLoadsInTheChild load the
//          fake DLL from a directory with a non-ASCII name;
//          J2534QueryParamTest.RelativeDllPathsAreRefused and
//          J2534SurrogateTest.SurrogateExeOverrideIsRestricted judge
//          paths with ecuio_is_absolute_path
//
// Paths cross the C ABI as UTF-8 (j2534.h); Win32 takes them as UTF-16
// through the W APIs. The A APIs would read the same bytes in the ANSI
// code page and mangle any non-ASCII character.

#ifndef ECUIO_WIN_UTF8_H
#define ECUIO_WIN_UTF8_H

#ifdef _WIN32

#include <windows.h>

#include <cstddef>

/* UTF-8 to UTF-16, terminator included. False, with out empty, when the
 * input isn't valid UTF-8 or doesn't fit in cap wide characters. */
inline bool ecuio_utf8_to_wide(const char* in, wchar_t* out, size_t cap)
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
inline bool ecuio_wide_to_utf8(const wchar_t* in, char* out, size_t cap)
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

/* True for a drive-absolute ("C:\", "C:/") or UNC ("\\") path, in
 * UTF-8 or UTF-16. A relative path, or one rooted without a drive
 * ("\dir", "C:dir"), would be resolved against the current directory or
 * the DLL search order. */
template <typename C>
inline bool ecuio_is_absolute_path(const C* p)
{
    const bool drive = ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
                       p[1] == ':' && (p[2] == '\\' || p[2] == '/');
    const bool unc = (p[0] == '\\' && p[1] == '\\');
    return drive || unc;
}

#endif /* _WIN32 */

#endif /* ECUIO_WIN_UTF8_H */
