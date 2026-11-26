// File/layer: core/src/operation_sink.cpp | internal, C++17, Qt-free, every
//             platform
// Testing: UNIT: DumpWriterTest (test_core)
//
// The dump's ring and its writer thread (operation_sink.h): the first
// pass written, the second held to the disk, the copy sealed.

#include "operation_sink.h"

#include "dump_handle.h"
#include "folder_files.h"
#include "frames_csv.h"
#include "store_file.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <new>
#include <system_error>

namespace files = ecuio_files;

static_assert(ECUIO_STEP_READ == ECUIO_DUMP_STEP_READ &&
                  ECUIO_STEP_UPLOAD == ECUIO_DUMP_STEP_UPLOAD &&
                  ECUIO_STEP_SESSION_EXTENDED == ECUIO_DUMP_SESSION_EXTENDED &&
                  ECUIO_STEP_SESSION_PROGRAMMING ==
                      ECUIO_DUMP_SESSION_PROGRAMMING,
              "a plan's step and session numbers are the record's");

namespace ecuio_operation {

namespace {

using ecuio_dump_file::kHeaderBytes;

enum : uint8_t { kData = 1, kIdent, kSeal, kAbandon };

/* Room kept for what follows no room() call: the identifiers' values,
 * a seal and an abandon */
constexpr size_t kReserve = 2048;

/* Whether the seam fails this step, taking the failure if so */
bool seam(int step)
{
    return dump_test_fail_at.compare_exchange_strong(step, 0);
}

void copy_words(char* out, size_t cap, const char* s)
{
    const size_t n = (std::min)(cap - 1, std::strlen(s));
    std::memcpy(out, s, n);
    out[n] = '\0';
}

} // namespace

/* One record's head in the ring; its size bytes of payload follow */
struct dump_sink::item {
    uint8_t kind = 0;
    uint8_t pass = 0;
    uint16_t zero = 0;
    uint32_t step = 0; /* an identifier's index too */
    uint64_t offset = 0;
    uint32_t size = 0;
    uint32_t zero2 = 0;
};

ecuio_status_t dump_sink::open(const plan& p, const std::string& folder,
                               const ecuio_dump_file::record& record,
                               std::string* why)
{
    try {
        p_ = &p;
        folder_ = folder;
        record_ = record;
        record_.ranges.clear();
        record_.idents.clear();
        for (const ecuio_operation::ident& d : p.idents) {
            ecuio_dump_file::ident made;
            made.did = d.did;
            made.role = d.role;
            record_.idents.push_back(std::move(made));
        }
        base_.clear();
        total_ = 0;
        for (const step& s : p.steps) {
            base_.push_back(total_);
            total_ += s.bytes;
        }
        std::string words;
        if (!record_reads(&words)) {
            *why = "the copy can't be started: " + words;
            return ECUIO_ERR_LOAD_FAILED;
        }
        ring_.resize(kRingBytes);
        payload_.resize(kDataMost);
        read_.resize((std::max)(size_t{ECUIO_DUMP_RECORD_MAX}, kDataMost));
        /* The partial's name starts as a copy's of this day and module,
         * so a folder's list shows it as unfinished */
        const uint8_t none[32] = {};
        const std::string out =
            folder + "/" +
            ecuio_dump_file::name_of(record_.utc, record_.tx_id, none);
        words.clear();
        file_ = files::partial_file(out, &partial_, &words);
        if (file_ == nullptr) {
            partial_.clear(); /* not this call's: never removed */
            *why = "the copy can't be started: " + words;
            return ECUIO_ERR_LOAD_FAILED;
        }
        const uint8_t kept[kHeaderBytes] = {};
        back_ = files::open_file(partial_, false, &words);
        if (back_ == nullptr ||
            std::fwrite(kept, 1, sizeof kept, file_) != sizeof kept) {
            let_go();
            *why = "the copy can't be started: " +
                   (words.empty() ? files::errno_words(errno) : words);
            return ECUIO_ERR_LOAD_FAILED;
        }
        std::setvbuf(back_, nullptr, _IONBF, 0); /* each read is the disk's */
        writer_ = std::thread(&dump_sink::run, this);
        return ECUIO_OK;
    } catch (const std::bad_alloc&) {
        let_go();
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (const std::system_error&) {
        let_go();
        *why = "the copy can't be started: no thread could be made";
        return ECUIO_ERR_LOAD_FAILED;
    }
}

/* The plan's steps as r's ranges, each at its place in the image */
void dump_sink::put_ranges(ecuio_dump_file::record& r) const
{
    r.ranges.clear();
    for (size_t i = 0; i < p_->steps.size(); ++i) {
        const step& s = p_->steps[i];
        ecuio_dump_file::range g;
        g.name = s.name;
        g.step = s.kind;
        g.session = s.session;
        g.address = s.address;
        g.bytes = s.bytes;
        g.offset = base_[i];
        r.ranges.push_back(std::move(g));
    }
}

/* Whether the record a seal would write reads back, judged at the start
 * on what is known then: the ranges, each identifier with a value of one
 * byte, and room left for the longest values and the run's facts */
bool dump_sink::record_reads(std::string* why) const
{
    ecuio_dump_file::record trial = record_;
    put_ranges(trial);
    trial.image_bytes = total_;
    for (ecuio_dump_file::ident& d : trial.idents) {
        d.value.assign(1, 0);
    }
    const std::string text = ecuio_dump_file::record_text(trial);
    /* each value's other bytes in hex, and the facts' digits */
    const size_t more =
        trial.idents.size() * 2 * (ECUIO_DUMP_IDENT_MAX - 1) + 256;
    if (text.size() + more > ECUIO_DUMP_RECORD_MAX) {
        *why = "its record is longer than a copy holds";
        return false;
    }
    ecuio_dump_file::header h;
    h.image_bytes = total_;
    h.record_bytes = static_cast<uint32_t>(text.size());
    ecuio_sha256::hasher sum;
    sum.add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    sum.finish(h.record_sha256);
    ecuio_dump_file::record again;
    std::string refusal;
    const ecuio_dump_file::judged j = ecuio_dump_file::read_record(
        text.data(), text.size(), h, &again, &refusal);
    if (j == ecuio_dump_file::judged::out_of_memory) {
        throw std::bad_alloc();
    }
    if (j != ecuio_dump_file::judged::ok) {
        *why = "its record isn't one a copy holds: " + refusal;
        return false;
    }
    return true;
}

bool dump_sink::room(size_t n) const
{
    const uint64_t used = head_.load(std::memory_order_relaxed) -
                          tail_.load(std::memory_order_acquire);
    return n <= kDataMost && ring_.size() - used >= sizeof(item) + n + kReserve;
}

bool dump_sink::idle() const
{
    return tail_.load(std::memory_order_acquire) ==
           head_.load(std::memory_order_acquire);
}

/* A record put whole or not at all: one that doesn't fit is the writer's
 * to report, since the worker waits for nothing */
void dump_sink::push(const item& head, const uint8_t* d, size_t n)
{
    const uint64_t at = head_.load(std::memory_order_relaxed);
    const uint64_t used = at - tail_.load(std::memory_order_acquire);
    item it = head;
    if (n > kDataMost || ring_.size() - used < sizeof it + n) {
        it.kind = 0; /* lost: the writer ends the copy */
        n = 0;
        if (ring_.size() - used < sizeof it) {
            return;
        }
    }
    it.size = static_cast<uint32_t>(n);
    const size_t cap = ring_.size();
    const auto put = [this, cap](uint64_t pos, const void* src, size_t len) {
        const size_t to = static_cast<size_t>(pos % cap);
        const size_t first = (std::min)(len, cap - to);
        std::memcpy(ring_.data() + to, src, first);
        if (len > first) {
            std::memcpy(ring_.data(), static_cast<const uint8_t*>(src) + first,
                        len - first);
        }
    };
    put(at, &it, sizeof it);
    if (n > 0) {
        put(at + sizeof it, d, n);
    }
    head_.store(at + sizeof it + n, std::memory_order_release);
    wake_.notify_one();
}

void dump_sink::data(uint32_t step, uint32_t pass, uint64_t offset,
                     const uint8_t* d, size_t n)
{
    item it;
    it.kind = kData;
    it.pass = static_cast<uint8_t>(pass);
    it.step = step;
    it.offset = offset;
    push(it, d, n);
}

void dump_sink::ident(uint32_t index, const uint8_t* d, size_t n)
{
    item it;
    it.kind = kIdent;
    it.step = index;
    push(it, d, n);
}

void dump_sink::seal(const read_facts& facts)
{
    item it;
    it.kind = kSeal;
    push(it, reinterpret_cast<const uint8_t*>(&facts), sizeof facts);
}

void dump_sink::abandon()
{
    item it;
    it.kind = kAbandon;
    push(it, nullptr, 0);
}

/* The record at the ring's tail into *head and payload_; the tail moves
 * only once it is taken, so room() never counts it free early */
bool dump_sink::pop(item* head)
{
    const uint64_t at = tail_.load(std::memory_order_relaxed);
    if (at == head_.load(std::memory_order_acquire)) {
        return false;
    }
    const size_t cap = ring_.size();
    const auto get = [this, cap](uint64_t pos, void* dst, size_t len) {
        const size_t from = static_cast<size_t>(pos % cap);
        const size_t first = (std::min)(len, cap - from);
        std::memcpy(dst, ring_.data() + from, first);
        if (len > first) {
            std::memcpy(static_cast<uint8_t*>(dst) + first, ring_.data(),
                        len - first);
        }
    };
    get(at, head, sizeof *head);
    get(at + sizeof *head, payload_.data(), head->size);
    return true;
}

void dump_sink::run()
{
    for (;;) {
        item head;
        if (dump_test_hold_writer.load() || !pop(&head)) {
            if (stop_.load(std::memory_order_acquire)) {
                return;
            }
            std::unique_lock<std::mutex> lock(wake_m_);
            wake_.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }
        if (state() == sink_state::writing) {
            try {
                if (const auto hook = dump_test_before_take.load()) {
                    hook();
                }
                take(head);
            } catch (const std::bad_alloc&) {
                end(sink_state::failed,
                    "there wasn't the memory to finish the copy");
            }
        }
        tail_.store(tail_.load(std::memory_order_relaxed) + sizeof head +
                        head.size,
                    std::memory_order_release);
    }
}

/* One record, the copy still being written */
void dump_sink::take(const item& head)
{
    switch (head.kind) {
    case kData:
        if (head.step >= p_->steps.size() || head.pass > 1) {
            end(sink_state::failed, "the copy's bytes came out of order");
        } else if (head.pass == 0) {
            first_pass(head);
        } else {
            second_pass(head);
        }
        break;
    case kIdent:
        if (head.step < record_.idents.size()) {
            record_.idents[head.step].value.assign(
                payload_.begin(), payload_.begin() + head.size);
        }
        break;
    case kSeal: {
        read_facts facts;
        std::memcpy(&facts, payload_.data(), sizeof facts);
        finish(facts);
        break;
    }
    case kAbandon:
        end(sink_state::dropped, "");
        break;
    default:
        end(sink_state::failed, "more was handed over than the copy's "
                                "ring holds");
        break;
    }
}

/* A piece of a range's first pass: written where the one before ended,
 * and the range flushed to the disk when it is whole */
void dump_sink::first_pass(const item& head)
{
    const step& s = p_->steps[head.step];
    if (base_[head.step] + head.offset != written_ ||
        head.offset + head.size > s.bytes) {
        end(sink_state::failed, "the copy's bytes came out of order");
        return;
    }
    if (seam(kSinkFailWrite) ||
        std::fwrite(payload_.data(), 1, head.size, file_) != head.size) {
        end(sink_state::failed, "the copy can't be written");
        return;
    }
    image_.add(payload_.data(), head.size);
    written_ += head.size;
    if (head.offset + head.size == s.bytes &&
        (seam(kSinkFailFlush) || !ecuio_store_file::flush_to_disk(file_))) {
        end(sink_state::failed, "the copy can't be flushed to the disk");
    }
}

/* TEST-ONLY: the byte at the file's place at becomes another than the
 * one written, or with add one more follows the file's last */
void dump_sink::test_change(uint64_t at, bool add)
{
    uint8_t b = 0x20;
    std::string why;
    /* read through a handle of its own: back_ keeps no byte it read */
    FILE* now = add ? nullptr : files::open_file(partial_, false, &why);
    if (now != nullptr) {
        if (ecuio_frames_csv::seek_to(now, at) &&
            std::fread(&b, 1, 1, now) == 1) {
            b ^= 0x01;
        }
        std::fclose(now);
    }
    const bool put = ecuio_frames_csv::seek_to(file_, at) &&
                     std::fwrite(&b, 1, 1, file_) == 1 &&
                     std::fflush(file_) == 0;
    (void)put;
    (void)ecuio_frames_csv::seek_to(file_, kHeaderBytes + written_);
}

/* A piece of a range's second pass, held to what the disk gives back
 * for the first */
void dump_sink::second_pass(const item& head)
{
    const step& s = p_->steps[head.step];
    const uint64_t at = base_[head.step] + head.offset;
    if (at != compared_ || written_ < base_[head.step] + s.bytes ||
        head.offset + head.size > s.bytes) {
        end(sink_state::failed, "the copy's bytes came out of order");
        return;
    }
    if (head.size > 0 && seam(kSinkChangeByte)) {
        test_change(kHeaderBytes + at, false);
    }
    if (seam(kSinkFailReadBack) ||
        !ecuio_frames_csv::seek_to(back_, kHeaderBytes + at) ||
        std::fread(read_.data(), 1, head.size, back_) != head.size) {
        end(sink_state::failed, "the copy can't be read back");
        return;
    }
    for (uint32_t i = 0; i < head.size; ++i) {
        if (read_[i] != payload_[i]) {
            differ_step_ = head.step;
            differ_offset_ = head.offset + i;
            end(sink_state::differ, "");
            return;
        }
    }
    compared_ += head.size;
}

/* The record after the image and the header ahead of it, flushed */
bool dump_sink::write_tail(const std::string& text, const uint8_t* head)
{
    if (seam(kSinkFailWrite) ||
        std::fwrite(text.data(), 1, text.size(), file_) != text.size() ||
        !ecuio_frames_csv::seek_to(file_, 0) ||
        std::fwrite(head, 1, kHeaderBytes, file_) != kHeaderBytes) {
        end(sink_state::failed, "the copy can't be written");
        return false;
    }
    if (seam(kSinkFailFlush) || !ecuio_store_file::flush_to_disk(file_)) {
        end(sink_state::failed, "the copy can't be flushed to the disk");
        return false;
    }
    const uint64_t end_at = kHeaderBytes + total_ + text.size();
    const struct {
        int step;
        uint64_t at;
    } changes[] = {{kSinkChangeHeader, 40},
                   {kSinkChangeImage, kHeaderBytes},
                   {kSinkChangeRecord, end_at - 2},
                   {kSinkAddByte, end_at}};
    for (const auto& c : changes) {
        if (seam(c.step)) {
            test_change(c.at, c.at == end_at);
        }
    }
    return true;
}

/* The whole file read back and held to what was written: the header and
 * the record byte for byte, the image by its checksum, and nothing after.
 * file_sum is the reading's own checksum, the copy's name. */
bool dump_sink::read_back(const std::string& text, const uint8_t* head,
                          uint8_t file_sum[32])
{
    ecuio_sha256::hasher whole;
    ecuio_sha256::hasher image;
    bool read =
        !seam(kSinkFailReadBack) && ecuio_frames_csv::seek_to(back_, 0) &&
        std::fread(read_.data(), 1, kHeaderBytes, back_) == kHeaderBytes;
    bool same = read && std::memcmp(read_.data(), head, kHeaderBytes) == 0;
    whole.add(read_.data(), read ? kHeaderBytes : 0);
    for (uint64_t left = total_; read && left > 0;) {
        const size_t n =
            static_cast<size_t>((std::min)(uint64_t{read_.size()}, left));
        read = std::fread(read_.data(), 1, n, back_) == n;
        whole.add(read_.data(), read ? n : 0);
        image.add(read_.data(), read ? n : 0);
        left -= n;
    }
    uint8_t sum[32];
    image.finish(sum);
    same = same && std::memcmp(sum, record_.image_sha256, 32) == 0;
    read =
        read && std::fread(read_.data(), 1, text.size(), back_) == text.size();
    same = same && read &&
           std::memcmp(read_.data(), text.data(), text.size()) == 0;
    whole.add(read_.data(), read ? text.size() : 0);
    uint8_t past = 0;
    same = same && std::fread(&past, 1, 1, back_) == 0;
    whole.finish(file_sum);
    if (!read) {
        end(sink_state::failed, "the copy can't be read back");
    } else if (!same) {
        end(sink_state::failed, "the copy didn't read back as it was written");
    }
    return read && same;
}

/* The seal: only a copy whose every byte was written once and compared
 * once gets its record, its header and its name */
void dump_sink::finish(const read_facts& facts)
{
    if (written_ != total_ || compared_ != total_) {
        end(sink_state::failed, "not every byte was read twice");
        return;
    }
    record_.image_bytes = total_;
    image_.finish(record_.image_sha256);
    put_ranges(record_);
    record_.passes = 2;
    record_.seconds = facts.seconds;
    record_.unlocks = facts.unlocks;
    record_.writes_before = facts.writes_before;
    record_.has_volts = facts.has_volts;
    record_.has_volts_min = facts.has_volts_min;
    record_.has_rpm = facts.has_rpm;
    record_.has_kmh = facts.has_kmh;
    record_.volts_mv = facts.volts_mv;
    record_.volts_min_mv = facts.volts_min_mv;
    record_.rpm = facts.rpm;
    record_.kmh = facts.kmh;
    record_.parked_tick = facts.parked_tick;
    const std::string text = ecuio_dump_file::record_text(record_);
    ecuio_dump_file::header h;
    h.image_bytes = total_;
    h.record_bytes = static_cast<uint32_t>(text.size());
    std::memcpy(h.image_sha256, record_.image_sha256, 32);
    ecuio_sha256::hasher sum;
    sum.add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    sum.finish(h.record_sha256);
    uint8_t head[kHeaderBytes];
    ecuio_dump_file::write_header(h, head);
    uint8_t file_sum[32];
    if (text.size() > ECUIO_DUMP_RECORD_MAX) {
        end(sink_state::failed, "the copy's record is longer than one holds");
        return;
    }
    /* Nothing is sealed that a reader would refuse */
    ecuio_dump_file::record again;
    std::string why;
    const ecuio_dump_file::judged j =
        ecuio_dump_file::read_record(text.data(), text.size(), h, &again, &why);
    if (j == ecuio_dump_file::judged::out_of_memory) {
        throw std::bad_alloc();
    }
    if (j != ecuio_dump_file::judged::ok ||
        !ecuio_dump_file::same(record_, again)) {
        end(sink_state::failed,
            ("the copy's record isn't one a copy holds: " + why).c_str());
        return;
    }
    if (!write_tail(text, head) || !read_back(text, head, file_sum)) {
        return;
    }
    place(ecuio_dump_file::name_of(record_.utc, record_.tx_id, file_sum));
}

/* The sealed partial under its name: read-only, renamed without
 * replacing, its folder flushed */
void dump_sink::place(const std::string& name)
{
    const bool closed = std::fclose(file_) == 0;
    file_ = nullptr;
    std::fclose(back_);
    back_ = nullptr;
    std::string why;
    std::string out = folder_ + "/" + name;
    if (!closed || !ecuio_dump_handle::set_read_only(partial_, &why)) {
        end(sink_state::failed, "the copy can't be made read-only");
        return;
    }
    if (seam(kSinkFailRename) ||
        !files::rename_to(partial_, out, false, &why)) {
        end(sink_state::failed,
            why.empty() ? "the copy can't be put in place"
                        : ("the copy can't be put in place: " + why).c_str());
        return;
    }
    partial_.swap(out); /* the placed file is the one a failure removes */
    if (seam(kSinkFailFolderFlush) ||
        !ecuio_dump_handle::flush_folder_of(partial_, &why)) {
        end(sink_state::failed,
            why.empty()
                ? "the copy's folder can't be flushed to the disk"
                : ("the copy's folder can't be flushed to the disk: " + why)
                      .c_str());
        return;
    }
    partial_.clear();
    copy_words(name_, sizeof name_, name.c_str());
    state_.store(sink_state::saved, std::memory_order_release);
}

/* The copy ended without a seal: nothing of it stays on the disk. It
 * allocates nothing, so it ends a copy that ran out of memory too. */
void dump_sink::end(sink_state s, const char* words)
{
    if (state() != sink_state::writing) {
        return;
    }
    copy_words(words_, sizeof words_, words);
    let_go();
    state_.store(s, std::memory_order_release);
}

void dump_sink::let_go()
{
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (back_ != nullptr) {
        std::fclose(back_);
        back_ = nullptr;
    }
    if (!partial_.empty()) {
        (void)ecuio_dump_handle::drop(partial_);
        partial_.clear();
    }
}

void dump_sink::close()
{
    stop_.store(true, std::memory_order_release);
    wake_.notify_one();
    if (writer_.joinable()) {
        writer_.join();
    }
    if (state() == sink_state::writing) {
        let_go();
        state_.store(sink_state::dropped, std::memory_order_release);
    }
}

} // namespace ecuio_operation
