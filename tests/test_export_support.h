// File/layer: tests/test_export_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_export*.cpp (test_core)

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "folder_files.h"
#include "isotp.h"
#include "open_handles.h"
#include "partial_left.h"
#include "recording_set.h"
#include "scrub.h"
#include "sha256.h"
#include "temp_name.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;

namespace {

/* The simulated ECU's VIN, the fixtures' made-up form */
const char* const kVin = "ZZZFAKE11M0000001";

// ---- the export ----

struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") / temp_name("ecuio-export");
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path / "rec", ec);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    std::string utf8(const std::string& name) const
    {
        return (path / name).generic_u8string();
    }
    void write(const std::string& name, const std::string& text) const
    {
        std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
        f << text;
    }
};

inline std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

inline std::string events_log()
{
    return "ecuio session recording, format 2 (ADR-0049)\n"
           "started  2023-11-14 22:13:20.250 (UTC)\n"
           "utc  2023-11-14T22:13:20.250Z\n"
           "profile  raw\n"
           "frame flags  complete\n"
           "\n";
}

/* A frames.csv row */
inline std::string row(uint64_t t, uint32_t id, uint32_t flags,
                const std::vector<uint8_t>& data)
{
    char head[96];
    std::snprintf(head, sizeof head, "%llu,%llu,0x%X,0x%X,",
                  static_cast<unsigned long long>(t),
                  static_cast<unsigned long long>(t), id, flags);
    std::string out = head;
    for (size_t i = 0; i < data.size(); ++i) {
        char b[4];
        std::snprintf(b, sizeof b, i == 0 ? "%02X" : " %02X", data[i]);
        out += b;
    }
    return out + "\n";
}

inline std::vector<uint8_t> text_bytes(std::initializer_list<uint8_t> head,
                                const std::string& text)
{
    /* sized once: GCC 15's -Warray-bounds misreads an insert here */
    std::vector<uint8_t> v(head.size() + text.size());
    std::copy(head.begin(), head.end(), v.begin());
    std::copy(text.begin(), text.end(),
              v.begin() + static_cast<std::ptrdiff_t>(head.size()));
    return v;
}

const uint32_t kWhole = ECUIO_FRAME_FLAG_REASSEMBLED;

/* A recording folder of frames.csv rows, with events.log (header lines
 * in place of its profile line when given) */
inline void recording(const Folder& f, const std::string& rows,
               const std::string& signals = "", const std::string& header = "")
{
    std::string events = events_log();
    if (!header.empty()) {
        const size_t at = events.find("profile  raw\n");
        events.replace(at, std::strlen("profile  raw\n"), header);
    }
    f.write("rec/events.log", events);
    f.write("rec/frames.csv", "host_us,timestamp_us,id,flags,data\n" + rows);
    if (!signals.empty()) {
        f.write("rec/signals.csv", "host_us,signal,value,unit\n" + signals);
    }
}

// The pcapng reader: little-endian blocks, as the export writes them

struct Packet {
    uint64_t ts = 0;
    std::vector<uint8_t> data;
    std::string comment;
    uint32_t flags = 0;
    uint32_t can_id() const
    {
        return (uint32_t{data[0]} << 24) | (uint32_t{data[1]} << 16) |
               (uint32_t{data[2]} << 8) | data[3];
    }
    std::vector<uint8_t> payload() const
    {
        return std::vector<uint8_t>(data.begin() + 8,
                                    data.begin() + 8 + data[4]);
    }
};

struct Pcap {
    std::string why; /* empty when it read */
    std::string comment;
    std::string application;
    uint16_t linktype = 0;
    int tsresol = -1;
    int interfaces = 0;
    std::vector<Packet> packets;
};

inline uint32_t le32(const std::string& b, size_t at)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | static_cast<uint8_t>(b[at + i]);
    }
    return v;
}

inline uint16_t le16(const std::string& b, size_t at)
{
    return static_cast<uint16_t>(static_cast<uint8_t>(b[at]) |
                                 (static_cast<uint8_t>(b[at + 1]) << 8));
}

template <typename Each>
bool options(const std::string& b, size_t at, size_t end, Each each)
{
    while (at + 4 <= end) {
        const uint16_t code = le16(b, at);
        const uint16_t n = le16(b, at + 2);
        if (code == 0) {
            return true;
        }
        if (at + 4 + n > end) {
            return false;
        }
        each(code, b.substr(at + 4, n));
        at += 4 + ((n + 3u) & ~3u);
    }
    return at == end;
}

inline Pcap read_pcapng(const std::string& b)
{
    Pcap p;
    size_t at = 0;
    bool first = true;
    while (at < b.size()) {
        if (at + 12 > b.size()) {
            p.why = "a block is cut short";
            return p;
        }
        const uint32_t type = le32(b, at);
        const uint32_t len = le32(b, at + 4);
        if (len < 12 || len % 4 != 0 || at + len > b.size() ||
            le32(b, at + len - 4) != len) {
            p.why = "a block's length is wrong";
            return p;
        }
        const size_t end = at + len - 4;
        if (first != (type == 0x0A0D0D0Au)) {
            p.why = "the section header isn't first, or isn't alone";
            return p;
        }
        first = false;
        if (type == 0x0A0D0D0Au) {
            if (le32(b, at + 8) != 0x1A2B3C4Du || le16(b, at + 12) != 1 ||
                le16(b, at + 14) != 0) {
                p.why = "the section header's magic or version is wrong";
                return p;
            }
            if (!options(b, at + 24, end,
                         [&](uint16_t c, const std::string& v) {
                             if (c == 1) {
                                 p.comment = v;
                             } else if (c == 4) {
                                 p.application = v;
                             }
                         })) {
                p.why = "the section header's options are wrong";
                return p;
            }
        } else if (type == 1) {
            ++p.interfaces;
            p.linktype = le16(b, at + 8);
            if (!options(b, at + 16, end,
                         [&](uint16_t c, const std::string& v) {
                             if (c == 9 && v.size() == 1) {
                                 p.tsresol = static_cast<uint8_t>(v[0]);
                             }
                         })) {
                p.why = "an interface's options are wrong";
                return p;
            }
        } else if (type == 6) {
            Packet k;
            if (le32(b, at + 8) != 0) {
                p.why = "a packet names another interface";
                return p;
            }
            k.ts = (uint64_t{le32(b, at + 12)} << 32) | le32(b, at + 16);
            const uint32_t cap = le32(b, at + 20);
            if (le32(b, at + 24) != cap || at + 28 + cap > end) {
                p.why = "a packet's lengths are wrong";
                return p;
            }
            k.data.assign(b.begin() + static_cast<std::ptrdiff_t>(at + 28),
                          b.begin() +
                              static_cast<std::ptrdiff_t>(at + 28 + cap));
            if (!options(b, at + 28 + ((cap + 3u) & ~3u), end,
                         [&](uint16_t c, const std::string& v) {
                             if (c == 1) {
                                 k.comment = v;
                             } else if (c == 2 && v.size() == 4) {
                                 k.flags = le32(v, 0);
                             }
                         })) {
                p.why = "a packet's options are wrong";
                return p;
            }
            p.packets.push_back(std::move(k));
        } else {
            p.why = "a block of another type";
            return p;
        }
        at += len;
    }
    return p;
}

struct Exported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::string bytes;
    Pcap pcap;
    Exported(const Folder& f, uint32_t flags = 0, uint64_t from_us = 0,
             uint64_t to_us = 0, const std::string& name = "out.pcapng")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_PCAPNG;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        if (fs::exists(f.path / name)) {
            bytes = read_file(f.path / name);
            pcap = read_pcapng(bytes);
        }
    }
};

/* Neither the file nor its partial copy */
inline void expect_no_file(const Folder& f, const std::string& name = "out.pcapng")
{
    EXPECT_FALSE(fs::exists(f.path / name));
    EXPECT_FALSE(partial_left(f.path, name));
}

// ---- the CSV export: signals.csv's values as samples ----

/* The long form (ECUIO_CSV_LONG), a row a value */
struct CsvExported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::string bytes;
    CsvExported(const Folder& f, uint32_t flags = 0, uint64_t from_us = 0,
                uint64_t to_us = 0, const std::string& name = "out.csv")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_CSV;
        o.layout = ECUIO_CSV_LONG;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        if (fs::exists(f.path / name)) {
            bytes = read_file(f.path / name);
        }
    }
};

/* The wide form (ECUIO_CSV_WIDE, the default): the columns named, or
 * none for the header's focus line or every number signal */
struct WideExported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::string bytes;
    WideExported(const Folder& f, std::vector<const char*> columns = {},
                 uint32_t row_ms = 0, uint32_t units = ECUIO_UNITS_METRIC,
                 uint32_t flags = 0, uint64_t from_us = 0, uint64_t to_us = 0,
                 const std::string& name = "wide.csv")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_CSV;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        o.row_ms = row_ms;
        o.units = units;
        o.column_count = static_cast<uint32_t>(columns.size());
        o.columns_utf8 = columns.empty() ? nullptr : columns.data();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        if (fs::exists(f.path / name)) {
            bytes = read_file(f.path / name);
        }
    }
};

/* The byte order mark a user CSV starts with */
inline const char* const kBom = "\xEF\xBB\xBF";

/* A user CSV without its byte order mark, which it must start with */
inline std::string unbommed(const std::string& bytes)
{
    EXPECT_EQ(bytes.substr(0, 3), kBom) << "no byte order mark";
    return bytes.compare(0, 3, kBom) == 0 ? bytes.substr(3) : bytes;
}

/* A wide file's lines, the byte order mark and the CRLFs checked and
 * dropped */
inline std::vector<std::string> wide_lines(const std::string& all)
{
    const std::string bytes = unbommed(all);
    std::vector<std::string> out;
    size_t at = 0;
    while (at < bytes.size()) {
        const size_t nl = bytes.find("\r\n", at);
        EXPECT_NE(nl, std::string::npos)
            << "a line without CRLF: " << bytes.substr(at);
        if (nl == std::string::npos) {
            out.push_back(bytes.substr(at));
            break;
        }
        out.push_back(bytes.substr(at, nl - at));
        at = nl + 2;
    }
    return out;
}

/* Three signals polled in turn every 100 ms, from 100 ms: RPM, Speed,
 * Throttle, the throttle to one decimal; sweeps sweeps of them */
inline std::string polled_rows(int sweeps, int from_ms = 100)
{
    std::string s;
    for (int i = 0; i < sweeps; ++i) {
        const int t = from_ms + 100 * i;
        char b[160];
        std::snprintf(b, sizeof b,
                      "%d000,RPM,%d,rpm\n%d000,Speed,%d,km/h\n"
                      "%d000,Throttle,%d.5,%%\n",
                      t, 800 + i, t + 30, i / 2, t + 60, 10 + i);
        s += b;
    }
    return s;
}

inline std::string csv_head(const char* kind, const std::string& counts,
                     uint64_t ident, uint64_t text)
{
    char b[512];
    std::snprintf(b, sizeof b,
                  "# Havne %s, %s (ADR-0054): %s; left out: %llu values of "
                  "identification signals, %llu of text signals\n"
                  "time_s,signal,unit,value\n",
                  ecuio_version_string(), kind, counts.c_str(),
                  static_cast<unsigned long long>(ident),
                  static_cast<unsigned long long>(text));
    return b;
}

struct FolderExported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::map<std::string, std::string> files;
    FolderExported(const Folder& f, uint32_t flags = 0, uint64_t from_us = 0,
                   uint64_t to_us = 0, const std::string& name = "copy")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_FOLDER;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(f.path / name, ec)) {
            files[e.path().filename().generic_u8string()] = read_file(e.path());
        }
    }
};

const char* const kBomHeader = "\xEF\xBB\xBFhost_us,signal,value,unit\n";

const char* const kColumns =
    "columns  local time, seconds since the recording started (host_us / "
    "1e6 in the CSV files), kind, text\n";

/* A message an adapter delivered whole, longer than a frame: one row */
const std::vector<uint8_t> kWholeReply = {0x41, 0x0C, 0x1A, 0xF8, 0x0D,
                                          0x32, 0x05, 0x5A, 0x04, 0x80};
/* Twelve bytes with no FD flag, which no CAN frame carries */
const std::vector<uint8_t> kTwelve = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

/* A recording with every file the export knows and one it doesn't: an
 * ELM327 session through the app begun after start (a focus and the
 * link's state on the header), a VIN reply in its frames and its
 * values, a marker, reply timing and a file of the user's */
inline void whole_recording(const Folder& f)
{
    f.write("rec/events.log",
            std::string("ecuio session recording, format 3 (ADR-0049)\n"
                        "started  2023-11-14 22:13:20.250 (UTC)\n"
                        "utc  2023-11-14T22:13:20.250Z\n"
                        "core  0.5.1\n"
                        "uri  elm327://COM3?profile=obd2&serial=115200&"
                        "filter=7E8&label=Daily\n"
                        "profile  obd2\n"
                        "frame flags  complete\n"
                        "focus  RPM\n"
                        "min sweep  250 ms\n"
                        "link  recovered (state 0, 1 episode)\n"
                        "signal  RPM,number,rpm,0,1\n"
                        "signal  VIN,text,,0,0\n"
                        "caller  Havne 0.5.1\n"
                        "caller  vehicle  Daily driver\n"
                        "caller  words of the caller's own\n") +
                kColumns +
                "\n"
                "22:13:21.250  +1.000  note  Connected to the Daily driver\n"
                "22:13:22.250  +2.000  link  vehicle not responding (state "
                "8, 1 episode)\n"
                "22:13:23.250  +3.000  asleep  Asleep for 1.0 s: nothing "
                "was read meanwhile\n"
                "22:13:24.250  +4.000  end  Disconnected by the user: 3 "
                "frames, 2 values, 0 records dropped, 0 notes dropped\n");
    f.write(
        "rec/frames.csv",
        "host_us,timestamp_us,id,flags,data\n" +
            row(1000000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}) +
            row(1500000, 0x7E8, kWhole, kWholeReply) +
            row(2000000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)) +
            row(3000000, 0x123, 0, {0x01, 0x02, 0x03}) +
            row(3200000, 0x456, 0, kTwelve));
    f.write("rec/signals.csv", std::string(kBomHeader) +
                                   "1000000,RPM,1726,rpm\n"
                                   "2500000,VIN," +
                                   kVin + ",\n3000000,RPM,1730,rpm\n");
    f.write("rec/markers.csv",
            "host_us,label\n1500000,\"pull, 2nd gear\"\n9000000,late\n");
    f.write("rec/latency.csv",
            "t_s,ecu,request,answered\n1.000,7E8,01 0C,5\n3.000,7E8,01 "
            "0C,9\n");
    f.write("rec/notes.txt", "the user's own file\n");
}

inline std::string export_line(const char* kind, uint64_t frames, uint64_t ident,
                        uint64_t values, uint64_t text, uint64_t markers,
                        uint64_t lines, unsigned others)
{
    char b[1024];
    std::snprintf(
        b, sizeof b,
        "export  %s (ADR-0054), Havne %s: frames.csv %llu frames (left out: "
        "%llu of identification replies, 0 of text signals' messages, 0 "
        "segments of messages that couldn't be reassembled, 0 of messages "
        "holding a VIN-shaped token); signals.csv "
        "%llu values (left out: 0 of identification signals, %llu of text "
        "signals); markers.csv %llu markers, their labels as typed; "
        "events.log %llu lines left out (the caller's notes, its other "
        "lines and lines of a kind the export doesn't know); other files "
        "left out: %u\n",
        kind, ecuio_version_string(), static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(ident),
        static_cast<unsigned long long>(values),
        static_cast<unsigned long long>(text),
        static_cast<unsigned long long>(markers),
        static_cast<unsigned long long>(lines), others);
    return b;
}

const char* const kHead =
    "ecuio session recording, format 3 (ADR-0049)\n"
    "started  2023-11-14 22:13:20.250 (UTC)\n"
    "utc  2023-11-14T22:13:20.250Z\n"
    "core  0.5.1\n"
    "uri  elm327://<address>?profile=obd2&serial=115200&filter=7E8\n"
    "profile  obd2\n"
    "frame flags  complete\n"
    "focus  RPM\n"
    "min sweep  250 ms\n"
    "link  recovered (state 0, 1 episode)\n"
    "signal  RPM,number,rpm,0,1\n"
    "signal  VIN,text,,0,0\n"
    "caller  Havne 0.5.1\n"
    "caller  vehicle  <vehicle>\n";

// ---- frames in memory (ecuio_scrub_frames) ----

struct Mem {
    uint64_t t = 0;
    uint32_t id = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
};

struct Source {
    std::vector<Mem> frames;
    size_t calls = 0;
    size_t cancel_at = SIZE_MAX; /* the call that cancels */
    uint16_t length_past = 0;    /* a data_length to give frame 0 instead */
};

inline int give(void* user, size_t index, ecuio_frame_t* out)
{
    auto& s = *static_cast<Source*>(user);
    if (s.calls++ == s.cancel_at) {
        return 1;
    }
    const Mem& m = s.frames.at(index);
    out->timestamp_us = m.t;
    out->arbitration_id = m.id;
    out->flags = m.flags;
    out->data_length = static_cast<uint16_t>(m.data.size());
    std::memcpy(out->data, m.data.data(), m.data.size());
    if (index == 0 && s.length_past != 0) {
        out->data_length = s.length_past;
    }
    return 0;
}

struct Scrubbed {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::vector<uint8_t> keep;
    std::string detail;
    Scrubbed(Source& s, const ecuio_signal_set_t* set = nullptr)
        : keep(s.frames.size(), 7)
    {
        rc = ecuio_scrub_frames(s.frames.size(), give, &s, set, 0, keep.data(),
                                &report);
        detail = ecuio_last_error_detail();
    }
};

inline std::vector<uint8_t> none_kept(size_t n)
{
    return std::vector<uint8_t>(n, 0);
}

/* frames.csv rows of frames in memory */
inline std::string rows_of(const std::vector<Mem>& frames)
{
    std::string rows;
    for (const Mem& m : frames) {
        rows += row(m.t, m.id, m.flags, m.data);
    }
    return rows;
}

// ---- a cancel (ecuio_export_with) ----

/* Counts its asks and stops the export at the stop_at-th (0: never) */
struct Asks {
    unsigned long count = 0;
    unsigned long stop_at = 0;
};

inline int ask(void* user)
{
    auto* a = static_cast<Asks*>(user);
    ++a->count;
    return a->stop_at != 0 && a->count >= a->stop_at ? 1 : 0;
}

/* The export's kinds: the pcapng, the long and the wide CSV, the folder,
 * each with the asks whole_recording gives it: one as each pass and each
 * file of a copy begins, its files too short for one as rows are read */
struct Kind {
    const char* name;
    uint32_t format;
    uint32_t layout;
    unsigned long asks;
};
const Kind kKinds[] = {{"pcapng", ECUIO_EXPORT_PCAPNG, 0, 3},
                       {"long", ECUIO_EXPORT_CSV, ECUIO_CSV_LONG, 4},
                       {"wide", ECUIO_EXPORT_CSV, ECUIO_CSV_WIDE, 4},
                       {"folder", ECUIO_EXPORT_FOLDER, 0, 13}};

/* f's recording exported as k to <name>, through ecuio_export_with when
 * asks is given, else ecuio_export */
inline ecuio_status_t export_as(const Folder& f, const Kind& k,
                         const std::string& name, Asks* asks,
                         ecuio_export_report_t* report = nullptr)
{
    const std::string folder = f.utf8("rec");
    const std::string out = f.utf8(name);
    ecuio_export_options_t o{};
    o.format = k.format;
    o.layout = k.layout;
    o.folder_utf8 = folder.c_str();
    o.out_path_utf8 = out.c_str();
    return asks != nullptr ? ecuio_export_with(&o, ask, asks, report)
                           : ecuio_export(&o, report);
}

/* f's recording sized as the wide CSV at a sweep and a 250 ms grid */
inline ecuio_status_t estimate_of(const Folder& f, Asks* asks,
                           ecuio_export_estimate_t* out)
{
    const std::string folder = f.utf8("rec");
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.folder_utf8 = folder.c_str();
    const uint32_t intervals[2] = {0, 250};
    return asks != nullptr
               ? ecuio_export_estimate_with(&o, intervals, 2, ask, asks, out)
               : ecuio_export_estimate(&o, intervals, 2, out);
}

} // namespace
