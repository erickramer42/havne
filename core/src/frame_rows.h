// File/layer: core/src/frame_rows.h | internal, C++17, Qt-free; a
//             recording's frames.csv read a row at a time
// Testing: UNIT: ExportTest, FolderExportTest, HealthTest (test_core)
//
// The rows an export and the health report read, in order, each parsed
// into one frame the reader holds: frames_csv.h's rules, a row earlier
// than the one before refused, a torn last row left out.

#ifndef ECUIO_FRAME_ROWS_H
#define ECUIO_FRAME_ROWS_H

#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"

#include "ecuio/core.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ecuio_frame_rows {

/* frames.csv's rows in order; a torn last row is left out (ADR-0049
 * section 7) */
class frame_rows
{
public:
    frame_rows()
        : buf_(ecuio_frames_csv::kLineMax + 2), frame_(new ecuio_frame_t{})
    {}

    bool open(const std::string& folder)
    {
        std::string why;
        f_.reset(ecuio_files::open_file(folder + "/frames.csv", false, &why));
        if (!f_) {
            ecuio_detail_set("the recording's frames.csv can't be opened: %s",
                             why.c_str());
            return false;
        }
        if (!ecuio_frames_csv::read_header(f_.get(), buf_)) {
            return false;
        }
        line_ = 1;
        prev_ = 0;
        return true;
    }

    /* 1 a row, 0 the end, -1 a row refused (the detail set) */
    int next()
    {
        bool too_long = false;
        bool torn = false;
        if (!ecuio_frames_csv::read_line(f_.get(), buf_, &too_long, &torn) ||
            torn) {
            return 0;
        }
        ++line_;
        if (too_long) {
            return refuse("the row is longer than a frame's");
        }
        const char* why = nullptr;
        std::memset(frame_.get(), 0, sizeof *frame_);
        if (!ecuio_frames_csv::parse_row(buf_.data(), &row_, frame_.get(),
                                         &why)) {
            return refuse(why);
        }
        if (row_.host_us < prev_) {
            return refuse("host_us is earlier than the row before");
        }
        prev_ = row_.host_us;
        return 1;
    }

    uint64_t host_us() const { return row_.host_us; }
    /* The row as it was read, its line end taken off */
    const char* line() const { return buf_.data(); }
    const ecuio_frame_t& frame() const { return *frame_; }

private:
    struct closer {
        void operator()(FILE* f) const
        {
            if (f != nullptr) {
                (void)std::fclose(f);
            }
        }
    };

    int refuse(const char* why)
    {
        ecuio_detail_set("frames.csv line %llu: %s",
                         static_cast<unsigned long long>(line_), why);
        return -1;
    }

    std::unique_ptr<FILE, closer> f_;
    std::vector<char> buf_;
    std::unique_ptr<ecuio_frame_t> frame_;
    ecuio_frames_csv::row row_;
    uint64_t line_ = 0;
    uint64_t prev_ = 0;
};

} // namespace ecuio_frame_rows

#endif /* ECUIO_FRAME_ROWS_H */
