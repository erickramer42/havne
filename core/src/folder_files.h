// File/layer: core/src/folder_files.h | internal, C++17, Qt-free; files and
//             folders by UTF-8 path, for the imports and exports (ADR-0054)
// Testing: UNIT: ImportTest, ExportTest, FolderExportTest (test_core)
//
// A file written whole under a temporary name and renamed into place, a
// folder made only when the name is free, each failure in words that
// never name the path. Windows takes UTF-16 paths (win_utf8.h).

#ifndef ECUIO_FOLDER_FILES_H
#define ECUIO_FOLDER_FILES_H

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include "win_utf8.h"
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace ecuio_files {

/* The C library's errno, in words */
inline std::string errno_words(int err)
{
    switch (err) {
    case ENOENT:
        return "the folder it goes in doesn't exist";
    case EACCES:
    case EPERM:
        return "permission denied";
    case EEXIST:
    case ENOTEMPTY:
        return "it exists already";
    case ENOSPC:
        return "the disk is full";
    default:
        return "error " + std::to_string(err);
    }
}

#ifdef _WIN32
inline std::wstring wide(const std::string& utf8)
{
    std::wstring w(utf8.size() + 1, L'\0');
    if (!ecuio_utf8_to_wide(utf8.c_str(), &w[0], w.size())) {
        return std::wstring();
    }
    w.resize(wcslen(w.c_str()));
    return w;
}

inline std::string os_words(DWORD code)
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
        return "there's no such file";
    case ERROR_PATH_NOT_FOUND:
        return "the folder it goes in doesn't exist";
    case ERROR_ACCESS_DENIED:
        return "permission denied";
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return "it exists already";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return "the disk is full";
    case ERROR_SHARING_VIOLATION:
        return "another program has it open";
    default:
        return "Windows error " + std::to_string(code);
    }
}
#else
inline std::string os_words(int err)
{
    return errno_words(err);
}
#endif

/* A file opened to read ("rb"), or made to write ("wb"): a write never
 * opens a file that exists (O_EXCL), so a file of the name is never
 * truncated, nor one made meanwhile by another export; nullptr with *why
 * ("it exists already" for that) */
inline FILE* open_file(const std::string& utf8, bool write, std::string* why)
{
#ifdef _WIN32
    const std::wstring w = wide(utf8);
    if (w.empty()) {
        *why = "the name isn't valid UTF-8";
        return nullptr;
    }
    FILE* f = nullptr;
    if (write) {
        int fd = -1;
        if (_wsopen_s(&fd, w.c_str(),
                      _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYWR,
                      _S_IREAD | _S_IWRITE) == 0) {
            f = _fdopen(fd, "wb");
            if (f == nullptr) {
                _close(fd);
            }
        }
    } else {
        f = _wfsopen(w.c_str(), L"rb", _SH_DENYNO);
    }
#else
    FILE* f = nullptr;
    if (write) {
        const int fd =
            open(utf8.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) {
            f = fdopen(fd, "wb");
            if (f == nullptr) {
                close(fd);
            }
        }
    } else {
        f = std::fopen(utf8.c_str(), "rb");
    }
#endif
    if (f == nullptr) {
        const int err = errno;
        *why = !write && err == ENOENT ? std::string("there's no such file")
                                       : errno_words(err);
    }
    return f;
}

inline bool exists(const std::string& utf8)
{
#ifdef _WIN32
    const std::wstring w = wide(utf8);
    return !w.empty() &&
           GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st{};
    return stat(utf8.c_str(), &st) == 0;
#endif
}

/* Makes the folder; false with *why when it can't, "it exists already"
 * when the name is taken */
inline bool make_folder(const std::string& utf8, std::string* why)
{
#ifdef _WIN32
    const std::wstring w = wide(utf8);
    if (w.empty()) {
        *why = "the name isn't valid UTF-8";
        return false;
    }
    if (CreateDirectoryW(w.c_str(), nullptr)) {
        return true;
    }
    *why = os_words(GetLastError());
    return false;
#else
    if (mkdir(utf8.c_str(), 0777) == 0) {
        return true;
    }
    *why = os_words(errno);
    return false;
#endif
}

/* A name of this call's own beside out, for what is written whole there
 * and then renamed into place: <out>.partial-<pid>-<n>, n counted across
 * the process. Another export's (one under way, or one a crash left) is
 * then never in the way, never truncated and never removed. */
inline std::string partial_name(const std::string& out)
{
    static std::atomic<unsigned> next{0};
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return out + ".partial-" + std::to_string(pid) + "-" +
           std::to_string(next.fetch_add(1));
}

/* How many names partial_file and partial_folder try while each is taken */
constexpr int kPartialTries = 16;

/* A file made under a name of partial_name's in *name; nullptr with *why */
inline FILE* partial_file(const std::string& out, std::string* name,
                          std::string* why)
{
    for (int i = 0; i < kPartialTries; ++i) {
        *name = partial_name(out);
        FILE* f = open_file(*name, true, why);
        if (f != nullptr || !exists(*name)) {
            return f;
        }
    }
    return nullptr;
}

/* A folder made under a name of partial_name's in *name; false with *why */
inline bool partial_folder(const std::string& out, std::string* name,
                           std::string* why)
{
    for (int i = 0; i < kPartialTries; ++i) {
        *name = partial_name(out);
        if (make_folder(*name, why)) {
            return true;
        }
        if (!exists(*name)) {
            return false;
        }
    }
    return false;
}

#ifndef _WIN32
/* Which of rename_to's ways without replacing a test starts from: 0 the
 * system's own (renameat2 or renamex_np), 1 a file's link or a folder's
 * reservation, 2 the check before rename */
inline std::atomic<int> rename_test_from{0};

/* An errno that says the file system can't do what was asked, rather than
 * that the name is taken */
inline bool not_supported(int err)
{
    return err == EINVAL || err == ENOSYS || err == EPERM ||
           err == EOPNOTSUPP || err == ENOTSUP || err == EXDEV || err == EMLINK;
}

/* The system's rename that refuses a taken name: 0, an errno, or -1 when
 * it isn't there or the file system can't */
inline int rename_exclusive(const char* from, const char* to)
{
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned kNoReplace = 1u; /* RENAME_NOREPLACE */
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, kNoReplace) == 0) {
        return 0;
    }
    return not_supported(errno) ? -1 : errno;
#elif defined(__APPLE__)
    if (renamex_np(from, to, RENAME_EXCL) == 0) {
        return 0;
    }
    return not_supported(errno) ? -1 : errno;
#else
    (void)from;
    (void)to;
    return -1;
#endif
}

/* A file renamed by a second name: link refuses a taken one */
inline int rename_by_link(const char* from, const char* to)
{
    if (link(from, to) != 0) {
        return not_supported(errno) ? -1 : errno;
    }
    if (unlink(from) != 0) {
        const int err = errno;
        unlink(to);
        return err;
    }
    return 0;
}

/* A folder renamed onto a reservation: mkdir refuses a taken name, and
 * rename refuses the reservation once anything is in it */
inline int rename_onto_reservation(const char* from, const char* to)
{
    if (mkdir(to, 0700) != 0) {
        return not_supported(errno) ? -1 : errno;
    }
    if (std::rename(from, to) != 0) {
        const int err = errno == ENOTEMPTY ? EEXIST : errno;
        rmdir(to); /* only while it is still empty */
        return err;
    }
    return 0;
}
#endif

/* Renames from to to; a file to replaces one there with replace, a
 * folder never does. Without replace, a name taken meanwhile is refused
 * wherever the file system can say so; where it can't, a name taken
 * before the rename starts is refused. False with *why. */
inline bool rename_to(const std::string& from, const std::string& to,
                      bool replace, std::string* why)
{
#ifdef _WIN32
    const std::wstring a = wide(from);
    const std::wstring b = wide(to);
    if (a.empty() || b.empty()) {
        *why = "the name isn't valid UTF-8";
        return false;
    }
    if (MoveFileExW(a.c_str(), b.c_str(),
                    replace ? MOVEFILE_REPLACE_EXISTING : 0)) {
        return true;
    }
    *why = os_words(GetLastError());
    return false;
#else
    if (!replace) {
        const int start = rename_test_from.load();
        int got = start <= 0 ? rename_exclusive(from.c_str(), to.c_str()) : -1;
        if (got < 0 && start <= 1) {
            struct stat st{};
            const bool folder =
                stat(from.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            got = folder ? rename_onto_reservation(from.c_str(), to.c_str())
                         : rename_by_link(from.c_str(), to.c_str());
        }
        if (got == 0) {
            return true;
        }
        if (got > 0) {
            *why = os_words(got);
            return false;
        }
        if (exists(to)) {
            *why = os_words(EEXIST);
            return false;
        }
    }
    if (std::rename(from.c_str(), to.c_str()) == 0) {
        return true;
    }
    *why = os_words(errno);
    return false;
#endif
}

inline void remove_file(const std::string& utf8)
{
#ifdef _WIN32
    const std::wstring w = wide(utf8);
    if (!w.empty()) {
        DeleteFileW(w.c_str());
    }
#else
    unlink(utf8.c_str());
#endif
}

inline void remove_folder(const std::string& utf8)
{
#ifdef _WIN32
    const std::wstring w = wide(utf8);
    if (!w.empty()) {
        RemoveDirectoryW(w.c_str());
    }
#else
    rmdir(utf8.c_str());
#endif
}

#ifdef _WIN32
struct find_closer {
    void operator()(void* find) const { FindClose(find); }
};
#else
struct dir_closer {
    void operator()(DIR* d) const { closedir(d); }
};
#endif

/* The names a folder holds, files and folders alike, "." and ".." left
 * out, in the order the system gives; false when it can't be read. The
 * search is closed on every way out, a bad_alloc from a name's copy
 * among them. */
inline bool list_folder(const std::string& utf8,
                        std::vector<std::string>* names)
{
    names->clear();
#ifdef _WIN32
    const std::wstring w = wide(utf8 + "\\*");
    if (w.empty()) {
        return false;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE first = FindFirstFileW(w.c_str(), &fd);
    if (first == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::unique_ptr<void, find_closer> find(first);
    do {
        char name[MAX_PATH * 4];
        if (std::wcscmp(fd.cFileName, L".") == 0 ||
            std::wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        names->push_back(ecuio_wide_to_utf8(fd.cFileName, name, sizeof name)
                             ? std::string(name)
                             : std::string("?"));
    } while (FindNextFileW(find.get(), &fd));
    return true;
#else
    const std::unique_ptr<DIR, dir_closer> d(opendir(utf8.c_str()));
    if (d == nullptr) {
        return false;
    }
    while (const dirent* e = readdir(d.get())) {
        if (std::strcmp(e->d_name, ".") != 0 &&
            std::strcmp(e->d_name, "..") != 0) {
            names->push_back(e->d_name);
        }
    }
    return true;
#endif
}

} // namespace ecuio_files

#endif /* ECUIO_FOLDER_FILES_H */
