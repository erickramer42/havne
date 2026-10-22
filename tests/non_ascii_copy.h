// File/layer: tests/non_ascii_copy.h | GTest helper, Win32-only, header-only
// Testing: HARNESS — used by test_j2534_load and test_j2534_surrogate
//
// A copy of a DLL in a directory whose name holds Greek and CJK
// characters, which no ANSI code page has all of: only a path that
// stays UTF-8 through core and UTF-16 at the Win32 call reaches it.

#ifndef QTTUNE_TEST_NON_ASCII_COPY_H
#define QTTUNE_TEST_NON_ASCII_COPY_H

#ifdef _WIN32

#include <windows.h>

#include <cstdio>

struct NonAsciiCopy {
    wchar_t dir[MAX_PATH] = {};
    wchar_t file[MAX_PATH] = {};
    char utf8[3 * MAX_PATH] = {};   /* the copy's path, for core */
    bool ok = false;

    explicit NonAsciiCopy(const char* source_utf8)
    {
        wchar_t src[MAX_PATH] = {};
        wchar_t tmp[MAX_PATH] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, source_utf8, -1, src, MAX_PATH) <= 0 ||
            GetTempPathW(MAX_PATH, tmp) == 0) {
            return;
        }
        swprintf(dir, MAX_PATH, L"%lsqttune-Ω测-%lu", tmp,
                 GetCurrentProcessId());
        swprintf(file, MAX_PATH, L"%ls\\fake_j2534.dll", dir);
        if (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return;
        }
        ok = CopyFileW(src, file, FALSE) &&
             WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8),
                                 nullptr, nullptr) > 0;
    }

    /* The DLL must be unloaded first, or the delete fails and leaves
     * the directory behind in TEMP. */
    ~NonAsciiCopy()
    {
        DeleteFileW(file);
        RemoveDirectoryW(dir);
    }

    NonAsciiCopy(const NonAsciiCopy&) = delete;
    NonAsciiCopy& operator=(const NonAsciiCopy&) = delete;
};

#endif /* _WIN32 */

#endif /* QTTUNE_TEST_NON_ASCII_COPY_H */
