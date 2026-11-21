// File/layer: tests/test_obd2_packing.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

/* ---------- a packed reply's records (obd2_reply.h) ---------- */

namespace {

struct Split {
    ecuio_obd2_reply reply;
    std::vector<std::pair<unsigned, std::vector<uint8_t>>> records;
};

/* What ecuio_obd2_split makes of bytes, each record as its PID and data */
Split split_of(std::initializer_list<uint8_t> bytes, uint8_t* learned)
{
    const std::vector<uint8_t> reply(bytes);
    ecuio_obd2_record out[ECUIO_OBD2_MAX_RECORDS];
    uint32_t n = 99;
    Split s;
    s.reply = ecuio_obd2_split(reply.data(), static_cast<uint16_t>(reply.size()),
                               learned, out, &n);
    for (uint32_t k = 0; k < n; ++k) {
        s.records.push_back(
            {out[k].pid, std::vector<uint8_t>(reply.begin() + out[k].start,
                                              reply.begin() + out[k].start +
                                                  out[k].bytes)});
    }
    return s;
}

using Rec = std::pair<unsigned, std::vector<uint8_t>>;

} // namespace

/* ADR-0039 Appendix A: a reply no longer than its first PID's data is
 * that PID's alone and decodes as it came; a longer one splits into its
 * records by J1979's lengths, in the order the ECU sent them */
TEST(Obd2ReplyTest, APackedReplySplitsIntoItsRecords)
{
    uint8_t learned[256] = {};
    EXPECT_EQ(split_of({0x41, 0x0C, 0x1A, 0xF8}, learned).reply,
              ecuio_obd2_reply::whole) << "one PID";
    const Split s =
        split_of({0x41, 0x0D, 0x32, 0x0C, 0x1A, 0xF8, 0x05, 0x5A}, learned);
    EXPECT_EQ(s.reply, ecuio_obd2_reply::split);
    EXPECT_EQ(s.records, (std::vector<Rec>{{0x0D, {0x32}},
                                           {0x0C, {0x1A, 0xF8}},
                                           {0x05, {0x5A}}}));
    EXPECT_EQ(split_of({0x41, 0x00, 1, 2, 3, 4, 0x20, 5, 6, 7, 8}, learned)
                  .records.size(),
              2u) << "the walk's bitmaps too";
    EXPECT_EQ(split_of({0x7F, 0x01, 0x12}, learned).reply,
              ecuio_obd2_reply::whole) << "not a positive reply";
    EXPECT_EQ(split_of({0x41, 0x0C}, learned).reply, ecuio_obd2_reply::whole);
    EXPECT_EQ(split_of({0x41, 0xB0, 1, 2, 3}, learned).reply,
              ecuio_obd2_reply::whole) << "a first PID of no known length";
}

/* A reply that won't split decodes its first record alone: a PID of no
 * known length, one twice, a record past the end or short of it */
TEST(Obd2ReplyTest, ARecordThatWontSplitStopsIt)
{
    uint8_t learned[256] = {};
    for (const auto& bytes : {
             std::vector<uint8_t>{0x41, 0x0C, 0x1A, 0xF8, 0xB0, 0x01},
             std::vector<uint8_t>{0x41, 0x0D, 0x32, 0x0D, 0x33},
             std::vector<uint8_t>{0x41, 0x0D, 0x32, 0x0C, 0x1A},
             std::vector<uint8_t>{0x41, 0x0D, 0x32, 0x05, 0x5A, 0x0C}}) {
        ecuio_obd2_record out[ECUIO_OBD2_MAX_RECORDS];
        uint32_t n = 99;
        EXPECT_EQ(ecuio_obd2_split(bytes.data(), static_cast<uint16_t>(bytes.size()),
                                   learned, out, &n),
                  ecuio_obd2_reply::unsplit) << bytes.size();
        EXPECT_EQ(n, 0u);
    }
    std::vector<uint8_t> many = {0x41};
    for (unsigned pid = 0x04; pid <= 0xFF && many.size() < 3 + 2 * ECUIO_OBD2_MAX_RECORDS;
         ++pid) {
        if (ECUIO_OBD2_PID_BYTES[pid] == 1) {
            many.push_back(static_cast<uint8_t>(pid));
            many.push_back(0x00);
        }
    }
    ASSERT_GT(many.size(), 1 + 2 * ECUIO_OBD2_MAX_RECORDS);
    ecuio_obd2_record out[ECUIO_OBD2_MAX_RECORDS];
    uint32_t n = 0;
    EXPECT_EQ(ecuio_obd2_split(many.data(), static_cast<uint16_t>(many.size()),
                               learned, out, &n),
              ecuio_obd2_reply::unsplit) << "past the most records";
}

/* The test car answers PID 55 with bank 1's byte alone
 * (docs/bench-sessions/2026-10-31-elm327-vehicle.md): a single reply
 * shorter than J1979's teaches the length, so a packed reply holding 55
 * splits; a reply as long as J1979's teaches the table's again */
TEST(Obd2ReplyTest, AShortSingleReplyTeachesItsLength)
{
    uint8_t learned[256] = {};
    const auto packed = [&learned]() {
        return split_of({0x41, 0x0C, 0x1A, 0xF8, 0x55, 0x80, 0x0D, 0x32},
                        learned);
    };
    EXPECT_EQ(packed().reply, ecuio_obd2_reply::unsplit)
        << "by J1979's length, 55 takes 0D's PID";

    EXPECT_EQ(split_of({0x41, 0x55, 0x80}, learned).reply,
              ecuio_obd2_reply::whole);
    EXPECT_EQ(learned[0x55], 1u);
    const Split s = packed();
    EXPECT_EQ(s.reply, ecuio_obd2_reply::split);
    EXPECT_EQ(s.records, (std::vector<Rec>{{0x0C, {0x1A, 0xF8}},
                                           {0x55, {0x80}},
                                           {0x0D, {0x32}}}));

    EXPECT_EQ(split_of({0x41, 0x55, 0x80, 0x7F}, learned).reply,
              ecuio_obd2_reply::whole) << "both banks after all";
    EXPECT_EQ(learned[0x55], 0u) << "the table's length again";
    EXPECT_EQ(split_of({0x41, 0x0C, 0x1A}, learned).reply,
              ecuio_obd2_reply::whole);
    EXPECT_EQ(learned[0x0C], 1u) << "any PID's";
}

namespace {

Bytes packed_fake_reply(const Bytes& req)
{
    return packed_from(fake_reply)(req);
}

bool slow_pid(const ecuio_request_engine& e, uint8_t pid)
{
    return class_of(e, pid) == 1;
}

} // namespace

/* A PID goes in a pack only once its own reply has shown its length,
 * so the first lap is unpacked; then every request carries up to six
 * kept PIDs of one class, never a bitmap, and every kept PID is still
 * asked */
TEST(RequestEngineTest, PacksCarryUpToSixKeptPidsOfOneClass)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    const std::vector<Bytes> sent = packing_turns(&e, 400, packed_fake_reply);
    std::vector<bool> answered_alone(256, false);
    size_t packs = 0;
    for (const Bytes& req : sent) {
        if (req.size() == 2) {
            answered_alone[req[1]] = true;
            continue;
        }
        ++packs;
        ASSERT_LE(req.size(), 1 + ECUIO_ENGINE_PACK_MAX);
        for (size_t k = 1; k < req.size(); ++k) {
            SCOPED_TRACE("PID " + std::to_string(req[k]));
            EXPECT_TRUE(answered_alone[req[k]]) << "packed before it went alone";
            EXPECT_NE(req[k] % 0x20, 0) << "a bitmap";
            EXPECT_EQ(slow_pid(e, req[k]), slow_pid(e, req[1])) << "one class";
        }
    }
    EXPECT_GT(packs, 0u);

    /* After the first laps: packs alone, every kept PID among them */
    std::vector<bool> asked(256, false);
    size_t singles = 0;
    for (size_t k = sent.size() - 60; k < sent.size(); ++k) {
        singles += sent[k].size() == 2 ? 1 : 0;
        for (size_t b = 1; b < sent[k].size(); ++b) {
            asked[sent[k][b]] = true;
        }
    }
    EXPECT_EQ(singles, 0u);
    for (uint32_t i = 0; i < e.count; ++i) {
        const ecuio_request& r = e.requests[i];
        if (r.dropped == 0) {
            EXPECT_TRUE(asked[r.bytes[1]]) << "PID " << int(r.bytes[1]);
        }
    }
    ecuio_channel_support_t s{};
    ecuio_engine_support(&e, 0, &s);
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_PACKED);
    EXPECT_FALSE(s.flags & ECUIO_SUPPORT_UNPACKED);
}

/* A pack's reply length is its records' at the lengths their own
 * replies showed, a short one (the test car's PID 55) included; a PID
 * whose own reply was longer than J1979's is never packed, since the
 * split can't tell it from a packed reply */
TEST(RequestEngineTest, APackIsSizedByItsPidsOwnReplies)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    const auto car = [](const Bytes& req) {
        if (req == Bytes{0x01, 0x55}) {
            return Bytes{0x41, 0x55, 0x80};
        }
        if (req == Bytes{0x01, 0x0C}) {
            return Bytes{0x41, 0x0C, 0x1A, 0xF8, 0x00};
        }
        return req.size() > 2 ? Bytes{} : fake_reply(req);
    };
    (void)packing_turns(&e, 200, car);
    const ecuio_engine_channel& ch = e.chans[0];
    EXPECT_EQ(ch.pid_len[0x55], 1u);
    EXPECT_EQ(ch.pid_len[0x0C], ECUIO_ENGINE_PID_LONG);

    ecuio_request_engine f;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&f));
    const std::vector<Bytes> sent = packing_turns(&f, 400, packed_from(car));
    bool packed55 = false;
    for (const Bytes& req : sent) {
        for (size_t k = 1; req.size() > 2 && k < req.size(); ++k) {
            EXPECT_NE(req[k], 0x0C) << "a reply longer than J1979's: alone";
            packed55 = packed55 || req[k] == 0x55;
        }
    }
    EXPECT_TRUE(packed55);
    const ecuio_engine_channel& fc = f.chans[0];
    ASSERT_GT(fc.pack_count, 1u);
    uint32_t reply = 1;
    for (uint32_t k = 0; k < fc.pack_count; ++k) {
        reply += 1u + fc.pid_len[fc.pack.bytes[1 + k]];
    }
    EXPECT_EQ(fc.pack.reply_len, reply);
}

/* A reply whose first record is any PID the pack asked answers it, in
 * any order, and one that leaves a PID out drops nothing */
TEST(RequestEngineTest, APackedReplyMatchesByAnyOfItsPids)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    const auto reversed_less_one = [](const Bytes& req) {
        if (req.size() <= 3) {
            return packed_fake_reply(req);
        }
        Bytes back = {0x01};
        for (size_t k = req.size() - 1; k >= 2; --k) {
            back.push_back(req[k]); /* the first PID left out */
        }
        return packed_fake_reply(back);
    };
    (void)packing_turns(&e, 400, reversed_less_one);
    ecuio_channel_support_t s{};
    ecuio_engine_support(&e, 0, &s);
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_PACKED);
    EXPECT_FALSE(s.flags & ECUIO_SUPPORT_UNPACKED);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_NE(e.requests[i].dropped, ECUIO_DROP_NOT_SUPPORTED)
            << "PID " << int(e.requests[i].bytes[1]);
    }
    EXPECT_EQ(e.chans[0].pack_failures, 0u);
}

/* Silence, a negative reply or one record, three packs in a row, puts
 * the channel back to one PID per request; one answered in between
 * starts the count again; a pending reply only extends the wait */
/* An ECU that goes quiet (the ignition off) leaves its packs unanswered
 * along with everything else, so the channel unpacks; once the link
 * recovers (the driver's rescan) packing is judged again and resumes */
TEST(RequestEngineTest, PackingResumesOnceTheLinkRecovers)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    int turn = 0;
    int packs_before = 0;
    int packs_after = 0;
    uint32_t quiet_flags = 0;
    (void)packing_turns(&e, 600, [&](const Bytes& req) {
        ++turn;
        if (turn <= 200) {
            packs_before += req.size() > 2 ? 1 : 0;
            return packed_fake_reply(req);
        }
        if (turn <= 300) {
            return Bytes{};
        }
        if (turn == 301) {
            ecuio_channel_support_t s{};
            ecuio_engine_support(&e, 0, &s);
            quiet_flags = s.flags;
            ecuio_engine_rescan(&e);
        }
        packs_after += req.size() > 2 ? 1 : 0;
        return packed_fake_reply(req);
    });
    EXPECT_GT(packs_before, 0);
    EXPECT_TRUE(quiet_flags & ECUIO_SUPPORT_UNPACKED)
        << "the quiet ECU unpacked it";
    EXPECT_GT(packs_after, 50);
    ecuio_channel_support_t s{};
    ecuio_engine_support(&e, 0, &s);
    EXPECT_FALSE(s.flags & ECUIO_SUPPORT_UNPACKED);
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_PACKED);
}

TEST(RequestEngineTest, ThreeFailedPacksEndPackingOnTheChannel)
{
    for (const char* how : {"silence", "negative", "first PID only"}) {
        SCOPED_TRACE(how);
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
        const std::string kind = how;
        int packs = 0;
        const std::vector<Bytes> sent =
            packing_turns(&e, 400, [&](const Bytes& req) {
                if (req.size() <= 2) {
                    return fake_reply(req);
                }
                ++packs;
                if (packs == 2) {
                    return packed_fake_reply(req); /* one in between */
                }
                if (kind == "silence") {
                    return Bytes{};
                }
                if (kind == "negative") {
                    return Bytes{0x7F, 0x01, 0x12};
                }
                return fake_reply({0x01, req[1]});
            });
        EXPECT_EQ(packs, 1 + 1 + int(ECUIO_ENGINE_PACK_FAILURES));
        ecuio_channel_support_t s{};
        ecuio_engine_support(&e, 0, &s);
        EXPECT_TRUE(s.flags & ECUIO_SUPPORT_UNPACKED);
        size_t k = sent.size();
        while (k > 0 && sent[k - 1].size() == 2) {
            --k;
        }
        EXPECT_LT(k + 100, sent.size()) << "one PID per request from then on";
        for (uint32_t i = 0; i < e.count; ++i) {
            EXPECT_NE(e.requests[i].dropped, ECUIO_DROP_NOT_SUPPORTED);
        }
    }

    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_fake_reply);
    int32_t r = ECUIO_ENGINE_NONE;
    int64_t now = 10 * 1000 * kMs;
    for (int k = 0; k < 50 && r != ECUIO_ENGINE_PACK; ++k) {
        r = ecuio_engine_due(&e, 0, now);
        if (r != ECUIO_ENGINE_PACK && r != ECUIO_ENGINE_NONE) {
            ecuio_engine_sent(&e, 0, r, true, now);
            const Bytes reply =
                fake_reply({0x01, ecuio_engine_request_of(&e, 0, r).bytes[1]});
            (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                        static_cast<uint16_t>(reply.size()), now,
                                        nullptr);
        }
        now += ECUIO_ENGINE_WAIT_US;
    }
    ASSERT_EQ(r, ECUIO_ENGINE_PACK);
    ecuio_engine_sent(&e, 0, r, true, now);
    const uint8_t pending[3] = {0x7F, 0x01, 0x78};
    uint8_t nrc = 0;
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, pending, 3, now + kMs, &nrc),
              ecuio_reply::pending);
    EXPECT_EQ(nrc, 0x78);
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, now + ECUIO_ENGINE_WAIT_US + kMs))
        << "the pending wait";
    EXPECT_EQ(e.chans[0].pack_failures, 0u);
}

/* Out of step, after a pack's wait ended with nothing, the request
 * before's reply come late answers nothing: a refusal naming Service 01,
 * or a record of one of the next pack's PIDs. That pack's own reply then
 * answers it, so late replies never add up to three failures. In step,
 * the same refusal fails the pack (ThreeFailedPacksEndPackingOnTheChannel) */
TEST(RequestEngineTest, OutOfStepALateReplyFailsNoPack)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_fake_reply);
    int64_t now = 10 * 1000 * kMs;
    const auto next_pack = [&] {
        for (int k = 0; k < 50; ++k, now += ECUIO_ENGINE_GAP_US) {
            const int32_t r = ecuio_engine_due(&e, 0, now);
            if (r == ECUIO_ENGINE_PACK) {
                ecuio_engine_sent(&e, 0, r, true, now);
                const ecuio_request& q = ecuio_engine_request_of(&e, 0, r);
                return Bytes(q.bytes, q.bytes + q.len);
            }
            if (r != ECUIO_ENGINE_NONE) {
                ecuio_engine_sent(&e, 0, r, true, now);
                const Bytes reply = fake_reply(
                    {0x01, ecuio_engine_request_of(&e, 0, r).bytes[1]});
                (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                            static_cast<uint16_t>(reply.size()),
                                            now + kMs, nullptr);
            }
        }
        return Bytes{};
    };
    const Bytes first = next_pack();
    ASSERT_GT(first.size(), 2u);
    now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.chans[0].pack_failures, 1u);
    EXPECT_FALSE(e.chans[0].in_step);

    now += ECUIO_ENGINE_GAP_US;
    const Bytes second = next_pack();
    ASSERT_GT(second.size(), 2u);
    const Bytes refusal = {0x7F, 0x01, 0x12};
    const Bytes record = fake_reply({0x01, second[1]});
    uint8_t nrc = 0;
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, refusal.data(), 3, now + kMs, &nrc),
              ecuio_reply::none);
    EXPECT_EQ(nrc, 0u);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, record.data(),
                                    static_cast<uint16_t>(record.size()),
                                    now + kMs, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_PACK) << "still waiting";
    const Bytes own = packed_fake_reply(second);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, own.data(),
                                    static_cast<uint16_t>(own.size()),
                                    now + 2 * kMs, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].pack_failures, 0u);
    EXPECT_TRUE(e.chans[0].in_step);
}

/* Packing waits for the driver's word, and a set walked with the probe
 * off has no bitmaps to pack by */
TEST(RequestEngineTest, PackingIsTheDriversToTurnOn)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar));
    for (const Bytes& req : packing_turns(&e, 300, packed_fake_reply)) {
        EXPECT_EQ(req.size(), 2u);
    }
    ecuio_request_engine f;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&f, kTestCar, false));
    f.pack = true;
    for (const Bytes& req : packing_turns(&f, 300, packed_fake_reply)) {
        EXPECT_EQ(req.size(), 2u) << "no bitmaps listed";
    }
}

/* ---- Reply timing (ADR-0042, request_timing.h) ---------------------- */

/* One sample reads back at most an eighth above itself and never below,
 * exactly below 16 us, and a sample past the top band reads its exact
 * value */
TEST(RequestTimingTest, AQuantileReadsAtMostAnEighthHigh)
{
    for (uint32_t v = 0; v < (1u << 24); v = v < 64 ? v + 1 : v + v / 7) {
        timing_hist h;
        h.add(v);
        const uint32_t q = h.quantile(0.5);
        ASSERT_GE(q, v);
        ASSERT_LE(static_cast<double>(q), static_cast<double>(v) * 1.125) << v;
        if (v < 16) {
            ASSERT_EQ(q, v);
        }
    }
    timing_hist past;
    past.add(20 * 1000 * 1000);
    past.add(1000);
    EXPECT_EQ(past.quantile(1.0), 20u * 1000 * 1000);
    EXPECT_EQ(past.max_us, 20u * 1000 * 1000);
}

/* A sample with its top bit set (36 minutes and more), or past 32 bits
 * (a wide CSV survey's gap between two samples hours apart), goes in the
 * last bucket at once: the log2 search stops at bit 31 */
TEST(RequestTimingTest, ASamplePastThirtyOneBitsLandsInTheLastBucket)
{
    timing_hist h;
    h.add(INT64_C(0x80000000));
    h.add(INT64_C(0xFFFFFFFF));
    h.add(INT64_C(1000000000000000));
    EXPECT_EQ(h.count, 3u);
    EXPECT_EQ(h.buckets[timing_hist::kBuckets - 1], 3u);
    EXPECT_EQ(h.max_us, 0xFFFFFFFFu);
    EXPECT_EQ(h.quantile(0.5), 0xFFFFFFFFu);
}

/* Over 1 to 1000 us, each quantile is within its bucket above the true
 * one, capped at the largest sample */
TEST(RequestTimingTest, QuantilesOfADistribution)
{
    timing_hist h;
    for (int v = 1; v <= 1000; ++v) {
        h.add(v);
    }
    EXPECT_EQ(h.count, 1000u);
    EXPECT_GE(h.quantile(0.5), 500u);
    EXPECT_LE(h.quantile(0.5), 563u);
    EXPECT_GE(h.quantile(0.99), 990u);
    EXPECT_LE(h.quantile(0.99), 1000u);
    EXPECT_EQ(h.quantile(1.0), 1000u);
    EXPECT_EQ(timing_hist().quantile(0.5), 0u) << "no samples";
}

/* A P99 needs 100 samples and a P99.9 1,000; below, 0 and no flag */
TEST(RequestTimingTest, TooFewSamplesLeaveAQuantileEmpty)
{
    for (uint32_t n : {99u, 100u, 999u, 1000u}) {
        SCOPED_TRACE(n);
        timing_row r;
        for (uint32_t k = 0; k <= n; ++k) {
            timing_answered(r, 4000, static_cast<int64_t>(k) * 100000, 0,
                            false);
        }
        /* n + 1 answers: n + 1 latencies, n intervals */
        const ecuio_request_timing_t row = timing_public(r);
        EXPECT_EQ(row.answered, n + 1);
        EXPECT_EQ(row.intervals, n);
        EXPECT_EQ(row.latency_p50_us, 4000u);
        EXPECT_EQ((row.flags & ECUIO_TIMING_INTERVAL_P99) != 0, n >= 100);
        EXPECT_EQ(row.interval_p99_us, n >= 100 ? 100000u : 0u);
        EXPECT_EQ((row.flags & ECUIO_TIMING_INTERVAL_P999) != 0, n >= 1000);
        EXPECT_EQ(row.interval_p999_us, n >= 1000 ? 100000u : 0u);
        EXPECT_NE(row.flags & ECUIO_TIMING_LATENCY_P99, 0u);
        EXPECT_EQ((row.flags & ECUIO_TIMING_LATENCY_P999) != 0, n + 1 >= 1000);
    }
}

/* An interval with a fallback of the frames' clock between its answers,
 * or one running backwards, is counted apart and kept out */
TEST(RequestTimingTest, AnIntervalAcrossAClockFallbackIsLeftOut)
{
    timing_row r;
    timing_answered(r, 4000, 0, 0, false);
    timing_answered(r, 4000, 100000, 0, false);
    timing_answered(r, 4000, 200000, 1, false); /* a fallback between */
    timing_answered(r, 4000, 300000, 1, false);
    timing_answered(r, 4000, 250000, 1, false); /* backwards */
    EXPECT_EQ(r.interval.count, 2u);
    EXPECT_EQ(r.intervals_mixed, 2u);
    EXPECT_EQ(r.interval.max_us, 100000u);
    timing_refused(r, 6000, true);
    EXPECT_EQ(r.refused, 1u);
    EXPECT_EQ(r.packed, 1u);
    EXPECT_EQ(r.latency.count, 6u) << "a refusal has a latency";
    EXPECT_EQ(r.interval.count, 2u) << "but refreshes nothing";
}

namespace {

/* The figures of an engine walked on the test car's bitmaps, timing on,
 * after n turns answered through respond, each reply 4 ms after its
 * request */
std::unique_ptr<ecuio_timing_table>
timed_turns(ecuio_request_engine* e, bool pack, int n, const Responder& respond)
{
    auto timing = std::make_unique<ecuio_timing_table>();
    if (pack) {
        packing_engine(e);
    } else {
        obd2_walked(e, kTestCar);
    }
    e->timing = timing.get();
    (void)packing_turns(e, n, respond);
    return timing;
}

} // namespace

/* Each rotation request's answers and timeouts, its latency from its
 * write to its reply, and its intervals between answers on the host's
 * clock when the driver gives no stamps */
TEST(RequestEngineTest, TimingRecordsEachRotationRequestsReplies)
{
    ecuio_request_engine e;
    const auto silent0D = [](const Bytes& req) {
        return req == Bytes{0x01, 0x0D} ? Bytes{} : fake_reply(req);
    };
    const auto t = timed_turns(&e, false, 400, silent0D);
    uint32_t rows = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const ecuio_request& q = e.requests[i];
        const timing_row& r = t->rows[i];
        SCOPED_TRACE("PID " + std::to_string(q.bytes[1]));
        if (q.dropped != 0) {
            EXPECT_EQ(r.answered, 0u);
            continue;
        }
        ++rows;
        if (q.bytes[1] == 0x0D) {
            EXPECT_EQ(r.answered, 0u);
            EXPECT_GT(r.timeouts, 0u);
            EXPECT_EQ(r.latency.count, 0u) << "a timeout has no latency";
            continue;
        }
        EXPECT_GT(r.answered, 1u);
        EXPECT_EQ(r.timeouts, 0u);
        EXPECT_EQ(r.packed, 0u);
        EXPECT_EQ(r.latency.quantile(0.5), 4000u);
        EXPECT_EQ(r.latency.max_us, 4000u);
        EXPECT_EQ(r.interval.count, r.answered - 1);
        EXPECT_GT(r.interval.quantile(0.5), 0u);
    }
    EXPECT_GT(rows, 10u);
}

/* A pack's reply answers each PID whose record it carries, with the
 * pack's latency, counted as packed */
TEST(RequestEngineTest, TimingCountsAPacksAnswersForEachOfItsPids)
{
    ecuio_request_engine e;
    const auto t = timed_turns(&e, true, 400, packed_fake_reply);
    uint32_t packed = 0;
    uint32_t kept = 0;
    uint32_t packed_rows = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const timing_row& r = t->rows[i];
        if (e.requests[i].dropped != 0) {
            continue;
        }
        SCOPED_TRACE("PID " + std::to_string(e.requests[i].bytes[1]));
        ++kept;
        packed_rows += r.packed > 0 ? 1 : 0;
        EXPECT_GT(r.answered, 0u);
        EXPECT_LE(r.packed, r.answered);
        EXPECT_EQ(r.latency.max_us, 4000u);
        packed += r.packed;
    }
    EXPECT_GT(packed, 100u);
    EXPECT_GT(packed_rows * 4, kept * 3)
        << "every record of a pack's reply answers its PID, not the first "
           "alone";
}

/* The driver's stamps time the intervals: on the frames' clock, an
 * interval across one of its fallbacks left out and counted */
TEST(RequestEngineTest, TimingTakesTheIntervalFromTheDriversStamps)
{
    ecuio_request_engine e;
    obd2_walked(&e, kTestCar);
    auto timing = std::make_unique<ecuio_timing_table>();
    e.timing = timing.get();
    e.stamp_set = true;
    int64_t now = 10 * 1000 * kMs;
    int answers = 0;
    for (int k = 0; k < 300; ++k, now += ECUIO_ENGINE_GAP_US) {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            continue;
        }
        ecuio_engine_sent(&e, 0, r, true, now);
        const ecuio_request& q = ecuio_engine_request_of(&e, 0, r);
        const Bytes reply = fake_reply(Bytes(q.bytes, q.bytes + q.len));
        e.stamp_us = static_cast<int64_t>(answers) * 7000; /* 7 ms apart */
        e.stamp_breaks = answers >= 150 ? 1u : 0u;
        (void)ecuio_engine_on_frame(&e, 0, reply.data(),
                                    static_cast<uint16_t>(reply.size()),
                                    now + 4 * kMs, nullptr);
        ++answers;
    }
    uint32_t mixed = 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        const timing_row& r = timing->rows[i];
        kept += e.requests[i].dropped == 0 ? 1 : 0;
        if (r.interval.count > 0) {
            EXPECT_EQ(r.interval.max_us % 7000, 0u) << "the stamps', not now's";
        }
        mixed += r.intervals_mixed;
    }
    EXPECT_GT(mixed, 0u);
    EXPECT_LE(mixed, kept) << "one fallback, at most one per request";
}
