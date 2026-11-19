// File/layer: tests/test_reply_segmented.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestEngineTest (whether the engine expects a reply to
//          take more than one CAN frame, ecuio_engine_reply_segmented in
//          request_engine_pack.h, and what teaches it: Known Issue #42's
//          hold reads it)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"

#include <cstdint>
#include <ios>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

/* Channel c's due request sent at now and answered by reply 4 ms later
 * (none when empty); the request's index */
int32_t ask(ecuio_request_engine* e, uint32_t c, int64_t now,
            const Bytes& reply)
{
    const int32_t r = ecuio_engine_due(e, c, now);
    EXPECT_NE(r, ECUIO_ENGINE_NONE);
    if (r == ECUIO_ENGINE_NONE) {
        return r;
    }
    ecuio_engine_sent(e, c, r, true, now);
    if (!reply.empty()) {
        (void)ecuio_engine_on_frame(e, c, reply.data(),
                                    static_cast<uint16_t>(reply.size()),
                                    now + 4 * kMs, nullptr);
    }
    return r;
}

/* A positive reply to 22 <did> with n bytes of data */
Bytes did_reply(uint16_t did, size_t n)
{
    Bytes b = {0x62, static_cast<uint8_t>(did >> 8), static_cast<uint8_t>(did)};
    b.insert(b.end(), n, uint8_t{0x2A});
    return b;
}

bool segmented(const ecuio_request_engine& e, int32_t r)
{
    return ecuio_engine_reply_segmented(&e, 0, r);
}

} // namespace

/* Nothing asked yet is one frame: a DID and a PID before their first
 * reply, no request, a row past the table */
TEST(RequestEngineTest, ARequestNotYetAnsweredCountsAsOneFrame)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    const uint8_t did[3] = {0x22, 0x1E, 0x00};
    const uint8_t pid[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid, 2), ecuio_engine_add_result::added);
    EXPECT_FALSE(segmented(e, 0));
    EXPECT_FALSE(segmented(e, 1));
    EXPECT_FALSE(segmented(e, ECUIO_ENGINE_NONE));
    EXPECT_FALSE(segmented(e, 2));
}

/* A row's last answer sets it: 7 bytes are a Single Frame, 8 a First
 * Frame and a Consecutive Frame, a negative reply one frame, and a
 * shorter reply after a long one makes it one frame again */
TEST(RequestEngineTest, ARowsLastAnswerSetsItsFrames)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    const uint8_t did[3] = {0x22, 0x1E, 0x00};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E00, 4)), 0);
    EXPECT_EQ(e.requests[0].reply_frames, 1u);
    EXPECT_FALSE(segmented(e, 0));

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E00, 5)), 0);
    EXPECT_EQ(e.requests[0].reply_frames, 2u);
    EXPECT_TRUE(segmented(e, 0));

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, {0x7F, 0x22, 0x31}), 0);
    ASSERT_EQ(e.requests[0].dropped, 0u) << "no probe: nothing drops";
    EXPECT_EQ(e.requests[0].reply_frames, 1u);
    EXPECT_FALSE(segmented(e, 0)) << "a refused row holds nothing";

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E00, 5)), 0);
    EXPECT_TRUE(segmented(e, 0));
    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E00, 4)), 0);
    EXPECT_FALSE(segmented(e, 0));

    /* a pending reply (7F 22 78) is no answer: the row keeps its frames */
    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E00, 5)), 0);
    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, {0x7F, 0x22, 0x78}), 0);
    EXPECT_TRUE(segmented(e, 0));
}

/* Out of step (after a timeout) a negative reply may be the late answer
 * to the row that timed out, of the same service: it teaches the row now
 * out nothing */
TEST(RequestEngineTest, ALateNegativeReplyOutOfStepTeachesNothing)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    const uint8_t a[3] = {0x22, 0x1E, 0x00};
    const uint8_t b[3] = {0x22, 0x1E, 0x01};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, b, 3), ecuio_engine_add_result::added);
    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    now += 200 * kMs;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_FALSE(e.chans[0].in_step);
    e.requests[1].reply_frames = 3;
    ASSERT_EQ(ask(&e, 0, now, {0x7F, 0x22, 0x31}), 1);
    EXPECT_EQ(e.requests[1].reply_frames, 3u) << "the refusal may be row 0's";
}

/* A pack's reply is its records' lengths added: 7 bytes one frame, 8
 * two, 20 three */
TEST(RequestEngineTest, APacksFramesAreItsRecordsLengths)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    e.chans[0].pack.reply_len = 7;
    EXPECT_FALSE(segmented(e, ECUIO_ENGINE_PACK));
    e.chans[0].pack.reply_len = 8;
    EXPECT_TRUE(segmented(e, ECUIO_ENGINE_PACK));
    e.chans[0].pack.reply_len = 20;
    EXPECT_TRUE(segmented(e, ECUIO_ENGINE_PACK));
}

/* A scanned DID joins the rotation knowing the frames its scan's reply
 * took, so its first poll is judged: a 20-byte answer three frames, a
 * 5-byte one a Single Frame */
TEST(RequestEngineTest, AScannedDidJoinsKnowingItsScanReplysFrames)
{
    for (const size_t n : {size_t{17}, size_t{2}}) {
        ecuio_request_engine e;
        ASSERT_TRUE(ecuio_engine_init(&e, 1));
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
        ASSERT_EQ(ask(&e, 0, 0, did_reply(0x1E20, n)), ECUIO_ENGINE_ONCE);
        ASSERT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
        ASSERT_EQ(e.count, 1u) << "the found DID joined the rotation";
        EXPECT_EQ(e.requests[0].reply_frames, n == 17 ? 3u : 1u);
        EXPECT_EQ(segmented(e, 0), n == 17) << n << " bytes of data";
    }
}

/* A scanned DID answered after its wait (the adapter hands a long reply
 * over only whole) joins knowing its frames too */
TEST(RequestEngineTest, ALateScanReplySeedsItsRowsFrames)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E21));
    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, {}), ECUIO_ENGINE_ONCE);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_EQ(ask(&e, 0, now, {}), ECUIO_ENGINE_ONCE);
    ASSERT_EQ(e.chans[0].once.bytes[2], 0x21u) << "1E21's request is out";
    const Bytes late = did_reply(0x1E20, 17);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, late.data(),
                                    static_cast<uint16_t>(late.size()),
                                    now + 4 * kMs, nullptr),
              ecuio_reply::none);
    ASSERT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    ASSERT_EQ(e.count, 1u);
    EXPECT_EQ(e.requests[0].bytes[2], 0x20u);
    EXPECT_TRUE(segmented(e, 0));
}

/* Every identification and diagnostics item is segmented before any
 * answer (most answers take several frames: a VIN or CALID three, an ECU
 * name four, a part number past four characters two), and so is a
 * scanned F190; a talk in the one-shot is never judged, and the walk's
 * bitmaps and any other scanned DID count as one frame */
TEST(RequestEngineTest, EveryIdentificationAndDiagnosticsItemIsSegmented)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ecuio_engine_identify(&e);
    ecuio_engine_diagnose(&e, nullptr);
    int64_t now = 0;
    uint32_t idents = 0;
    uint32_t diags = 0;
    for (int i = 0; i < 40 && (e.chans[0].ident_pending > 0 ||
                               e.chans[0].diag_pending > 0);
         ++i, now += 100 * kMs) {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << "item " << i;
        ASSERT_TRUE(e.chans[0].identifying || e.chans[0].diagnosing);
        idents += e.chans[0].identifying ? 1u : 0u;
        diags += e.chans[0].diagnosing ? 1u : 0u;
        EXPECT_TRUE(segmented(e, ECUIO_ENGINE_ONCE)) << "item " << i;
        e.chans[0].talk.out = true;
        EXPECT_FALSE(segmented(e, ECUIO_ENGINE_ONCE)) << "a talk";
        e.chans[0].talk.out = false;
        ecuio_engine_sent(&e, 0, r, true, now);
        const uint8_t sid = e.chans[0].once.bytes[0];
        const Bytes refused = {0x7F, sid, 0x31};
        (void)ecuio_engine_on_frame(&e, 0, refused.data(), 3, now + 4 * kMs,
                                    nullptr);
    }
    EXPECT_EQ(idents, ECUIO_IDENT_COUNT);
    EXPECT_GT(diags, 0u);

    for (const uint16_t did : {uint16_t{0xF190}, uint16_t{0x1E20}}) {
        ecuio_request_engine scan;
        ASSERT_TRUE(ecuio_engine_init(&scan, 1));
        ASSERT_TRUE(ecuio_engine_scan_add(&scan, 0, did));
        ASSERT_EQ(ecuio_engine_due(&scan, 0, 0), ECUIO_ENGINE_ONCE);
        ASSERT_TRUE(scan.chans[0].scanning);
        EXPECT_EQ(ecuio_engine_reply_segmented(&scan, 0, ECUIO_ENGINE_ONCE),
                  did == 0xF190)
            << std::hex << did;
    }

    ecuio_request_engine walk;
    ASSERT_TRUE(ecuio_engine_init(&walk, 1));
    const uint8_t pid[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&walk, 0, pid, 2),
              ecuio_engine_add_result::added);
    ecuio_engine_probe(&walk);
    ASSERT_EQ(ecuio_engine_due(&walk, 0, 0), ECUIO_ENGINE_ONCE);
    ASSERT_TRUE(walk.chans[0].walking);
    EXPECT_FALSE(ecuio_engine_reply_segmented(&walk, 0, ECUIO_ENGINE_ONCE));
}
