// File/layer: core/src/import.cpp | C++17, Qt-free; a log from another tool
//             as a recording (ADR-0054 sections 4 and 5)
// Testing: UNIT: ImportTest, CsvImportTest, CsvGuessTest (test_core)
//
// ecuio_import: a candump log read twice, first to check every line and
// list the interfaces (so a refusal comes before anything is written),
// then to write the chosen interface's frames as a recording folder:
// frames.csv as they arrived, and an events.log header naming the import.
// A CSV the same way through its column map (csv_import.h's pass, run to
// check and then to write): signals.csv and a format 3 header, no frames.
// ecuio_import_csv_guess reads a CSV's start and hands csv_guess.h's map
// back as JSON, importing nothing.
// The folder is written under a name of its own beside <folder>
// (folder_files.h's partial_name) and renamed once whole.

#include "candump.h"
#include "csv_guess.h"
#include "csv_import.h"
#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"      /* kHeader */
#include "recording_lines.h" /* csv_quoted, signal_line */
#include "recording_rows.h"  /* kFormat2, kFormat3, append_value */

#include "ecuio/core.h"
#include "ecuio/internal.h" /* ecuio_import_test_between_passes */
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

/* The test seam's hook (internal.h), NULL outside a test */
void (*g_between_passes)(void*) = nullptr;
void* g_between_passes_user = nullptr;

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

    /* The line last read had no line end: the file ends inside it, a
     * line cut short (ADR-0049 section 7's torn row) */
    bool cut() const { return cut_; }

    bool next(std::string& line, bool* too_long)
    {
        line.clear();
        *too_long = false;
        int c = 0;
        bool any = false;
        bool ended = false;
        while ((c = std::fgetc(f_)) != EOF) {
            any = true;
            if (c == '\n') {
                ended = true;
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
        cut_ = any && !ended;
        return any;
    }

private:
    FILE* f_;
    bool cut_ = false;
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
    bool cut = false; /* the last line has no line end: left out */
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
        if (lines.cut()) {
            s.cut = true; /* candump ends every line: this one was cut */
            continue;
        }
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
 * order; report's counts and times. Lines other than the first pass's
 * (more, fewer, or the cut one ended since) are a log that changed. */
bool write_frames(FILE* in, FILE* out, const survey& s,
                  const std::string& iface, ecuio_import_report_t& r,
                  std::string* why)
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
    bool cut = false;
    while (lines.next(line, &too_long)) {
        ++line_no;
        if (lines.cut()) {
            cut = true;
            continue;
        }
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
    if (line_no != s.lines || cut != s.cut) {
        *why = "the log changed while it was read";
        return false;
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
std::string events_log(const std::string& iface, bool cut,
                       const ecuio_import_report_t& r)
{
    std::string e = std::string(ecuio_recording_rows::kFormat2) + "\n";
    e += "started  " + utc_text(r.utc_start_us, ' ') + " (UTC)\n";
    e += "utc  " + utc_text(r.utc_start_us, 'T') + "Z\n";
    e += std::string("core  ") + ecuio_version_string() + "\n";
    e += "import  candump log, interface " + iface + "\n";
    if (cut) {
        e += "import  the log's last line has no line end: cut short, left "
             "out\n";
    }
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
                    ecuio_import_report_t& r, std::string* made)
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
    /* the name goes in *made before the folder is made */
    if (!files::partial_folder(out, made, &why)) {
        made->clear();
        return fail("the recording's folder can't be made: %s", why);
    }
    const std::string partial = *made;
    if (g_between_passes != nullptr) {
        g_between_passes(g_between_passes_user);
    }
    if (!ecuio_frames_csv::seek_to(in.get(), 0)) {
        return fail("the log can't be read again");
    }
    {
        std::unique_ptr<FILE, file_closer> frames(
            files::open_file(partial + "/frames.csv", true, &why));
        if (!frames) {
            return fail("frames.csv can't be written: %s", why);
        }
        if (!write_frames(in.get(), frames.get(), s, iface, r, &why)) {
            return fail("%s", why);
        }
        if (std::fclose(frames.release()) != 0) {
            return fail("frames.csv can't be written: %s",
                        files::errno_words(errno));
        }
    }
    const std::string log = events_log(iface, s.cut, r);
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
    made->clear();
    return true;
}

/* ---- CSV (section 5) ---- */

namespace csv = ecuio_csv_import;

struct file_source {
    FILE* f;
    int get()
    {
        const int c = std::fgetc(f);
        return c == EOF ? -1 : c;
    }
};

/* The second pass's sink: signals.csv's rows, written in chunks */
struct rows_writer {
    FILE* out;
    std::vector<std::string> names; /* quoted */
    std::vector<std::string> units; /* quoted */
    std::string rows;
    std::string error;

    bool flush()
    {
        if (!rows.empty() &&
            std::fwrite(rows.data(), 1, rows.size(), out) != rows.size()) {
            error =
                "signals.csv can't be written: " + files::errno_words(errno);
            return false;
        }
        rows.clear();
        return true;
    }

    void head(uint64_t host_us, size_t s)
    {
        append_u64(rows, host_us);
        rows += ',';
        rows += names[s];
        rows += ',';
    }

    bool tail(size_t s)
    {
        rows += ',';
        rows += units[s];
        rows += '\n';
        return rows.size() < kWriteChunk || flush();
    }

    bool number(uint64_t host_us, size_t s, double v)
    {
        head(host_us, s);
        ecuio_recording_rows::append_value(rows, v);
        return tail(s);
    }

    bool text(uint64_t host_us, size_t s, const std::string& v)
    {
        head(host_us, s);
        rows += ecuio_recording_lines::csv_quoted(v);
        return tail(s);
    }

    std::string failure() const { return error; }
};

const char* unit_words(csv::time_unit u)
{
    switch (u) {
    case csv::time_unit::s:
        return "seconds";
    case csv::time_unit::ms:
        return "milliseconds";
    case csv::time_unit::us:
        return "microseconds";
    case csv::time_unit::iso8601:
        return "ISO 8601 times";
    case csv::time_unit::time_of_day:
        return "times of day";
    }
    return "";
}

/* The recording's events.log: format 3's header with the map's signals,
 * the import as its source (never the file's name, which can name a car
 * or a person), a utc line only when the file or the map gives one */
std::string csv_events_log(const csv::column_map& m, const csv::outcome& o)
{
    namespace lines = ecuio_recording_lines;
    std::string e = std::string(ecuio_recording_rows::kFormat3) + "\n";
    if (o.has_utc) {
        e += "started  " + utc_text(o.utc_start_us, ' ') + " (UTC)\n";
        e += "utc  " + utc_text(o.utc_start_us, 'T') + "Z\n";
    }
    e += std::string("core  ") + ecuio_version_string() + "\n";
    e += "import  CSV, its time from the column " +
         ecuio_json::quoted(m.time_column) + " as " + unit_words(m.unit) + "\n";
    if (!o.ignored.empty()) {
        e += "import  columns ignored: ";
        append_u64(e, o.ignored.size());
        e += " (";
        for (size_t i = 0; i < o.ignored.size() && i < csv::kIgnoredListed;
             ++i) {
            e += i == 0 ? "" : ", ";
            e += ecuio_json::quoted(o.ignored[i]);
        }
        if (o.ignored.size() > csv::kIgnoredListed) {
            e += " and ";
            append_u64(e, o.ignored.size() - csv::kIgnoredListed);
            e += " more";
        }
        e += ")\n";
    }
    e += "profile  raw\n";
    for (const csv::signal_spec& s : m.signals) {
        e += lines::signal_line(
            lines::csv_quoted(s.name), s.text ? "text" : "number",
            lines::csv_quoted(s.unit), s.decimals, s.rank, s.group);
    }
    e += o.has_utc ? "columns  UTC time" : "columns  time since the start";
    e += ", seconds since the recording started (host_us / 1e6 in the CSV "
         "files), kind, text\n";
    e += "\n";
    e += line_stamp(o.utc_start_us + o.duration_us, o.duration_us) +
         "  end  import: 0 frames, ";
    append_u64(e, o.values);
    e += " values, 0 records dropped, 0 notes dropped\n";
    return e;
}

bool import_csv(const ecuio_import_options_t& o, const std::string& out,
                ecuio_import_report_t& r, std::string* made)
{
    std::string why;
    csv::column_map m;
    const char* map = o.column_map_utf8;
    if (!csv::parse_map(map, strnlen(map, ecuio_json::kMaxBytes + 1), m,
                        &why)) {
        return fail("%s", why);
    }
    std::unique_ptr<FILE, file_closer> in(
        files::open_file(o.in_path_utf8, false, &why));
    if (!in) {
        return fail("the file can't be opened: %s", why);
    }
    csv::outcome first;
    {
        file_source src{in.get()};
        csv::counting_sink count;
        if (!csv::run(src, m, count, first, &why)) {
            return fail("%s", why);
        }
        if (std::ferror(in.get()) != 0) {
            return fail("the file can't be read");
        }
    }
    if (files::exists(out)) {
        return fail("the recording's folder exists already");
    }
    /* the name goes in *made before the folder is made */
    if (!files::partial_folder(out, made, &why)) {
        made->clear();
        return fail("the recording's folder can't be made: %s", why);
    }
    const std::string partial = *made;
    if (!ecuio_frames_csv::seek_to(in.get(), 0)) {
        return fail("the file can't be read again");
    }
    csv::outcome second;
    {
        std::unique_ptr<FILE, file_closer> sig(
            files::open_file(partial + "/signals.csv", true, &why));
        if (!sig) {
            return fail("signals.csv can't be written: %s", why);
        }
        rows_writer w{sig.get(), {}, {}, {}, {}};
        for (const csv::signal_spec& s : m.signals) {
            w.names.push_back(ecuio_recording_lines::csv_quoted(s.name));
            w.units.push_back(ecuio_recording_lines::csv_quoted(s.unit));
        }
        w.rows = std::string("\xEF\xBB\xBF") +
                 ecuio_recording_rows::kSignalsHeader + "\n";
        file_source src{in.get()};
        if (!csv::run(src, m, w, second, &why)) {
            return fail("%s", why.empty()
                                  ? std::string("the file changed while "
                                                "it was read")
                                  : why);
        }
        if (second.values != first.values || second.rows != first.rows) {
            return fail("the file changed while it was read");
        }
        if (!w.flush()) {
            return fail("%s", w.error);
        }
        if (std::fclose(sig.release()) != 0) {
            return fail("signals.csv can't be written: %s",
                        files::errno_words(errno));
        }
    }
    const std::string log = csv_events_log(m, second);
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
    made->clear();
    r.lines = second.lines;
    r.values = second.values;
    r.columns_ignored = second.ignored.size();
    r.utc_start_us = second.has_utc ? second.utc_start_us : 0;
    r.duration_us = second.duration_us;
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
    const bool is_csv = options->format == ECUIO_IMPORT_CSV;
    /* A column map is the CSV's alone, and it needs one; an interface is
     * candump's alone */
    if ((options->column_map_utf8 != nullptr) != is_csv ||
        (is_csv && options->interface_utf8 != nullptr)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_detail_clear();
    ecuio_import_report_t r{};
    /* The partial folder an import made and didn't put in place goes on
     * every way out, an exception's too */
    struct partial_guard {
        std::string made; /* the folder's name while it isn't in place */
        partial_guard() = default;
        partial_guard(const partial_guard&) = delete;
        partial_guard& operator=(const partial_guard&) = delete;
        ~partial_guard()
        {
            if (made.empty()) {
                return;
            }
            try {
                files::remove_file(made + "/frames.csv");
                files::remove_file(made + "/signals.csv");
                files::remove_file(made + "/events.log");
                files::remove_folder(made);
            } catch (...) { // NOLINT(bugprone-empty-catch): out of memory
                            // again while unwinding; the partial stays
            }
        }
    };
    try {
        std::string out = options->out_folder_utf8;
        while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
            out.pop_back();
        }
        partial_guard g;
        const bool ok = is_csv ? import_csv(*options, out, r, &g.made)
                               : import_candump(*options, out, r, &g.made);
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

/* ---- the guess (ADR-0054's appendix "a column map guessed from the
 * header") ---- */

namespace guess = ecuio_map_guess;

static_assert(guess::kTimeUnitKnown == ECUIO_CSV_GUESS_TIME_UNIT_KNOWN &&
                  guess::kDecimalComma == ECUIO_CSV_GUESS_DECIMAL_COMMA &&
                  guess::kPreamble == ECUIO_CSV_GUESS_PREAMBLE &&
                  guess::kTimeNumbers == ECUIO_CSV_GUESS_TIME_NUMBERS,
              "the guess's flags are recording.h's");

extern "C" ecuio_status_t ecuio_import_csv_guess(const char* path_utf8,
                                                 const char* time_unit,
                                                 char* map_out, size_t cap,
                                                 size_t* needed,
                                                 ecuio_csv_guess_t* report)
{
    if (report != nullptr) {
        *report = ecuio_csv_guess_t{};
    }
    if (needed != nullptr) {
        *needed = 0;
    }
    /* Blank before any refusal, the arguments' too (recording.h) */
    if (map_out != nullptr && cap > 0) {
        map_out[0] = '\0';
    }
    if (path_utf8 == nullptr || needed == nullptr ||
        (map_out == nullptr && cap > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (time_unit != nullptr && std::strcmp(time_unit, "s") != 0 &&
        std::strcmp(time_unit, "ms") != 0 &&
        std::strcmp(time_unit, "us") != 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_detail_clear();
    try {
        std::string why;
        std::unique_ptr<FILE, file_closer> in(
            files::open_file(path_utf8, false, &why));
        if (!in) {
            fail("the file can't be opened: %s", why);
            return ECUIO_ERR_LOAD_FAILED;
        }
        /* The file's start, one byte past what the guess reads to know
         * whether it goes on; then cut at its last line feed */
        std::vector<unsigned char> bytes(guess::kReadMax + 1);
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), in.get());
        if (std::ferror(in.get()) != 0) {
            fail("the file can't be read");
            return ECUIO_ERR_LOAD_FAILED;
        }
        size_t n = got;
        const bool cut = got > guess::kReadMax;
        if (cut) {
            n = guess::kReadMax;
            while (n > 0 && bytes[n - 1] != '\n') {
                --n;
            }
            if (n == 0) {
                /* Not an empty file: rows too long for any reader */
                fail("%s", "the file's first " +
                               std::to_string(guess::kReadMax >> 10) +
                               " KiB hold no line end");
                return ECUIO_ERR_LOAD_FAILED;
            }
        }
        guess::result r;
        if (!guess::guess(bytes.data(), n, cut, time_unit, r, &why)) {
            fail("%s", why);
            return ECUIO_ERR_LOAD_FAILED;
        }
        *needed = r.text.size() + 1;
        if (cap >= *needed) {
            std::memcpy(map_out, r.text.c_str(), *needed);
        }
        if (report != nullptr) {
            report->flags = r.flags;
            report->columns = r.columns;
            report->left_out = r.left_out;
            report->rows_read = r.rows_read;
            report->map_offset = static_cast<uint32_t>(r.map_offset);
            report->map_length = static_cast<uint32_t>(r.map_length);
        }
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

void ecuio_import_test_between_passes(void (*hook)(void* user), void* user)
{
    g_between_passes = hook;
    g_between_passes_user = user;
}
