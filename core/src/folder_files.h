// File/layer: core/src/folder_files.h | internal, C++17, Qt-free; files and
//             folders by UTF-8 path, for the imports and exports (ADR-0054)
// Testing: UNIT: ImportTest, ExportTest, FolderExportTest (test_core)
//
// A file written whole under a temporary name and renamed into place, a
// folder made only when the name is free, each failure in words that
// never name the path. Windows takes UTF-16 paths (win_utf8.h).

#ifndef ECUIO_FOLDER_FILES_H
#define ECUIO_FOLDER_FILES_H

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include "win_utf8.h"
#include <share.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
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

/* A file opened to read ("rb") or write ("wb"); nullptr with *why */
inline FILE* open_file(const std::string& utf8, bool write, std::string* why)
{
#ifdef _WIN32
    const std::wstring w = wide(utf8);
    if (w.empty()) {
        *why = "the name isn't valid UTF-8";
        return nullptr;
    }
    FILE* f = _wfsopen(w.c_str(), write ? L"wb" : L"rb",
                       write ? _SH_DENYWR : _SH_DENYNO);
#else
    FILE* f = std::fopen(utf8.c_str(), write ? "wb" : "rb");
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

/* Renames from to to; a file to replaces one there with replace, a
 * folder never does. False with *why. */
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
    if (!replace && exists(to)) {
        *why = os_words(EEXIST);
        return false;
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

/* The names a folder holds, files and folders alike, "." and ".." left
 * out, in the order the system gives; false when it can't be read */
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
    HANDLE find = FindFirstFileW(w.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        char name[MAX_PATH * 4];
        if (std::wcscmp(fd.cFileName, L".") == 0 ||
            std::wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        names->push_back(ecuio_wide_to_utf8(fd.cFileName, name, sizeof name)
                             ? std::string(name)
                             : std::string("?"));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return true;
#else
    DIR* d = opendir(utf8.c_str());
    if (d == nullptr) {
        return false;
    }
    while (const dirent* e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") != 0 &&
            std::strcmp(e->d_name, "..") != 0) {
            names->push_back(e->d_name);
        }
    }
    closedir(d);
    return true;
#endif
}

} // namespace ecuio_files

#endif /* ECUIO_FOLDER_FILES_H */
