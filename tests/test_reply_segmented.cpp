// File/layer: tests/test_reply_segmented.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestEngineTest (whether the engine expects a reply to
//          take more than one CAN frame, ecuio_engine_reply_segmented in
//          request_engine_pack.h, and what teaches it: a row's defs, its
//          answers, a wait run out; Known Issue #42's hold reads it)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"

#include <cstdint>
#include <cstring>
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

/* A def on 0x7E8 keyed key (2 bytes: 41 <PID>, 3: 62 <DID>): a number of
 * bits bits at bit offset off from byte at, or with bits 0 a text of
 * text bytes (0: to the payload's end) */
ecuio_signal_def_t def(uint32_t key, uint8_t key_len, uint16_t at, uint8_t bits,
                       uint8_t off = 0, uint16_t text = 0)
{
    ecuio_signal_def_t d = {};
    d.message_id = 0x7E8;
    d.mux_length = key_len;
    d.mux_key = key;
    d.kind =
        static_cast<uint8_t>(bits == 0 ? ECUIO_VALUE_TEXT : ECUIO_VALUE_UINT);
    d.byte_order = off != 0 ? ECUIO_BYTE_ORDER_LE : ECUIO_BYTE_ORDER_BE;
    d.start_byte = at;
    d.bit_offset = off;
    d.bit_length = bits;
    d.byte_length = text;
    d.scale = 1.0;
    return d;
}

ecuio_signal_set_t set_of(const std::vector<ecuio_signal_def_t>& defs)
{
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(defs.size());
    s.defs = defs.data();
    return s;
}

} // namespace

/* A row not yet answered is segmented when a def it was enrolled for
 * reads past a Single Frame's 7 bytes or to the payload's end, or, for a
 * Service 01 PID, when J1979's length takes two frames; any other is one
 * frame, a row added with no def too. Derive and the plan's derive mark
 * the same rows */
TEST(RequestEngineTest, ARowNotYetAnsweredIsJudgedByItsDefsAndJ1979)
{
    const std::vector<ecuio_signal_def_t> defs = {
        def(0x621E00, 3, 3, 16),       /* row 0: a word, to byte 5 */
        def(0x621E01, 3, 3, 0, 0, 10), /* row 1: 10 bytes of text */
        def(0x621E02, 3, 3, 0),        /* row 2: text to the end */
        def(0x621E03, 3, 3, 8),        /* row 3: a byte, */
        def(0x621E04, 3, 3, 32),       /* row 4: to byte 7, one frame */
        def(0x621E05, 3, 6, 8, 4),     /* row 5: bits 4-11 of byte 6 on */
        def(0x621E03, 3, 4, 32),       /* row 3's word to byte 8 */
        def(0x410C, 2, 2, 16),         /* row 6: rpm, one frame */
        def(0x4168, 2, 3, 8),          /* row 7: J1979's 7 bytes */
        def(0x4196, 2, 2, 8),          /* row 8: no J1979 length */
    };
    const bool expected[] = {false, true,  true, true, false,
                             true,  false, true, false};
    const ecuio_signal_set_t set = set_of(defs);
    const uint32_t rx = 0x7E8;
    std::vector<uint32_t> rows(defs.size());
    ecuio_derived derived;
    ASSERT_EQ(ecuio_engine_plan(&set, &rx, 1, rows.data(), &derived),
              ecuio_plan_result::ok);
    ASSERT_EQ(derived.total, 9u);
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1, derived.total + 1));
    ASSERT_TRUE(ecuio_engine_derive_planned(&e, &set, &rx, 1, rows.data()));
    ecuio_request_engine derives;
    ASSERT_TRUE(ecuio_engine_init(&derives, 1, derived.total));
    ASSERT_TRUE(ecuio_engine_derive(&derives, &set, &rx, 1, nullptr));
    ASSERT_EQ(derives.count, derived.total);
    EXPECT_EQ(std::memcmp(derives.requests.data(), e.requests.data(),
                          derived.total * sizeof(ecuio_request)),
              0);
    for (int32_t r = 0; r < 9; ++r) {
        EXPECT_EQ(segmented(e, r), expected[r]) << "row " << r;
    }

    const uint8_t did[3] = {0x22, 0x1E, 0x10};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    EXPECT_FALSE(segmented(e, 9)) << "a row with no def";
    EXPECT_FALSE(segmented(e, ECUIO_ENGINE_NONE));
    EXPECT_FALSE(segmented(e, 10));
}

/* A row its defs mark segmented stays so while its first ask is out (the
 * hold judges the request on the bus by it); once its wait has run out
 * before any answer it counts one frame, so a row that only times out
 * holds the bus once, not on every lap; an answer sets it either way */
TEST(RequestEngineTest, AWaitRunOutOrAnAnswerOutweighsTheDefs)
{
    const std::vector<ecuio_signal_def_t> defs = {
        def(0x621E01, 3, 3, 0, 0, 10), def(0x621E02, 3, 3, 0, 0, 10)};
    const ecuio_signal_set_t set = set_of(defs);
    const uint32_t rx = 0x7E8;
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ASSERT_TRUE(ecuio_engine_derive(&e, &set, &rx, 1, nullptr));
    ASSERT_TRUE(segmented(e, 0));
    ASSERT_TRUE(segmented(e, 1));

    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    EXPECT_TRUE(segmented(e, 0)) << "a first ask out";
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.requests[0].reply_frames, 1u);
    EXPECT_FALSE(segmented(e, 0));

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E02, 4)), 1);
    EXPECT_FALSE(segmented(e, 1)) << "answered in one frame";
    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E01, 10)), 0);
    EXPECT_TRUE(segmented(e, 0)) << "answered in two";
}

/* A positive reply after its row's wait ran out teaches the row its
 * frames, whether nothing, another row or a pack is out: a DID's by its key, a
 * PID's at J1979's length for it alone. A reply shaped as a pack, or
 * naming no row, teaches nothing */
TEST(RequestEngineTest, ALateReplyTeachesItsRowsFrames)
{
    const std::vector<ecuio_signal_def_t> defs = {
        def(0x621E01, 3, 3, 0, 0, 10), /* row 0: 10 bytes of text */
        def(0x621E02, 3, 3, 16),       /* row 1: a word */
        def(0x4168, 2, 3, 8),          /* row 2: J1979's 7 bytes */
        def(0x410C, 2, 2, 16),         /* row 3: rpm */
    };
    const ecuio_signal_set_t set = set_of(defs);
    const uint32_t rx = 0x7E8;
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ASSERT_TRUE(ecuio_engine_derive(&e, &set, &rx, 1, nullptr));
    const auto late = [&e](const Bytes& reply, int64_t at) {
        return ecuio_engine_on_frame(&e, 0, reply.data(),
                                     static_cast<uint16_t>(reply.size()), at,
                                     nullptr);
    };

    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_FALSE(segmented(e, 0)) << "its wait ran out: one frame";
    EXPECT_EQ(late(did_reply(0x1E01, 10), now + kMs), ecuio_reply::none);
    EXPECT_EQ(e.requests[0].reply_frames, 2u) << "nothing out";
    EXPECT_TRUE(segmented(e, 0));

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, {}), 1);
    EXPECT_EQ(late(did_reply(0x1E01, 4), now + kMs), ecuio_reply::none);
    EXPECT_EQ(e.requests[0].reply_frames, 1u) << "another row out";
    EXPECT_EQ(e.chans[0].outstanding, 1) << "the row out is still out";
    EXPECT_EQ(late(did_reply(0x1E7F, 10), now + kMs), ecuio_reply::none);
    EXPECT_EQ(e.requests[1].reply_frames, 0u) << "a DID of no row";

    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US));
    Bytes pid68 = {0x41, 0x68};
    pid68.insert(pid68.end(), 7u, uint8_t{0x2A});
    EXPECT_EQ(late(pid68, now + ECUIO_ENGINE_WAIT_US), ecuio_reply::none);
    EXPECT_EQ(e.requests[2].reply_frames, 2u) << "a PID at J1979's length";
    const Bytes pack = {0x41, 0x0C, 0x1A, 0xF8, 0x68, 0x2A};
    EXPECT_EQ(late(pack, now + ECUIO_ENGINE_WAIT_US), ecuio_reply::none);
    EXPECT_EQ(e.requests[3].reply_frames, 0u) << "a pack's reply";

    /* A pack out (set by hand): a late DID reply is no pack's, so it
     * teaches its row and the pack waits on */
    e.requests[0].reply_frames = 1;
    ecuio_engine_channel& ch = e.chans[0];
    ch.pack = ecuio_request{};
    ch.pack.bytes[0] = 0x01;
    ch.pack.bytes[1] = 0x0C;
    ch.pack.bytes[2] = 0x68;
    ch.pack.len = 3;
    ch.pack_rows[0] = 3;
    ch.pack_rows[1] = 2;
    ch.pack_count = 2;
    ch.outstanding = ECUIO_ENGINE_PACK;
    now += 2 * ECUIO_ENGINE_WAIT_US;
    ch.deadline_us = now + ECUIO_ENGINE_WAIT_US;
    EXPECT_EQ(late(did_reply(0x1E01, 10), now + kMs), ecuio_reply::none);
    EXPECT_EQ(e.requests[0].reply_frames, 2u) << "a pack out";
    EXPECT_EQ(ch.outstanding, ECUIO_ENGINE_PACK) << "the pack is still out";
    const Bytes own = {0x41, 0x0C, 0x1A, 0xF8};
    (void)late(own, now + 2 * kMs);
    EXPECT_EQ(e.requests[3].reply_frames, 0u) << "a 41 is the pack's own";
}

/* A recovery's rescan takes back a wait run out before any answer, so the
 * defs judge the row again; an answer it had stays, a refusal too, and
 * the module's waking does the same for its own rows */
TEST(RequestEngineTest, AnOutOfStepAnswerAtAnotherLengthEndsTheWaitsRunOut)
{
    const std::vector<ecuio_signal_def_t> defs = {def(0x410C, 2, 2, 16)};
    const ecuio_signal_set_t set = set_of(defs);
    const uint32_t rx = 0x7E8;
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ASSERT_TRUE(ecuio_engine_derive(&e, &set, &rx, 1, nullptr));

    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_EQ(e.requests[0].ran_out, 1u);
    ASSERT_FALSE(e.chans[0].in_step);

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, {0x41, 0x0C, 0x1A, 0xF8, 0x2A}), 0);
    EXPECT_EQ(e.requests[0].ran_out, 0u);
    EXPECT_EQ(e.requests[0].reply_frames, 1u) << "nothing learned";
    EXPECT_EQ(e.chans[0].pid_len[0x0C], 0u);
    EXPECT_TRUE(e.chans[0].in_step);

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US));
    EXPECT_EQ(e.requests[0].ran_out, 0u) << "an answered row";
}

TEST(RequestEngineTest, ALateAnswerAtAnotherLengthEndsTheWaitsRunOut)
{
    const std::vector<ecuio_signal_def_t> defs = {def(0x410C, 2, 2, 16)};
    const ecuio_signal_set_t set = set_of(defs);
    const uint32_t rx = 0x7E8;
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ASSERT_TRUE(ecuio_engine_derive(&e, &set, &rx, 1, nullptr));

    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_EQ(e.requests[0].ran_out, 1u);

    const Bytes late = {0x41, 0x0C, 0x1A, 0xF8, 0x2A};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, late.data(),
                                    static_cast<uint16_t>(late.size()),
                                    now + kMs, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(e.requests[0].ran_out, 0u);
    EXPECT_EQ(e.requests[0].reply_frames, 1u) << "nothing learned";
    EXPECT_EQ(e.chans[0].pid_len[0x0C], 0u);
}

TEST(RequestEngineTest, ARecoveryTakesBackAWaitRunOutBeforeAnyAnswer)
{
    const std::vector<ecuio_signal_def_t> defs = {
        def(0x621E01, 3, 3, 0, 0, 10), def(0x621E02, 3, 3, 0, 0, 10)};
    const ecuio_signal_set_t set = set_of(defs);
    const uint32_t rx = 0x7E8;
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    ASSERT_TRUE(ecuio_engine_derive(&e, &set, &rx, 1, nullptr));
    int64_t now = 0;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_FALSE(segmented(e, 0)) << "its wait ran out: one frame";
    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E02, 4)), 1);
    ASSERT_FALSE(segmented(e, 1)) << "answered in one frame";

    ecuio_engine_rescan(&e);
    EXPECT_TRUE(segmented(e, 0)) << "the defs judge it again";
    EXPECT_EQ(e.requests[0].reply_frames, 0u);
    EXPECT_FALSE(segmented(e, 1)) << "an answer stays";

    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    ASSERT_FALSE(segmented(e, 0));
    ecuio_engine_forget_ran_out(&e, 0);
    EXPECT_TRUE(segmented(e, 0)) << "its module woke";
    EXPECT_FALSE(segmented(e, 1));

    /* A late answer after a wait run out is an answer: a rescan keeps it */
    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, did_reply(0x1E02, 4)), 1);
    now += 100 * kMs;
    ASSERT_EQ(ask(&e, 0, now, {}), 0);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    const Bytes late = did_reply(0x1E01, 4);
    (void)ecuio_engine_on_frame(&e, 0, late.data(),
                                static_cast<uint16_t>(late.size()), now + kMs,
                                nullptr);
    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.requests[0].reply_frames, 1u) << "the late answer stays";
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

/* A row never answered counts its waits run out in a row (one frame from
 * the first); past 255 the count goes on from 128, so every eighth comes
 * round for ever (the J2534 hold's probe); an answer ends it, and a wait
 * run out after an answer counts nothing */
TEST(RequestEngineTest, ARowsWaitsRunOutBeforeAnyAnswerAreCountedForEver)
{
    ecuio_request q{};
    uint32_t eighths = 0;
    for (uint32_t k = 1; k <= 600; ++k) {
        ecuio_engine_row_ran_out(q);
        ASSERT_EQ(q.reply_frames, 1u);
        ASSERT_NE(q.ran_out, 0u) << k;
        ASSERT_EQ(q.ran_out, k <= 255 ? k : 128 + (k - 256) % 128) << k;
        eighths += q.ran_out % 8 == 0 ? 1u : 0u;
    }
    EXPECT_EQ(eighths, 600u / 8);
    ecuio_engine_row_frames(q, 3);
    EXPECT_EQ(q.ran_out, 0u);
    ecuio_engine_row_ran_out(q);
    EXPECT_EQ(q.reply_frames, 3u) << "an answered row keeps its frames";
    EXPECT_EQ(q.ran_out, 0u);
}
