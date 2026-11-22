// File/layer: core/src/folder_files.h | internal, C++17, Qt-free; files and
//             folders by UTF-8 path, for the imports and exports (ADR-0054)
// Testing: UNIT: ImportTest, ExportTest, FolderExportTest, SecurityStoreTest
//          (test_core)
//
// A file written whole under a temporary name and renamed into place, a
// folder made only when the name is free, a lock file held across a
// store's save, each failure in words that never name the path. Windows
// takes UTF-16 paths (win_utf8.h).

#ifndef ECUIO_FOLDER_FILES_H
#define ECUIO_FOLDER_FILES_H

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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
#include <sys/file.h>
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

/* A file that is there, opened to read. nullptr with *none true when
 * there is no such file (its name, or a folder on the way to it, isn't
 * there), else with *none false and *why: it is there, or may be, and
 * can't be opened. Windows judges by its own code: the C library reads
 * an unreachable share as no such file too. */
inline FILE* open_existing(const std::string& utf8, bool* none,
                           std::string* why)
{
    *none = false;
#ifdef _WIN32
    const std::wstring w = wide(utf8);
    if (w.empty()) {
        *why = "the name isn't valid UTF-8";
        return nullptr;
    }
    FILE* f = _wfsopen(w.c_str(), L"rb", _SH_DENYNO);
    if (f == nullptr) {
        const int err = errno;
        unsigned long code = 0;
        _get_doserrno(&code);
        *none = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
        if (!*none) {
            *why = errno_words(err);
        }
    }
#else
    FILE* f = std::fopen(utf8.c_str(), "rb");
    if (f == nullptr) {
        const int err = errno;
        *none = err == ENOENT;
        if (!*none) {
            *why = errno_words(err);
        }
    }
#endif
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

/* A name of this call's own beside out for what is written whole and then
 * renamed into place: <out>.partial-<pid>-<n>, n counted across the
 * process, so another export's (under way or left by a crash) is never
 * in the way, truncated or removed. */
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

#ifdef _WIN32
/* How often replace_with tries a move Windows refuses because another
 * program has the file open (a scanner, a backup), and the wait between */
constexpr int kReplaceTries = 4;
constexpr DWORD kReplaceWaitMs = 10;

/* TEST-ONLY: called before each further try of replace_with's move */
inline std::atomic<void (*)()> replace_test_before_retry{nullptr};
#endif

/* Puts from where to is, replacing a file there: for a store core owns
 * and rewrites whole, the one file a rename may replace. The change is
 * asked through to the disk: Windows writes the move through, and
 * elsewhere the folder holding to is flushed after the rename (a file
 * system that can't flush a folder passes). A move Windows refuses for
 * a file in use is tried kReplaceTries times. False with *why; from is
 * left where it was unless the rename itself went through, which
 * *renamed (may be NULL) says, set before the words: once it has, only a
 * failure's words allocate. */
inline bool replace_with(const std::string& from, const std::string& to,
                         std::string* why, bool* renamed = nullptr)
{
    bool ignored = false;
    bool* moved = renamed != nullptr ? renamed : &ignored;
    *moved = false;
#ifdef _WIN32
    const std::wstring a = wide(from);
    const std::wstring b = wide(to);
    if (a.empty() || b.empty()) {
        *why = "the name isn't valid UTF-8";
        return false;
    }
    DWORD code = 0;
    for (int tries = 0; tries < kReplaceTries; ++tries) {
        if (tries != 0) {
            if (void (*hook)() = replace_test_before_retry.load()) {
                hook();
            }
            Sleep(kReplaceWaitMs);
        }
        if (MoveFileExW(a.c_str(), b.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            *moved = true;
            return true;
        }
        code = GetLastError();
        if (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED) {
            break;
        }
    }
    *why = os_words(code);
    return false;
#else
    const size_t slash = to.find_last_of('/');
    const std::string folder = slash == std::string::npos ? "."
                               : slash == 0               ? "/"
                                                          : to.substr(0, slash);
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        *why = os_words(errno);
        return false;
    }
    *moved = true;
    const int fd = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        *why = "its folder can't be flushed: " + os_words(errno);
        return false;
    }
    const bool flushed = fsync(fd) == 0 || not_supported(errno);
    const int err = errno;
    close(fd);
    if (!flushed) {
        *why = "its folder can't be flushed: " + os_words(err);
    }
    return flushed;
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

/* A lock on a file, held until release or the object's end */
class file_lock
{
public:
    file_lock() = default;
    ~file_lock() { release(); }
    file_lock(const file_lock&) = delete;
    file_lock& operator=(const file_lock&) = delete;

    void release()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            if (locked) {
                /* Explicitly: a close frees a lock only when the system
                 * gets to it */
                OVERLAPPED at{};
                UnlockFileEx(handle, 0, 1, 0, &at);
            }
            CloseHandle(handle);
        }
        handle = INVALID_HANDLE_VALUE;
#else
        if (fd >= 0) {
            close(fd); /* frees the flock */
        }
        fd = -1;
#endif
        locked = false;
    }

#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool locked = false;
};

enum class lock_got : uint8_t {
    locked,
    busy,   /* another holder kept it past the wait */
    failed, /* *why */
};

/* TEST-ONLY: called each time a try of lock_file finds the lock held */
inline std::atomic<void (*)()> lock_test_held{nullptr};

/* One try of lock_file's: locked, busy while another holds it, or failed
 * with *why */
inline lock_got lock_once(const std::string& utf8, file_lock* lock,
                          std::string* why)
{
#ifdef _WIN32
    if (lock->handle == INVALID_HANDLE_VALUE) {
        const std::wstring w = wide(utf8);
        if (w.empty()) {
            *why = "the name isn't valid UTF-8";
            return lock_got::failed;
        }
        lock->handle =
            CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock->handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_SHARING_VIOLATION) {
                return lock_got::busy;
            }
            *why = os_words(code);
            return lock_got::failed;
        }
    }
    OVERLAPPED at{};
    if (LockFileEx(lock->handle,
                   LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &at)) {
        lock->locked = true;
        return lock_got::locked;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_LOCK_VIOLATION) {
        return lock_got::busy;
    }
    *why = os_words(code);
    return lock_got::failed;
#else
    if (lock->fd < 0) {
        lock->fd = open(utf8.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (lock->fd < 0) {
            *why = os_words(errno);
            return lock_got::failed;
        }
    }
    int got = flock(lock->fd, LOCK_EX | LOCK_NB);
    while (got != 0 && errno == EINTR) {
        got = flock(lock->fd, LOCK_EX | LOCK_NB);
    }
    if (got == 0) {
        lock->locked = true;
        return lock_got::locked;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return lock_got::busy;
    }
    *why = os_words(errno);
    return lock_got::failed;
#endif
}

/* The lock on the file at utf8 into *lock, the file made when there is
 * none and left there (a lock file removed lets two holders each lock a
 * file of the name). Waits up to wait_ms while another handle holds it,
 * in this process or another: busy once that ran out, failed with *why
 * for any other refusal. Windows locks the file's first byte
 * (LockFileEx), elsewhere the whole file (flock); either ends with its
 * handle, so a crash leaves no lock held. */
inline lock_got lock_file(const std::string& utf8, int wait_ms, file_lock* lock,
                          std::string* why)
{
    lock->release();
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        const lock_got got = lock_once(utf8, lock, why);
        if (got != lock_got::busy) {
            if (got == lock_got::failed) {
                lock->release();
            }
            return got;
        }
        if (void (*hook)() = lock_test_held.load()) {
            hook();
        }
        if (std::chrono::steady_clock::now() >= until) {
            lock->release();
            return lock_got::busy;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
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
