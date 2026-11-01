// File/layer: core/src/surrogate_log_names.h | internal, not ABI-exported;
//             included by j2534_surrogate_client.cpp (its only consumer)
// C++17, Qt-free, no platform headers.
// Testing: UNIT: CoreTest.SurrogateLog* (test_core); the client's use of
//     it is HARNESS (J2534SurrogateTest.EarlierSessionLogIsKept)

#ifndef ECUIO_SURROGATE_LOG_NAMES_H
#define ECUIO_SURROGATE_LOG_NAMES_H

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <string>
#include <vector>

/* The surrogate's session logs in TEMP (j2534_surrogate_client.cpp):
 *   - "ecuio-surrogate.log", the current session's;
 *   - "ecuio-surrogate.YYYYMMDD-HHMMSS-mmm.log", an earlier session's,
 *     kept under its last write's local time, so the names sort by time;
 *   - "ecuio-surrogate-<pid>-<n>.log", a session's that found the fixed
 *     name held by another (sessions at once: a parallel test run, the
 *     app and havne-cli), whose names don't sort by time.
 * Each kind is kept to its newest SUR_LOG_KEEP. Names are checked
 * exactly, so pruning deletes nothing else. */
constexpr size_t SUR_LOG_KEEP = 50;

inline bool sur_log_is_kept_name(const wchar_t* name)
{
    static const wchar_t kShape[] = L"ecuio-surrogate.########-######-###.log";
    size_t i = 0;
    for (; kShape[i] != L'\0'; ++i) {
        const wchar_t c = name[i];
        if (kShape[i] == L'#' ? (c < L'0' || c > L'9') : (c != kShape[i])) {
            return false;
        }
    }
    return name[i] == L'\0';
}

/* "ecuio-surrogate-" then two runs of 1 to 10 digits split by '-', then
 * ".log": a process ID and the client's session count */
inline bool sur_log_is_per_spawn_name(const wchar_t* name)
{
    static const wchar_t kHead[] = L"ecuio-surrogate-";
    size_t i = 0;
    for (; kHead[i] != L'\0'; ++i) {
        if (name[i] != kHead[i]) {
            return false;
        }
    }
    for (int run = 0; run < 2; ++run) {
        size_t digits = 0;
        while (name[i] >= L'0' && name[i] <= L'9') {
            ++i;
            if (++digits > 10) {
                return false;
            }
        }
        if (digits == 0) {
            return false;
        }
        if (run == 0 && name[i++] != L'-') {
            return false;
        }
    }
    return std::wstring(name + i) == L".log";
}

struct SurLogFile {
    std::wstring name;
    uint64_t written; /* the last write, any monotonic unit (FILETIME) */
};

/* The logs past the newest `keep` to delete, oldest first: by last
 * write, a name breaking a tie */
inline std::vector<std::wstring> sur_log_excess(std::vector<SurLogFile> files,
                                                size_t keep)
{
    std::vector<std::wstring> out;
    if (files.size() <= keep) {
        return out;
    }
    std::sort(files.begin(), files.end(),
              [](const SurLogFile& a, const SurLogFile& b) {
                  return a.written != b.written ? a.written < b.written
                                                : a.name < b.name;
              });
    for (size_t i = 0; i + keep < files.size(); ++i) {
        out.push_back(files[i].name);
    }
    return out;
}

#endif
