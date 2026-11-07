// File/layer: tests/open_handles.h | test-only, header-only, C++17
// Testing: UNIT: ExportTest (test_core); HARNESS: Elm327LogTest
//          (test_elm327)
//
// The handles the process holds open: Windows' handle count, elsewhere
// the descriptors /dev/fd lists (Linux and macOS). Counted before and
// after a call run out of memory at each allocation, so a search handle
// or a DIR* left open by a throw shows as a rise.

#ifndef ECUIO_TESTS_OPEN_HANDLES_H
#define ECUIO_TESTS_OPEN_HANDLES_H

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

inline unsigned long open_handles()
{
#ifdef _WIN32
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
#else
    unsigned long n = 0;
    DIR* d = opendir("/dev/fd");
    if (d == nullptr) {
        return 0;
    }
    while (readdir(d) != nullptr) {
        ++n;
    }
    closedir(d);
    return n;
#endif
}

#endif /* ECUIO_TESTS_OPEN_HANDLES_H */
