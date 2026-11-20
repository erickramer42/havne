// File/layer: fuzz/fuzz_recording.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (rows, a folder, the stretches' rules)

#include "fuzz_recording_common.h"
#include "fuzz_recording_lines.h"
#include "fuzz_recording_conditions.h"
#include "fuzz_recording_query.h"
#include "fuzz_recording_reads.h"
#include "fuzz_recording_stretches.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 262144) {
        return 0;
    }
    if (data[0] == 'S') {
        stretches(data + 1, size - 1);
        return 0;
    }
    /* 'F' a folder; '3' or '4' a folder whose events.log starts with
     * that format's first line, so its header's label lines (and format
     * 4's keyed signal lines) are reached */
    const bool format3 = data[0] == '3' || data[0] == '4';
    if (data[0] != 'F' && !format3) {
        each_line(data + 1, size - 1);
        return 0;
    }
    /* events.log, signals.csv, markers.csv, frames.csv, conditions.json:
     * absent past the cuts given */
    const uint8_t* parts[5] = {};
    size_t lens[5] = {};
    size_t count = 0;
    size_t start = 1;
    for (size_t i = 1; i <= size && count < 5; ++i) {
        if (i == size || data[i] == 0x1E) {
            parts[count] = data + start;
            lens[count] = i - start;
            ++count;
            start = i + 1;
        }
    }
    std::string events;
    if (format3 && count > 0) {
        events = std::string(data[0] == '4' ? rows::kFormat4 : rows::kFormat3) +
                 "\n" +
                 std::string(reinterpret_cast<const char*>(parts[0]), lens[0]);
        parts[0] = reinterpret_cast<const uint8_t*>(events.data());
        lens[0] = events.size();
    }
    write_file("events.log", parts[0], lens[0], count > 0);
    write_file("signals.csv", parts[1], lens[1], count > 1);
    write_file("markers.csv", parts[2], lens[2], count > 2);
    write_file("frames.csv", parts[3], lens[3], count > 3);
    write_file("conditions.json", parts[4], lens[4], count > 4);
    g_want = want_conditions(reinterpret_cast<const char*>(parts[0]), lens[0],
                             count > 0,
                             reinterpret_cast<const char*>(parts[4]), lens[4],
                             count > 4);
    /* signals.csv, then the frames through obd2 (packed replies split),
     * the mock's set (a computed signal) and a float, each twice alike;
     * every open that succeeds reads the same conditions from events.log */
    const ecuio_signal_set_t* sets[] = {nullptr, ecuio_obd2_signal_set(),
                                        ecuio_mock_signal_set(), float_set()};
    opened first_ok;
    bool any_ok = false;
    for (const ecuio_signal_set_t* set : sets) {
        const opened a = open_and_query(set);
        const opened b = open_and_query(set);
        if (a.rc != b.rc || a.detail != b.detail ||
            memcmp(&a.info, &b.info, sizeof a.info) != 0 ||
            !same_conditions(a, b)) {
            abort();
        }
        if (a.rc != ECUIO_OK) {
            continue;
        }
        if (any_ok &&
            (!same_conditions(a, first_ok) ||
             a.info.condition_events != first_ok.info.condition_events)) {
            abort();
        }
        if (!any_ok) {
            first_ok = a;
            any_ok = true;
        }
    }
    const checked x = check_folder(0);
    const checked y = check_folder(0);
    if (x.rc != y.rc || x.detail != y.detail ||
        memcmp(&x.summary, &y.summary, sizeof x.summary) != 0) {
        abort();
    }
    if (x.rc == ECUIO_OK) {
        if (x.last_end > x.summary.duration_us) {
            abort();
        }
        if (x.seen > 0) {
            const checked z = check_folder(1);
            if (z.rc != ECUIO_ERR_CANCELED || z.seen != 1) {
                abort();
            }
        }
    }
    read_frames_back(data, size);
    return 0;
}
