// File/layer: tests/test_recording_stats.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingStatsTest (test_core)

#include "test_recording_support.h"

// ---- The span's statistics and the step (ADR-0049 section 5) --------------

namespace {

constexpr uint64_t kNone = ECUIO_RECORDING_NONE;

ecuio_recording_stats_t stats_of(const Reader& r, uint32_t signal, uint64_t t0,
                                 uint64_t t1)
{
    ecuio_recording_stats_t st{};
    EXPECT_EQ(ecuio_recording_stats(r.r, &signal, 1, t0, t1, &st), ECUIO_OK);
    return st;
}

uint64_t step_of(const Reader& r, uint32_t signal, uint64_t t,
                 uint32_t direction)
{
    uint64_t at = 0;
    EXPECT_EQ(ecuio_recording_step(r.r, signal, t, direction, &at), ECUIO_OK);
    return at;
}

/* A raw pass over the hour's samples: what ecuio_recording_stats owes */
struct RawStats {
    uint64_t samples = 0;
    uint64_t valued = 0;
    double sum = 0.0;
    double magnitude = 0.0; /* the sum of |x|, for the mean's bound */
    double lowest = NAN;
    double highest = NAN;
    uint64_t lowest_us = kNone;
    uint64_t highest_us = kNone;
    double first = NAN;
    double last = NAN;
};

RawStats raw_stats(const std::vector<Sample>& samples, uint32_t signal,
                   uint64_t t0, uint64_t t1)
{
    RawStats s;
    for (const Sample& x : samples) {
        if (x.signal != signal) {
            continue;
        }
        if (x.us <= t0) {
            s.first = x.value;
        }
        if (x.us <= t1) {
            s.last = x.value;
        }
        if (x.us < t0 || x.us > t1) {
            continue;
        }
        ++s.samples;
        if (std::isnan(x.value)) {
            continue;
        }
        ++s.valued;
        s.sum += x.value;
        s.magnitude += std::fabs(x.value);
        if (std::isnan(s.lowest) || x.value < s.lowest) {
            s.lowest = x.value;
            s.lowest_us = x.us;
        }
        if (std::isnan(s.highest) || x.value > s.highest) {
            s.highest = x.value;
            s.highest_us = x.us;
        }
    }
    return s;
}

} // namespace

TEST(RecordingStatsTest, ASpanTakesWholeSecondsAndItsPartialEnds)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");

    /* Seconds 1 and 2 whole, 3's sample at the span's end */
    const ecuio_recording_stats_t st = stats_of(r, rpm, 1000000, 3000000);
    EXPECT_EQ(st.samples, 6u);
    EXPECT_EQ(st.first, 300.0);
    EXPECT_EQ(st.last, 400.0);
    EXPECT_EQ(st.lowest, 50.0);
    EXPECT_EQ(st.lowest_us, 2000000u) << "a summary's extreme, its time read";
    EXPECT_EQ(st.highest, 400.0);
    EXPECT_EQ(st.highest_us, 2500000u) << "the first sample at the extreme";
    EXPECT_EQ(st.mean, 1650.0 / 6);
    EXPECT_EQ(st.flags, 0u);
    EXPECT_EQ(st.reserved0, 0u);

    /* Inside one second, its value in force from before the span */
    const ecuio_recording_stats_t in = stats_of(r, rpm, 1200000, 1950000);
    EXPECT_EQ(in.samples, 2u);
    EXPECT_EQ(in.first, 300.0);
    EXPECT_EQ(in.last, 300.0);
    EXPECT_EQ(in.lowest_us, 1400000u);
    EXPECT_EQ(in.highest_us, 1900000u);
    EXPECT_EQ(in.mean, 250.0);

    /* t0's second holds only a later sample: the second before's last */
    EXPECT_EQ(stats_of(r, rpm, 5200000, 5600000).first, 20.0);

    /* The whole recording */
    const ecuio_recording_stats_t all = stats_of(r, rpm, 0, 6000000);
    EXPECT_EQ(all.samples, 10u);
    EXPECT_EQ(all.lowest, 7.0);
    EXPECT_EQ(all.lowest_us, 5500000u);
    EXPECT_EQ(all.mean, 1787.0 / 10);

    /* Before the first sample: nothing in force, nothing in the span */
    const ecuio_recording_stats_t none = stats_of(r, rpm, 0, 400000);
    EXPECT_EQ(none.samples, 0u);
    EXPECT_TRUE(std::isnan(none.first));
    EXPECT_TRUE(std::isnan(none.last));
    EXPECT_TRUE(std::isnan(none.lowest));
    EXPECT_TRUE(std::isnan(none.highest));
    EXPECT_TRUE(std::isnan(none.mean));
    EXPECT_EQ(none.lowest_us, kNone);
    EXPECT_EQ(none.highest_us, kNone);
}

TEST(RecordingStatsTest, StatsCloseTheSpanWhereAQueryLeavesItsEndOut)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");
    /* A sample exactly at t1: the statistics' closed span counts it */
    EXPECT_EQ(stats_of(r, rpm, 2500000, 3000000).samples, 2u);
    EXPECT_EQ(stats_of(r, rpm, 2500000, 2999999).samples, 1u);
    ecuio_recording_column_t col{};
    ASSERT_EQ(ecuio_recording_query(r.r, &rpm, 1, 2500000, 3000000, &col, 1),
              ECUIO_OK);
    EXPECT_EQ(col.samples, 1u) << "the query's half-open span leaves it out";
}

TEST(RecordingStatsTest, TheMeanLeavesANaNOutAndTakesAnInf)
{
    Folder f;
    f.write("events.log", events_v2({"RPM,number,rpm,0,1"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n"
                               "1100000,RPM,10,rpm\n"
                               "1200000,RPM,nan,rpm\n"
                               "1300000,RPM,30,rpm\n"
                               "2100000,RPM,inf,rpm\n"
                               "2200000,RPM,-inf,rpm\n"
                               "3100000,RPM,5,rpm\n"
                               "3200000,RPM,inf,rpm\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");

    /* A whole second holding a NaN: its rows read again for the mean */
    const ecuio_recording_stats_t nan_second =
        stats_of(r, rpm, 1000000, 1999999);
    EXPECT_EQ(nan_second.samples, 3u) << "the NaN is a sample";
    EXPECT_EQ(nan_second.mean, 20.0) << "and not in the mean";
    EXPECT_EQ(nan_second.lowest, 10.0);
    EXPECT_EQ(nan_second.highest, 30.0);
    EXPECT_EQ(nan_second.highest_us, 1300000u);

    /* An inf and a -inf, no NaN: the second's sum is NaN, so it's read
     * again, and the mean is NaN */
    const ecuio_recording_stats_t both = stats_of(r, rpm, 2000000, 2999999);
    EXPECT_EQ(both.samples, 2u);
    EXPECT_TRUE(std::isnan(both.mean));
    EXPECT_EQ(both.lowest, -HUGE_VAL);
    EXPECT_EQ(both.highest, HUGE_VAL);

    /* An inf alone: the summary's sum is inf, and so is the mean */
    const ecuio_recording_stats_t inf = stats_of(r, rpm, 3000000, 3999999);
    EXPECT_EQ(inf.samples, 2u);
    EXPECT_EQ(inf.mean, HUGE_VAL);
    EXPECT_EQ(inf.highest, HUGE_VAL);
    EXPECT_EQ(inf.highest_us, 3200000u);
    EXPECT_EQ(inf.lowest, 5.0);
}

TEST(RecordingStatsTest, ATextSignalCountsItsSamplesAlone)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t vin = r.index_of("VIN");
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t both[] = {vin, rpm};
    ecuio_recording_stats_t st[2];
    std::memset(st, 0xAB, sizeof st);
    ASSERT_EQ(ecuio_recording_stats(r.r, both, 2, 0, 6000000, st), ECUIO_OK);
    EXPECT_EQ(st[0].samples, 2u);
    EXPECT_TRUE(std::isnan(st[0].first));
    EXPECT_TRUE(std::isnan(st[0].last));
    EXPECT_TRUE(std::isnan(st[0].lowest));
    EXPECT_TRUE(std::isnan(st[0].highest));
    EXPECT_TRUE(std::isnan(st[0].mean));
    EXPECT_EQ(st[0].lowest_us, kNone);
    EXPECT_EQ(st[0].highest_us, kNone);
    EXPECT_EQ(st[0].flags, 0u);
    EXPECT_EQ(st[0].reserved[2], 0u);
    EXPECT_EQ(st[1].samples, 10u) << "each entry its own signal's";
    EXPECT_EQ(stats_of(r, vin, 600000, 2500000).samples, 1u)
        << "a partial second's text rows read raw";
}

TEST(RecordingStatsTest, StatsRefuseWhatTheyCantAnswer)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t past = r.info().signal_count;
    ecuio_recording_stats_t st{};
    EXPECT_EQ(ecuio_recording_stats(nullptr, &rpm, 1, 0, 1, &st),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, nullptr, 1, 0, 1, &st),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 1, 0, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 0, 0, 1, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 1, 5, 5, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &rpm, 1, 6, 5, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stats(r.r, &past, 1, 0, 1, &st),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST(RecordingStatsTest, TheSummariesGiveWhatARawPassGivesOverTheHour)
{
    const Hour& h = fixture_hour();
    Folder f;
    h.write(f.path);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    const uint64_t end = r.info().duration_us;
    const std::pair<uint64_t, uint64_t> spans[] = {
        {0, end},
        {123456789, 2345678901},
        {1000000, 1000000000},
        {59999999, 61000001},
        {1800000000, 1800400000},
    };
    for (uint32_t d = 0; d < h.defs.size(); ++d) {
        if (h.defs[d].text) {
            continue;
        }
        const uint32_t k = r.index_of(h.defs[d].name.c_str());
        for (const auto& span : spans) {
            const RawStats want =
                raw_stats(h.samples, d, span.first, span.second);
            const ecuio_recording_stats_t got =
                stats_of(r, k, span.first, span.second);
            SCOPED_TRACE(h.defs[d].name + " " + std::to_string(span.first) +
                         " to " + std::to_string(span.second));
            EXPECT_EQ(got.samples, want.samples);
            if (want.valued == 0) {
                EXPECT_TRUE(std::isnan(got.mean));
                continue;
            }
            EXPECT_EQ(got.lowest, want.lowest);
            EXPECT_EQ(got.highest, want.highest);
            EXPECT_EQ(got.lowest_us, want.lowest_us);
            EXPECT_EQ(got.highest_us, want.highest_us);
            /* NaN when nothing is in force yet, which no == matches */
            EXPECT_TRUE(std::isnan(want.first) ? std::isnan(got.first)
                                               : got.first == want.first)
                << got.first << " for " << want.first;
            EXPECT_TRUE(std::isnan(want.last) ? std::isnan(got.last)
                                              : got.last == want.last)
                << got.last << " for " << want.last;
            /* The summaries add each second's sum, a raw pass each
             * sample: they agree to a rounding of the magnitudes summed,
             * which a mean near zero after cancellation still meets */
            const double magnitude_bound =
                1e-12 * want.magnitude / static_cast<double>(want.valued);
            EXPECT_NEAR(got.mean, want.sum / static_cast<double>(want.valued),
                        magnitude_bound);
        }
    }
}

TEST(RecordingStatsTest, AStepFindsTheNextOrTheLastSample)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.index_of("RPM");
    const uint32_t vin = r.index_of("VIN");
    constexpr uint32_t fwd = ECUIO_RECORDING_STEP_FORWARD;
    constexpr uint32_t back = ECUIO_RECORDING_STEP_BACKWARD;
    EXPECT_EQ(step_of(r, rpm, 0, fwd), 500000u);
    EXPECT_EQ(step_of(r, rpm, 500000, fwd), 1000000u) << "strictly after";
    EXPECT_EQ(step_of(r, rpm, 1450000, fwd), 1900000u);
    EXPECT_EQ(step_of(r, rpm, 1900000, fwd), 2000000u)
        << "into the next second";
    EXPECT_EQ(step_of(r, rpm, 4000000, fwd), 5500000u)
        << "past an empty second";
    EXPECT_EQ(step_of(r, rpm, 5500000, fwd), kNone);
    EXPECT_EQ(step_of(r, rpm, 5500000, back), 4000000u);
    EXPECT_EQ(step_of(r, rpm, 5200000, back), 4000000u)
        << "t's second holds only a later sample";
    EXPECT_EQ(step_of(r, rpm, 1000000, back), 500000u) << "strictly before";
    EXPECT_EQ(step_of(r, rpm, 1400000, back), 1000000u);
    EXPECT_EQ(step_of(r, rpm, 500000, back), kNone);
    EXPECT_EQ(step_of(r, rpm, 99000000, back), 5500000u);
    EXPECT_EQ(step_of(r, vin, 700000, fwd), 2600000u) << "a text signal's";
    EXPECT_EQ(step_of(r, vin, 2600000, back), 700000u);

    uint64_t at = 0;
    EXPECT_EQ(ecuio_recording_step(nullptr, rpm, 0, fwd, &at),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, rpm, 0, fwd, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, rpm, 0, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, rpm, 0, 3, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_step(r.r, r.info().signal_count, 0, fwd, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
}
