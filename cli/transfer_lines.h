// File/layer: cli/transfer_lines.h | havne-cli, a recording out to another
//             tool's format and a log in as one (ADR-0054)
// Testing: UNIT: CliTransferTest (test_cli)
//
// `export <recording> <out> --format folder|csv|pcapng [--from S] [--to S]
// [--raw] [--layout wide|long] [--rows sweep|<ms>] [--units metric|imperial]
// [--signal <name>]...` (the last four the CSV's shape, ADR-0056 section
// 5: wide by default, a column a named signal), `export <recording> --format
// csv --estimate` (the wide CSV's rows and bytes, no file), `import candump <in>
// <folder> [--interface <name>]` and
// `import csv <in> <folder> [--map <file> | --time-unit s|ms|us]` or
// `import csv <in> --print-map [--time-unit ...]`: their arguments, and
// their lines, each in core's words (recording.h): `exported` with what a
// scrubbed export left out by class, `imported` with the log's counts,
// `guessed` with a guessed column map, and `export_failed` naming the
// class a failed check found, never its value.

#ifndef HAVNE_CLI_TRANSFER_LINES_H
#define HAVNE_CLI_TRANSFER_LINES_H

#include "jsonl.h"
#include "view_lines.h" /* parse_seconds */

#include "ecuio/recording.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cli {

struct ExportArgs {
    const char* folder = nullptr;
    const char* out = nullptr;
    const char* format_word = nullptr;
    const char* layout_word = nullptr; /* --layout, or none */
    const char* rows_word = nullptr;   /* --rows, or none */
    bool units_given = false;
    bool estimate = false;            /* --estimate: sizes, no file */
    std::vector<const char*> signals; /* --signal, in order; options'
                                         columns point into it */
    ecuio_export_options_t options{};

    /* options.columns_utf8 points into signals: a copy would point into
     * the original's, so there is none */
    ExportArgs() = default;
    ExportArgs(const ExportArgs&) = delete;
    ExportArgs& operator=(const ExportArgs&) = delete;
};

// The named columns the recording has no values for, in their order: a
// typo stays an empty column (core's call), so havne-cli says so
inline std::vector<const char*>
missing_names(const std::vector<const char*>& named,
              const std::vector<std::string>& recorded)
{
    std::vector<const char*> missing;
    for (const char* name : named) {
        bool found = false;
        for (const std::string& r : recorded) {
            found = found || r == name;
        }
        if (!found) {
            missing.push_back(name);
        }
    }
    return missing;
}

// --rows' milliseconds: digits alone, 1 to a day
inline bool parse_row_ms(const char* word, uint32_t* ms)
{
    if (word[0] == '\0') {
        return false;
    }
    for (const char* p = word; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(word, &end, 10);
    if (end == nullptr || *end != '\0' || v < 1 || v > 86400000ul) {
        return false;
    }
    *ms = static_cast<uint32_t>(v);
    return true;
}

struct ImportArgs {
    const char* format_word = nullptr; /* candump or csv */
    const char* in = nullptr;
    const char* folder = nullptr;
    const char* interface_name = nullptr; /* candump */
    const char* map_path = nullptr;       /* csv: --map, read by main */
    const char* time_unit = nullptr;      /* csv, no --map: s, ms or us */
    bool print_map = false;               /* csv: the guess printed alone */
    ecuio_import_options_t options{};
};

// --format's word as core's format, 0 for none
inline uint32_t export_format(const char* word)
{
    if (std::strcmp(word, "folder") == 0) {
        return ECUIO_EXPORT_FOLDER;
    }
    if (std::strcmp(word, "csv") == 0) {
        return ECUIO_EXPORT_CSV;
    }
    if (std::strcmp(word, "pcapng") == 0) {
        return ECUIO_EXPORT_PCAPNG;
    }
    return 0;
}

// The arguments after `export`; false with *why, a usage error
inline bool parse_export(int argc, char** argv, ExportArgs& a, std::string* why)
{
    bool from = false;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        const bool has_value = i + 1 < argc;
        if (s == "--format" && has_value) {
            a.format_word = argv[++i];
            a.options.format = export_format(a.format_word);
            if (a.options.format == 0) {
                *why = "--format takes folder, csv or pcapng";
                return false;
            }
        } else if ((s == "--from" || s == "--to") && has_value) {
            int64_t us = 0;
            if (!parse_seconds(argv[++i], us) || us < 0) {
                *why = s + " takes seconds, 0 or more";
                return false;
            }
            if (s == "--from") {
                a.options.from_us = static_cast<uint64_t>(us);
                from = true;
            } else if (us == 0) {
                *why = "--to takes seconds after 0";
                return false;
            } else {
                a.options.to_us = static_cast<uint64_t>(us);
            }
        } else if (s == "--raw") {
            a.options.flags |= ECUIO_EXPORT_RAW;
        } else if (s == "--estimate") {
            a.estimate = true;
        } else if (s == "--layout" && has_value) {
            a.layout_word = argv[++i];
            if (std::strcmp(a.layout_word, "wide") == 0) {
                a.options.layout = ECUIO_CSV_WIDE;
            } else if (std::strcmp(a.layout_word, "long") == 0) {
                a.options.layout = ECUIO_CSV_LONG;
            } else {
                *why = "--layout takes wide or long";
                return false;
            }
        } else if (s == "--rows" && has_value) {
            a.rows_word = argv[++i];
            uint32_t ms = 0;
            if (std::strcmp(a.rows_word, "sweep") == 0) {
                a.options.row_ms = 0;
            } else if (parse_row_ms(a.rows_word, &ms)) {
                a.options.row_ms = ms;
            } else {
                *why = "--rows takes sweep or a number of milliseconds, 1 to "
                       "86400000";
                return false;
            }
        } else if (s == "--units" && has_value) {
            const char* u = argv[++i];
            if (std::strcmp(u, "metric") == 0) {
                a.options.units = ECUIO_UNITS_METRIC;
            } else if (std::strcmp(u, "imperial") == 0) {
                a.options.units = ECUIO_UNITS_IMPERIAL;
            } else {
                *why = "--units takes metric or imperial";
                return false;
            }
            a.units_given = true;
        } else if (s == "--signal" && has_value) {
            const char* name = argv[++i];
            if (name[0] == '\0') {
                *why = "--signal takes a signal's name";
                return false;
            }
            a.signals.push_back(name);
        } else if (s.rfind("--", 0) == 0) {
            *why = "unexpected '" + s + "'";
            return false;
        } else if (a.folder == nullptr) {
            a.folder = argv[i];
        } else if (a.out == nullptr) {
            a.out = argv[i];
        } else {
            *why = "export takes one recording and one file";
            return false;
        }
    }
    if (a.folder == nullptr || (a.out == nullptr && !a.estimate)) {
        *why = "export needs a recording's folder and the file to write";
        return false;
    }
    if (a.estimate && a.out != nullptr) {
        *why = "--estimate writes no file: give no file to write";
        return false;
    }
    if (a.format_word == nullptr) {
        *why = "export needs --format";
        return false;
    }
    if (from && a.options.to_us != 0 && a.options.to_us <= a.options.from_us) {
        *why = "--to comes after --from";
        return false;
    }
    /* The CSV's shape: the wide layout's alone */
    const bool shape =
        a.rows_word != nullptr || a.units_given || !a.signals.empty();
    if (a.options.format != ECUIO_EXPORT_CSV &&
        (a.layout_word != nullptr || shape)) {
        *why = "--layout, --rows, --units and --signal go with --format csv";
        return false;
    }
    if (a.options.layout == ECUIO_CSV_LONG && shape) {
        *why = "--rows, --units and --signal go with the wide layout";
        return false;
    }
    if (a.estimate && (a.options.format != ECUIO_EXPORT_CSV ||
                       a.options.layout == ECUIO_CSV_LONG)) {
        *why = "--estimate sizes the wide CSV: --format csv, no --layout long";
        return false;
    }
    a.options.column_count = static_cast<uint32_t>(a.signals.size());
    a.options.columns_utf8 = a.signals.empty() ? nullptr : a.signals.data();
    a.options.folder_utf8 = a.folder;
    a.options.out_path_utf8 = a.out;
    return true;
}

// The row intervals --estimate sizes: --rows' alone, else the ones the
// app's Export offers (a row a sweep, every 100 ms, every 250 ms)
inline std::vector<uint32_t> estimate_intervals(const ExportArgs& a)
{
    if (a.rows_word != nullptr) {
        return {a.options.row_ms};
    }
    return {0u, 100u, 250u};
}

// The arguments after `import`; false with *why, a usage error. The
// CSV's map is read by main from map_path into options.column_map_utf8,
// or with none guessed from the file (ecuio_import_csv_guess, its time
// unit time_unit's); --print-map prints the guess and imports nothing
inline bool parse_import(int argc, char** argv, ImportArgs& a, std::string* why)
{
    const bool candump = argc >= 3 && std::strcmp(argv[2], "candump") == 0;
    const bool csv = argc >= 3 && std::strcmp(argv[2], "csv") == 0;
    if (!candump && !csv) {
        *why = "import takes candump or csv";
        return false;
    }
    a.format_word = argv[2];
    const char* what = candump ? "log" : "file";
    for (int i = 3; i < argc; ++i) {
        const std::string s = argv[i];
        if (candump && s == "--interface" && i + 1 < argc) {
            a.interface_name = argv[++i];
        } else if (csv && s == "--map" && i + 1 < argc) {
            a.map_path = argv[++i];
        } else if (csv && s == "--time-unit" && i + 1 < argc) {
            const std::string u = argv[++i];
            if (u != "s" && u != "ms" && u != "us") {
                *why = "--time-unit takes s, ms or us";
                return false;
            }
            a.time_unit = argv[i];
        } else if (csv && s == "--print-map") {
            a.print_map = true;
        } else if (s.rfind("--", 0) == 0) {
            *why = "unexpected '" + s + "'";
            return false;
        } else if (a.in == nullptr) {
            a.in = argv[i];
        } else if (a.folder == nullptr && !a.print_map) {
            a.folder = argv[i];
        } else {
            *why = a.print_map ? std::string("import csv --print-map takes "
                                             "one file and no folder")
                               : std::string("import takes one ") + what +
                                     " and one folder";
            return false;
        }
    }
    if (a.print_map && a.folder != nullptr) {
        *why = "import csv --print-map takes one file and no folder";
        return false;
    }
    if (a.in == nullptr || (a.folder == nullptr && !a.print_map)) {
        *why =
            std::string("import needs the ") + what + " and the folder to make";
        return false;
    }
    if (a.map_path != nullptr && (a.time_unit != nullptr || a.print_map)) {
        *why = a.print_map ? "--print-map prints a guessed map: give no --map"
                           : "--time-unit is a guessed map's: a map given "
                             "with --map names its own";
        return false;
    }
    a.options.format = candump ? ECUIO_IMPORT_CANDUMP : ECUIO_IMPORT_CSV;
    a.options.in_path_utf8 = a.in;
    a.options.out_folder_utf8 = a.folder;
    a.options.interface_utf8 = a.interface_name;
    return true;
}

// The class words of ECUIO_EXPORT_LEFT_OUT_*
inline const char* left_out_word(uint32_t cls)
{
    switch (cls) {
    case ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION:
        return "identification";
    case ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE:
        return "unattributable";
    case ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES:
        return "text_values";
    case ECUIO_EXPORT_LEFT_OUT_ACTIVITY:
        return "activity";
    case ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED:
        return "vin_shaped";
    default:
        return "unknown";
    }
}

// The bytes exported_line (or export_failed_line) needs: its counts and
// keys, and the file's path at its longest escape
inline size_t exported_line_bytes(const ExportArgs& a)
{
    return 1024 + Line::string_bytes(a.out) + Line::string_bytes(a.format_word);
}

// The bytes imported_line needs: its counts and keys, the folder's path
// and the interface's name at their longest escapes
inline size_t imported_line_bytes(const ImportArgs& a)
{
    return 1024 + Line::string_bytes(a.folder) +
           Line::string_bytes(a.interface_name);
}

// A guess's text on one line: core writes no line break inside a string
// (each is escaped), so every break and the indent after it go
inline std::string one_line(const std::string& text)
{
    std::string o;
    o.reserve(text.size());
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            o += c;
            if (c == '\\' && i + 1 < text.size()) {
                o += text[++i]; /* an escape's second byte, a quote too */
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            o += c;
            in_string = c == '"';
        }
    }
    return o;
}

// {"type":"guessed","map":{...},"columns":[...],"notes":[...]}: the
// guess's text (ecuio_import_csv_guess) on one line, its keys the line's
inline size_t guessed_line_bytes(const std::string& text)
{
    return 64 + text.size();
}

inline void guessed_line(Line& l, const std::string& text)
{
    const std::string o = one_line(text);
    l.raw("{\"type\":\"guessed\",");
    /* core's text is one object: its members after its opening brace */
    l.raw(o.size() > 1 ? o.c_str() + 1 : "}");
}

// {"type":"exported","format":...,"file":...,"scrubbed":...,"frames":N,
//  "made_up":N,"left_out":{"identification":N,...}}; csv and folder add
//  "values":N and "signals":N after made_up, csv then "layout":"wide"|"long",
//  "rows":N, "row_ms":N and "missing":N (the named columns the recording
//  has no values for; schema 1 growth), folder "files":N and
//  "files_left_out":N
inline void exported_line(Line& l, const ExportArgs& a,
                          const ecuio_export_report_t& r, uint64_t missing = 0)
{
    l.raw("{\"type\":\"exported\"");
    l.key("format");
    l.string(a.format_word, SIZE_MAX);
    l.key("file");
    l.string(a.out, SIZE_MAX);
    l.key("scrubbed");
    l.raw((a.options.flags & ECUIO_EXPORT_RAW) != 0 ? "false" : "true");
    l.key("frames");
    l.u64(r.frames);
    l.key("made_up");
    l.u64(r.made_up);
    if (a.options.format == ECUIO_EXPORT_CSV ||
        a.options.format == ECUIO_EXPORT_FOLDER) {
        l.key("values");
        l.u64(r.values);
        l.key("signals");
        l.u64(r.signal_count);
    }
    if (a.options.format == ECUIO_EXPORT_CSV) {
        l.key("layout");
        l.string(a.options.layout == ECUIO_CSV_LONG ? "long" : "wide",
                 SIZE_MAX);
        l.key("rows");
        l.u64(r.rows);
        l.key("row_ms");
        l.u64(a.options.row_ms);
        l.key("missing");
        l.u64(missing);
    }
    if (a.options.format == ECUIO_EXPORT_FOLDER) {
        l.key("files");
        l.u64(r.files);
        l.key("files_left_out");
        l.u64(r.files_left_out);
    }
    l.key("left_out");
    l.ch('{');
    for (uint32_t c = 0; c < ECUIO_EXPORT_LEFT_OUT_COUNT; ++c) {
        if (c == ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED) {
            continue; /* the check's alone: never anything left out */
        }
        if (c > 0) {
            l.ch(',');
        }
        l.ch('"');
        l.raw(left_out_word(c));
        l.raw("\":");
        l.u64(r.left_out[c]);
    }
    l.raw("}}");
}

// {"type":"estimate","format":"csv","layout":"wide","columns":N,"missing":N,
//  "sizes":[{"row_ms":N,"rows":N,"bytes":N},...]}: what ecuio_export_estimate
//  says the wide CSV would hold at each interval, the bytes exact
inline void estimate_line(Line& l, const uint32_t* row_ms,
                          const ecuio_export_estimate_t* sized, size_t count,
                          uint64_t missing)
{
    l.raw("{\"type\":\"estimate\",\"format\":\"csv\",\"layout\":\"wide\"");
    l.key("columns");
    l.u64(count > 0 ? sized[0].columns : 0);
    l.key("missing");
    l.u64(missing);
    l.key("sizes");
    l.ch('[');
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.raw("{\"row_ms\":");
        l.u64(row_ms[i]);
        l.key("rows");
        l.u64(sized[i].rows);
        l.key("bytes");
        l.u64(sized[i].bytes);
        l.ch('}');
    }
    l.raw("]}");
}

// {"type":"export_failed","class":...}: a failed check, before the error
inline void export_failed_line(Line& l, const ecuio_export_report_t& r)
{
    l.raw("{\"type\":\"export_failed\"");
    l.key("class");
    l.string(left_out_word(r.failed_class - 1), SIZE_MAX);
    l.ch('}');
}

// {"type":"imported","format":"candump","folder":...,"interface":...|null,
//  "lines":N,"frames":N,"error_frames":N,"interfaces":N,
//  "utc_start_us":N,"duration_us":N}; csv's: "format":"csv","folder":...,
//  "lines":N,"values":N,"columns_ignored":N,"utc_start_us":N|null,
//  "duration_us":N
inline void imported_line(Line& l, const ImportArgs& a,
                          const ecuio_import_report_t& r)
{
    if (a.options.format == ECUIO_IMPORT_CSV) {
        l.raw("{\"type\":\"imported\",\"format\":\"csv\"");
        l.key("folder");
        l.string(a.folder, SIZE_MAX);
        l.key("lines");
        l.u64(r.lines);
        l.key("values");
        l.u64(r.values);
        l.key("columns_ignored");
        l.u64(r.columns_ignored);
        l.key("utc_start_us");
        if (r.utc_start_us == 0) {
            l.raw("null");
        } else {
            l.u64(r.utc_start_us);
        }
        l.key("duration_us");
        l.u64(r.duration_us);
        l.ch('}');
        return;
    }
    l.raw("{\"type\":\"imported\",\"format\":\"candump\"");
    l.key("folder");
    l.string(a.folder, SIZE_MAX);
    l.key("interface");
    if (a.interface_name != nullptr) {
        l.string(a.interface_name, SIZE_MAX);
    } else {
        l.raw("null");
    }
    l.key("lines");
    l.u64(r.lines);
    l.key("frames");
    l.u64(r.frames);
    l.key("error_frames");
    l.u64(r.error_frames);
    l.key("interfaces");
    l.u64(r.interfaces);
    l.key("utc_start_us");
    l.u64(r.utc_start_us);
    l.key("duration_us");
    l.u64(r.duration_us);
    l.ch('}');
}

} // namespace cli

#endif /* HAVNE_CLI_TRANSFER_LINES_H */
