// File/layer: fuzz/fuzz_recording_reads.h | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (the check and the frame reads)

#pragma once

#include "fuzz_recording_common.h"

namespace {

struct checked {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    ecuio_check_summary_t summary{};
    uint64_t passed[8] = {};
    uint64_t seen = 0;
    uint64_t last_end = 0;   /* the latest at_us + length_us passed */
    uint64_t stop_after = 0; /* 0: never stop */
};

int on_finding(const ecuio_check_finding_t* f, void* user)
{
    auto* c = static_cast<checked*>(user);
    if (f->kind < ECUIO_CHECK_GAP || f->kind > ECUIO_CHECK_ASLEEP ||
        f->reserved[0] != 0 || f->reserved[1] != 0) {
        abort();
    }
    const bool whole = (f->flags & ECUIO_CHECK_FINDING_WHOLE_BUS) != 0;
    if ((whole && f->arbitration_id != 0) ||
        (whole && (f->flags & ECUIO_CHECK_FINDING_EXTENDED) != 0) ||
        (f->kind != ECUIO_CHECK_LINK && f->code != 0) ||
        f->at_us + f->length_us < f->at_us) {
        abort();
    }
    if (f->kind == ECUIO_CHECK_ISOTP &&
        !ecuio_check::iso15765_id(f->arbitration_id,
                                  (f->flags & ECUIO_CHECK_FINDING_EXTENDED) !=
                                      0)) {
        abort();
    }
    ++c->passed[f->kind];
    ++c->seen;
    if (f->at_us + f->length_us > c->last_end) {
        c->last_end = f->at_us + f->length_us;
    }
    return c->stop_after != 0 && c->seen >= c->stop_after ? 1 : 0;
}

checked check_folder(uint64_t stop_after)
{
    checked c;
    c.stop_after = stop_after;
    c.rc = ecuio_recording_check(folder().c_str(), nullptr, nullptr, on_finding,
                                 &c, &c.summary);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-recording") != nullptr) {
        abort();
    }
    c.detail = detail;
    if (c.rc == ECUIO_ERR_LOAD_FAILED) {
        return c;
    }
    if (c.rc != ECUIO_OK && !(c.rc == ECUIO_ERR_CANCELED && stop_after != 0 &&
                              c.seen == stop_after)) {
        abort();
    }
    for (uint32_t k = 1; k < 8; ++k) {
        if (c.summary.counts[k] != c.passed[k]) {
            abort();
        }
    }
    if (c.summary.counts[0] != 0 || c.summary.torn_rows > 1 ||
        c.summary.ids > c.summary.frames) {
        abort();
    }
    return c;
}

/* One row as ecuio_recording_read_frames handed it on */
struct frame_row {
    uint64_t host_us;
    uint64_t timestamp_us;
    uint32_t id;
    uint32_t flags;
    uint32_t length;
    uint64_t sum; /* the bytes' FNV-1a */
    bool operator==(const frame_row& o) const
    {
        return host_us == o.host_us && timestamp_us == o.timestamp_us &&
               id == o.id && flags == o.flags && length == o.length &&
               sum == o.sum;
    }
};

struct read_back {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    ecuio_frames_read_t summary{};
    std::vector<frame_row> rows;
    size_t stop_after = 0; /* 0: never stop */
};

int on_frame_row(void* user, const ecuio_frame_t* f, uint64_t host_us)
{
    auto* r = static_cast<read_back*>(user);
    if (f->data_length > ECUIO_FRAME_DATA_MAX || f->reserved != 0) {
        abort();
    }
    uint64_t sum = 14695981039346656037ULL;
    for (uint32_t i = 0; i < f->data_length; ++i) {
        sum = (sum ^ f->data[i]) * 1099511628211ULL;
    }
    for (uint32_t i = f->data_length; i < ECUIO_FRAME_DATA_MAX; ++i) {
        if (f->data[i] != 0) {
            abort();
        }
    }
    r->rows.push_back(frame_row{host_us, f->timestamp_us, f->arbitration_id,
                                f->flags, f->data_length, sum});
    return r->stop_after != 0 && r->rows.size() >= r->stop_after ? 1 : 0;
}

read_back read_frames(uint64_t from_us, uint64_t to_us, size_t stop_after)
{
    read_back r;
    r.stop_after = stop_after;
    r.rc = ecuio_recording_read_frames(folder().c_str(), from_us, to_us,
                                       on_frame_row, &r, &r.summary);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-recording") != nullptr ||
        r.summary.rows != r.rows.size() || r.summary.torn_rows > 1 ||
        r.summary.reserved0 != 0 || r.summary.reserved[0] != 0 ||
        r.summary.reserved[1] != 0) {
        abort();
    }
    r.detail = detail;
    if (r.rc != ECUIO_OK && r.rc != ECUIO_ERR_LOAD_FAILED &&
        !(r.rc == ECUIO_ERR_CANCELED && stop_after != 0 &&
          r.rows.size() == stop_after)) {
        abort();
    }
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (r.rows[i].host_us < from_us ||
            (to_us != 0 && r.rows[i].host_us > to_us) ||
            (i > 0 && r.rows[i].host_us < r.rows[i - 1].host_us)) {
            abort();
        }
    }
    return r;
}

/* The whole file read, then stretches of it: each the whole read's rows
 * in its range, found by the search as a plain scan finds them */
void read_frames_back(const uint8_t* data, size_t size)
{
    const read_back whole = read_frames(0, 0, 0);
    const read_back again = read_frames(0, 0, 0);
    if (whole.rc != again.rc || whole.detail != again.detail ||
        !(whole.rows == again.rows)) {
        abort();
    }
    if (whole.rc != ECUIO_OK || whole.rows.empty()) {
        return;
    }
    const size_t n = whole.rows.size();
    const size_t a = (size > 1 ? data[size - 1] : 0) % n;
    const size_t b = a + (size > 2 ? data[size - 2] : 0) % (n - a);
    const uint64_t from_us = whole.rows[a].host_us + (data[0] & 1);
    const uint64_t to_us = whole.rows[b].host_us;
    if (to_us != 0 && to_us < from_us) {
        return;
    }
    const read_back part = read_frames(from_us, to_us, 0);
    if (part.rc != ECUIO_OK) {
        abort();
    }
    std::vector<frame_row> want;
    for (const frame_row& row : whole.rows) {
        if (row.host_us >= from_us && (to_us == 0 || row.host_us <= to_us)) {
            want.push_back(row);
        }
    }
    if (!(part.rows == want)) {
        abort();
    }
    const read_back stopped = read_frames(0, 0, 1);
    if (stopped.rc != ECUIO_ERR_CANCELED || stopped.rows.size() != 1 ||
        !(stopped.rows[0] == whole.rows[0])) {
        abort();
    }
}

} // namespace
