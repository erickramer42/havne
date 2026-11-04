// File/layer: tests/test_recording_check.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CheckRulesTest: recording_check.h's median estimate, its
//          line parsers and each finding's rule, a checker fed frame by
//          frame; HARNESS: RecordingCheckTest: ecuio_recording_check
//          (ADR-0049 section 6) on folders written by hand and on the
//          replay fixture with one finding of each kind put in it, its
//          stops, refusals and torn row, and its allocations the same for
//          the fixture looped ten times as once
//
// Each case writes into a folder of its own under TEMP/TMPDIR, removed at
// the end.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "recording_check.h" /* core/src */

/* test_core.cpp's: operator new calls on this thread (new_counter.h) */
unsigned long news_on_this_thread();

namespace fs = std::filesystem;
namespace check = ecuio_check;

namespace {

struct Folder {
    fs::path path;
    explicit Folder(const char* tag = "")
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") /
            (std::string("ecuio-check-") +
             ::testing::UnitTest::GetInstance()->current_test_info()->name() +
             tag);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    std::string utf8() const { return path.generic_u8string(); }
    void write(const char* name, const std::string& text) const
    {
        std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
        f << text;
    }
};

const char* const kFramesHeader = "host_us,timestamp_us,id,flags,data\n";

/* A frames.csv row */
struct Row {
    uint64_t host_us;
    uint32_t id;
    uint32_t flags;
    std::string data;
};

std::string frames_csv(const std::vector<Row>& rows)
{
    std::string s = kFramesHeader;
    for (const Row& r : rows) {
        char head[96];
        std::snprintf(head, sizeof head, "%llu,%llu,0x%X,0x%X,",
                      static_cast<unsigned long long>(r.host_us),
                      static_cast<unsigned long long>(r.host_us), r.id,
                      r.flags);
        s += head + r.data + "\n";
    }
    return s;
}

/* "HH:mm:ss.zzz  +S.sss  <rest>" at host_us t, the session's start at
 * 12:00:00 */
std::string event_at(uint64_t t, const std::string& rest)
{
    const uint64_t ms = t / 1000;
    char buf[64];
    std::snprintf(buf, sizeof buf, "12:%02u:%02u.%03u  +%llu.%03u  ",
                  static_cast<unsigned>(ms / 60000 % 60),
                  static_cast<unsigned>(ms / 1000 % 60),
                  static_cast<unsigned>(ms % 1000),
                  static_cast<unsigned long long>(ms / 1000),
                  static_cast<unsigned>(ms % 1000));
    return buf + rest + "\n";
}

/* events.log in format 2, its frame flags line as given, these lines after
 * the header */
std::string events_v2(const std::string& frame_flags,
                      const std::string& lines = "")
{
    return "ecuio session recording, format 2 (ADR-0049)\n"
           "started  2026-11-03 12:00:00.000 (local time)\n"
           "utc  2026-11-03T18:00:00.000Z\ncore  0.3.6\nuri  mock://x\n"
           "profile  test\nframe flags  " +
           frame_flags +
           "\ncolumns  local time, seconds since the recording started\n\n" +
           lines;
}

using Finding = std::tuple<uint32_t, uint32_t, uint64_t, uint64_t, uint64_t,
                           uint32_t, int32_t>;

Finding as_tuple(const ecuio_check_finding_t& f)
{
    return Finding{f.kind,  f.arbitration_id, f.at_us, f.length_us,
                   f.count, f.flags,          f.code};
}

std::string describe(const Finding& f)
{
    char buf[160];
    std::snprintf(
        buf, sizeof buf,
        "kind %u id 0x%X at %llu for %llu count %llu flags %u code %d",
        std::get<0>(f), std::get<1>(f),
        static_cast<unsigned long long>(std::get<2>(f)),
        static_cast<unsigned long long>(std::get<3>(f)),
        static_cast<unsigned long long>(std::get<4>(f)), std::get<5>(f),
        std::get<6>(f));
    return buf;
}

/* A checker fed rows directly, its findings kept */
struct Fed {
    std::vector<Finding> found;
    ecuio_check_summary_t summary{};

    static bool keep(const ecuio_check_finding_t& f, void* user)
    {
        static_cast<Fed*>(user)->found.push_back(as_tuple(f));
        return true;
    }

    Fed(const std::vector<Row>& rows, bool isotp = false,
        std::vector<check::event> events = {},
        ecuio_check_options_t options = {})
    {
        check::checker c(options, std::move(events), isotp, keep, this);
        for (const Row& r : rows) {
            std::vector<uint8_t> bytes;
            std::istringstream in(r.data);
            unsigned b = 0;
            while (in >> std::hex >> b) {
                bytes.push_back(static_cast<uint8_t>(b));
            }
            EXPECT_TRUE(c.frame(r.host_us, r.id, r.flags, bytes.data(),
                                static_cast<uint32_t>(bytes.size())));
        }
        EXPECT_TRUE(c.finish());
        summary = c.summary();
    }

    std::vector<Finding> of(uint32_t kind) const
    {
        std::vector<Finding> out;
        for (const Finding& f : found) {
            if (std::get<0>(f) == kind) {
                out.push_back(f);
            }
        }
        return out;
    }
};

/* An ID every period from start for n frames */
void every(std::vector<Row>& rows, uint32_t id, uint64_t start, uint64_t period,
           int n, const std::string& data = "41 0C 1A F8")
{
    for (int i = 0; i < n; ++i) {
        rows.push_back(
            Row{start + static_cast<uint64_t>(i) * period, id, 0, data});
    }
}

void by_time(std::vector<Row>& rows)
{
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.host_us < b.host_us;
    });
}

constexpr uint32_t kWhole = ECUIO_CHECK_FINDING_WHOLE_BUS;

} // namespace

TEST(CheckRulesTest, TheMedianEstimateFollowsTheExactMedian)
{
    check::p2_median m;
    std::vector<double> seen;
    uint32_t lcg = 12345;
    for (int i = 0; i < 20000; ++i) {
        lcg = lcg * 1103515245u + 12345u;
        /* intervals around 40 ms with a long tail, as a request rotation's */
        const double x = 30000.0 + (lcg >> 16) % 20000 +
                         ((lcg >> 8) % 50 == 0 ? 500000.0 : 0.0);
        m.add(x);
        seen.push_back(x);
        if (seen.size() <= 4) {
            std::vector<double> s = seen;
            std::sort(s.begin(), s.end());
            const size_t n = s.size();
            const double exact =
                n % 2 == 1 ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
            EXPECT_EQ(m.value(), exact) << "exact over the first four";
        }
    }
    std::sort(seen.begin(), seen.end());
    const double exact = seen[seen.size() / 2];
    EXPECT_NEAR(m.value(), exact, exact * 0.02);
    EXPECT_EQ(m.count(), 20000u);
}

TEST(CheckRulesTest, LinkAndAsleepLinesAreRead)
{
    int32_t code = -1;
    uint64_t episodes = 0;
    const std::string fault = "adapter fault (state 9, 3 episodes)";
    ASSERT_TRUE(
        check::parse_link(fault.data(), fault.size(), &code, &episodes));
    EXPECT_EQ(code, 9);
    EXPECT_EQ(episodes, 3u);
    const std::string ok = "recovered (state 0, 1 episode)";
    ASSERT_TRUE(check::parse_link(ok.data(), ok.size(), &code, &episodes));
    EXPECT_EQ(code, 0);
    EXPECT_EQ(episodes, 1u);
    /* The last "(state" is the one: a status's words may hold one */
    const std::string twice = "a (state x) (state 7, 2 episodes)";
    ASSERT_TRUE(
        check::parse_link(twice.data(), twice.size(), &code, &episodes));
    EXPECT_EQ(code, 7);
    for (const char* bad :
         {"recovered", "x (state 9 3 episodes)", "x (state , 3 episodes)",
          "x (state 9, episodes)", "x (state 9, 3 episodes", "(state 9, 3)",
          "x (state 99999999999, 1 episode)"}) {
        EXPECT_FALSE(check::parse_link(bad, std::strlen(bad), &code, &episodes))
            << bad;
    }

    uint64_t us = 0;
    const std::string core = "Asleep for 12.3 s: nothing was read meanwhile";
    ASSERT_TRUE(check::parse_asleep(core.data(), core.size(), &us));
    EXPECT_EQ(us, 12300000u);
    const std::string whole = "Asleep for 5 s: nothing was read meanwhile";
    ASSERT_TRUE(check::parse_asleep(whole.data(), whole.size(), &us));
    EXPECT_EQ(us, 5000000u);
    for (const char* bad : {"Asleep for s: x", "asleep for 1.0 s: x",
                            "Asleep for 1.0s: x", "Asleep for ", "Awake"}) {
        EXPECT_FALSE(check::parse_asleep(bad, std::strlen(bad), &us)) << bad;
    }
}

TEST(CheckRulesTest, OnlyIso15765sIdsCarrySequences)
{
    EXPECT_TRUE(check::iso15765_id(0x7DF, false));
    EXPECT_TRUE(check::iso15765_id(0x7E0, false));
    EXPECT_TRUE(check::iso15765_id(0x7EF, false));
    EXPECT_FALSE(check::iso15765_id(0x7F0, false));
    EXPECT_FALSE(check::iso15765_id(0x0C0, false));
    EXPECT_FALSE(check::iso15765_id(0x7E8, true));
    EXPECT_TRUE(check::iso15765_id(0x18DAF110, true));
    EXPECT_TRUE(check::iso15765_id(0x18DB33F1, true));
    EXPECT_FALSE(check::iso15765_id(0x18DB33F2, true));
    EXPECT_FALSE(check::iso15765_id(0x18DAF110, false));
}

TEST(CheckRulesTest, AGapIsTheWholeBusQuietPastItsLengthOutsideASleep)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 100000, 10);        /* to 0.9 s */
    every(rows, 0x7E8, 2400000, 100000, 10);  /* 1.5 s quiet: a gap */
    every(rows, 0x7E8, 4250000, 100000, 10);  /* 0.95 s quiet: no gap */
    every(rows, 0x7E8, 8200000, 100000, 10);  /* 3 s quiet, slept 2.5 s */
    every(rows, 0x7E8, 13100000, 100000, 10); /* 4 s quiet, slept 2 s */
    check::event sleep1{};
    sleep1.at_us = 8150000;
    sleep1.kind = ECUIO_CHECK_ASLEEP;
    sleep1.length_us = 2500000;
    check::event sleep2 = sleep1;
    sleep2.at_us = 13000000;
    sleep2.length_us = 2000000;
    /* 4x the median: the gap's and the sleeps' intervals are far past it */
    ecuio_check_options_t strict{};
    strict.rate_drop_x100 = 400;
    const Fed fed(rows, false, {sleep1, sleep2}, strict);
    const std::vector<Finding> gaps = fed.of(ECUIO_CHECK_GAP);
    ASSERT_EQ(gaps.size(), 2u);
    EXPECT_EQ(gaps[0],
              Finding(ECUIO_CHECK_GAP, 0, 900000, 1500000, 0, kWhole, 0));
    EXPECT_EQ(gaps[1],
              Finding(ECUIO_CHECK_GAP, 0, 9100000, 4000000, 0, kWhole, 0))
        << "two of its four seconds outside the sleep";
    const std::vector<Finding> sleeps = fed.of(ECUIO_CHECK_ASLEEP);
    ASSERT_EQ(sleeps.size(), 2u);
    EXPECT_EQ(sleeps[0],
              Finding(ECUIO_CHECK_ASLEEP, 0, 5650000, 2500000, 0, kWhole, 0));
    /* The 0.95 s quiet is under the gap's length but 9.5x the ID's 100 ms:
     * a rate drop at 4x; the intervals across the gap and the sleeps
     * aren't */
    const std::vector<Finding> drops = fed.of(ECUIO_CHECK_RATE_DROP);
    ASSERT_EQ(drops.size(), 1u);
    EXPECT_EQ(drops[0],
              Finding(ECUIO_CHECK_RATE_DROP, 0x7E8, 3300000, 950000, 1, 0, 0));
    EXPECT_EQ(fed.summary.counts[ECUIO_CHECK_GAP], 2u);
    EXPECT_EQ(fed.summary.counts[ECUIO_CHECK_ASLEEP], 2u);
}

TEST(CheckRulesTest, ARateDropIsARunOfIntervalsPastTheMedian)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 40000, 20);               /* 19 intervals of 40 ms */
    every(rows, 0x7E8, 760000 + 480000, 480000, 3); /* three of 480 ms */
    every(rows, 0x7E8, 2200000 + 40000, 40000, 20);
    every(rows, 0x7E9, 0, 300000, 8); /* a slow ID, steady */
    by_time(rows);
    const Fed fed(rows);
    const std::vector<Finding> drops = fed.of(ECUIO_CHECK_RATE_DROP);
    ASSERT_EQ(drops.size(), 1u);
    EXPECT_EQ(drops[0],
              Finding(ECUIO_CHECK_RATE_DROP, 0x7E8, 760000, 1440000, 3, 0, 0));
    EXPECT_EQ(fed.of(ECUIO_CHECK_GAP).size(), 0u);

    /* Under 16 intervals an ID isn't judged; a higher multiple lets 12x by */
    std::vector<Row> few;
    every(few, 0x7E8, 0, 40000, 10);
    every(few, 0x7E8, 360000 + 600000, 40000, 10);
    EXPECT_EQ(Fed(few).of(ECUIO_CHECK_RATE_DROP).size(), 0u);
    ecuio_check_options_t lenient{};
    lenient.rate_drop_x100 = 1500;
    EXPECT_EQ(Fed(rows, false, {}, lenient).of(ECUIO_CHECK_RATE_DROP).size(),
              0u);
}

TEST(CheckRulesTest, ASlowIntervalUnderATenthOfASecondIsNoRateDrop)
{
    /* Frames 1 ms apart, as a USB adapter's bursts hand them over, then
     * 60 ms (60x the median, under the floor) and 150 ms (over it) */
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 1000, 100);
    every(rows, 0x7E8, 99000 + 60000, 1000, 100);
    every(rows, 0x7E8, 258000 + 150000, 1000, 100);
    const std::vector<Finding> drops = Fed(rows).of(ECUIO_CHECK_RATE_DROP);
    ASSERT_EQ(drops.size(), 1u);
    EXPECT_EQ(drops[0],
              Finding(ECUIO_CHECK_RATE_DROP, 0x7E8, 258000, 150000, 1, 0, 0));
}

TEST(CheckRulesTest, OnTheWireAnIdsRateIsItsMessages)
{
    /* 0x7E8's replies as they crossed the bus: a first frame and two
     * consecutive frames 5 ms apart, every 150 ms, the tester's flow
     * control on 0x7E0; then one reply 2 s late, while another ID keeps
     * the bus from a gap */
    std::vector<Row> rows;
    uint64_t t = 0;
    for (int i = 0; i < 40; ++i) {
        t += i == 30 ? 2000000 : 150000;
        rows.push_back(Row{t, 0x7E8, 0, "10 0F 49 02 01 46 41 4B"});
        rows.push_back(Row{t + 200, 0x7E0, 0, "30 00 00 00 00 00 00 00"});
        rows.push_back(Row{t + 5000, 0x7E8, 0, "21 45 4A 32 35 33 34 56"});
        rows.push_back(Row{t + 10000, 0x7E8, 0, "22 49 4E 30 30 30 30 31"});
    }
    every(rows, 0x123, 50000, 100000, static_cast<int>(t / 100000) + 1, "00");
    by_time(rows);
    const Fed wire(rows, true);
    EXPECT_EQ(wire.of(ECUIO_CHECK_ISOTP).size(), 0u) << "every message whole";
    const std::vector<Finding> drops = wire.of(ECUIO_CHECK_RATE_DROP);
    ASSERT_EQ(drops.size(), 1u) << "only the late reply, not each pause";
    EXPECT_EQ(drops[0], Finding(ECUIO_CHECK_RATE_DROP, 0x7E8, 30 * 150000,
                                2000000, 1, 0, 0));

    /* With the frame flags unknown each frame is judged: the pauses after
     * the consecutive frames are drops */
    EXPECT_GT(Fed(rows, false).of(ECUIO_CHECK_RATE_DROP).size(), 10u);
}

TEST(CheckRulesTest, ALateIdIsSeenPastTheSettleTimeAndZeroIsAnId)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 100000, 40);
    rows.push_back(Row{1500000, 0x7E9, 0, "41 0C 1A F8"}); /* in time */
    rows.push_back(Row{2500000, 0x000, 0, "01"});          /* late */
    rows.push_back(Row{3000000, 0x18DAF110, ECUIO_FRAME_FLAG_EXTENDED, "01"});
    rows.push_back(Row{3100000, 0x0C0, ECUIO_FRAME_FLAG_ERROR, "00"});
    by_time(rows);
    const Fed fed(rows);
    const std::vector<Finding> late = fed.of(ECUIO_CHECK_LATE_ID);
    ASSERT_EQ(late.size(), 2u);
    EXPECT_EQ(late[0], Finding(ECUIO_CHECK_LATE_ID, 0x000, 2500000, 0, 1, 0, 0))
        << "0x000 is an ID, not the whole bus";
    EXPECT_EQ(late[1], Finding(ECUIO_CHECK_LATE_ID, 0x18DAF110, 3000000, 0, 1,
                               ECUIO_CHECK_FINDING_EXTENDED, 0));
    EXPECT_EQ(fed.summary.ids, 4u) << "an error frame's class isn't an ID";
    EXPECT_EQ(fed.summary.frames, 44u);
}

TEST(CheckRulesTest, OverflowsInARowAreOneFinding)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 10000, 30);
    rows[5].flags = ECUIO_FRAME_FLAG_OVERFLOW;
    rows[6].flags = ECUIO_FRAME_FLAG_OVERFLOW;
    rows[7].flags = ECUIO_FRAME_FLAG_OVERFLOW;
    rows[20].flags = ECUIO_FRAME_FLAG_OVERFLOW;
    rows[29].flags =
        ECUIO_FRAME_FLAG_OVERFLOW; /* the last: settled at the end */
    const Fed fed(rows);
    const std::vector<Finding> o = fed.of(ECUIO_CHECK_OVERFLOW);
    ASSERT_EQ(o.size(), 3u);
    EXPECT_EQ(o[0],
              Finding(ECUIO_CHECK_OVERFLOW, 0, 50000, 20000, 3, kWhole, 0));
    EXPECT_EQ(o[1], Finding(ECUIO_CHECK_OVERFLOW, 0, 200000, 0, 1, kWhole, 0));
    EXPECT_EQ(o[2], Finding(ECUIO_CHECK_OVERFLOW, 0, 290000, 0, 1, kWhole, 0));
}

TEST(CheckRulesTest, IsotpSequencesOnTheWireAreChecked)
{
    std::vector<Row> rows = {
        /* a whole message: 20 bytes in a first frame and two consecutive */
        {1000, 0x7E8, 0, "10 14 49 02 01 31 32 33"},
        {1100, 0x7E0, 0, "30 00 00"}, /* flow control on the tester's ID */
        {1200, 0x7E8, 0, "21 34 35 36 37 38 39 30"},
        {1300, 0x7E8, 0, "22 41 42 43 44 45 46 47"},
        /* out of order: 23 after 21 */
        {2000, 0x7E8, 0, "10 14 49 02 01 31 32 33"},
        {2100, 0x7E8, 0, "21 34 35 36 37 38 39 30"},
        {2200, 0x7E8, 0, "23 41 42 43 44 45 46 47"},
        /* a first frame a single frame cuts off */
        {3000, 0x7E8, 0, "10 14 49 02 01 31 32 33"},
        {3100, 0x7E8, 0, "21 34 35 36 37 38 39 30"},
        {3200, 0x7E8, 0, "03 41 0C 1A"},
        /* a stray consecutive frame, and payloads with no PCI */
        {4000, 0x7E8, 0, "25 00"},
        {4100, 0x7E8, 0, "41 0C 1A F8"},
        /* reassembled: not checked */
        {5000, 0x7E8, ECUIO_FRAME_FLAG_REASSEMBLED, "21 00"},
        /* another ID: not ISO 15765-4's */
        {5100, 0x123, 0, "21 00"},
        /* a first frame still open at the end */
        {6000, 0x7E9, 0, "10 30 62 F1 90 41 42 43"},
        {6100, 0x7E9, 0, "21 44 45 46 47 48 49 4A"},
    };
    const Fed fed(rows, true);
    const std::vector<Finding> t = fed.of(ECUIO_CHECK_ISOTP);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t[0], Finding(ECUIO_CHECK_ISOTP, 0x7E8, 2200, 0, 1, 0, 0));
    EXPECT_EQ(t[1], Finding(ECUIO_CHECK_ISOTP, 0x7E8, 3000, 100, 2, 0, 0));
    EXPECT_EQ(t[2], Finding(ECUIO_CHECK_ISOTP, 0x7E8, 4000, 0, 1, 0, 0));
    EXPECT_EQ(t[3], Finding(ECUIO_CHECK_ISOTP, 0x7E9, 6000, 100, 2, 0, 0));
    EXPECT_EQ(fed.summary.flags, 0u);

    /* Not complete frame flags: skipped, and said why */
    const Fed unknown(rows, false);
    EXPECT_EQ(unknown.of(ECUIO_CHECK_ISOTP).size(), 0u);
    EXPECT_EQ(unknown.summary.flags,
              ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN);
    /* Complete, but every diagnostic frame reassembled */
    std::vector<Row> payloads;
    every(payloads, 0x7E8, 0, 40000, 5);
    for (Row& r : payloads) {
        r.flags = ECUIO_FRAME_FLAG_REASSEMBLED;
    }
    EXPECT_EQ(Fed(payloads, true).summary.flags,
              ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_NO_WIRE);
}

TEST(CheckRulesTest, FindingsComeAsTheyAreSettled)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 40000, 20);
    every(rows, 0x7E8, 760000 + 600000, 40000, 20);  /* a rate drop */
    rows.push_back(Row{2800000, 0x7E9, 0, "41 00"}); /* a late ID after it */
    by_time(rows);
    check::event link{};
    link.at_us = 900000;
    link.kind = ECUIO_CHECK_LINK;
    link.code = ECUIO_ERR_VEHICLE_TIMEOUT;
    link.episodes = 1;
    /* A line at a frame's very time comes before what that frame settles */
    check::event back = link;
    back.at_us = 2800000;
    back.code = ECUIO_OK;
    back.episodes = 2;
    const Fed fed(rows, false, {link, back});
    ASSERT_EQ(fed.found.size(), 4u);
    EXPECT_EQ(fed.found[2],
              Finding(ECUIO_CHECK_LINK, 0, 2800000, 0, 2, kWhole, ECUIO_OK));
    EXPECT_EQ(std::get<0>(fed.found[3]), ECUIO_CHECK_LATE_ID);
    /* the link line inside the drop comes first: the drop is settled at
     * the frame that ends it */
    EXPECT_EQ(fed.found[0], Finding(ECUIO_CHECK_LINK, 0, 900000, 0, 1, kWhole,
                                    ECUIO_ERR_VEHICLE_TIMEOUT));
    EXPECT_EQ(std::get<0>(fed.found[1]), ECUIO_CHECK_RATE_DROP);
    EXPECT_EQ(std::get<2>(fed.found[1]), 760000u);
}

namespace {

std::vector<Row> fixture_rows()
{
    std::ifstream f(std::string(ECUIO_REPLAY_FIXTURES) +
                        "/2026-10-25-engine-start/frames.csv",
                    std::ios::binary);
    std::vector<Row> rows;
    std::string line;
    std::getline(f, line); /* the header */
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream in(line);
        std::string host, ts, id, flags, data;
        std::getline(in, host, ',');
        std::getline(in, ts, ',');
        std::getline(in, id, ',');
        std::getline(in, flags, ',');
        std::getline(in, data);
        rows.push_back(
            Row{std::stoull(host),
                static_cast<uint32_t>(std::stoul(id, nullptr, 16)),
                static_cast<uint32_t>(std::stoul(flags, nullptr, 16)), data});
    }
    return rows;
}

struct Checked {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_check_summary_t summary{};
    std::vector<Finding> found;
    explicit Checked(const std::string& folder,
                     const ecuio_check_options_t* options = nullptr)
    {
        rc = ecuio_recording_check(
            folder.c_str(), options, nullptr,
            [](const ecuio_check_finding_t* f, void* user) {
                static_cast<Checked*>(user)->found.push_back(as_tuple(*f));
                return 0;
            },
            this, &summary);
    }
};

/* The last row before t's host_us, and the first at or after */
uint64_t last_before(const std::vector<Row>& rows, uint64_t t)
{
    uint64_t at = 0;
    for (const Row& r : rows) {
        if (r.host_us < t) {
            at = r.host_us;
        }
    }
    return at;
}

uint64_t first_from(const std::vector<Row>& rows, uint64_t t)
{
    for (const Row& r : rows) {
        if (r.host_us >= t) {
            return r.host_us;
        }
    }
    return 0;
}

void remove_between(std::vector<Row>& rows, uint64_t a, uint64_t b)
{
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const Row& r) {
                                  return r.host_us >= a && r.host_us < b;
                              }),
               rows.end());
}

} // namespace

TEST(RecordingCheckTest, TheFixtureWithOneOfEachFindingReportsEachAtItsTime)
{
    const std::vector<Row> plain = fixture_rows();
    ASSERT_GT(plain.size(), 7000u);
    Folder base("-plain");
    base.write("frames.csv", frames_csv(plain));
    base.write("events.log", events_v2("complete"));
    const Checked before(base.utf8());
    ASSERT_EQ(before.rc, ECUIO_OK) << ecuio_last_error_detail();

    std::vector<Row> rows = plain;
    /* A gap: 1.5 s of rows out at 60 s */
    remove_between(rows, 60000000, 61500000);
    const uint64_t gap_at = last_before(rows, 60000000);
    const uint64_t gap_end = first_from(rows, 61500000);
    /* A sleep at 80 s, its line where reading began again */
    remove_between(rows, 80000000, 82500000);
    /* A rate drop: half a second out at 120 s */
    remove_between(rows, 120000000, 120500000);
    const uint64_t drop_at = last_before(rows, 120000000);
    const uint64_t drop_end = first_from(rows, 120500000);
    /* A late ID, 0x000 */
    rows.push_back(Row{100000000, 0x000, 0, "00"});
    /* An overflow */
    for (Row& r : rows) {
        if (r.host_us >= 140000000) {
            r.flags |= ECUIO_FRAME_FLAG_OVERFLOW;
            break;
        }
    }
    const uint64_t overflow_at = first_from(rows, 140000000);
    /* A first frame never completed */
    rows.push_back(Row{150000000, 0x7E8, 0, "10 14 49 02 01 31 32 33"});
    by_time(rows);
    Folder f;
    f.write("frames.csv", frames_csv(rows));
    f.write("events.log",
            events_v2("complete",
                      event_at(82500000, "asleep  Asleep for 2.5 s: nothing "
                                         "was read meanwhile") +
                          event_at(160000000, "link  vehicle not responding "
                                              "(state 8, 2 episodes)")));
    const Checked after(f.utf8());
    ASSERT_EQ(after.rc, ECUIO_OK) << ecuio_last_error_detail();

    const std::vector<Finding> put = {
        {ECUIO_CHECK_GAP, 0, gap_at, gap_end - gap_at, 0, kWhole, 0},
        {ECUIO_CHECK_ASLEEP, 0, 80000000, 2500000, 0, kWhole, 0},
        {ECUIO_CHECK_LATE_ID, 0x000, 100000000, 0, 1, 0, 0},
        {ECUIO_CHECK_RATE_DROP, 0x7E8, drop_at, drop_end - drop_at, 1, 0, 0},
        {ECUIO_CHECK_OVERFLOW, 0, overflow_at, 0, 1, kWhole, 0},
        {ECUIO_CHECK_ISOTP, 0x7E8, 150000000, 0, 1, 0, 0},
        {ECUIO_CHECK_LINK, 0, 160000000, 0, 2, kWhole, 8},
    };
    std::vector<Finding> want = before.found;
    want.insert(want.end(), put.begin(), put.end());
    std::vector<Finding> got = after.found;
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got.size(), want.size());
    for (const Finding& w : want) {
        EXPECT_NE(std::find(got.begin(), got.end(), w), got.end())
            << "missing: " << describe(w);
    }
    for (const Finding& g : got) {
        EXPECT_NE(std::find(want.begin(), want.end(), g), want.end())
            << "unexpected: " << describe(g);
    }
    for (uint32_t k = ECUIO_CHECK_GAP; k <= ECUIO_CHECK_ASLEEP; ++k) {
        EXPECT_EQ(after.summary.counts[k], before.summary.counts[k] + 1) << k;
    }
    EXPECT_EQ(after.summary.ids, 2u);
    EXPECT_EQ(after.summary.flags, 0u) << "wire frames on 0x7E8: checked";
    EXPECT_EQ(after.summary.frames, rows.size());
    RecordProperty("fixture_gaps",
                   std::to_string(before.summary.counts[ECUIO_CHECK_GAP]));
    RecordProperty(
        "fixture_rate_drops",
        std::to_string(before.summary.counts[ECUIO_CHECK_RATE_DROP]));
}

TEST(RecordingCheckTest, ReassembledPayloadsSkipTheIsotpKindAndSayWhy)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 40000, 10, "21 00");
    for (Row& r : rows) {
        r.flags = ECUIO_FRAME_FLAG_REASSEMBLED;
    }
    Folder f;
    f.write("frames.csv", frames_csv(rows));
    f.write("events.log", events_v2("complete"));
    const Checked c(f.utf8());
    ASSERT_EQ(c.rc, ECUIO_OK);
    EXPECT_EQ(c.summary.flags,
              ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_NO_WIRE);
    f.write("events.log", events_v2("from an older recording"));
    EXPECT_EQ(Checked(f.utf8()).summary.flags,
              ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN);
    std::error_code ec;
    fs::remove(f.path / "events.log", ec);
    const Checked bare(f.utf8());
    ASSERT_EQ(bare.rc, ECUIO_OK) << "events.log is optional";
    EXPECT_EQ(bare.summary.flags,
              ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN);
    EXPECT_EQ(bare.summary.duration_us, 360000u);
}

TEST(RecordingCheckTest, AnOlderRecordingsSleepLineKeepsItFromBeingAGap)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 100000, 10);
    every(rows, 0x7E8, 6000000, 100000, 10); /* 5.1 s quiet */
    Folder f;
    f.write("frames.csv", frames_csv(rows));
    f.write("events.log",
            "Havne session recording (ADR-0017)\n"
            "started  2026-10-25 12:00:00.000 (local time)\n"
            "columns  local time, seconds since the recording started\n\n" +
                event_at(100000, "Connected: j2534://x") +
                event_at(6000000, "Asleep for 5.0 s: nothing was read "
                                  "meanwhile"));
    const Checked c(f.utf8());
    ASSERT_EQ(c.rc, ECUIO_OK);
    EXPECT_EQ(c.summary.counts[ECUIO_CHECK_GAP], 0u);
    ASSERT_EQ(c.summary.counts[ECUIO_CHECK_ASLEEP], 1u);
    EXPECT_EQ(c.found.back(),
              Finding(ECUIO_CHECK_ASLEEP, 0, 1000000, 5000000, 0, kWhole, 0));
    EXPECT_EQ(c.summary.flags,
              ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN);
}

TEST(RecordingCheckTest, ANonzeroReturnStopsItWithTheSummaryFilled)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 100000, 10);
    every(rows, 0x7E8, 3000000, 100000, 10); /* a gap */
    every(rows, 0x7E8, 6000000, 100000, 10); /* another */
    Folder f;
    f.write("frames.csv", frames_csv(rows));
    struct Stop {
        int seen = 0;
    } stop;
    ecuio_check_summary_t s{};
    EXPECT_EQ(ecuio_recording_check(
                  f.utf8().c_str(), nullptr, nullptr,
                  [](const ecuio_check_finding_t*, void* user) {
                      return ++static_cast<Stop*>(user)->seen;
                  },
                  &stop, &s),
              ECUIO_ERR_CANCELED);
    EXPECT_EQ(stop.seen, 1);
    EXPECT_EQ(s.counts[ECUIO_CHECK_GAP], 1u);
    EXPECT_EQ(s.frames, 11u) << "stopped at the frame that ended the gap";
    EXPECT_EQ(s.ids, 1u);

    /* A progress callback stops a check that has found nothing */
    std::vector<Row> quiet;
    every(quiet, 0x7E8, 0, 40000, 50);
    f.write("frames.csv", frames_csv(quiet));
    int calls = 0;
    auto at_end = [](uint64_t read, uint64_t total, void* user) {
        ++*static_cast<int*>(user);
        return read == total && total != 0 ? 1 : 0;
    };
    EXPECT_EQ(ecuio_recording_check(f.utf8().c_str(), nullptr, at_end, nullptr,
                                    &calls, &s),
              ECUIO_ERR_CANCELED);
    EXPECT_EQ(calls, 2) << "at the start and at the end";
    EXPECT_EQ(s.frames, 50u) << "filled";
    calls = 0;
    auto at_start = [](uint64_t, uint64_t, void* user) {
        ++*static_cast<int*>(user);
        return 1;
    };
    EXPECT_EQ(ecuio_recording_check(f.utf8().c_str(), nullptr, at_start,
                                    nullptr, &calls, &s),
              ECUIO_ERR_CANCELED);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(s.frames, 0u);
}

TEST(RecordingCheckTest, ItsMemoryIsTheSameForTheFixtureLoopedTenTimes)
{
    const std::vector<Row> plain = fixture_rows();
    const uint64_t span = plain.back().host_us + 40000;
    Folder once("-once");
    once.write("frames.csv", frames_csv(plain));
    std::vector<Row> looped;
    for (uint64_t loop = 0; loop < 10; ++loop) {
        for (const Row& r : plain) {
            looped.push_back(
                Row{r.host_us + loop * span, r.id, r.flags, r.data});
        }
    }
    Folder ten("-ten");
    ten.write("frames.csv", frames_csv(looped));
    ecuio_check_summary_t s{};
    const std::string a = once.utf8();
    const std::string b = ten.utf8();
    unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_recording_check(a.c_str(), nullptr, nullptr, nullptr,
                                    nullptr, &s),
              ECUIO_OK);
    const unsigned long news_once = news_on_this_thread() - before;
    EXPECT_EQ(s.frames, plain.size());
    before = news_on_this_thread();
    ASSERT_EQ(ecuio_recording_check(b.c_str(), nullptr, nullptr, nullptr,
                                    nullptr, &s),
              ECUIO_OK);
    const unsigned long news_ten = news_on_this_thread() - before;
    EXPECT_EQ(s.frames, looped.size());
    EXPECT_EQ(news_ten, news_once);
    RecordProperty("allocations", std::to_string(news_once));
}

TEST(RecordingCheckTest, ATornLastRowIsDroppedAndCounted)
{
    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 40000, 5);
    std::string text = frames_csv(rows) + "200000,200000,0x7E8,0x0,41 0C";
    Folder f;
    f.write("frames.csv", text);
    const Checked c(f.utf8());
    ASSERT_EQ(c.rc, ECUIO_OK);
    EXPECT_EQ(c.summary.frames, 5u);
    EXPECT_EQ(c.summary.torn_rows, 1u);
}

TEST(RecordingCheckTest, TheCheckRefusesWhatItMust)
{
    ecuio_check_summary_t s{};
    s.frames = 9;
    EXPECT_EQ(
        ecuio_recording_check(nullptr, nullptr, nullptr, nullptr, nullptr, &s),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(s.frames, 0u) << "out is cleared first";
    Folder f;
    EXPECT_EQ(ecuio_recording_check(f.utf8().c_str(), nullptr, nullptr, nullptr,
                                    nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(Checked(f.utf8()).rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(), "the folder has no frames.csv");

    std::vector<Row> rows;
    every(rows, 0x7E8, 0, 40000, 5);
    f.write("frames.csv", frames_csv(rows));
    for (auto set : {+[](ecuio_check_options_t& o) { o.flags = 1; },
                     +[](ecuio_check_options_t& o) { o.reserved[3] = 1; },
                     +[](ecuio_check_options_t& o) { o.rate_drop_x100 = 100; },
                     +[](ecuio_check_options_t& o) { o.rate_drop_x100 = 1; }}) {
        ecuio_check_options_t o{};
        set(o);
        EXPECT_EQ(Checked(f.utf8(), &o).rc, ECUIO_ERR_INVALID_ARGUMENT);
    }
    ecuio_check_options_t ok{};
    ok.rate_drop_x100 = 101;
    EXPECT_EQ(Checked(f.utf8(), &ok).rc, ECUIO_OK);

    const struct {
        std::string text;
        const char* detail;
    } bad[] = {
        {"", "the recording's frames.csv is empty"},
        {"time,id\n1,2\n",
         "not a Havne frames.csv, or a newer format: the header row isn't "
         "host_us,timestamp_us,id,flags,data"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41\nx,1,0x7E8,0x0,41\n",
         "frames.csv line 3: host_us isn't a decimal count"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41\n1,1,7E8,0x0,41\n",
         "frames.csv line 3: id isn't 0x-prefixed hex"},
        {std::string(kFramesHeader) + "5,5,0x7E8,0x0,41\n4,4,0x7E8,0x0,41\n",
         "frames.csv line 3: host_us is earlier than the row before"},
    };
    for (const auto& b : bad) {
        f.write("frames.csv", b.text);
        const Checked c(f.utf8());
        EXPECT_EQ(c.rc, ECUIO_ERR_LOAD_FAILED) << b.detail;
        EXPECT_STREQ(ecuio_last_error_detail(), b.detail);
        EXPECT_EQ(std::string(ecuio_last_error_detail()).find(f.utf8()),
                  std::string::npos);
    }
}
