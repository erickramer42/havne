// File/layer: core/src/operation_sink.h | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: DumpWriterTest (test_core)
//
// Where a dump's run (operation_run.h) hands its bytes: a ring the worker
// fills without waiting, allocating or touching a file, and a writer
// thread that writes the first pass to a partial file, holds the second
// to what the disk gives back, and seals the copy under its checksum's
// name (ecuio/dump.h) once every byte was read twice and found equal.
//
// The order on the disk: 128 bytes kept for the header, then each range's
// first pass, flushed to the disk when the range is whole. The second
// pass is compared with the partial read back, never with memory, so one
// compare holds the transfer and the write. The record and the header are
// written only at the seal, after the last compare: until then the file
// is a partial that no reader takes for a copy. The seal reads its record
// as a reader would before writing it, so nothing is sealed that a reader
// refuses; then it flushes, reads the whole file back against what it
// wrote, names it by that reading's checksum, sets it read-only and
// renames it without replacing.
//
// No case on a working disk tells a flush made from one left out: the
// flushes are pinned through the seam's failures alone.

#ifndef ECUIO_OPERATION_SINK_H
#define ECUIO_OPERATION_SINK_H

#include "dump_file.h"
#include "operation_plan.h"
#include "sha256.h"

#include "ecuio/core.h"
#include "ecuio/dump.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ecuio_operation {

/* TEST-ONLY: the writer's step that fails next, once; 0 none */
constexpr int kSinkFailWrite = 1;
constexpr int kSinkFailFlush = 2;
constexpr int kSinkFailReadBack = 3;
constexpr int kSinkFailRename = 4;
/* One byte of the partial changed on the disk between a range's flush
 * and its read back */
constexpr int kSinkChangeByte = 5;
/* One byte changed on the disk between the seal's flush and its read
 * back: the header's, the image's, the record's, or one added after */
constexpr int kSinkChangeHeader = 6;
constexpr int kSinkChangeImage = 7;
constexpr int kSinkChangeRecord = 8;
constexpr int kSinkAddByte = 9;
/* The folder's flush after the rename */
constexpr int kSinkFailFolderFlush = 10;
inline std::atomic<int> dump_test_fail_at{0};

/* TEST-ONLY: while set, the writer takes nothing from the ring */
inline std::atomic<bool> dump_test_hold_writer{false};

/* TEST-ONLY: called on the writer's thread before it takes each record */
inline std::atomic<void (*)()> dump_test_before_take{nullptr};

enum class sink_state : uint8_t {
    writing,
    saved,   /* name() is the copy's */
    differ,  /* differ_step() and differ_offset() say where */
    failed,  /* words() says why */
    dropped, /* abandoned or closed before a seal: nothing is kept */
};

/* What a run knows only at its end, for the record's "read" */
struct read_facts {
    uint32_t seconds = 0;
    uint32_t unlocks = 0;
    uint32_t writes_before = 0;
    uint32_t volts_mv = 0;
    uint32_t volts_min_mv = 0;
    uint32_t rpm = 0;
    uint32_t kmh = 0;
    bool has_volts = false;
    bool has_volts_min = false;
    bool has_rpm = false;
    bool has_kmh = false;
    bool parked_tick = false;
};

class dump_sink
{
public:
    static constexpr size_t kRingBytes = 256 * 1024;
    /* The most data one record carries: a 76's, a 63's */
    static constexpr size_t kDataMost = 4096;

    dump_sink() = default;
    ~dump_sink() { close(); }
    dump_sink(const dump_sink&) = delete;
    dump_sink& operator=(const dump_sink&) = delete;

    /* The caller's thread, before the run: the partial made in folder (an
     * existing one), every buffer sized and the writer started. p
     * outlives the sink. record holds what is known at the start (its
     * time, the module, the plugin, the transport); the ranges, the
     * image's and the identifiers' values are the sink's to fill.
     * A record the reader would refuse, or one too long to seal, is
     * refused here, before a byte is asked of the module.
     * ECUIO_ERR_LOAD_FAILED with *why (no path), or OUT_OF_MEMORY;
     * nothing is thrown and nothing is left behind a failure. */
    ecuio_status_t open(const plan& p, const std::string& folder,
                        const ecuio_dump_file::record& record,
                        std::string* why);

    /* The run's sink, the worker's calls: none waits, allocates or
     * touches a file. room(n) says whether data(.., n) fits now. */
    bool room(size_t n) const;
    void data(uint32_t step, uint32_t pass, uint64_t offset, const uint8_t* d,
              size_t n);
    void ident(uint32_t index, const uint8_t* d, size_t n);
    /* The run ended complete: the copy is sealed once the writer has
     * taken everything before this */
    void seal(const read_facts& facts);
    /* The run ended any other way: nothing is kept */
    void abandon();

    /* The writer's word, for any thread */
    sink_state state() const { return state_.load(std::memory_order_acquire); }
    /* Once state() says so; they stay put after it */
    uint32_t differ_step() const { return differ_step_; }
    uint64_t differ_offset() const { return differ_offset_; }
    const char* name() const { return name_; }
    const char* words() const { return words_; }
    /* Whether the writer has taken everything handed over so far */
    bool idle() const;

    /* The caller's thread: the writer joined; a partial that was never
     * sealed is removed. Safe to call twice. */
    void close();

private:
    struct item;
    void push(const item& head, const uint8_t* d, size_t n);
    bool pop(item* head);
    void run();
    void take(const item& head);
    void first_pass(const item& head);
    void second_pass(const item& head);
    void finish(const read_facts& facts);
    void put_ranges(ecuio_dump_file::record& r) const;
    bool record_reads(std::string* why) const;
    bool write_tail(const std::string& text, const uint8_t* head);
    bool read_back(const std::string& text, const uint8_t* head,
                   uint8_t file_sum[32]);
    void place(const std::string& name);
    void end(sink_state s, const char* words);
    void test_change(uint64_t at, bool add);
    void let_go();

    const plan* p_ = nullptr;
    std::string folder_;
    std::string partial_; /* the file to remove unless the copy is kept */
    ecuio_dump_file::record record_;
    std::vector<uint64_t> base_; /* each step's place in the image */
    uint64_t total_ = 0;
    FILE* file_ = nullptr;  /* the partial, written */
    FILE* back_ = nullptr;  /* the same file, read back */
    uint64_t written_ = 0;  /* image bytes of the first pass */
    uint64_t compared_ = 0; /* image bytes of the second */
    ecuio_sha256::hasher image_;
    std::vector<uint8_t> payload_; /* the record being taken */
    std::vector<uint8_t> read_;    /* what the disk gives back */

    std::vector<uint8_t> ring_;
    std::atomic<uint64_t> head_{0}; /* the worker's: bytes ever put */
    std::atomic<uint64_t> tail_{0}; /* the writer's: bytes ever taken */
    std::mutex wake_m_;
    std::condition_variable wake_;
    std::atomic<bool> stop_{false};
    std::thread writer_;

    std::atomic<sink_state> state_{sink_state::writing};
    uint32_t differ_step_ = 0;
    uint64_t differ_offset_ = 0;
    char name_[ECUIO_DUMP_NAME_MAX] = {};
    char words_[160] = {};
};

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_SINK_H */
