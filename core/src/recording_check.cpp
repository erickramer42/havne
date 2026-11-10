// File/layer: core/src/recording_check.cpp | C++17, Qt-free; the frames
//             check (ADR-0049 section 6)
// Testing: UNIT: RecordingCheckTest (test_core); FUZZ: fuzz_recording
//
// One pass: events.log first (whether its frame flags are complete, a
// focus's minimum time between sweeps, and its link and asleep lines,
// which are few), then frames.csv a row at a
// time into recording_check.h's checker, which keeps per ID only a fixed
// handful of numbers.

#include "error_detail.h"
#include "frames_csv.h"
#include "recording_check.h"
#include "recording_rows.h"

#include "ecuio/recording.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace rows = ecuio_recording_rows;
namespace check = ecuio_check;
namespace csv = ecuio_frames_csv;

namespace {

constexpr uint64_t kProgressBytes = 4u << 20;

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
};
using file_ptr = std::unique_ptr<FILE, file_closer>;

file_ptr open_file(const std::string& utf8)
{
    csv::native_path p;
    if (!p.set(utf8)) {
        return nullptr;
    }
    return file_ptr(p.open());
}

/* One line of events.log, its ending dropped; a line past the buffer is
 * skipped whole, and a torn last one left out. False at the end. */
bool next_event_line(FILE* f, std::vector<char>& buf, size_t* n)
{
    for (;;) {
        bool too_long = false;
        bool torn = false;
        if (!csv::read_line(f, buf, &too_long, &torn)) {
            return false;
        }
        if (too_long) {
            do {
                if (!csv::read_line(f, buf, &too_long, &torn)) {
                    return false;
                }
            } while (too_long);
            continue;
        }
        if (torn) {
            return false;
        }
        *n = std::strlen(buf.data());
        return true;
    }
}

/* events.log: whether its header says the frame flags are complete, the
 * focus's minimum time between sweeps it names (0 for none), and its link
 * and asleep lines. A folder without one has none of them. */
void read_events(const std::string& folder, bool* complete,
                 uint64_t* min_sweep_us, std::vector<check::event>& out)
{
    *complete = false;
    *min_sweep_us = 0;
    file_ptr f = open_file(folder + "/events.log");
    if (f == nullptr) {
        return;
    }
    std::vector<char> buf(rows::kRowMax + 2);
    uint32_t format = ECUIO_RECORDING_FORMAT_UNKNOWN;
    bool in_header = false;
    bool first = true;
    size_t n = 0;
    while (next_event_line(f.get(), buf, &n)) {
        const char* p = buf.data();
        if (first) {
            first = false;
            const size_t bom = rows::bom_length(p, n);
            p += bom;
            n -= bom;
            const size_t m = std::strlen(rows::kFormat1End);
            /* Format 3 keeps format 2's shape for every line read here */
            if (rows::equals(p, n, rows::kFormat2) ||
                rows::equals(p, n, rows::kFormat3)) {
                format = ECUIO_RECORDING_FORMAT_CORE;
                in_header = true;
                continue;
            }
            if (n >= m && std::memcmp(p + n - m, rows::kFormat1End, m) == 0) {
                format = ECUIO_RECORDING_FORMAT_OLDER;
                in_header = true;
                continue;
            }
        }
        if (in_header) {
            if (n == 0) {
                in_header = false;
            } else if (format == ECUIO_RECORDING_FORMAT_CORE) {
                if (rows::equals(p, n, "frame flags  complete")) {
                    *complete = true;
                } else {
                    (void)check::parse_min_sweep(p, n, min_sweep_us);
                }
            }
            continue;
        }
        rows::event_line e;
        if (!rows::parse_event_line(p, n, &e)) {
            continue;
        }
        check::event ev{};
        ev.at_us = e.ms * 1000;
        const char* text = e.rest;
        size_t text_len = e.rest_len;
        size_t kind_len = 0;
        if (format == ECUIO_RECORDING_FORMAT_CORE) {
            if (!rows::split_kind(e.rest, e.rest_len, &text, &text_len,
                                  &kind_len)) {
                continue;
            }
            if (rows::equals(e.rest, kind_len, "link") &&
                check::parse_link(text, text_len, &ev.code, &ev.episodes)) {
                ev.kind = ECUIO_CHECK_LINK;
                out.push_back(ev);
            } else if (rows::equals(e.rest, kind_len, "asleep") &&
                       check::parse_asleep(text, text_len, &ev.length_us)) {
                ev.kind = ECUIO_CHECK_ASLEEP;
                out.push_back(ev);
            }
        } else if (check::parse_asleep(text, text_len, &ev.length_us)) {
            /* ADR-0017's lines have no kind: the app's sleep line by its
             * wording */
            ev.kind = ECUIO_CHECK_ASLEEP;
            out.push_back(ev);
        }
    }
}

struct sink_context {
    ecuio_check_finding_callback_t finding;
    void* user;
};

bool to_callback(const ecuio_check_finding_t& f, void* user)
{
    const auto* s = static_cast<const sink_context*>(user);
    return s->finding == nullptr || s->finding(&f, s->user) == 0;
}

ecuio_status_t refuse_row(uint64_t line_no, const char* why)
{
    ecuio_detail_set("frames.csv line %llu: %s",
                     static_cast<unsigned long long>(line_no), why);
    return ECUIO_ERR_LOAD_FAILED;
}

ecuio_status_t run(const std::string& folder,
                   const ecuio_check_options_t& options,
                   ecuio_open_progress_t progress,
                   ecuio_check_finding_callback_t finding, void* user,
                   ecuio_check_summary_t* out)
{
    file_ptr f = open_file(folder + "/frames.csv");
    if (f == nullptr) {
        ecuio_detail_set("the folder has no frames.csv");
        return ECUIO_ERR_LOAD_FAILED;
    }
    bool complete = false;
    uint64_t min_sweep_us = 0;
    std::vector<check::event> events;
    read_events(folder, &complete, &min_sweep_us, events);
    sink_context context{finding, user};
    check::checker c(options, std::move(events), complete, to_callback,
                     &context, min_sweep_us);
    auto stop = [&](ecuio_status_t rc) {
        c.complete();
        *out = c.summary();
        return rc;
    };

    const uint64_t total = csv::file_size(f.get());
    if (progress != nullptr && progress(0, total, user) != 0) {
        return stop(ECUIO_ERR_CANCELED);
    }
    std::vector<char> buf(csv::kLineMax + 2);
    if (!csv::read_header(f.get(), buf)) {
        return ECUIO_ERR_LOAD_FAILED;
    }
    std::unique_ptr<ecuio_frame_t> frame(new ecuio_frame_t);
    uint64_t line_no = 1;
    uint64_t bytes = std::strlen(buf.data()) + 1;
    uint64_t next_progress = kProgressBytes;
    uint64_t torn_rows = 0;
    uint64_t prev_host_us = 0;
    for (;;) {
        bool too_long = false;
        bool torn = false;
        if (!csv::read_line(f.get(), buf, &too_long, &torn)) {
            break;
        }
        ++line_no;
        if (too_long) {
            return refuse_row(line_no, "the row is longer than a frame's");
        }
        if (torn) {
            torn_rows = 1; /* a write cut short: dropped (section 7) */
            break;
        }
        const size_t n = std::strlen(buf.data());
        bytes += n + 1;
        csv::row r;
        const char* why = nullptr;
        std::memset(frame.get(), 0, sizeof *frame);
        if (!csv::parse_row(buf.data(), &r, frame.get(), &why)) {
            return refuse_row(line_no, why);
        }
        if (r.host_us < prev_host_us) {
            return refuse_row(line_no,
                              "host_us is earlier than the row before");
        }
        prev_host_us = r.host_us;
        if (!c.frame(r.host_us, frame->arbitration_id, frame->flags,
                     frame->data, frame->data_length)) {
            return stop(ECUIO_ERR_CANCELED);
        }
        if (progress != nullptr && bytes >= next_progress) {
            next_progress = bytes + kProgressBytes;
            if (progress(bytes, total, user) != 0) {
                return stop(ECUIO_ERR_CANCELED);
            }
        }
    }
    if (!c.finish()) {
        return stop(ECUIO_ERR_CANCELED);
    }
    *out = c.summary();
    out->torn_rows = torn_rows;
    if (progress != nullptr && progress(total, total, user) != 0) {
        return ECUIO_ERR_CANCELED;
    }
    return ECUIO_OK;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_recording_check(const char* folder_utf8,
                                     const ecuio_check_options_t* options,
                                     ecuio_open_progress_t progress,
                                     ecuio_check_finding_callback_t finding,
                                     void* user_data,
                                     ecuio_check_summary_t* out)
{
    if (out != nullptr) {
        *out = ecuio_check_summary_t{};
    }
    if (folder_utf8 == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_check_options_t o{};
    if (options != nullptr) {
        o = *options;
        bool reserved = false;
        for (uint64_t r : o.reserved) {
            reserved = reserved || r != 0;
        }
        if (o.flags != 0 || reserved ||
            (o.rate_drop_x100 != 0 && o.rate_drop_x100 <= 100)) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    ecuio_detail_clear();
    try {
        std::string folder = folder_utf8;
        while (folder.size() > 1 &&
               (folder.back() == '/' || folder.back() == '\\')) {
            folder.pop_back();
        }
        return run(folder, o, progress, finding, user_data, out);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

} /* extern "C" */
