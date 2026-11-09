// File/layer: fuzz/fuzz_export.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_export

// The exports and the health report over a recording folder
// (ADR-0054): fuzz_recording's input, 'F' or '3' then events.log,
// signals.csv, markers.csv and frames.csv cut at 0x1E bytes, written to
// a scratch folder, then exported as a folder, CSV and pcapng, scrubbed
// and raw, whole and over a range, and reported as JSON and text, whole
// and over a range. The CSV is also written long, as a wide grid of
// several row intervals in both unit systems and with picked columns,
// and each wide export's size is checked against ecuio_export_estimate's
// exact answer. A baseline recording is read for the health report.

#include <ecuio/core.h>
#include <ecuio/recording.h>
#include <ecuio/signals.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h> /* _getpid */
#define ecuio_fuzz_getpid _getpid
#else
#include <unistd.h>
#define ecuio_fuzz_getpid getpid
#endif

namespace fs = std::filesystem;

namespace {

/* The scratch folder, one per process (libFuzzer's -jobs run several):
 * the recording in rec/, each output beside it */
std::string g_folder;

void remove_folder()
{
    std::error_code ec;
    fs::remove_all(fs::u8path(g_folder), ec);
}

const std::string& folder()
{
    if (g_folder.empty()) {
        const char* tmp = getenv("TEMP");
        if (tmp == nullptr) {
            tmp = getenv("TMPDIR");
        }
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\') {
                c = '/';
            }
        }
        while (base.size() > 1 && base.back() == '/') {
            base.pop_back();
        }
        g_folder = base + "/ecuio-fuzz-export-" +
                   std::to_string(static_cast<long>(ecuio_fuzz_getpid()));
        std::error_code ec;
        fs::create_directories(fs::u8path(g_folder + "/rec"), ec);
        atexit(remove_folder);
    }
    return g_folder;
}

void write_file(const char* name, const uint8_t* p, size_t n, bool present)
{
    const std::string path = folder() + "/rec/" + name;
    if (!present) {
        remove(path.c_str());
        return;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        abort();
    }
    if (n > 0 && fwrite(p, 1, n, f) != n) {
        abort();
    }
    fclose(f);
}

/* A file's bytes, or a folder's files by name with their bytes */
std::string read_tree(const fs::path& p)
{
    std::error_code ec;
    std::string all;
    if (fs::is_directory(p, ec)) {
        for (const auto& e : fs::directory_iterator(p, ec)) {
            all += e.path().filename().u8string() + "\n" + read_tree(e.path()) +
                   "\x1f";
        }
        return all;
    }
    FILE* f = fopen(p.u8string().c_str(), "rb");
    if (f == nullptr) {
        abort();
    }
    char buf[65536];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        all.append(buf, n);
    }
    fclose(f);
    return all;
}

/* Whether anything in the scratch folder is named <name>.partial-...: a
 * call's own file left behind (core's partial_name) */
bool partial_left(const std::string& name)
{
    const std::string prefix = name + ".partial-";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(g_folder), ec)) {
        if (e.path().filename().u8string().rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

void remove_out(const std::string& p)
{
    std::error_code ec;
    fs::remove_all(fs::u8path(p), ec);
    const std::string prefix =
        fs::u8path(p).filename().u8string() + ".partial-";
    std::vector<fs::path> leftovers;
    for (const auto& e : fs::directory_iterator(fs::u8path(g_folder), ec)) {
        if (e.path().filename().u8string().rfind(prefix, 0) == 0) {
            leftovers.push_back(e.path());
        }
    }
    for (const fs::path& l : leftovers) {
        fs::remove_all(l, ec);
    }
}

/* A refusal or a check's failure: LOAD_FAILED, one line that never
 * names the scratch folder; success writes the output and nothing else */
void judge(ecuio_status_t rc, uint32_t failed_class, const std::string& out,
           bool may_be_invalid = false)
{
    if (rc != ECUIO_OK && rc != ECUIO_ERR_LOAD_FAILED &&
        !(may_be_invalid && rc == ECUIO_ERR_INVALID_ARGUMENT)) {
        abort();
    }
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-export") != nullptr) {
        abort();
    }
    if (rc == ECUIO_OK && failed_class != 0) {
        abort();
    }
    std::error_code ec;
    if (partial_left(fs::u8path(out).filename().u8string())) {
        abort(); /* a partial copy left behind */
    }
    if ((rc == ECUIO_OK) != fs::exists(fs::u8path(out), ec)) {
        abort();
    }
}

struct result {
    ecuio_status_t rc = ECUIO_OK;
    std::string bytes;
};

/* One export; a wide CSV's size and rows are then checked against the
 * estimate's, which promises the file's exact size */
result export_with(ecuio_export_options_t o, bool may_be_invalid = false)
{
    const std::string out = folder() + "/out";
    const std::string rec = folder() + "/rec";
    remove_out(out);
    o.folder_utf8 = rec.c_str();
    o.out_path_utf8 = out.c_str();
    ecuio_export_report_t report{};
    result r;
    r.rc = ecuio_export(&o, &report);
    judge(r.rc, report.failed_class, out, may_be_invalid);
    if (r.rc == ECUIO_OK) {
        r.bytes = read_tree(fs::u8path(out));
        if (o.format == ECUIO_EXPORT_CSV && o.layout == ECUIO_CSV_WIDE) {
            ecuio_export_estimate_t est{};
            const uint32_t ms = o.row_ms;
            if (ecuio_export_estimate(&o, &ms, 1, &est) != ECUIO_OK ||
                est.bytes != r.bytes.size() || est.rows != report.rows ||
                est.columns != report.signal_count) {
                abort();
            }
        }
    }
    remove_out(out);
    return r;
}

result export_once(uint32_t format, uint32_t flags, uint64_t from_us,
                   uint64_t to_us)
{
    ecuio_export_options_t o{};
    o.format = format;
    o.flags = flags;
    o.from_us = from_us;
    o.to_us = to_us;
    return export_with(o);
}

/* The same recording exported twice gives the same answer and bytes */
void same(const result& a, const result& b)
{
    if (a.rc != b.rc || a.bytes != b.bytes) {
        abort();
    }
}

/* The CSV's other shapes: long, and wide as a grid in each unit system
 * and over a range, and with columns named (the input's own signal
 * names, one the recording lacks, and whatever else the input makes) */
void csv_shapes(const uint8_t* signals, size_t signals_len, bool present)
{
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    for (const uint32_t flags : {0U, ECUIO_EXPORT_RAW}) {
        o.flags = flags;
        o.layout = ECUIO_CSV_LONG;
        o.units = 0;
        o.row_ms = 0;
        same(export_with(o), export_with(o));
        o.layout = ECUIO_CSV_WIDE;
        for (const uint32_t units :
             {ECUIO_UNITS_METRIC, ECUIO_UNITS_IMPERIAL}) {
            for (const uint32_t row_ms : {0U, 1U, 100U, 1000U}) {
                o.units = units;
                o.row_ms = row_ms;
                same(export_with(o), export_with(o));
            }
        }
    }
    o = ecuio_export_options_t{};
    o.format = ECUIO_EXPORT_CSV;
    o.row_ms = 100;
    o.from_us = 1000000;
    o.to_us = 2000000;
    export_with(o);

    /* signals.csv's signal column on its first lines, then made-up names */
    std::vector<std::string> names;
    if (present) {
        const std::string text(reinterpret_cast<const char*>(signals),
                               signals_len);
        size_t at = text.find('\n');
        while (at != std::string::npos && names.size() < 3) {
            const size_t a = text.find(',', at + 1);
            const size_t z = a == std::string::npos ? a : text.find(',', a + 1);
            if (z == std::string::npos) {
                break;
            }
            names.push_back(text.substr(a + 1, z - a - 1));
            at = text.find('\n', z);
        }
    }
    names.push_back("Speed");
    names.push_back("Not in the recording");
    std::vector<const char*> picked;
    for (const std::string& n : names) {
        picked.push_back(n.c_str());
    }
    o.column_count = static_cast<uint32_t>(picked.size());
    o.columns_utf8 = picked.data();
    for (const uint32_t row_ms : {0U, 100U}) {
        o.row_ms = row_ms;
        same(export_with(o, true), export_with(o, true));
    }
}

result health_once(uint32_t format, uint64_t from_us, uint64_t to_us,
                   bool with_baseline = false)
{
    const std::string out = folder() + "/health";
    const std::string rec = folder() + "/rec";
    const std::string base = folder() + "/base";
    remove_out(out);
    ecuio_health_options_t o{};
    o.format = format;
    o.units = ECUIO_UNITS_METRIC;
    o.latest_model_year = 2027;
    o.from_us = from_us;
    o.to_us = to_us;
    o.utc_us = 1790000000000000ULL;
    o.folder_utf8 = rec.c_str();
    o.out_path_utf8 = out.c_str();
    if (with_baseline) {
        o.baseline_folder_utf8 = base.c_str();
    }
    ecuio_health_report_t report{};
    result r;
    r.rc = ecuio_health_export(&o, &report);
    judge(r.rc, report.failed_class, out);
    if (r.rc == ECUIO_OK) {
        r.bytes = read_tree(fs::u8path(out));
    }
    remove_out(out);
    return r;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2 || size > 65536 || (data[0] != 'F' && data[0] != '3')) {
        return 0;
    }
    const uint8_t* parts[4] = {};
    size_t lens[4] = {};
    size_t count = 0;
    size_t start = 1;
    for (size_t i = 1; i <= size && count < 4; ++i) {
        if (i == size || data[i] == 0x1E) {
            parts[count] = data + start;
            lens[count] = i - start;
            ++count;
            start = i + 1;
        }
    }
    std::string events;
    if (data[0] == '3' && count > 0) {
        events = std::string("ecuio session recording, format 3\n") +
                 std::string(reinterpret_cast<const char*>(parts[0]), lens[0]);
        parts[0] = reinterpret_cast<const uint8_t*>(events.data());
        lens[0] = events.size();
    }
    write_file("events.log", parts[0], lens[0], count > 0);
    write_file("signals.csv", parts[1], lens[1], count > 1);
    write_file("markers.csv", parts[2], lens[2], count > 2);
    write_file("frames.csv", parts[3], lens[3], count > 3);
    for (const uint32_t format :
         {ECUIO_EXPORT_FOLDER, ECUIO_EXPORT_CSV, ECUIO_EXPORT_PCAPNG}) {
        for (const uint32_t flags : {0U, ECUIO_EXPORT_RAW}) {
            same(export_once(format, flags, 0, 0),
                 export_once(format, flags, 0, 0));
            export_once(format, flags, 1000000, 2000000);
        }
    }
    csv_shapes(parts[1], lens[1], count > 1);
    for (const uint32_t format : {ECUIO_HEALTH_JSON, ECUIO_HEALTH_TEXT}) {
        same(health_once(format, 0, 0), health_once(format, 0, 0));
        health_once(format, 500000, 3000000);
    }
    /* A baseline: the input's events.log, whole or from its middle, as
     * a second recording's */
    {
        std::error_code ec;
        fs::create_directories(fs::u8path(folder() + "/base"), ec);
        const std::string path = folder() + "/base/events.log";
        const size_t from = lens[0] / 2 * (data[size - 1] & 1);
        FILE* f = fopen(path.c_str(), "wb");
        if (f == nullptr) {
            abort();
        }
        if (count > 0 && lens[0] > from &&
            fwrite(parts[0] + from, 1, lens[0] - from, f) != lens[0] - from) {
            abort();
        }
        fclose(f);
        for (const uint32_t format : {ECUIO_HEALTH_JSON, ECUIO_HEALTH_TEXT}) {
            same(health_once(format, 0, 0, true),
                 health_once(format, 0, 0, true));
        }
    }
    return 0;
}
