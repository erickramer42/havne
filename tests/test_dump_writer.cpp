// File/layer: tests/test_dump_writer.cpp | C++17, Qt-free, every platform
// Testing: UNIT: DumpWriterTest (test_core)
//
// A dump's ring and writer (core/src/operation_sink.h), fed as a run
// feeds them: the sealed copy and its name, that nothing reads as a copy
// before the seal, where a difference is found, what each failing step
// leaves on the disk (nothing), and a ring that is full.

#include "dump_file_support.h"
#include "operation_sink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

/* test_core.cpp's, over tests/new_counter.h */
void fail_new_on_this_thread(unsigned long n);

namespace {

using namespace dump_file_test;
using ecuio_operation::dump_sink;
using ecuio_operation::dump_test_fail_at;
using ecuio_operation::dump_test_hold_writer;
using ecuio_operation::plan;
using ecuio_operation::read_facts;
using ecuio_operation::sink_state;

std::vector<std::string> names_in(const std::string& folder)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(folder), ec)) {
        names.push_back(e.path().filename().generic_u8string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

/* Two ranges, a read and an upload, and the two identifiers; every
 * address is made up */
plan two_ranges()
{
    plan p;
    ecuio_operation::step a;
    a.name = "calibration";
    a.address = 0x00010000;
    a.bytes = 3000;
    a.chunk = 256;
    ecuio_operation::step b;
    b.kind = ECUIO_STEP_UPLOAD;
    b.name = "boot";
    b.address = 0x00020000;
    b.bytes = 5000;
    b.size_bytes = 4;
    p.steps = {a, b};
    p.idents = {{0xD220, ECUIO_DUMP_ROLE_HARDWARE},
                {0xD221, ECUIO_DUMP_ROLE_SOFTWARE}};
    return p;
}

read_facts facts_of()
{
    /* None of them record_of()'s */
    read_facts f;
    f.seconds = 77;
    f.unlocks = 2;
    f.writes_before = 3;
    f.has_volts = f.has_rpm = true;
    f.volts_mv = 12001;
    f.volts_min_mv = 11900; /* not written: has_volts_min is off */
    f.rpm = 5;
    f.kmh = 9; /* not written */
    return f;
}

/* A sink in a folder of its own, fed as a run feeds one */
struct Fed {
    static inline int made = 0;
    plan p = two_ranges();
    Folder f{"-" + std::to_string(++made)};
    Bytes image = image_of(8000);
    ecuio_dump_file::record known = record_of(); /* at the start */
    dump_sink sink;

    ~Fed()
    {
        dump_test_fail_at = 0;
        dump_test_hold_writer = false;
        ecuio_operation::dump_test_before_take = nullptr;
    }
    ecuio_status_t open(std::string* why = nullptr)
    {
        std::string words;
        const ecuio_status_t s =
            sink.open(p, f.str(), known, why != nullptr ? why : &words);
        EXPECT_TRUE(s != ECUIO_OK || words.empty()) << words;
        return s;
    }
    size_t base(uint32_t step) const { return step == 0 ? 0 : 3000; }
    /* One pass over a step's range, piece by piece, waiting for room;
     * wrong_at, when inside it, has one bit wrong */
    void pass(uint32_t step, uint32_t n, size_t piece = 256,
              size_t wrong_at = SIZE_MAX)
    {
        const size_t bytes = static_cast<size_t>(p.steps[step].bytes);
        for (size_t at = 0; at < bytes; at += piece) {
            const size_t size = (std::min)(piece, bytes - at);
            Bytes part(image.begin() + base(step) + at,
                       image.begin() + base(step) + at + size);
            if (wrong_at >= at && wrong_at < at + size) {
                part[wrong_at - at] ^= 0x10;
            }
            for (int i = 0; i < 5000 && !sink.room(size); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ASSERT_TRUE(sink.room(size));
            sink.data(step, n, at, part.data(), size);
        }
    }
    void idents()
    {
        sink.ident(0, reinterpret_cast<const uint8_t*>("HW-1"), 4);
        const uint8_t sw[] = {0x00, 0xFF, 0x7A};
        sink.ident(1, sw, sizeof sw);
    }
    void both_passes()
    {
        idents();
        for (const uint32_t step : {0u, 1u}) {
            pass(step, 0);
            pass(step, 1);
        }
    }
    /* The writer's word once it has one, or once it has taken all */
    sink_state settle()
    {
        for (int i = 0; i < 5000; ++i) {
            if (sink.state() != sink_state::writing || sink.idle()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return sink.state();
    }
    sink_state sealed()
    {
        sink.seal(facts_of());
        for (int i = 0; i < 5000 && sink.state() == sink_state::writing; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return sink.state();
    }
    /* The record a seal writes for this feed */
    ecuio_dump_file::record want() const
    {
        ecuio_dump_file::record r = record_of();
        r.ranges.clear();
        uint64_t offset = 0;
        for (const ecuio_operation::step& s : p.steps) {
            ecuio_dump_file::range g;
            g.name = s.name;
            g.step = s.kind;
            g.session = s.session;
            g.address = s.address;
            g.bytes = s.bytes;
            g.offset = offset;
            offset += s.bytes;
            r.ranges.push_back(g);
        }
        r.idents[1].role = ECUIO_DUMP_ROLE_SOFTWARE;
        const read_facts facts = facts_of();
        r.seconds = facts.seconds;
        r.unlocks = facts.unlocks;
        r.writes_before = facts.writes_before;
        r.has_volts = r.has_rpm = true;
        r.has_volts_min = r.has_kmh = false;
        r.volts_mv = facts.volts_mv;
        r.rpm = facts.rpm;
        r.volts_min_mv = r.kmh = 0;
        r.parked_tick = false;
        return r;
    }
};

TEST(DumpWriterTest, ASealedCopyIsTheFileItsRecordDescribes)
{
    Fed d;
    ASSERT_EQ(d.open(), ECUIO_OK);
    d.both_passes();
    ASSERT_EQ(d.sealed(), sink_state::saved) << d.sink.words();
    const std::string bytes =
        ecuio_dump_file::build(d.want(), d.image.data(), d.image.size());
    const std::string name = d.sink.name();
    EXPECT_EQ(name, name_for(bytes)) << "the whole file's checksum, not the "
                                        "image's";
    EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{name});
    const std::string path = d.f.at(name);
    EXPECT_EQ(read_bytes(path), bytes);
    const fs::perms perms = fs::status(fs::u8path(path)).permissions();
    EXPECT_EQ(perms & (fs::perms::owner_write | fs::perms::group_write |
                       fs::perms::others_write),
              fs::perms::none)
        << "a kept copy is read-only";
    const Dump copy = open_dump(path);
    ASSERT_NE(copy, nullptr) << ecuio_last_error_detail();
    char own[ECUIO_DUMP_NAME_MAX];
    EXPECT_EQ(ecuio_dump_verify(copy.get(), nullptr, nullptr, own, sizeof own),
              ECUIO_OK)
        << ecuio_last_error_detail();
    d.sink.close();
    EXPECT_EQ(d.sink.state(), sink_state::saved) << "a close keeps the copy";
    EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{name});
}

/* Until the seal the file is a partial with no header: nothing that reads
 * it takes it for a copy, however much of it is written and compared */
TEST(DumpWriterTest, NothingReadsAsACopyBeforeTheSeal)
{
    Fed d;
    ASSERT_EQ(d.open(), ECUIO_OK);
    d.both_passes();
    ASSERT_EQ(d.settle(), sink_state::writing);
    const std::vector<std::string> names = names_in(d.f.str());
    ASSERT_EQ(names.size(), 1u);
    EXPECT_TRUE(ecuio_dump_file::partial_name(names[0])) << names[0];
    const std::string partial = read_bytes(d.f.at(names[0]));
    ASSERT_EQ(partial.size(), 128u + 8000u) << "no record yet";
    EXPECT_EQ(partial.substr(0, 128), std::string(128, '\0')) << "no header";
    EXPECT_EQ(Bytes(partial.begin() + 128, partial.end()), d.image);
    ecuio_dump_entry_t entry;
    size_t count = 0;
    ASSERT_EQ(ecuio_dump_list(d.f.str().c_str(), &entry, 1, &count), ECUIO_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(entry.state, ECUIO_DUMP_STATE_UNFINISHED);
    d.sink.close();
    EXPECT_EQ(d.sink.state(), sink_state::dropped);
    EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{})
        << "a close before the seal keeps nothing";
}

/* The second pass is held to the first byte for byte: the first that
 * differs ends the copy, and names its range and its place */
TEST(DumpWriterTest, ADifferenceSaysWhereAndKeepsNothing)
{
    const struct {
        uint32_t step;
        size_t at;
    } places[] = {{0, 0}, {0, 2999}, {1, 1234}, {1, 4999}};
    for (const auto& place : places) {
        Fed d;
        ASSERT_EQ(d.open(), ECUIO_OK);
        d.idents();
        for (uint32_t step = 0; step <= place.step; ++step) {
            d.pass(step, 0);
            d.pass(step, 1, 256, step == place.step ? place.at : SIZE_MAX);
        }
        EXPECT_EQ(d.settle(), sink_state::differ);
        EXPECT_EQ(d.sink.differ_step(), place.step);
        EXPECT_EQ(d.sink.differ_offset(), place.at);
        EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{});
        EXPECT_EQ(d.sealed(), sink_state::differ) << "no seal after it";
        EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{});
    }
    /* A wrong byte in the first pass is found by the second, which is
     * the module's word again */
    Fed first;
    ASSERT_EQ(first.open(), ECUIO_OK);
    first.pass(0, 0, 256, 700);
    first.pass(0, 1);
    EXPECT_EQ(first.settle(), sink_state::differ);
    EXPECT_EQ(first.sink.differ_offset(), 700u);
}

/* The compare is against the disk, not the bytes still in memory: a byte
 * that changed on the disk after its flush is a difference */
TEST(DumpWriterTest, TheSecondPassIsHeldToTheDisk)
{
    /* At the first piece, and at the second: read once the first was,
     * so a read that kept what it had read ahead would miss it */
    for (const size_t pieces_before : {size_t{0}, size_t{1}}) {
        Fed d;
        ASSERT_EQ(d.open(), ECUIO_OK);
        d.pass(0, 0);
        for (size_t i = 0; i < pieces_before; ++i) {
            d.sink.data(0, 1, i * 256, d.image.data() + i * 256, 256);
        }
        ASSERT_EQ(d.settle(), sink_state::writing);
        dump_test_fail_at = ecuio_operation::kSinkChangeByte;
        for (size_t at = pieces_before * 256; at < 3000; at += 256) {
            d.sink.data(0, 1, at, d.image.data() + at,
                        (std::min)(size_t{256}, 3000 - at));
        }
        EXPECT_EQ(d.settle(), sink_state::differ);
        EXPECT_EQ(dump_test_fail_at.load(), 0) << "the seam was reached";
        EXPECT_EQ(d.sink.differ_step(), 0u);
        EXPECT_EQ(d.sink.differ_offset(), pieces_before * 256);
        EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{});
    }
}

/* The seal reads the whole file back against what it wrote: a byte the
 * disk gives back changed, wherever it is, or one more, is no copy */
TEST(DumpWriterTest, TheSealReadsTheWholeFileBack)
{
    for (const int change :
         {ecuio_operation::kSinkChangeHeader, ecuio_operation::kSinkChangeImage,
          ecuio_operation::kSinkChangeRecord, ecuio_operation::kSinkAddByte}) {
        Fed d;
        ASSERT_EQ(d.open(), ECUIO_OK);
        d.both_passes();
        ASSERT_EQ(d.settle(), sink_state::writing);
        dump_test_fail_at = change;
        EXPECT_EQ(d.sealed(), sink_state::failed) << change;
        EXPECT_STREQ(d.sink.words(),
                     "the copy didn't read back as it was written")
            << change;
        EXPECT_EQ(dump_test_fail_at.load(), 0) << "the seam was reached";
        EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{}) << change;
    }
}

/* A step that fails, while the ranges are written or at the seal, ends
 * the copy in words and leaves neither a partial nor a copy */
TEST(DumpWriterTest, AFailingStepLeavesNothing)
{
    const struct {
        int step;
        bool at_seal;
        const char* words;
    } fails[] = {
        {ecuio_operation::kSinkFailWrite, false, "the copy can't be written"},
        {ecuio_operation::kSinkFailWrite, true, "the copy can't be written"},
        {ecuio_operation::kSinkFailFlush, false,
         "the copy can't be flushed to the disk"},
        {ecuio_operation::kSinkFailFlush, true,
         "the copy can't be flushed to the disk"},
        {ecuio_operation::kSinkFailReadBack, false,
         "the copy can't be read back"},
        {ecuio_operation::kSinkFailReadBack, true,
         "the copy can't be read back"},
        {ecuio_operation::kSinkFailRename, true,
         "the copy can't be put in place"},
        {ecuio_operation::kSinkFailFolderFlush, true,
         "the copy's folder can't be flushed to the disk"},
    };
    for (const auto& fail : fails) {
        Fed d;
        ASSERT_EQ(d.open(), ECUIO_OK);
        if (!fail.at_seal) {
            dump_test_fail_at = fail.step;
        }
        d.both_passes();
        d.settle();
        if (fail.at_seal) {
            ASSERT_EQ(d.sink.state(), sink_state::writing);
            dump_test_fail_at = fail.step;
        } else {
            EXPECT_EQ(d.sink.state(), sink_state::failed) << "before a seal";
        }
        EXPECT_EQ(d.sealed(), sink_state::failed)
            << fail.step << (fail.at_seal ? " at the seal" : "");
        EXPECT_STREQ(d.sink.words(), fail.words);
        EXPECT_EQ(dump_test_fail_at.load(), 0) << "the step was reached";
        EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{})
            << fail.step << (fail.at_seal ? " at the seal" : "");
    }
}

/* A seal is only for a copy whose every byte came twice, in order */
TEST(DumpWriterTest, ASealNeedsEveryByteTwiceAndInOrder)
{
    Fed once;
    ASSERT_EQ(once.open(), ECUIO_OK);
    once.pass(0, 0);
    once.pass(0, 1);
    once.pass(1, 0);
    EXPECT_EQ(once.sealed(), sink_state::failed);
    EXPECT_STREQ(once.sink.words(), "not every byte was read twice");
    EXPECT_EQ(names_in(once.f.str()), std::vector<std::string>{});

    Fed gap;
    ASSERT_EQ(gap.open(), ECUIO_OK);
    gap.sink.data(0, 0, 256, gap.image.data() + 256, 256);
    EXPECT_EQ(gap.settle(), sink_state::failed);
    EXPECT_STREQ(gap.sink.words(), "the copy's bytes came out of order");

    Fed early;
    ASSERT_EQ(early.open(), ECUIO_OK);
    early.pass(0, 0, 256);
    early.sink.data(1, 1, 0, early.image.data() + 3000, 256);
    EXPECT_EQ(early.settle(), sink_state::failed)
        << "a second pass before its range's first is whole";
    EXPECT_EQ(names_in(early.f.str()), std::vector<std::string>{});

    Fed past;
    ASSERT_EQ(past.open(), ECUIO_OK);
    past.pass(0, 0, 256);
    past.sink.data(0, 0, 3000, past.image.data(), 1);
    EXPECT_EQ(past.settle(), sink_state::failed) << "past its range's end";

    /* The second pass's own order: from its range's start, once the
     * first is whole, and no further than the range */
    const char* const order = "the copy's bytes came out of order";
    Fed skipped;
    ASSERT_EQ(skipped.open(), ECUIO_OK);
    skipped.pass(0, 0);
    skipped.sink.data(0, 1, 256, skipped.image.data() + 256, 256);
    EXPECT_EQ(skipped.settle(), sink_state::failed);
    EXPECT_STREQ(skipped.sink.words(), order);
    Fed soon;
    ASSERT_EQ(soon.open(), ECUIO_OK);
    soon.sink.data(0, 0, 0, soon.image.data(), 256);
    soon.sink.data(0, 1, 0, soon.image.data(), 256);
    EXPECT_EQ(soon.settle(), sink_state::failed);
    EXPECT_STREQ(soon.sink.words(), order);
    Fed beyond;
    ASSERT_EQ(beyond.open(), ECUIO_OK);
    beyond.pass(0, 0);
    beyond.pass(0, 1);
    beyond.sink.data(0, 1, 3000, beyond.image.data(), 1);
    EXPECT_EQ(beyond.settle(), sink_state::failed);
    EXPECT_STREQ(beyond.sink.words(), order);

    /* A record no reader would take, or one too long to seal, is refused
     * at the start, before a byte is asked of the module */
    Fed wordy;
    wordy.known.adapter.assign(ECUIO_DUMP_RECORD_MAX - 2000, 'a');
    std::string why;
    EXPECT_EQ(wordy.open(&why), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(why, "the copy can't be started: its record is longer than a "
                   "copy holds");
    EXPECT_EQ(names_in(wordy.f.str()), std::vector<std::string>{});
    Fed odd;
    odd.known.scheme = "No Scheme";
    EXPECT_EQ(odd.open(&why), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(why.rfind("the copy can't be started: its record isn't one a "
                        "copy holds: ",
                        0),
              0u)
        << why;
    EXPECT_EQ(names_in(odd.f.str()), std::vector<std::string>{});

    /* Nothing is sealed that a reader would refuse: here a record whose
     * identifiers were never read */
    Fed bare;
    ASSERT_EQ(bare.open(), ECUIO_OK);
    for (const uint32_t step : {0u, 1u}) {
        bare.pass(step, 0);
        bare.pass(step, 1);
    }
    EXPECT_EQ(bare.sealed(), sink_state::failed);
    const std::string words = bare.sink.words();
    EXPECT_EQ(words.rfind("the copy's record isn't one a copy holds: ", 0), 0u)
        << words;
    EXPECT_EQ(names_in(bare.f.str()), std::vector<std::string>{});

    Fed abandoned;
    ASSERT_EQ(abandoned.open(), ECUIO_OK);
    abandoned.both_passes();
    abandoned.sink.abandon();
    EXPECT_EQ(abandoned.sealed(), sink_state::dropped);
    EXPECT_STREQ(abandoned.sink.words(), "");
    EXPECT_EQ(names_in(abandoned.f.str()), std::vector<std::string>{});
}

/* A writer that takes nothing fills the ring: room() says so, nothing
 * handed over before that is lost, and what is handed over all the same
 * ends the copy in words */
TEST(DumpWriterTest, AFullRingHoldsTheReaderAndDropsNothing)
{
    Fed d;
    d.p.steps[1].bytes = 600000; /* more than the ring holds */
    d.image = image_of(603000);
    ASSERT_EQ(d.open(), ECUIO_OK);
    EXPECT_TRUE(d.sink.room(dump_sink::kDataMost));
    EXPECT_FALSE(d.sink.room(dump_sink::kDataMost + 1)) << "however empty";
    d.idents();
    d.pass(0, 0);
    d.pass(0, 1);
    ASSERT_EQ(d.settle(), sink_state::writing);
    dump_test_hold_writer = true;
    size_t fed = 0;
    while (d.sink.room(4000) && fed < 600000) {
        d.sink.data(1, 0, fed, d.image.data() + 3000 + fed, 4000);
        fed += 4000;
    }
    EXPECT_GE(fed, dump_sink::kRingBytes - 3 * 4096) << "the ring's worth";
    EXPECT_LT(fed, dump_sink::kRingBytes);
    dump_test_hold_writer = false;
    /* The rest, each piece waiting for its room: all of it arrives */
    for (; fed < 600000; fed += 4000) {
        for (int i = 0; i < 5000 && !d.sink.room(4000); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_TRUE(d.sink.room(4000));
        d.sink.data(1, 0, fed, d.image.data() + 3000 + fed, 4000);
    }
    d.pass(1, 1, 4000);
    ASSERT_EQ(d.sealed(), sink_state::saved) << d.sink.words();
    const Dump copy = open_dump(d.f.at(d.sink.name()));
    ASSERT_NE(copy, nullptr);
    Bytes last(4000);
    size_t count = 0;
    ASSERT_EQ(ecuio_dump_read(copy.get(), 603000 - 4000, last.data(),
                              last.size(), &count),
              ECUIO_OK);
    EXPECT_EQ(last, Bytes(d.image.end() - 4000, d.image.end()));

    /* Room is kept for what asks for none: with no room left for a
     * byte of data, the identifiers and the run's end still arrive */
    Fed full;
    full.p.steps[0].bytes = 600000;
    full.p.steps[1].address = 0x00200000; /* past the longer range */
    full.image = image_of(605000);
    ASSERT_EQ(full.open(), ECUIO_OK);
    dump_test_hold_writer = true;
    size_t filled = 0;
    for (const size_t piece : {size_t{4000}, size_t{1}}) {
        for (; full.sink.room(piece); filled += piece) {
            full.sink.data(0, 0, filled, full.image.data() + filled, piece);
        }
    }
    for (uint32_t i = 0; i < ECUIO_PLUGIN_MAX_IDENTS; ++i) {
        const Bytes value(ECUIO_DUMP_IDENT_MAX, 0x41);
        full.sink.ident(i, value.data(), value.size());
    }
    full.sink.abandon();
    dump_test_hold_writer = false;
    EXPECT_EQ(full.settle(), sink_state::dropped) << full.sink.words();

    /* Handed over with no room asked: the copy ends, nothing is torn */
    Fed over;
    over.p.steps[0].bytes = 600000;
    over.p.steps[1].address = 0x00200000;
    over.image = image_of(605000);
    ASSERT_EQ(over.open(), ECUIO_OK);
    dump_test_hold_writer = true;
    for (size_t at = 0; at < 400000; at += 4000) {
        over.sink.data(0, 0, at, over.image.data() + at, 4000);
    }
    dump_test_hold_writer = false;
    EXPECT_EQ(over.settle(), sink_state::failed);
    EXPECT_STREQ(over.sink.words(),
                 "more was handed over than the copy's ring holds");
    EXPECT_EQ(names_in(over.f.str()), std::vector<std::string>{});
}

/* A name that is taken is never replaced: the seal fails and the file
 * that was there keeps its bytes */
TEST(DumpWriterTest, ASealNeverReplacesAFile)
{
    Fed first;
    ASSERT_EQ(first.open(), ECUIO_OK);
    first.both_passes();
    ASSERT_EQ(first.sealed(), sink_state::saved);
    const std::string name = first.sink.name();
    const std::string bytes = read_bytes(first.f.at(name));

    Fed again;
    again.f.put(name, "mine");
    ASSERT_EQ(again.open(), ECUIO_OK);
    again.both_passes();
    EXPECT_EQ(again.sealed(), sink_state::failed);
    const std::string words = again.sink.words();
    EXPECT_EQ(words.rfind("the copy can't be put in place", 0), 0u) << words;
    EXPECT_EQ(words.find(again.f.str()), std::string::npos) << "no path";
    EXPECT_EQ(names_in(again.f.str()), std::vector<std::string>{name});
    EXPECT_EQ(read_bytes(again.f.at(name)), "mine");
    EXPECT_NE(bytes, "mine");
}

/* An open that fails, for want of a folder or of memory at any
 * allocation, leaves nothing and starts no writer */
TEST(DumpWriterTest, AnOpenThatFailsLeavesNothing)
{
    {
        Fed d;
        std::string why;
        const plan p = two_ranges();
        dump_sink sink;
        EXPECT_EQ(sink.open(p, d.f.at("none"), record_of(), &why),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(why.rfind("the copy can't be started: ", 0), 0u) << why;
        EXPECT_EQ(why.find(d.f.str()), std::string::npos) << "no path";
    }
    bool opened = false;
    for (unsigned long n = 1; n < 4000 && !opened; ++n) {
        Fed d;
        const ecuio_dump_file::record r = record_of();
        const std::string folder = d.f.str();
        std::string why;
        fail_new_on_this_thread(n);
        const ecuio_status_t s = d.sink.open(d.p, folder, r, &why);
        fail_new_on_this_thread(0);
        if (s == ECUIO_OK) {
            opened = true;
            d.both_passes();
            EXPECT_EQ(d.sealed(), sink_state::saved) << n;
        } else {
            ASSERT_EQ(s, ECUIO_ERR_OUT_OF_MEMORY) << n << ": " << why;
            EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{}) << n;
        }
    }
    EXPECT_TRUE(opened);
}

std::atomic<unsigned long> g_fail_nth{0};

/* Armed once, at the writer's first record */
void fail_on_the_writer()
{
    const unsigned long n = g_fail_nth.exchange(0);
    if (n != 0) {
        fail_new_on_this_thread(n);
    }
}

/* Each allocation the writer makes, failing in turn: the copy ends in
 * words with nothing on the disk, the placed file included */
TEST(DumpWriterTest, AWriterOutOfMemoryLeavesNothing)
{
    bool saved = false;
    unsigned long failed = 0;
    for (unsigned long n = 1; n < 2000 && !saved; ++n) {
        Fed d;
        ASSERT_EQ(d.open(), ECUIO_OK);
        g_fail_nth = n;
        ecuio_operation::dump_test_before_take = fail_on_the_writer;
        d.both_passes();
        const sink_state s = d.sealed();
        ecuio_operation::dump_test_before_take = nullptr;
        if (s == sink_state::saved) {
            saved = true;
            EXPECT_NE(open_dump(d.f.at(d.sink.name())), nullptr) << n;
            continue;
        }
        ++failed;
        ASSERT_EQ(s, sink_state::failed) << n;
        EXPECT_STREQ(d.sink.words(),
                     "there wasn't the memory to finish the copy")
            << n;
        EXPECT_EQ(names_in(d.f.str()), std::vector<std::string>{}) << n;
    }
    EXPECT_TRUE(saved);
    EXPECT_GT(failed, 8u) << "the identifiers', the record's, the name's";
}

} // namespace
