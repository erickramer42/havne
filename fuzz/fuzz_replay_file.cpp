// File/layer: fuzz/fuzz_replay_file.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_replay_file

#include <ecuio/core.h>

#include "replay_faults.h" /* core/src: ?faults= and its draws, pure */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#ifdef _WIN32
#include <process.h> /* _getpid */
#define ecuio_fuzz_getpid _getpid
#else
#include <unistd.h>
#define ecuio_fuzz_getpid getpid
#endif

namespace {

/* The scratch file, one per process (libFuzzer's -jobs run several), in
 * TEMP / TMPDIR as the core tests' recordings are; removed at exit. */
std::string g_csv;

void remove_scratch()
{
    if (!g_csv.empty()) {
        remove(g_csv.c_str());
    }
}

const std::string& scratch()
{
    if (g_csv.empty()) {
        const char* tmp = getenv("TEMP");
        if (tmp == nullptr) tmp = getenv("TMPDIR");
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\') c = '/';
        }
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        g_csv = base + "/ecuio-fuzz-replay-" +
                std::to_string(static_cast<long>(ecuio_fuzz_getpid())) +
                ".csv";
        atexit(remove_scratch);
    }
    return g_csv;
}

struct created {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    uint32_t frames = 0;
    uint32_t span_ms = 0;
};

created create(const std::string& uri)
{
    created c;
    ecuio_session_t* s = nullptr;
    c.rc = ecuio_session_create(uri.c_str(), &s);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512) abort();
    c.detail = detail;
    if (c.rc == ECUIO_OK) {
        if (s == nullptr) abort();
        ecuio_session_info_t info{};
        if (ecuio_session_get_info(s, &info) != ECUIO_OK) abort();
        if (info.backend != ECUIO_BACKEND_REPLAY) abort();
        c.frames = info.replay_frames;
        c.span_ms = info.replay_span_ms;
        ecuio_session_destroy(s); /* never started: no worker */
    } else if (s != nullptr) {
        abort();
    }
    return c;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* Thousands of rows; a longer file only repeats them. */
    if (size > 262144) return 0;

    /* 'F', then the faults' text to the first newline (64 bytes at most) */
    std::string faults;
    const bool faulted = size > 0 && data[0] == 'F';
    if (faulted) {
        size_t n = 1;
        while (n < size && n <= 64 && data[n] != '\n')
            ++n;
        faults.assign(reinterpret_cast<const char*>(data) + 1, n - 1);
        if (faults.find('\0') != std::string::npos)
            return 0;
        const size_t skip = n < size ? n + 1 : n;
        data += skip;
        size -= skip;

        replay_faults parsed;
        if (replay_parse_faults(faults.data(), faults.size(), &parsed)) {
            if (!parsed.any)
                abort();
            uint32_t state = 1;
            for (uint32_t len = 0; len < 80; ++len) {
                const uint16_t data_len =
                    static_cast<uint16_t>(len % 9 == 8 ? 4096 : len);
                const replay_fault_verdict v =
                    replay_fault_draw(parsed, state, data_len);
                if (v.stall_ms > REPLAY_FAULT_STALL_MS_MAX)
                    abort();
                if (v.overflow && !v.drop)
                    abort();
                if (v.drop && (v.corrupt || v.truncate))
                    abort();
                if (v.corrupt && v.corrupt_bit >= data_len * 8u)
                    abort();
                if (v.truncate && v.truncate_to >= data_len)
                    abort();
                if (state == 0)
                    abort();
            }
        }
    }

    /* The header row, so the rows themselves are what's fuzzed; the
     * input is everything after it, NULs and all. */
    const std::string& path = scratch();
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) abort();
    static const char kHeader[] = "host_us,timestamp_us,id,flags,data\n";
    if (fwrite(kHeader, 1, sizeof(kHeader) - 1, f) != sizeof(kHeader) - 1 ||
        (size > 0 && fwrite(data, 1, size, f) != size)) {
        abort();
    }
    fclose(f);

    const std::string uri =
        "replay://" + path + (faulted ? "?faults=" + faults : std::string());
    const created a = create(uri);
    const created b = create(uri);
    if (a.rc != b.rc || a.detail != b.detail || a.frames != b.frames ||
        a.span_ms != b.span_ms) {
        abort();
    }
    /* The faults' text reaches the query as is, so an '&' in it adds
     * keys of its own: a profile it names may not exist */
    const bool query_refused =
        a.rc == ECUIO_ERR_BAD_URI || a.rc == ECUIO_ERR_UNKNOWN_PROFILE;
    if (a.rc != ECUIO_OK && a.rc != ECUIO_ERR_LOAD_FAILED &&
        !(faulted && query_refused)) {
        abort();
    }
    if (query_refused) {
        if (a.detail.empty() || a.detail.find('\n') != std::string::npos) {
            abort();
        }
        return 0;
    }
    if (a.rc == ECUIO_ERR_LOAD_FAILED) {
        if (a.detail.empty() || a.detail.find('\n') != std::string::npos ||
            a.detail.find(path) != std::string::npos) {
            abort();
        }
    } else if (a.frames == 0) {
        abort();
    }
    return 0;
}
