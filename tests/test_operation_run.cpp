// File/layer: tests/test_operation_run.cpp | C++17, Qt-free, every platform
// Testing: UNIT: OperationRunTest (test_obd2)
//
// A dump's run as a machine (core/src/operation_run.h), stepped against
// the simulated ECU's memory with no clock and no wire: the order of its
// requests, each range twice, the identifiers before and after, and what
// each reply, silence, cancel and full sink does to it.

#include "operation_run_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace operation_run_test;
using sim_memory_test::nrc;
using sim_memory_test::read_of;
using sim_memory_test::upload_of;

const Bytes kHardware = {0x22, 0xD2, 0x20};
const Bytes kSoftware = {0x22, 0xD2, 0x21};

/* A script answering reply to the nth request sent, and to no other */
Drive::Script at(size_t nth, Bytes reply, uint32_t waited_ms = 10)
{
    return [=](const Bytes&, size_t n, Bytes* out, uint32_t* waited) {
        if (n != nth) {
            return false;
        }
        *out = reply;
        *waited = waited_ms;
        return true;
    };
}

/* A script answering reply to the first times asks of the nth request's
 * bytes, then leaving it to the ECU */
Drive::Script times(size_t nth, int asks, Bytes reply)
{
    auto left = std::make_shared<int>(asks);
    auto bytes = std::make_shared<Bytes>();
    return [=](const Bytes& q, size_t n, Bytes* out, uint32_t*) {
        if (n == nth) {
            *bytes = q;
        }
        if (n < nth || q != *bytes || *left == 0) {
            return false;
        }
        --*left;
        *out = reply;
        return true;
    };
}

TEST(OperationRunTest, AReadIsEachRangeTwiceBetweenTheIdentifiers)
{
    Drive d(plan_of(
        {read_step(kBase, 1000, 256), read_step(kBase + 0x8000, 512, 512)}));
    EXPECT_EQ(d.go(), run_end::complete);
    EXPECT_EQ(d.entered, std::vector<uint32_t>{ECUIO_STEP_SESSION_EXTENDED});
    /* 2 identifiers, 4 chunks twice, 1 chunk twice, 2 identifiers */
    ASSERT_EQ(d.sent.size(), 2u + 8u + 2u + 2u);
    EXPECT_EQ(d.sent[0], kHardware);
    EXPECT_EQ(d.sent[1], kSoftware);
    for (const size_t pass : {size_t{0}, size_t{1}}) {
        const size_t first = 2 + 4 * pass;
        EXPECT_EQ(d.sent[first], read_of(kBase, 256));
        EXPECT_EQ(d.sent[first + 1], read_of(kBase + 256, 256));
        EXPECT_EQ(d.sent[first + 3], read_of(kBase + 768, 232))
            << "the last chunk asks what is left";
    }
    EXPECT_EQ(d.sent[10], read_of(kBase + 0x8000, 512));
    EXPECT_EQ(d.sent[11], read_of(kBase + 0x8000, 512));
    EXPECT_EQ(d.sent[12], kHardware);
    EXPECT_EQ(d.sent[13], kSoftware);
    EXPECT_EQ(d.sink->got[0][0], region(kBase, 1000));
    EXPECT_EQ(d.sink->got[1][0], region(kBase, 1000));
    EXPECT_EQ(d.sink->got[0][1], region(kBase + 0x8000, 512));
    EXPECT_EQ(d.sink->got[1][1], d.sink->got[0][1]);
    ASSERT_EQ(d.sink->idents.size(), 2u) << "the first reading alone";
    EXPECT_EQ(d.sink->idents[0],
              Bytes(ECUIO_SIM_HARDWARE_TEXT, ECUIO_SIM_HARDWARE_TEXT + 16));
    EXPECT_EQ(d.r.handed, 2u * 1512u);
    EXPECT_EQ(ecuio_operation::total_bytes(d.r), 2u * 1512u);

    Drive bare(plan_of({read_step(kBase, 64, 64)}, false));
    EXPECT_EQ(bare.go(), run_end::complete);
    EXPECT_EQ(bare.sent,
              (std::vector<Bytes>{read_of(kBase, 64), read_of(kBase, 64)}));
}

/* A request's address and size take the bytes the step gives them, and
 * no chunk reaches past its range */
TEST(OperationRunTest, EveryRequestStaysInsideItsRange)
{
    const struct {
        uint8_t address_bytes;
        uint8_t size_bytes;
        uint32_t bytes;
        uint32_t chunk;
    } shapes[] = {{4, 2, 4096, 4094},
                  {3, 1, 700, 255},
                  {3, 2, 1, 256},
                  {4, 4, 1025, 1},
                  {3, 3, 65536, 1000}};
    for (const auto& shape : shapes) {
        step s = read_step(kBase, shape.bytes, shape.chunk);
        s.address_bytes = shape.address_bytes;
        s.size_bytes = shape.size_bytes;
        Drive d(plan_of({s}, false));
        ASSERT_EQ(d.go(), run_end::complete) << shape.bytes;
        uint64_t next = kBase;
        for (const Bytes& q : d.sent) {
            ASSERT_EQ(q.size(), 2u + shape.address_bytes + shape.size_bytes);
            EXPECT_EQ(q[1], (shape.size_bytes << 4) | shape.address_bytes);
            uint64_t address = 0;
            uint64_t size = 0;
            for (int i = 0; i < shape.address_bytes; ++i) {
                address = address << 8 | q[2 + i];
            }
            for (int i = 0; i < shape.size_bytes; ++i) {
                size = size << 8 | q[2 + shape.address_bytes + i];
            }
            next = next == kBase + shape.bytes ? kBase : next; /* pass 2 */
            EXPECT_EQ(address, next);
            EXPECT_GE(size, 1u);
            EXPECT_LE(size, shape.chunk);
            next += size;
            ASSERT_LE(next, kBase + shape.bytes) << "past the range";
        }
        EXPECT_EQ(next, kBase + shape.bytes);
        EXPECT_EQ(d.sink->got[1][0], region(kBase, shape.bytes));
    }
}

TEST(OperationRunTest, AnUploadIsOpenedTwiceAndItsCounterWraps)
{
    Drive d(plan_of({upload_step(kBase, 4000)}));
    d.ecu.m.faults.block = 0x0A; /* 8 bytes of data in each 76 */
    EXPECT_EQ(d.go(), run_end::complete);
    /* Each pass: a 35, 500 blocks, a 37 */
    ASSERT_EQ(d.sent.size(), 2u + 2u * 502u + 2u);
    for (const size_t first : {size_t{2}, size_t{2 + 502}}) {
        EXPECT_EQ(d.sent[first], upload_of(kBase, 4000));
        EXPECT_EQ(d.sent[first + 1], (Bytes{0x36, 0x01}));
        EXPECT_EQ(d.sent[first + 255], (Bytes{0x36, 0xFF}));
        EXPECT_EQ(d.sent[first + 256], (Bytes{0x36, 0x00}));
        EXPECT_EQ(d.sent[first + 257], (Bytes{0x36, 0x01}));
        EXPECT_EQ(d.sent[first + 500], (Bytes{0x36, 0xF4}));
        EXPECT_EQ(d.sent[first + 501], (Bytes{0x37}));
    }
    EXPECT_EQ(d.sink->got[0][0], region(kBase, 4000));
    EXPECT_EQ(d.sink->got[1][0], region(kBase, 4000));

    /* A last block shorter than the rest, and a read after an upload */
    Drive mixed(plan_of({upload_step(kBase, 2500), read_step(kBase, 64, 64)}));
    EXPECT_EQ(mixed.go(), run_end::complete);
    EXPECT_EQ(mixed.count(0x36), 2u * 3u) << "1024, 1024 and 452 bytes";
    EXPECT_EQ(mixed.sink->got[1][0], region(kBase, 2500));
    EXPECT_EQ(mixed.sink->got[1][1], region(kBase, 64));
}

/* A chunk or an identifier answered 21, or not at all, is asked again:
 * three times in all */
TEST(OperationRunTest, AChunkIsAskedThreeTimesAtMost)
{
    for (const Bytes& bad : {nrc(0x23, 0x21), Bytes{}}) {
        Drive twice(plan_of({read_step(kBase, 512, 256)}));
        twice.script = times(3, 2, bad); /* the second chunk */
        EXPECT_EQ(twice.go(), run_end::complete);
        EXPECT_EQ(twice.count(0x23), 4u + 2u);
        EXPECT_EQ(twice.sent[3], twice.sent[4]);
        EXPECT_EQ(twice.sent[4], twice.sent[5]);
        EXPECT_EQ(twice.sink->got[0][0], region(kBase, 512));

        Drive thrice(plan_of({read_step(kBase, 512, 256)}));
        thrice.script = times(3, 3, bad);
        EXPECT_EQ(thrice.go(),
                  bad.empty() ? run_end::no_answer : run_end::refused);
        EXPECT_EQ(thrice.r.nrc, bad.empty() ? 0x00 : 0x21);
        EXPECT_EQ(thrice.sent.size(), 3u + 3u) << "nothing after the third";
    }
    /* The count is each request's own: two chunks each asked three times */
    Drive each(plan_of({read_step(kBase, 512, 256)}, false));
    int asks = 0;
    each.script = [&asks](const Bytes&, size_t, Bytes* out, uint32_t*) {
        *out = Bytes{};
        return ++asks % 3 != 0;
    };
    EXPECT_EQ(each.go(), run_end::complete);
    EXPECT_EQ(each.sent.size(), 4u * 3u);

    Drive ident(plan_of({read_step(kBase, 64, 64)}));
    ident.script = times(1, 2, nrc(0x22, 0x21));
    EXPECT_EQ(ident.go(), run_end::complete);
    EXPECT_EQ(ident.count(0x22), 4u + 2u);
    Drive lost(plan_of({read_step(kBase, 64, 64)}));
    lost.script = times(0, 3, Bytes{});
    EXPECT_EQ(lost.go(), run_end::no_answer);
    EXPECT_EQ(lost.sent.size(), 3u);
}

TEST(OperationRunTest, ARefusalEndsTheRunWithItsCode)
{
    const struct {
        uint8_t code;
        run_end end;
    } codes[] = {{0x33, run_end::session_lost},
                 {0x14, run_end::refused},
                 {0x31, run_end::refused},
                 {0x22, run_end::refused}};
    for (const auto& c : codes) {
        Drive d(plan_of({read_step(kBase, 512, 256)}));
        d.script = at(3, nrc(0x23, c.code));
        EXPECT_EQ(d.go(), c.end) << int{c.code};
        EXPECT_EQ(d.r.nrc, c.code);
        EXPECT_EQ(d.sent.size(), 4u) << "nothing after a refusal";
    }
    /* A refusal naming another service isn't this request's answer */
    Drive other(plan_of({read_step(kBase, 512, 256)}));
    other.script = at(2, nrc(0x22, 0x31));
    EXPECT_EQ(other.go(), run_end::bad_reply);
    EXPECT_EQ(other.sent.size(), 3u);
    /* The sim's own: every third data reply refused */
    Drive sim(plan_of({read_step(kBase, 2048, 256)}, false));
    sim.ecu.m.faults.refuse = 3;
    EXPECT_EQ(sim.go(), run_end::refused);
    EXPECT_EQ(sim.r.nrc, 0x22);
    EXPECT_EQ(sim.sent.size(), 3u);
    /* An unlock lost mid-read: the next chunk answers 33 */
    Drive relock(plan_of({read_step(kBase, 2048, 256)}, false));
    relock.ecu.m.faults.relock = 2;
    EXPECT_EQ(relock.go(), run_end::session_lost);
    EXPECT_EQ(relock.sent.size(), 3u);
}

/* 78 keeps the wait up to 60 s for one request, then the run ends */
TEST(OperationRunTest, APendingAnswerWaitsToItsBound)
{
    Drive waits(plan_of({read_step(kBase, 512, 256)}));
    waits.script = at(3, nrc(0x23, 0x78), 59999);
    EXPECT_EQ(waits.go(), run_end::complete);
    EXPECT_EQ(waits.count(0x23), 4u) << "a wait is no second request";
    EXPECT_EQ(waits.waited_on, 1);
    EXPECT_EQ(waits.sink->got[0][0], region(kBase, 512));

    Drive late(plan_of({read_step(kBase, 512, 256)}));
    late.script = at(3, nrc(0x23, 0x78), 60000);
    EXPECT_EQ(late.go(), run_end::no_answer);
    EXPECT_EQ(late.sent.size(), 4u);

    /* An upload's block that only waits is a block lost: one 37 */
    Drive block(plan_of({upload_step(kBase, 4096)}, false));
    block.script = at(2, nrc(0x36, 0x78), 60000);
    EXPECT_EQ(block.go(), run_end::no_answer);
    ASSERT_EQ(block.sent.size(), 4u);
    EXPECT_EQ(block.sent[3], (Bytes{0x37}));
}

TEST(OperationRunTest, AReplyOfAnotherLengthIsAFault)
{
    Drive cut(plan_of({read_step(kBase, 2048, 256)}, false));
    cut.ecu.m.faults.cut = 3; /* the third data reply is one byte short */
    EXPECT_EQ(cut.go(), run_end::bad_reply);
    EXPECT_EQ(cut.sent.size(), 3u);
    EXPECT_EQ(cut.sink->got[0][0].size(), 512u) << "never padded";

    Bytes longer = region(kBase + 256, 257);
    longer.insert(longer.begin(), 0x63);
    Bytes another = region(kBase + 256, 256); /* its length, not its service */
    another.insert(another.begin(), 0x62);
    for (const Bytes& odd :
         {longer, another, Bytes{0x63}, Bytes{0x62, 0x00}, Bytes{0x7F, 0x23}}) {
        Drive d(plan_of({read_step(kBase, 512, 256)}, false));
        d.script = at(1, odd);
        EXPECT_EQ(d.go(), run_end::bad_reply) << odd.size();
        EXPECT_EQ(d.sent.size(), 2u);
        EXPECT_EQ(d.sink->got[0][0].size(), 256u) << "never cut";
    }
    /* An identifier: the DID echoed, 1 to 64 bytes of value */
    for (const Bytes& odd : {Bytes{0x62, 0xD2, 0x20},
                             Bytes{0x62, 0xD2, 0x21, 1}, Bytes(3 + 65, 0x62)}) {
        Drive d(plan_of({read_step(kBase, 64, 64)}));
        Bytes reply = odd;
        if (reply.size() > 4) {
            reply[1] = 0xD2;
            reply[2] = 0x20;
        }
        d.script = at(0, reply);
        EXPECT_EQ(d.go(), run_end::bad_reply) << odd.size();
        EXPECT_EQ(d.sent.size(), 1u);
    }
    Drive most(plan_of({read_step(kBase, 64, 64)}));
    Bytes whole(3 + 64, 0x41);
    whole[0] = 0x62;
    whole[1] = 0xD2;
    whole[2] = 0x20;
    most.script = [whole](const Bytes& q, size_t, Bytes* out, uint32_t*) {
        *out = whole;
        return q == kHardware;
    };
    EXPECT_EQ(most.go(), run_end::complete) << "64 bytes of value fit";
}

/* A block lost or wrong is never asked again: one 37, whose answer isn't
 * judged, and the run ends */
TEST(OperationRunTest, AnUploadsFaultSendsOneExitAndEnds)
{
    const struct {
        const char* what;
        Bytes reply;
        run_end end;
    } faults[] = {
        {"no answer", {}, run_end::no_answer},
        {"the wrong counter", {0x76, 0x09, 1, 2, 3}, run_end::bad_reply},
        {"a short block", {0x76, 0x02, 1, 2, 3}, run_end::bad_reply},
        {"a refusal", nrc(0x36, 0x73), run_end::refused},
        {"busy", nrc(0x36, 0x21), run_end::refused},
    };
    for (const auto& f : faults) {
        Drive d(plan_of({upload_step(kBase, 4096)}, false));
        d.script = at(2, f.reply); /* the second 36 */
        EXPECT_EQ(d.go(), f.end) << f.what;
        ASSERT_EQ(d.sent.size(), 4u) << f.what;
        EXPECT_EQ(d.sent[2], (Bytes{0x36, 0x02})) << "asked once";
        EXPECT_EQ(d.sent[3], (Bytes{0x37}));
    }
    /* The sim's own wrong counter, and its 37 refused or unanswered */
    for (const Bytes& exit : {nrc(0x37, 0x24), Bytes{}, Bytes{0x77}}) {
        Drive d(plan_of({upload_step(kBase, 4096)}, false));
        d.ecu.m.faults.seq = 2;
        d.script = [exit](const Bytes& q, size_t, Bytes* out, uint32_t*) {
            *out = exit;
            return q[0] == 0x37;
        };
        EXPECT_EQ(d.go(), run_end::bad_reply);
        EXPECT_EQ(d.count(0x37), 1u);
        EXPECT_EQ(d.sent.back(), (Bytes{0x37}));
    }
    /* A 35 unanswered may have opened one all the same; a 75 that names
     * no usable block did */
    for (const Bytes& open :
         {Bytes{}, Bytes{0x75, 0x20, 0x00, 0x02}, Bytes{0x75, 0x20, 0x10, 0x00},
          Bytes{0x75, 0x20, 0x04}, Bytes{0x75, 0x50, 0, 0, 0, 4, 2}}) {
        Drive d(plan_of({upload_step(kBase, 4096)}, false));
        d.script = at(0, open);
        EXPECT_EQ(d.go(),
                  open.empty() ? run_end::no_answer : run_end::bad_reply);
        EXPECT_EQ(d.sent,
                  (std::vector<Bytes>{upload_of(kBase, 4096), Bytes{0x37}}));
    }
    /* A 35 refused opened none: nothing more is sent */
    Drive refused(plan_of({upload_step(kBase, 4096)}, false));
    refused.script = at(0, nrc(0x35, 0x70));
    EXPECT_EQ(refused.go(), run_end::refused);
    EXPECT_EQ(refused.r.nrc, 0x70);
    EXPECT_EQ(refused.sent.size(), 1u);
    /* The largest block a 75 may name is taken, and a 37 answered
     * anything but 77 is a fault */
    Drive big(plan_of({upload_step(kBase, 8192)}, false));
    big.ecu.m.faults.block = 0xFFF;
    EXPECT_EQ(big.go(), run_end::complete);
    EXPECT_EQ(big.count(0x36), 2u * 3u) << "4093 bytes in each 76";
    Drive exit(plan_of({upload_step(kBase, 1024)}, false));
    exit.script = at(2, nrc(0x37, 0x24));
    EXPECT_EQ(exit.go(), run_end::refused);
    EXPECT_EQ(exit.sent.size(), 3u) << "the 37 itself is not sent again";
    Drive other(plan_of({upload_step(kBase, 1024)}, false));
    other.script = at(2, Bytes{0x76, 0x02});
    EXPECT_EQ(other.go(), run_end::bad_reply);
    EXPECT_EQ(other.sent.size(), 3u);
}

/* Where the second pass isn't the first, the sink stops the run: no
 * request follows, in an upload not even its 37 */
TEST(OperationRunTest, ADifferenceSendsNothingMore)
{
    /* Every fifth data reply has a bit wrong, another bit on each pass:
     * the twentieth is the second pass's fourth chunk */
    Drive flip(plan_of({read_step(kBase, 4096, 256)}));
    flip.ecu.m.faults.flip = 5;
    EXPECT_EQ(flip.go(), run_end::differ);
    EXPECT_EQ(flip.sent.size(), 2u + 20u);
    EXPECT_EQ(flip.sink->differ_step, 0u);
    EXPECT_EQ(flip.sink->differ_at / 256, 3u);

    Drive moving(plan_of(
        {read_step(kBase, 64, 64), read_step(kBase + 0x1000, 4096, 256)}));
    moving.ecu.m.faults.moving = true;
    moving.ecu.m.faults.moving_at = 0x1300;
    EXPECT_EQ(moving.go(), run_end::differ);
    EXPECT_EQ(moving.sink->differ_step, 1u);
    EXPECT_EQ(moving.sink->differ_at, 0x300u);
    EXPECT_EQ(moving.sent.size(), 2u + 2u + 16u + 4u);

    /* The last chunk of all: the machine has every byte, and the sink's
     * word still ends the run as a difference */
    Drive last(plan_of({read_step(kBase, 256, 256)}, false));
    last.ecu.m.faults.flip = 2;
    EXPECT_EQ(last.go(), run_end::differ);
    EXPECT_EQ(last.sent.size(), 2u);
    Drive ahead(plan_of({read_step(kBase, 256, 256)}));
    ahead.ecu.m.faults.flip = 2;
    EXPECT_EQ(ahead.go(), run_end::differ);
    EXPECT_EQ(ahead.sent.size(), 2u + 2u) << "no identifier is read after it";

    Drive upload(plan_of({upload_step(kBase, 4096)}, false));
    upload.ecu.m.faults.moving = true;
    upload.ecu.m.faults.moving_at = 0x10;
    EXPECT_EQ(upload.go(), run_end::differ);
    EXPECT_EQ(upload.sent.size(), 6u + 2u);
    EXPECT_EQ(upload.sent.back(), (Bytes{0x36, 0x01}));
    EXPECT_EQ(upload.count(0x37), 1u) << "the first pass's own";
}

TEST(OperationRunTest, TheIdentifiersMustReadTheSameAfter)
{
    /* From the second data reply the software DID answers another text */
    Drive d(plan_of({read_step(kBase, 512, 256)}));
    d.ecu.m.faults.ident = 2;
    EXPECT_EQ(d.go(), run_end::identity_changed);
    ASSERT_EQ(d.sent.size(), 2u + 4u + 2u);
    EXPECT_EQ(d.sent.back(), kSoftware) << "the hardware's held";
    EXPECT_EQ(d.sink->got[1][0], region(kBase, 512));
    EXPECT_EQ(d.sink->idents.size(), 2u);

    /* The same bytes and one fewer is another value too */
    Drive cut(plan_of({read_step(kBase, 512, 256)}));
    int asked = 0;
    cut.script = [&asked](const Bytes& q, size_t, Bytes* out, uint32_t*) {
        if (q != kHardware || ++asked != 2) {
            return false;
        }
        *out = {0x62, 0xD2, 0x20};
        out->insert(out->end(), ECUIO_SIM_HARDWARE_TEXT,
                    ECUIO_SIM_HARDWARE_TEXT + 15);
        return true;
    };
    EXPECT_EQ(cut.go(), run_end::identity_changed);
    EXPECT_EQ(cut.sent.size(), 2u + 4u + 1u);
}

/* A cancel sends nothing in a read; an upload that is open, or may be,
 * gets its one 37 first */
TEST(OperationRunTest, ACancelSendsNothingInAReadAndOneExitInAnUpload)
{
    const auto cancel_at = [](size_t sent) {
        return [sent](Drive& d) {
            if (d.sent.size() == sent) {
                ecuio_operation::cancel(d.r);
            }
        };
    };
    Drive idle(plan_of({read_step(kBase, 2048, 256)}, false));
    idle.before_next = cancel_at(3);
    EXPECT_EQ(idle.go(), run_end::canceled);
    EXPECT_EQ(idle.sent.size(), 3u);

    for (const bool answered : {true, false}) {
        Drive out(plan_of({read_step(kBase, 2048, 256)}, false));
        out.while_out = cancel_at(3);
        if (!answered) {
            out.script = at(2, Bytes{});
        }
        EXPECT_EQ(out.go(), run_end::canceled);
        EXPECT_EQ(out.sent.size(), 3u) << "not asked again, nothing after";
        EXPECT_EQ(out.sink->got[0][0].size(), 512u) << "its answer dropped";
    }
    Drive pending(plan_of({read_step(kBase, 2048, 256)}, false));
    pending.while_out = cancel_at(2);
    pending.script = at(1, nrc(0x23, 0x78), 100);
    EXPECT_EQ(pending.go(), run_end::canceled) << "the wait ends";
    EXPECT_EQ(pending.sent.size(), 2u);
    EXPECT_EQ(pending.waited_on, 0) << "at the 78, not at the answer behind it";

    Drive between(plan_of({upload_step(kBase, 4096)}, false));
    between.before_next = cancel_at(3);
    EXPECT_EQ(between.go(), run_end::canceled);
    ASSERT_EQ(between.sent.size(), 4u);
    EXPECT_EQ(between.sent[3], (Bytes{0x37}));

    Drive block(plan_of({upload_step(kBase, 4096)}, false));
    block.while_out = cancel_at(3);
    EXPECT_EQ(block.go(), run_end::canceled);
    ASSERT_EQ(block.sent.size(), 4u);
    EXPECT_EQ(block.sent[3], (Bytes{0x37}));
    EXPECT_EQ(block.sink->got[0][0].size(), 1024u);

    Drive opening(plan_of({upload_step(kBase, 4096)}, false));
    opening.while_out = cancel_at(1);
    EXPECT_EQ(opening.go(), run_end::canceled);
    EXPECT_EQ(opening.sent,
              (std::vector<Bytes>{upload_of(kBase, 4096), Bytes{0x37}}));

    Drive refused(plan_of({upload_step(kBase, 4096)}, false));
    refused.while_out = cancel_at(1);
    refused.script = at(0, nrc(0x35, 0x70));
    EXPECT_EQ(refused.go(), run_end::canceled);
    EXPECT_EQ(refused.sent.size(), 1u) << "none was opened";

    /* While a session is entered: at once, and the entry's end is no
     * longer the run's */
    Drive entering(plan_of({read_step(kBase, 64, 64)}));
    ecuio_operation::cancel(entering.r);
    ecuio_operation::entered(entering.r);
    EXPECT_EQ(entering.go(), run_end::canceled);
    EXPECT_TRUE(entering.sent.empty());
    EXPECT_TRUE(entering.entered.empty());

    Drive over(plan_of({read_step(kBase, 64, 64)}, false));
    EXPECT_EQ(over.go(), run_end::complete);
    ecuio_operation::cancel(over.r);
    ecuio_operation::silence(over.r);
    EXPECT_EQ(over.r.result, run_end::complete) << "an ended run stays ended";
}

/* A request that brings data waits for the sink's room; one that brings
 * none never does, and a held request isn't one of a chunk's three */
TEST(OperationRunTest, AFullSinkHoldsTheNextDataRequest)
{
    Drive d(plan_of({read_step(kBase, 512, 256), upload_step(kBase, 2048)}));
    /* Full once ahead of each request; the driver makes room on a hold */
    size_t seen = SIZE_MAX;
    d.before_next = [&seen](Drive& at) {
        if (at.sent.size() != seen) {
            seen = at.sent.size();
            at.sink->full = true;
        }
    };
    d.script = times(3, 2, Bytes{}); /* the second chunk, twice unanswered */
    EXPECT_EQ(d.go(), run_end::complete);
    EXPECT_EQ(d.holds, 4 + 2 + 4) << "each 23 and each 36, the two asked again";
    EXPECT_EQ(d.sent.size(), 2u + 4u + 2u + 2u * 4u + 2u);
    EXPECT_EQ(d.sink->got[1][1], region(kBase, 2048));
}

/* A step naming another session than the one the run is in asks for it:
 * one entry, and so one unlock, for each run of steps in a session */
TEST(OperationRunTest, EachSessionEnteredIsOneUnlock)
{
    const auto in = [](uint32_t session, uint32_t at) {
        step s = read_step(kBase + at, 64, 64);
        s.session = session;
        return s;
    };
    const uint32_t e = ECUIO_STEP_SESSION_EXTENDED;
    const uint32_t g = ECUIO_STEP_SESSION_PROGRAMMING;
    const plan p =
        plan_of({in(e, 0), in(g, 0x100), in(g, 0x200), in(e, 0x300)});
    Drive d(p);
    EXPECT_EQ(d.go(), run_end::complete);
    EXPECT_EQ(d.entered, (std::vector<uint32_t>{e, g, e}));
    EXPECT_EQ(d.entered.size(), ecuio_operation::unlock_sessions(p));
    EXPECT_EQ(d.count(0x22), 4u) << "the identifiers once before, once after";
    EXPECT_EQ(d.sent[0], kHardware);
    EXPECT_EQ(d.count(0x23), 8u);

    Drive never(p);
    never.enter_fails = true;
    EXPECT_EQ(never.go(), run_end::unlock_refused);
    EXPECT_TRUE(never.sent.empty());
    EXPECT_EQ(never.entered.size(), 1u);

    Drive second(p);
    second.before_next = [](Drive& at) {
        at.enter_fails = at.entered.size() == 1;
    };
    EXPECT_EQ(second.go(), run_end::unlock_refused);
    EXPECT_EQ(second.entered, (std::vector<uint32_t>{e, g}));
    EXPECT_EQ(second.sent.size(), 2u + 2u) << "nothing after the refusal";
}

} // namespace
