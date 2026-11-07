// File/layer: cli/transfer_lines.h | havne-cli, a recording out to another
//             tool's format and a log in as one (ADR-0054)
// Testing: UNIT: CliTransferTest (test_cli)
//
// `export <recording> <out> --format folder|csv|pcapng [--from S] [--to S]
// [--raw]` and `import candump <in> <folder> [--interface <name>]`: their
// arguments, and their lines, each in core's words (recording.h):
// `exported` with what a scrubbed export left out by class, `imported`
// with the log's counts, and `export_failed` naming the class a failed
// check found, never its value.

#ifndef HAVNE_CLI_TRANSFER_LINES_H
#define HAVNE_CLI_TRANSFER_LINES_H

#include "jsonl.h"
#include "view_lines.h" /* parse_seconds */

#include "ecuio/recording.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace cli {

struct ExportArgs {
    const char* folder = nullptr;
    const char* out = nullptr;
    const char* format_word = nullptr;
    ecuio_export_options_t options{};
};

struct ImportArgs {
    const char* in = nullptr;
    const char* folder = nullptr;
    const char* interface_name = nullptr;
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
    if (a.folder == nullptr || a.out == nullptr) {
        *why = "export needs a recording's folder and the file to write";
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
    a.options.folder_utf8 = a.folder;
    a.options.out_path_utf8 = a.out;
    return true;
}

// The arguments after `import`; false with *why, a usage error
inline bool parse_import(int argc, char** argv, ImportArgs& a, std::string* why)
{
    if (argc < 3 || std::strcmp(argv[2], "candump") != 0) {
        *why = "import takes candump";
        return false;
    }
    for (int i = 3; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--interface" && i + 1 < argc) {
            a.interface_name = argv[++i];
        } else if (s.rfind("--", 0) == 0) {
            *why = "unexpected '" + s + "'";
            return false;
        } else if (a.in == nullptr) {
            a.in = argv[i];
        } else if (a.folder == nullptr) {
            a.folder = argv[i];
        } else {
            *why = "import takes one log and one folder";
            return false;
        }
    }
    if (a.in == nullptr || a.folder == nullptr) {
        *why = "import needs the log and the folder to make";
        return false;
    }
    a.options.format = ECUIO_IMPORT_CANDUMP;
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

// {"type":"exported","format":...,"file":...,"scrubbed":...,"frames":N,
//  "made_up":N,"left_out":{"identification":N,...}}; csv and folder add
//  "values":N and "signals":N after made_up, folder then "files":N and
//  "files_left_out":N
inline void exported_line(Line& l, const ExportArgs& a,
                          const ecuio_export_report_t& r)
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
//  "utc_start_us":N,"duration_us":N}
inline void imported_line(Line& l, const ImportArgs& a,
                          const ecuio_import_report_t& r)
{
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
