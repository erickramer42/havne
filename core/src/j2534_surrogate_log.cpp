// File/layer: core/src/j2534_surrogate_log.cpp | C++17, Qt-free,
//             Win32-guarded
// Testing: HARNESS: J2534SurrogateTest.EarlierSessionLogIsKept,
//          .ClientAndChildLinesShareTheSessionLog; J2534ConnectTest (the
//          prune out of memory); Cross-arch
//
// The surrogate's session log (surrogate.md): the file each spawn opens
// for the child's output and the client's lines, the earlier logs it keeps
// and the ones it prunes. The client writes its own lines (sur_log).

#include "j2534_surrogate_log.h"

#ifdef _WIN32

#include <cwchar>
#include <new>

#include "surrogate_log_names.h"
#include "win_utf8.h"

namespace ecuio_sur_log {

namespace {

/* Deletes the oldest kept logs past SUR_LOG_KEEP (a suite run spawns dozens
 * of sessions; this bounds TEMP). The stamp sorts by time. */
void prune_kept_logs(const wchar_t* prefix)
{
    wchar_t pattern[MAX_PATH + 64];
    swprintf(pattern, _countof(pattern), L"%lsecuio-surrogate.*.log", prefix);
    for (;;) {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(pattern, &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        int count = 0;
        wchar_t oldest[MAX_PATH] = {};
        do {
            if (!sur_log_is_kept_name(fd.cFileName)) {
                continue;
            }
            ++count;
            if (oldest[0] == L'\0' || wcscmp(fd.cFileName, oldest) < 0) {
                wcscpy_s(oldest, fd.cFileName);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        if (count <= static_cast<int>(SUR_LOG_KEEP)) {
            return;
        }
        wchar_t victim[MAX_PATH + 64];
        swprintf(victim, _countof(victim), L"%ls%ls", prefix, oldest);
        if (!DeleteFileW(victim)) return;   /* held open: try again next spawn */
    }
}

/* Deletes the oldest per-spawn logs past SUR_LOG_KEEP, by last write (their
 * names don't sort by time). One still held is skipped; a later spawn takes
 * it. False, with nothing deleted, when the names don't fit in memory. */
bool prune_per_spawn_logs(const wchar_t* prefix)
{
    wchar_t pattern[MAX_PATH + 64];
    swprintf(pattern, _countof(pattern), L"%lsecuio-surrogate-*.log", prefix);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return true;
    }
    std::vector<std::wstring> excess;
    try {
        std::vector<SurLogFile> logs;
        do {
            if (!sur_log_is_per_spawn_name(fd.cFileName)) {
                continue;
            }
            const uint64_t written =
                (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime)
                 << 32) |
                fd.ftLastWriteTime.dwLowDateTime;
            logs.push_back(SurLogFile{fd.cFileName, written});
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        find = INVALID_HANDLE_VALUE;
        excess = sur_log_excess(std::move(logs), SUR_LOG_KEEP);
    } catch (const std::bad_alloc&) {
        if (find != INVALID_HANDLE_VALUE) {
            FindClose(find);
        }
        return false;
    }
    for (const std::wstring& name : excess) {
        wchar_t victim[MAX_PATH + 64];
        swprintf(victim, _countof(victim), L"%ls%ls", prefix, name.c_str());
        (void)DeleteFileW(victim);
    }
    return true;
}

/* Keeps the previous session's log: the file at the fixed name is renamed
 * after its last write. One another session holds can't be renamed; the
 * open that follows then fails too and takes a per-spawn name. */
void keep_previous_log(const wchar_t* prefix)
{
    wchar_t fixed[MAX_PATH + 64];
    swprintf(fixed, _countof(fixed), L"%lsecuio-surrogate.log", prefix);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    SYSTEMTIME utc, t;
    if (!GetFileAttributesExW(fixed, GetFileExInfoStandard, &fa) ||
        !FileTimeToSystemTime(&fa.ftLastWriteTime, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &t)) {
        return;
    }
    wchar_t kept[MAX_PATH + 64];
    swprintf(kept, _countof(kept),
             L"%lsecuio-surrogate.%04u%02u%02u-%02u%02u%02u-%03u.log", prefix,
             static_cast<unsigned>(t.wYear), static_cast<unsigned>(t.wMonth),
             static_cast<unsigned>(t.wDay), static_cast<unsigned>(t.wHour),
             static_cast<unsigned>(t.wMinute), static_cast<unsigned>(t.wSecond),
             static_cast<unsigned>(t.wMilliseconds));
    if (MoveFileExW(fixed, kept, 0)) {
        prune_kept_logs(prefix);
    }
}

} // namespace

/* The session log's path. The fixed name is the documented one for a single
 * session (the log it replaces is kept); a concurrent session finds it held
 * and gets a per-spawn name, so no child runs without a log. path receives
 * UTF-8 (TEMP can hold a non-ASCII name). Returns INVALID_HANDLE_VALUE, with
 * path empty, if neither name opens, or with *out_of_memory set and nothing
 * opened when the prune ran out of memory. */
HANDLE open_session_log(LONG seq, SECURITY_ATTRIBUTES* sa, char* path,
                        size_t cap, bool* out_of_memory)
{
    wchar_t dir[MAX_PATH] = {};
    const DWORD tn = GetTempPathW(MAX_PATH, dir);
    /* No usable TEMP: a dotfile in the current directory. */
    const wchar_t* prefix = (tn == 0 || tn >= MAX_PATH) ? L"." : dir;

    keep_previous_log(prefix);
    *out_of_memory = !prune_per_spawn_logs(prefix);
    if (*out_of_memory) {
        path[0] = '\0';
        return INVALID_HANDLE_VALUE;
    }
    wchar_t wpath[MAX_PATH + 64];
    swprintf(wpath, _countof(wpath), L"%lsecuio-surrogate.log", prefix);
    HANDLE f = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        swprintf(wpath, _countof(wpath), L"%lsecuio-surrogate-%lu-%ld.log",
                 prefix, GetCurrentProcessId(), seq);
        f = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (f == INVALID_HANDLE_VALUE || !ecuio_wide_to_utf8(wpath, path, cap)) {
        path[0] = '\0';
    }
    return f;
}

} // namespace ecuio_sur_log

#endif /* _WIN32 */
