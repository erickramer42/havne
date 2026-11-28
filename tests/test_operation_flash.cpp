#include "../core/src/operation_flash.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ecuio_operation::flash_action;
using ecuio_operation::flash_end;
using ecuio_operation::flash_run;
using ecuio_operation::flash_stage;
using ecuio_operation::flash_wait;
using ecuio_operation::kFlashWireMost;
using ecuio_operation::plan;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kSector = 0x1000;
constexpr uint32_t kBase = 0x00080000;

plan flash_plan(uint32_t sectors = 2, uint32_t sector_bytes = kSector)
{
    plan p;
    p.kind = ECUIO_OPERATION_KIND_FLASH;
    p.has_flash = true;
    p.leave = ECUIO_OPERATION_LEAVE_RESET;
    ecuio_operation::step s;
    s.kind = ECUIO_STEP_DOWNLOAD;
    s.session = ECUIO_STEP_SESSION_PROGRAMMING;
    s.address = kBase;
    s.bytes = uint64_t{sectors} * sector_bytes;
    s.address_bytes = 4;
    s.size_bytes = 4;
    s.name = "code";
    s.blob.resize(s.bytes);
    for (size_t i = 0; i < s.blob.size(); ++i) {
        s.blob[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    p.steps.push_back(s);
    p.writes.erase = 0xFF00;
    p.writes.check = 0xFF01;
    p.writes.pass = {0x00};
    for (uint32_t i = 0; i < sectors; ++i) {
        p.writes.sectors.push_back({kBase + i * sector_bytes, sector_bytes});
    }
    return p;
}

struct Wire {
    flash_run r;
    std::vector<Bytes> sent;
    uint32_t journals = 0;
    uint32_t pauses = 0;

    /* The next request, journals and pauses taken on the way */
    Bytes next()
    {
        uint8_t out[kFlashWireMost];
        uint32_t len = 0;
        for (;;) {
            const flash_action a = ecuio_operation::flash_next(r, out, &len);
            if (a == flash_action::journal) {
                ++journals;
                ecuio_operation::flash_journaled(r);
            } else if (a == flash_action::pause) {
                ++pauses;
            } else if (a == flash_action::done) {
                return {};
            } else {
                sent.emplace_back(out, out + len);
                return sent.back();
            }
        }
    }

    flash_wait answer(const Bytes& reply)
    {
        return ecuio_operation::flash_reply(r, reply.data(), reply.size());
    }
};

Bytes be(uint64_t v, unsigned n)
{
    Bytes b;
    for (unsigned i = n; i > 0; --i) {
        b.push_back(static_cast<uint8_t>(v >> (8 * (i - 1))));
    }
    return b;
}

Bytes erase_of(uint32_t address, uint32_t size)
{
    Bytes q = {0x31, 0x01, 0xFF, 0x00, 0x44};
    for (const uint8_t b : be(address, 4)) {
        q.push_back(b);
    }
    for (const uint8_t b : be(size, 4)) {
        q.push_back(b);
    }
    return q;
}

Bytes open_of(uint32_t address, uint32_t size)
{
    Bytes q = erase_of(address, size);
    q.erase(q.begin(), q.begin() + 4);
    q.insert(q.begin(), {0x34, 0x00});
    return q;
}

const Bytes kErased = {0x71, 0x01, 0xFF, 0x00};
const Bytes kOpened = {0x74, 0x20, 0x04, 0x02};
const Bytes kExited = {0x77};
const Bytes kCheck = {0x31, 0x01, 0xFF, 0x01};
const Bytes kPassed = {0x71, 0x01, 0xFF, 0x01, 0x00};

/* One sector's download whole: 34, its 36 blocks of block bytes, 37 */
void write_one(Wire& w, uint32_t address, uint32_t bytes,
               const Bytes& opened = kOpened, uint32_t block = 1024)
{
    ASSERT_EQ(w.next(), open_of(address, bytes));
    ASSERT_EQ(w.answer(opened), flash_wait::go_on);
    uint8_t counter = 1;
    for (uint32_t at = 0; at < bytes; at += block, ++counter) {
        const Bytes q = w.next();
        const uint32_t n = bytes - at < block ? bytes - at : block;
        ASSERT_EQ(q.size(), 2u + n);
        ASSERT_EQ(q[0], 0x36);
        ASSERT_EQ(q[1], counter);
        ASSERT_EQ(w.answer({0x76, counter}), flash_wait::go_on);
    }
    ASSERT_EQ(w.next(), Bytes{0x37});
    ASSERT_EQ(w.answer(kExited), flash_wait::go_on);
}

} // namespace

TEST(OperationFlashTest, EachSectorIsErasedWrittenAndTheWholeChecked)
{
    const plan p = flash_plan();
    Wire w;
    ecuio_operation::flash_begin(w.r, p, kFlashWireMost, nullptr);
    EXPECT_EQ(w.r.chosen, 2u);
    EXPECT_EQ(ecuio_operation::flash_bytes(w.r), 2u * kSector);
    ASSERT_EQ(w.next(), erase_of(kBase, kSector));
    EXPECT_EQ(w.journals, 1u) << "the journal goes before the first erase";
    EXPECT_FALSE(w.r.floor);
    ASSERT_EQ(w.answer(kErased), flash_wait::go_on);
    EXPECT_TRUE(w.r.floor);
    write_one(w, kBase, kSector);
    ASSERT_EQ(w.next(), erase_of(kBase + kSector, kSector));
    EXPECT_EQ(w.journals, 2u) << "and again at each sector's end";
    ASSERT_EQ(w.answer(kErased), flash_wait::go_on);
    write_one(w, kBase + kSector, kSector);
    ASSERT_EQ(w.next(), kCheck);
    EXPECT_EQ(w.journals, 3u) << "the last sector's end too";
    ASSERT_EQ(w.answer(kPassed), flash_wait::go_on);
    EXPECT_EQ(w.r.result, flash_end::written);
    EXPECT_EQ(w.r.written, 2u * kSector);
    EXPECT_EQ(w.r.done_sectors, 2u);
    EXPECT_TRUE(w.next().empty());
    const Bytes& first = w.sent[2];
    EXPECT_EQ(Bytes(first.begin() + 2, first.begin() + 6),
              Bytes(p.steps[0].blob.begin(), p.steps[0].blob.begin() + 4))
        << "a block carries the image's bytes";
}

TEST(OperationFlashTest, TheFloorMovesOnAPositivePendingOrLostReplyAlone)
{
    const plan p = flash_plan();
    Wire refused;
    ecuio_operation::flash_begin(refused.r, p, kFlashWireMost, nullptr);
    refused.next();
    EXPECT_EQ(refused.answer({0x7F, 0x31, 0x22}), flash_wait::go_on);
    EXPECT_EQ(refused.r.result, flash_end::refused);
    EXPECT_EQ(refused.r.nrc, 0x22);
    EXPECT_FALSE(refused.r.floor) << "a refused erase is a clean abort";

    Wire pending;
    ecuio_operation::flash_begin(pending.r, p, kFlashWireMost, nullptr);
    pending.next();
    EXPECT_EQ(pending.answer({0x7F, 0x31, 0x78}), flash_wait::keep_waiting);
    EXPECT_TRUE(pending.r.floor);
    EXPECT_EQ(pending.answer({0x7F, 0x31, 0x22}), flash_wait::go_on);
    EXPECT_EQ(pending.r.result, flash_end::refused);
    EXPECT_TRUE(pending.r.floor) << "a refusal after a 78 leaves it moved";

    Wire lost;
    ecuio_operation::flash_begin(lost.r, p, kFlashWireMost, nullptr);
    lost.next();
    ecuio_operation::flash_silence(lost.r);
    EXPECT_EQ(lost.r.result, flash_end::no_answer);
    EXPECT_TRUE(lost.r.floor) << "an erase unanswered may have run";

    Wire empty;
    ecuio_operation::flash_begin(empty.r, p, kFlashWireMost, nullptr);
    empty.next();
    EXPECT_EQ(empty.answer({}), flash_wait::go_on);
    EXPECT_TRUE(empty.r.floor);
}

TEST(OperationFlashTest, APendingModuleIsWaitedForWithNoEndOfItsOwn)
{
    const plan p = flash_plan(1);
    Wire w;
    ecuio_operation::flash_begin(w.r, p, kFlashWireMost, nullptr);
    w.next();
    for (int i = 0; i < 10000; ++i) {
        ASSERT_EQ(w.answer({0x7F, 0x31, 0x78}), flash_wait::keep_waiting);
    }
    EXPECT_EQ(w.r.result, flash_end::none);
    EXPECT_EQ(w.answer(kErased), flash_wait::go_on);
    EXPECT_EQ(w.r.at, flash_stage::open);
}

TEST(OperationFlashTest, ARefusedBlockFollowsItsCode)
{
    const plan p = flash_plan(1);
    const auto at_first_block = [&](Wire& w) {
        ecuio_operation::flash_begin(w.r, p, kFlashWireMost, nullptr);
        w.next();
        w.answer(kErased);
        w.next();
        w.answer(kOpened);
        return w.next();
    };
    Wire counter;
    const Bytes block = at_first_block(counter);
    counter.answer({0x7F, 0x36, 0x73});
    EXPECT_EQ(counter.next(), block) << "73: the same block once more";
    counter.answer({0x7F, 0x36, 0x73});
    EXPECT_EQ(counter.r.result, flash_end::refused) << "and once only";
    EXPECT_EQ(counter.r.nrc, 0x73);

    Wire again;
    at_first_block(again);
    again.answer({0x7F, 0x36, 0x73});
    again.next();
    again.answer({0x76, 0x01});
    const Bytes second = again.next();
    again.answer({0x7F, 0x36, 0x73});
    EXPECT_EQ(again.next(), second) << "each block has its one resend";

    for (const uint8_t volts : {uint8_t{0x92}, uint8_t{0x93}}) {
        Wire w;
        const Bytes first = at_first_block(w);
        for (uint32_t i = 0; i < ecuio_operation::kFlashPausesMost; ++i) {
            w.answer({0x7F, 0x36, volts});
            ASSERT_EQ(w.next(), first);
            ASSERT_EQ(w.pauses, i + 1) << "a pause before each resend";
        }
        w.answer({0x7F, 0x36, volts});
        EXPECT_EQ(w.r.result, flash_end::refused) << "the pauses are bounded";
        EXPECT_EQ(w.r.nrc, volts);
    }

    for (const uint8_t code :
         {uint8_t{0x71}, uint8_t{0x72}, uint8_t{0x31}, uint8_t{0x24}}) {
        Wire w;
        at_first_block(w);
        w.answer({0x7F, 0x36, code});
        EXPECT_EQ(w.r.result, flash_end::refused) << int{code};
        EXPECT_EQ(w.r.nrc, code);
        EXPECT_TRUE(w.r.floor);
        EXPECT_TRUE(w.r.open) << "the module holds a download half written";
    }
}

TEST(OperationFlashTest, TheCheckPassesOnItsDeclaredBytesAlone)
{
    plan p = flash_plan(1);
    p.writes.pass = {0xA5, 0x01};
    const auto checked = [&](const Bytes& reply) {
        Wire w;
        ecuio_operation::flash_begin(w.r, p, kFlashWireMost, nullptr);
        w.next();
        w.answer(kErased);
        write_one(w, kBase, kSector);
        EXPECT_EQ(w.next(), kCheck);
        w.answer(reply);
        return w.r.result;
    };
    EXPECT_EQ(checked({0x71, 0x01, 0xFF, 0x01, 0xA5, 0x01}),
              flash_end::written);
    EXPECT_EQ(checked({0x71, 0x01, 0xFF, 0x01, 0xA5, 0x01, 0x09}),
              flash_end::written);
    EXPECT_EQ(checked({0x71, 0x01, 0xFF, 0x01, 0xA5, 0x02}),
              flash_end::check_failed)
        << "a positive answer with a failed status";
    EXPECT_EQ(checked({0x71, 0x01, 0xFF, 0x01, 0xA5}), flash_end::check_failed);
    Wire cut;
    ecuio_operation::flash_begin(cut.r, p, kFlashWireMost, nullptr);
    cut.next();
    cut.answer(kErased);
    write_one(cut, kBase, kSector);
    cut.next();
    const Bytes held = {0x71, 0x01, 0xFF, 0x01, 0xA5, 0x01};
    ecuio_operation::flash_reply(cut.r, held.data(), 5);
    EXPECT_EQ(cut.r.result, flash_end::check_failed)
        << "the pass bytes are the answer's own, never past its end";
    EXPECT_EQ(checked({0x71, 0x01, 0xFF, 0x01}), flash_end::check_failed);
    EXPECT_EQ(checked({0x71, 0x01, 0xFF, 0x00, 0xA5, 0x01}),
              flash_end::bad_reply);
    EXPECT_EQ(checked({0x7F, 0x31, 0x22}), flash_end::refused);
}

TEST(OperationFlashTest, TheBlockCounterRunsFromOneAndWraps)
{
    const plan p = flash_plan(1, 300 * 16);
    Wire w;
    ecuio_operation::flash_begin(w.r, p, kFlashWireMost, nullptr);
    w.next();
    w.answer(kErased);
    write_one(w, kBase, 300 * 16, {0x74, 0x10, 0x12}, 16);
    EXPECT_EQ(w.sent[2][1], 0x01) << "the first block is 01";
    EXPECT_EQ(w.sent[2 + 254][1], 0xFF);
    EXPECT_EQ(w.sent[2 + 255][1], 0x00) << "FF wraps to 00";
    EXPECT_EQ(w.sent[2 + 256][1], 0x01);
}

TEST(OperationFlashTest, ABlockIsTheModulesLengthOrTheTransportsLess)
{
    const plan exact = flash_plan(1, 2048);
    Wire at_it;
    ecuio_operation::flash_begin(at_it.r, exact, kFlashWireMost, nullptr);
    at_it.next();
    at_it.answer(kErased);
    write_one(at_it, kBase, 2048, kOpened, 1024);
    EXPECT_EQ(at_it.sent.size(), 5u) << "an exact multiple: no empty block";

    const plan short_last = flash_plan(1, 2500);
    Wire tail;
    ecuio_operation::flash_begin(tail.r, short_last, kFlashWireMost, nullptr);
    tail.next();
    tail.answer(kErased);
    write_one(tail, kBase, 2500, kOpened, 1024);
    EXPECT_EQ(tail.sent[4].size(), 2u + 452u) << "the short last block";

    Wire clamped;
    ecuio_operation::flash_begin(clamped.r, exact, 514, nullptr);
    clamped.next();
    clamped.answer(kErased);
    write_one(clamped, kBase, 2048, {0x74, 0x20, 0x0F, 0xFF}, 512);
    EXPECT_EQ(clamped.r.block, 512u) << "the transport's write, not the 74's";

    Wire wide;
    ecuio_operation::flash_begin(wide.r, exact, 100000, nullptr);
    EXPECT_EQ(wide.r.wire_max, kFlashWireMost);

    Wire none;
    ecuio_operation::flash_begin(none.r, exact, 2, nullptr);
    none.next();
    none.answer(kErased);
    none.next();
    none.answer(kOpened);
    EXPECT_EQ(none.r.result, flash_end::bad_reply)
        << "no block fits a write of two bytes";

    for (const Bytes& bad :
         {Bytes{0x74, 0x20, 0x00, 0x02}, Bytes{0x74, 0x00},
          Bytes{0x74, 0x50, 1, 2, 3, 4, 5}, Bytes{0x74, 0x20, 0x04}}) {
        Wire w;
        ecuio_operation::flash_begin(w.r, exact, kFlashWireMost, nullptr);
        w.next();
        w.answer(kErased);
        w.next();
        w.answer(bad);
        EXPECT_EQ(w.r.result, flash_end::bad_reply);
    }
}

TEST(OperationFlashTest, OnlyTheSectorsThatDifferAreWritten)
{
    plan p = flash_plan(3);
    Bytes current = p.steps[0].blob;
    current[kSector + 9] ^= 0x40;
    Wire w;
    ecuio_operation::flash_begin(w.r, p, kFlashWireMost, current.data());
    ASSERT_EQ(w.r.chosen, 1u);
    EXPECT_EQ(w.r.sectors[0].address, kBase + kSector);
    EXPECT_EQ(ecuio_operation::flash_bytes(w.r), uint64_t{kSector});
    EXPECT_EQ(w.next(), erase_of(kBase + kSector, kSector));

    Wire whole;
    ecuio_operation::flash_begin(whole.r, p, kFlashWireMost, nullptr);
    EXPECT_EQ(whole.r.chosen, 3u) << "no copy: every sector";

    const Bytes same = p.steps[0].blob;
    Wire nothing;
    ecuio_operation::flash_begin(nothing.r, p, kFlashWireMost, same.data());
    EXPECT_EQ(nothing.r.chosen, 0u);
    EXPECT_EQ(nothing.next(), kCheck) << "nothing to write: the check alone";
    EXPECT_EQ(nothing.journals, 0u);

    p.steps[0].dfi = 0x11;
    Wire packed;
    ecuio_operation::flash_begin(packed.r, p, kFlashWireMost, same.data());
    EXPECT_EQ(packed.r.chosen, 3u)
        << "a packed image can't be compared with the memory";
}

TEST(OperationFlashTest, ACancelIsCleanBeforeTheFloorAndSaysSoAfter)
{
    const plan p = flash_plan(1);
    Wire before;
    ecuio_operation::flash_begin(before.r, p, kFlashWireMost, nullptr);
    ecuio_operation::flash_cancel(before.r);
    EXPECT_EQ(before.r.result, flash_end::canceled);
    EXPECT_FALSE(before.r.floor);
    EXPECT_TRUE(before.next().empty());

    Wire refused;
    ecuio_operation::flash_begin(refused.r, p, kFlashWireMost, nullptr);
    refused.next();
    ecuio_operation::flash_cancel(refused.r);
    EXPECT_EQ(refused.r.result, flash_end::none) << "the erase is out";
    refused.answer({0x7F, 0x31, 0x22});
    EXPECT_EQ(refused.r.result, flash_end::refused);
    EXPECT_FALSE(refused.r.floor);

    Wire after;
    ecuio_operation::flash_begin(after.r, p, kFlashWireMost, nullptr);
    after.next();
    after.answer(kErased);
    after.next();
    ecuio_operation::flash_cancel(after.r);
    after.answer(kOpened);
    EXPECT_EQ(after.r.result, flash_end::canceled);
    EXPECT_TRUE(after.r.floor);
    EXPECT_TRUE(after.r.open);
}

TEST(OperationFlashTest, AnAnswerOfTheWrongShapeEndsTheRun)
{
    const plan p = flash_plan(1);
    for (const Bytes& bad : {Bytes{0x71, 0x01, 0xFF, 0x01}, Bytes{0x71, 0x01},
                             Bytes{0x51, 0x01}}) {
        Wire w;
        ecuio_operation::flash_begin(w.r, p, kFlashWireMost, nullptr);
        w.next();
        w.answer(bad);
        EXPECT_EQ(w.r.result, flash_end::bad_reply);
        EXPECT_TRUE(w.r.floor) << "an answer at all may follow a run erase";
    }
    Wire block;
    ecuio_operation::flash_begin(block.r, p, kFlashWireMost, nullptr);
    block.next();
    block.answer(kErased);
    block.next();
    block.answer(kOpened);
    block.next();
    block.answer({0x76, 0x02});
    EXPECT_EQ(block.r.result, flash_end::bad_reply) << "the counter echoed";
    const plan tiny = flash_plan(1, 16);
    Wire exit;
    ecuio_operation::flash_begin(exit.r, tiny, kFlashWireMost, nullptr);
    exit.next();
    exit.answer(kErased);
    exit.next();
    exit.answer(kOpened);
    exit.next();
    exit.answer({0x76, 0x01});
    exit.next();
    exit.answer({0x76});
    EXPECT_EQ(exit.r.result, flash_end::bad_reply);
}
