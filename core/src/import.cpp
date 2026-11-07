// File/layer: core/src/import.cpp | C++17, Qt-free; a log from another tool
//             as a recording (ADR-0054 section 4)
// Testing: UNIT: ImportTest (test_core)
//
// ecuio_import: a candump log read twice, first to check every line and
// list the interfaces (so a refusal comes before anything is written),
// then to write the chosen interface's frames as a recording folder:
// frames.csv as they arrived, and an events.log header naming the import.
// The folder is written as <folder>.partial and renamed once whole.

#include "candump.h"
#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"     /* kHeader */
#include "recording_rows.h" /* kFormat2 */

#include "ecuio/core.h"
#include "ecuio/recording.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

namespace dump = ecuio_candump;
namespace files = ecuio_files;

/* A frame stamped this much before the one before it takes that one's
 * time: candump stamps each frame on the CPU that took it, so two CPUs'
 * stamps interleave slightly out of order (the SocketCAN transport's
 * SOCKETCAN_CLOCK_REORDER_US rule) */
constexpr uint64_t kReorderUs = 100000;
/* A log's bounds: its frames, and the interfaces it names */
constexpr uint64_t kFramesMax = 100000000;
constexpr size_t kInterfacesMax = 64;
constexpr size_t kWriteChunk = 1u << 16;

/* Lines of a file, each without its line end; a line past the format's
 * longest is read to its end and flagged */
class lines_of
{
public:
    explicit lines_of(FILE* f) : f_(f) {}

    bool next(std::string& line, bool* too_long)
    {
        line.clear();
        *too_long = false;
        int c = 0;
        bool any = false;
        while ((c = std::fgetc(f_)) != EOF) {
            any = true;
            if (c == '\n') {
                break;
            }
            if (line.size() < dump::kLineMax + 1) {
                line.push_back(static_cast<char>(c));
            } else {
                *too_long = true;
            }
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return any;
    }

private:
    FILE* f_;
};

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};

/* "a", "a and b", "a, b and c" */
std::string joined(const std::vector<std::string>& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        out += i == 0 ? "" : i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

/* Days since 1970-01-01 to a civil date (Howard Hinnant's
 * days_from_civil, inverted), so no gmtime, which isn't thread-safe */
void civil(int64_t days, int* y, unsigned* m, unsigned* d)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (*m <= 2));
}

/* A UTC time: "yyyy-MM-ddTHH:mm:ss.zzz" (date_t 'T') or with a space */
std::string utc_text(uint64_t us, char between)
{
    const uint64_t s = us / 1000000;
    int y = 0;
    unsigned mo = 0;
    unsigned d = 0;
    civil(static_cast<int64_t>(s / 86400), &y, &mo, &d);
    const uint64_t in_day = s % 86400;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u%c%02u:%02u:%02u.%03u", y, mo,
                  d, between, static_cast<unsigned>(in_day / 3600),
                  static_cast<unsigned>(in_day / 60 % 60),
                  static_cast<unsigned>(in_day % 60),
                  static_cast<unsigned>(us / 1000 % 1000));
    return buf;
}

/* "HH:MM:SS.mmm  +S.sss", a timed events.log line's start */
std::string line_stamp(uint64_t utc_us, uint64_t since_us)
{
    const std::string clock = utc_text(utc_us, ' ').substr(11);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s  +%llu.%03llu", clock.c_str(),
                  static_cast<unsigned long long>(since_us / 1000000),
                  static_cast<unsigned long long>(since_us / 1000 % 1000));
    return buf;
}

void append_u64(std::string& out, uint64_t v)
{
    char buf[24];
    const int n = std::snprintf(buf, sizeof buf, "%llu",
                                static_cast<unsigned long long>(v));
    out.append(buf, static_cast<size_t>(n));
}

void append_hex(std::string& out, uint32_t v)
{
    char buf[16];
    const int n = std::snprintf(buf, sizeof buf, "0x%X", v);
    out.append(buf, static_cast<size_t>(n));
}

/* What the first pass found */
struct survey {
    uint64_t lines = 0;
    std::vector<std::string> interfaces;
};

bool fail(const char* fmt, const std::string& a = std::string())
{
    ecuio_detail_set(fmt, a.c_str());
    return false;
}

/* Every line checked, the interfaces listed */
bool read_survey(FILE* f, survey& s)
{
    lines_of lines(f);
    std::string line;
    bool too_long = false;
    dump::line l;
    while (lines.next(line, &too_long)) {
        ++s.lines;
        if (line.empty() && !too_long) {
            continue; /* a blank line, as canplayer skips one */
        }
        const char* why = nullptr;
        if (too_long) {
            why = "the line is longer than a candump line can be";
        } else if (!dump::parse_line(line.data(), line.size(), l, &why)) {
            /* why is set */
        }
        if (why != nullptr) {
            ecuio_detail_set("line %llu: %s",
                             static_cast<unsigned long long>(s.lines), why);
            return false;
        }
        if (s.lines > kFramesMax) {
            return fail("the log holds more frames than an import takes");
        }
        const std::string name(l.iface, l.iface_len);
        if (std::find(s.interfaces.begin(), s.interfaces.end(), name) ==
            s.interfaces.end()) {
            if (s.interfaces.size() == kInterfacesMax) {
                return fail("the log names more interfaces than an import "
                            "reads");
            }
            s.interfaces.push_back(name);
        }
    }
    if (std::ferror(f) != 0) {
        return fail("the log can't be read");
    }
    return true;
}

/* The second pass: the interface's frames into frames.csv, in time
 * order; report's counts and times */
bool write_frames(FILE* in, FILE* out, const std::string& iface,
                  ecuio_import_report_t& r, std::string* why)
{
    lines_of lines(in);
    std::string line;
    bool too_long = false;
    dump::line l;
    std::string rows = std::string(ecuio_frames_csv::kHeader) + "\n";
    uint64_t line_no = 0;
    uint64_t first = 0;
    uint64_t prev = 0;
    bool any = false;
    while (lines.next(line, &too_long)) {
        ++line_no;
        if (line.empty() && !too_long) {
            continue;
        }
        const char* bad = nullptr;
        if (!dump::parse_line(line.data(), line.size(), l, &bad)) {
            *why = "the log changed while it was read";
            return false;
        }
        if (iface.compare(0, std::string::npos, l.iface, l.iface_len) != 0) {
            continue;
        }
        uint64_t t = l.seconds * 1000000 + l.microseconds;
        if (any && t < prev) {
            if (prev - t > kReorderUs) {
                char buf[96];
                std::snprintf(buf, sizeof buf,
                              "line %llu: its time is more than 0.1 s before "
                              "the frame before it",
                              static_cast<unsigned long long>(line_no));
                *why = buf;
                return false;
            }
            t = prev;
        }
        if (!any) {
            first = t;
            any = true;
        }
        prev = t;
        const uint64_t at = t - first;
        append_u64(rows, at);
        rows += ',';
        append_u64(rows, at);
        rows += ',';
        append_hex(rows, l.id);
        rows += ',';
        append_hex(rows, l.flags);
        rows += ',';
        if ((l.flags & ECUIO_FRAME_FLAG_REMOTE) == 0) {
            static const char kHex[] = "0123456789ABCDEF";
            for (uint8_t i = 0; i < l.length; ++i) {
                if (i > 0) {
                    rows += ' ';
                }
                rows += kHex[l.data[i] >> 4];
                rows += kHex[l.data[i] & 0x0F];
            }
        }
        rows += '\n';
        ++r.frames;
        if ((l.flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
            ++r.error_frames;
        }
        if (rows.size() >= kWriteChunk) {
            if (std::fwrite(rows.data(), 1, rows.size(), out) != rows.size()) {
                *why =
                    "frames.csv can't be written: " + files::errno_words(errno);
                return false;
            }
            rows.clear();
        }
    }
    if (!rows.empty() &&
        std::fwrite(rows.data(), 1, rows.size(), out) != rows.size()) {
        *why = "frames.csv can't be written: " + files::errno_words(errno);
        return false;
    }
    r.utc_start_us = first;
    r.duration_us = prev - first;
    return true;
}

/* The recording's events.log: format 2's header, the import as its
 * source, its end line */
std::string events_log(const std::string& iface, const ecuio_import_report_t& r)
{
    std::string e = std::string(ecuio_recording_rows::kFormat2) + "\n";
    e += "started  " + utc_text(r.utc_start_us, ' ') + " (UTC)\n";
    e += "utc  " + utc_text(r.utc_start_us, 'T') + "Z\n";
    e += std::string("core  ") + ecuio_version_string() + "\n";
    e += "import  candump log, interface " + iface + "\n";
    e += "profile  raw\n";
    e += "frame flags  complete\n";
    e += "columns  UTC time, seconds since the recording started (host_us / "
         "1e6 in the CSV files), kind, text\n";
    e += "\n";
    e += line_stamp(r.utc_start_us + r.duration_us, r.duration_us) +
         "  end  import: ";
    append_u64(e, r.frames);
    e += " frames, 0 values, 0 records dropped, 0 notes dropped\n";
    return e;
}

/* The import proper, its files under partial, which the caller removes
 * on failure */
bool import_candump(const ecuio_import_options_t& o, const std::string& out,
                    ecuio_import_report_t& r, bool* made)
{
    std::string why;
    std::unique_ptr<FILE, file_closer> in(
        files::open_file(o.in_path_utf8, false, &why));
    if (!in) {
        return fail("the log can't be opened: %s", why);
    }
    survey s;
    if (!read_survey(in.get(), s)) {
        return false;
    }
    r.lines = s.lines;
    r.interfaces = s.interfaces.size();
    std::string iface;
    if (o.interface_utf8 != nullptr) {
        iface = o.interface_utf8;
        if (std::find(s.interfaces.begin(), s.interfaces.end(), iface) ==
            s.interfaces.end()) {
            return fail("%s", s.interfaces.empty()
                                  ? std::string("the log holds no frames")
                                  : "the log has no interface '" + iface +
                                        "': it has " + joined(s.interfaces));
        }
    } else if (s.interfaces.empty()) {
        return fail("the log holds no frames");
    } else if (s.interfaces.size() > 1) {
        return fail("the log has several interfaces: name one of %s",
                    joined(s.interfaces));
    } else {
        iface = s.interfaces[0];
    }
    if (files::exists(out)) {
        return fail("the recording's folder exists already");
    }
    const std::string partial = out + ".partial";
    if (!files::make_folder(partial, &why)) {
        return fail("the recording's folder can't be made: %s", why);
    }
    *made = true;
    if (!ecuio_frames_csv::seek_to(in.get(), 0)) {
        return fail("the log can't be read again");
    }
    {
        std::unique_ptr<FILE, file_closer> frames(
            files::open_file(partial + "/frames.csv", true, &why));
        if (!frames) {
            return fail("frames.csv can't be written: %s", why);
        }
        if (!write_frames(in.get(), frames.get(), iface, r, &why)) {
            return fail("%s", why);
        }
        if (std::fclose(frames.release()) != 0) {
            return fail("frames.csv can't be written: %s",
                        files::errno_words(errno));
        }
    }
    const std::string log = events_log(iface, r);
    std::unique_ptr<FILE, file_closer> events(
        files::open_file(partial + "/events.log", true, &why));
    if (!events) {
        return fail("events.log can't be written: %s", why);
    }
    if (std::fwrite(log.data(), 1, log.size(), events.get()) != log.size() ||
        std::fclose(events.release()) != 0) {
        return fail("events.log can't be written: %s",
                    files::errno_words(errno));
    }
    if (!files::rename_to(partial, out, false, &why)) {
        return fail("the recording's folder can't be put in place: %s", why);
    }
    *made = false;
    return true;
}

} // namespace

extern "C" ecuio_status_t ecuio_import(const ecuio_import_options_t* options,
                                       ecuio_import_report_t* report)
{
    if (report != nullptr) {
        *report = ecuio_import_report_t{};
    }
    if (options == nullptr || options->in_path_utf8 == nullptr ||
        options->out_folder_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    for (uint64_t v : options->reserved) {
        if (v != 0) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    if (options->flags != 0 || (options->format != ECUIO_IMPORT_CANDUMP &&
                                options->format != ECUIO_IMPORT_CSV)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (options->format == ECUIO_IMPORT_CSV) {
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    if (options->column_map_utf8 != nullptr) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_detail_clear();
    std::string out = options->out_folder_utf8;
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    ecuio_import_report_t r{};
    bool made = false;
    try {
        const bool ok = import_candump(*options, out, r, &made);
        if (made) {
            const std::string partial = out + ".partial";
            files::remove_file(partial + "/frames.csv");
            files::remove_file(partial + "/events.log");
            files::remove_folder(partial);
        }
        if (!ok) {
            return ECUIO_ERR_LOAD_FAILED;
        }
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (report != nullptr) {
        *report = r;
    }
    return ECUIO_OK;
}
