// File/layer: cli/partial_file.h | havne-cli, a file written whole beside
//             its place; public headers only
// Testing: UNIT: CliPartialTest (test_cli)
//
// A file the CLI writes whole goes first into a partial of the call's own,
// <path>.partial-<pid>-<n>, created exclusively, so a second writer of the
// same path (another havne-cli, or the same one twice) never truncates the
// first's partial, which could then be renamed over the user's file. A
// name already there is another writer's and is left alone: the next n is
// tried, kPartialTries of them, then the call is refused. Kept out of the
// pure headers: it brings the platform's file headers.

#ifndef HAVNE_CLI_PARTIAL_FILE_H
#define HAVNE_CLI_PARTIAL_FILE_H

#include <cerrno>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cli {

/* The partial names tried before the call is refused */
constexpr unsigned kPartialTries = 16;

#ifdef _WIN32
/* UTF-8 as Windows' UTF-16 file calls take it, or "" if it isn't UTF-8 */
inline std::wstring utf8_to_wide(const char* utf8)
{
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                      nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}
#endif

inline unsigned long process_id()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

/* path's n-th partial name for the process pid */
inline std::string partial_name(const std::string& path, unsigned long pid,
                                unsigned n)
{
    return path + ".partial-" + std::to_string(pid) + "-" + std::to_string(n);
}

/* A new file at name for writing, never one already there (EEXIST) and
 * never inherited by a child; nullptr with *e */
inline FILE* create_exclusive(const std::string& name, int* e)
{
#ifdef _WIN32
    FILE* f = nullptr;
    *e = _wfopen_s(&f, utf8_to_wide(name.c_str()).c_str(), L"wbxN");
    return f;
#else
    const int fd =
        open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        *e = errno;
        return nullptr;
    }
    FILE* f = fdopen(fd, "wb");
    if (f == nullptr) {
        *e = errno;
        close(fd);
        unlink(name.c_str());
    }
    return f;
#endif
}

/* Removes a file the call made */
inline void remove_file(const std::string& name)
{
#ifdef _WIN32
    _wremove(utf8_to_wide(name.c_str()).c_str());
#else
    std::remove(name.c_str());
#endif
}

/* path's partial for the process pid: the first of kPartialTries names
 * not there, created, its name in *name; nullptr with *e (EEXIST when
 * every name was there, another error at once) */
inline FILE* create_partial(const std::string& path, unsigned long pid,
                            std::string* name, int* e)
{
    for (unsigned n = 0; n < kPartialTries; ++n) {
        *name = partial_name(path, pid, n);
        FILE* f = create_exclusive(*name, e);
        if (f != nullptr || *e != EEXIST) {
            return f;
        }
    }
    return nullptr;
}

} // namespace cli

#endif // HAVNE_CLI_PARTIAL_FILE_H
