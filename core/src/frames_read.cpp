// File/layer: core/src/frames_read.cpp | core, C++17, Qt-free; ALL platforms
// Testing: UNIT: FramesReadTest (test_core); FUZZ: fuzz_recording
//
// A recording's frames read back as it recorded them (ADR-0049's
// appendix "frames read back"): frames.csv's rows in a host_us range,
// each handed to the caller's callback on its thread. host_us never goes
// back in a frames.csv (the recorder stamps it on one worker, the candump
// import gives an early row the previous row's time, and every reader
// refuses a backward row), so the range's first row is found by a binary
// search on the file's offsets: a stretch costs its own rows.

#include "ecuio/recording.h"

#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

namespace fc = ecuio_frames_csv;
namespace files = ecuio_files;

/* The search stops once the range's first row lies within this many
 * bytes, which a forward scan then reads */
constexpr uint64_t kScanBytes = 64 * 1024;

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};

/* The file's position, past 2 GiB too */
uint64_t tell(FILE* f)
{
#ifdef _WIN32
    const __int64 n = _ftelli64(f);
#else
    const off_t n = ftello(f);
#endif
    return n > 0 ? static_cast<uint64_t>(n) : 0;
}

void refuse_row(uint64_t at, const char* why)
{
    ecuio_detail_set("frames.csv, the row at byte %llu: %s",
                     static_cast<unsigned long long>(at), why);
}

class rows_reader
{
public:
    explicit rows_reader(FILE* f)
        : f_(f), buf_(fc::kLineMax + 2), frame_(new ecuio_frame_t{})
    {}

    std::vector<char>& buf() { return buf_; }

    /* The next row from where the file is: 1 a row (its offset in *at),
     * 0 the end (*torn for a last row with no line end), -1 refused */
    int next(uint64_t* at, bool* torn)
    {
        *at = tell(f_);
        bool too_long = false;
        if (!fc::read_line(f_, buf_, &too_long, torn) || *torn) {
            return 0;
        }
        if (too_long) {
            refuse_row(*at, "the row is longer than a frame's");
            return -1;
        }
        const char* why = nullptr;
        std::memset(frame_.get(), 0, sizeof *frame_);
        if (!fc::parse_row(buf_.data(), &row_, frame_.get(), &why)) {
            refuse_row(*at, why);
            return -1;
        }
        return 1;
    }

    /* The first row starting at or after offset, which lies inside the
     * rows (past the header): 1 with its offset and host_us, 0 none,
     * -1 refused */
    int first_at(uint64_t offset, uint64_t* at)
    {
        if (!fc::seek_to(f_, offset)) {
            ecuio_detail_set("frames.csv can't be read at byte %llu",
                             static_cast<unsigned long long>(offset));
            return -1;
        }
        bool too_long = false;
        bool torn = false;
        /* The rest of the row the offset falls in */
        if (!fc::read_line(f_, buf_, &too_long, &torn) || torn) {
            return 0;
        }
        return next(at, &torn);
    }

    uint64_t host_us() const { return row_.host_us; }
    const ecuio_frame_t* frame() const { return frame_.get(); }

private:
    FILE* f_;
    std::vector<char> buf_;
    std::unique_ptr<ecuio_frame_t> frame_;
    fc::row row_;
};

ecuio_status_t read_frames(const char* folder_utf8, uint64_t from_us,
                           uint64_t to_us, ecuio_frame_row_callback_t callback,
                           void* user_data, ecuio_frames_read_t& s)
{
    std::string path = folder_utf8;
    fc::to_file(path);
    if (!files::exists(path)) {
        ecuio_detail_set("the folder has no frames.csv");
        return ECUIO_ERR_LOAD_FAILED;
    }
    std::string why;
    std::unique_ptr<FILE, file_closer> f(files::open_file(path, false, &why));
    if (!f) {
        ecuio_detail_set("the recording's frames.csv can't be opened: %s",
                         why.c_str());
        return ECUIO_ERR_LOAD_FAILED;
    }
    const uint64_t size = fc::file_size(f.get());
    rows_reader rows(f.get());
    if (!fc::read_header(f.get(), rows.buf())) {
        return ECUIO_ERR_LOAD_FAILED;
    }
    const uint64_t data_start = tell(f.get());

    /* lo is always the header's end or a row before from_us */
    uint64_t lo = data_start;
    if (from_us > 0) {
        uint64_t hi = size;
        while (hi > lo && hi - lo > kScanBytes) {
            const uint64_t mid = lo + (hi - lo) / 2;
            uint64_t at = 0;
            const int got = rows.first_at(mid, &at);
            if (got < 0) {
                return ECUIO_ERR_LOAD_FAILED;
            }
            if (got == 0 || rows.host_us() >= from_us || at >= hi) {
                hi = mid;
            } else {
                lo = at;
            }
        }
    }
    if (!fc::seek_to(f.get(), lo)) {
        ecuio_detail_set("frames.csv can't be read at byte %llu",
                         static_cast<unsigned long long>(lo));
        return ECUIO_ERR_LOAD_FAILED;
    }

    uint64_t prev = 0;
    for (;;) {
        uint64_t at = 0;
        bool torn = false;
        const int got = rows.next(&at, &torn);
        if (got < 0) {
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (got == 0) {
            s.torn_rows = torn ? 1 : 0;
            return ECUIO_OK;
        }
        const uint64_t host_us = rows.host_us();
        if (host_us < prev) {
            refuse_row(at, "host_us is earlier than the row before");
            return ECUIO_ERR_LOAD_FAILED;
        }
        prev = host_us;
        if (host_us < from_us) {
            continue;
        }
        if (to_us != 0 && host_us > to_us) {
            return ECUIO_OK;
        }
        ++s.rows;
        if (callback(user_data, rows.frame(), host_us) != 0) {
            return ECUIO_ERR_CANCELED;
        }
    }
}

} // namespace

extern "C" ecuio_status_t
ecuio_recording_read_frames(const char* folder_utf8, uint64_t from_us,
                            uint64_t to_us, ecuio_frame_row_callback_t callback,
                            void* user_data, ecuio_frames_read_t* summary)
{
    ecuio_frames_read_t s{};
    if (summary != nullptr) {
        *summary = s;
    }
    if (folder_utf8 == nullptr || callback == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (to_us != 0 && to_us < from_us) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_detail_clear();
    ecuio_status_t status = ECUIO_ERR_OUT_OF_MEMORY;
    try {
        status =
            read_frames(folder_utf8, from_us, to_us, callback, user_data, s);
    } catch (const std::bad_alloc&) {
        status = ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (summary != nullptr) {
        *summary = s;
    }
    return status;
}
