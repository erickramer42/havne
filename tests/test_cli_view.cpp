// File/layer: tests/test_cli_view.cpp | havne-cli's line formatting and output ring (ADR-0031)
// Testing: UNIT (test_cli): this file's suites

#include "test_cli_support.h"

TEST(CliViewTest, AlignOnTakesItsFormsAndNothingElse)
{
    cli::AlignOn a;
    ASSERT_TRUE(cli::parse_align_on("rise:RPM:4000", a));
    EXPECT_EQ(a.kind, cli::AlignOn::rise);
    EXPECT_EQ(a.signal, "RPM");
    EXPECT_EQ(a.level, 4000);
    /* The level is the last colon's: a name may hold one */
    ASSERT_TRUE(cli::parse_align_on("fall:Trim:B1:-2.5", a));
    EXPECT_EQ(a.kind, cli::AlignOn::fall);
    EXPECT_EQ(a.signal, "Trim:B1");
    EXPECT_EQ(a.level, -2.5);
    ASSERT_TRUE(cli::parse_align_on("marker:lap: 2", a));
    EXPECT_EQ(a.kind, cli::AlignOn::marker);
    EXPECT_EQ(a.label, "lap: 2");
    ASSERT_TRUE(cli::parse_align_on("engine-start", a));
    EXPECT_EQ(a.kind, cli::AlignOn::engine_start);
    /* A pull by its number, 1 the first: one for all, or one for each */
    ASSERT_TRUE(cli::parse_align_on("pull:2", a));
    EXPECT_EQ(a.kind, cli::AlignOn::pull);
    EXPECT_EQ(a.pulls, (std::vector<uint32_t>{2}));
    ASSERT_TRUE(cli::parse_align_on("pull:3,1,100000", a));
    EXPECT_EQ(a.pulls, (std::vector<uint32_t>{3, 1, 100000}));
    ASSERT_TRUE(cli::parse_align_on("utc", a));
    EXPECT_EQ(a.kind, cli::AlignOn::utc);
    for (const char* bad : {"",
                            "UTC",
                            "rise",
                            "rise:RPM",
                            "rise::4000",
                            "rise:RPM:",
                            "rise:RPM:fast",
                            "rise:RPM:inf",
                            "rise:RPM:4000x",
                            "marker:",
                            "sideways:RPM:1",
                            "Engine-Start",
                            "engine-start:RPM",
                            "engine",
                            "engine-start ",
                            "pull",
                            "pull:",
                            "pull:0",
                            "pull:x",
                            "pull:1,",
                            "pull:,1",
                            "pull:1,,2",
                            "pull:-1",
                            "pull:1.5",
                            "pull:100001",
                            "pull:1 ",
                            "pull: 1",
                            "pull:1,0",
                            "Pull:1",
                            "pull:0000001"}) {
        EXPECT_FALSE(cli::parse_align_on(bad, a)) << bad;
    }
    EXPECT_EQ(a.kind, cli::AlignOn::utc) << "a refusal leaves it as it was";
    EXPECT_TRUE(a.pulls.empty());
}

TEST(CliViewTest, AColumnsSpanIsTheSamplesTheReaderCountsInIt)
{
    uint64_t from = 0;
    uint64_t to = 0;
    /* Columns under a second: their own microseconds, the end left out */
    ASSERT_TRUE(cli::column_span(250000, 1250000, 4, 0, from, to));
    EXPECT_EQ(from, 250000u);
    EXPECT_EQ(to, 499999u);
    ASSERT_TRUE(cli::column_span(250000, 1250000, 4, 3, from, to));
    EXPECT_EQ(from, 1000000u);
    EXPECT_EQ(to, 1249999u);

    /* Columns of 2.5 s from 0.4 s: whole seconds, each in the column it
     * starts in, the first taking the second across the start */
    ASSERT_TRUE(cli::column_span(400000, 10400000, 4, 0, from, to));
    EXPECT_EQ(from, 0u);
    EXPECT_EQ(to, 2999999u) << "seconds 0, 1 and 2";
    ASSERT_TRUE(cli::column_span(400000, 10400000, 4, 1, from, to));
    EXPECT_EQ(from, 3000000u);
    EXPECT_EQ(to, 5999999u) << "second 5 starts at 5.0, under 5.4";
    ASSERT_TRUE(cli::column_span(400000, 10400000, 4, 2, from, to));
    EXPECT_EQ(from, 6000000u);
    EXPECT_EQ(to, 7999999u);
    /* The last second whole, though it runs past the end */
    ASSERT_TRUE(cli::column_span(400000, 10400000, 4, 3, from, to));
    EXPECT_EQ(from, 8000000u);
    EXPECT_EQ(to, 10999999u);

    /* Columns of exactly a second on the second: one each */
    ASSERT_TRUE(cli::column_span(3000000, 6000000, 3, 1, from, to));
    EXPECT_EQ(from, 4000000u);
    EXPECT_EQ(to, 4999999u);
    /* Off it by half: the first takes two seconds, the last none */
    ASSERT_TRUE(cli::column_span(3500000, 6500000, 3, 0, from, to));
    EXPECT_EQ(from, 3000000u);
    EXPECT_EQ(to, 4999999u);
    ASSERT_TRUE(cli::column_span(3500000, 6500000, 3, 2, from, to));
    EXPECT_EQ(from, 6000000u);
    EXPECT_EQ(to, 6999999u);
    /* The column before a recording's start, whole seconds from 0 */
    ASSERT_TRUE(cli::column_span(0, 14000000, 1, 0, from, to));
    EXPECT_EQ(from, 0u);
    EXPECT_EQ(to, 13999999u);
}

TEST(CliViewTest, AnAlignedViewsWideColumnsHoldTheirOwnWindow)
{
    cli::Axis a;
    ASSERT_TRUE(cli::make_axis(-31000000, 9000000, 8, a));
    ASSERT_EQ(a.column_us, 5000000);
    EXPECT_TRUE(cli::columns_own_window(true, a));
    EXPECT_FALSE(cli::columns_own_window(false, a))
        << "an unaligned view's recordings share their seconds";
    cli::Axis second;
    ASSERT_TRUE(cli::make_axis(0, 8000000, 8, second));
    EXPECT_TRUE(cli::columns_own_window(true, second)) << "a second counts";
    cli::Axis under;
    ASSERT_TRUE(cli::make_axis(0, 7999999, 8, under));
    ASSERT_EQ(under.column_us, 1000000) << "rounded out to the second";
    EXPECT_TRUE(cli::columns_own_window(true, under));
    ASSERT_TRUE(cli::make_axis(0, 7999992, 8, under));
    ASSERT_EQ(under.column_us, 999999);
    EXPECT_FALSE(cli::columns_own_window(true, under))
        << "under a second a query counts the column's own samples";

    /* A recording whose zero sits 31.5 s into it: each axis column is
     * the same 5 s of the axis, here in the recording's own time */
    uint64_t from = 0;
    uint64_t to = 0;
    ASSERT_TRUE(cli::own_span(a, -31500000, 0, from, to));
    EXPECT_EQ(from, 500000u);
    EXPECT_EQ(to, 5499999u) << "both ends counted, the next column's left";
    ASSERT_TRUE(cli::own_span(a, -31500000, 6, from, to));
    EXPECT_EQ(from, 30500000u);
    EXPECT_EQ(to, 35499999u);
    ASSERT_TRUE(cli::own_span(a, -31500000, 7, from, to));
    EXPECT_EQ(from, 35500000u);
    EXPECT_EQ(to, 40500000u) << "the last column holds the axis's end too";
    /* One 12.5 s in: the first two columns end before its start, the
     * third is cut at it */
    EXPECT_FALSE(cli::own_span(a, -12500000, 0, from, to));
    EXPECT_FALSE(cli::own_span(a, -12500000, 1, from, to));
    EXPECT_FALSE(cli::own_span(a, -21000000, 1, from, to))
        << "a column ending on the microsecond before the start";
    ASSERT_TRUE(cli::own_span(a, -21000001, 1, from, to));
    EXPECT_EQ(from, 0u);
    EXPECT_EQ(to, 0u) << "a column ending on the first microsecond";
    ASSERT_TRUE(cli::own_span(a, -12500000, 3, from, to));
    EXPECT_EQ(from, 0u);
    EXPECT_EQ(to, 1499999u);
    ASSERT_TRUE(cli::own_span(a, -12500000, 4, from, to));
    EXPECT_EQ(from, 1500000u);
    EXPECT_EQ(to, 6499999u);
    /* A recording put later on the axis (utc): its time runs behind */
    ASSERT_TRUE(cli::own_span(a, 4000000, 7, from, to));
    EXPECT_EQ(from, 0u);
    EXPECT_EQ(to, 5000000u);
    /* One that starts on the axis's last microsecond: that alone */
    ASSERT_TRUE(cli::own_span(a, 9000000, 7, from, to));
    EXPECT_EQ(from, 0u);
    EXPECT_EQ(to, 0u);
    EXPECT_FALSE(cli::own_span(a, 9000001, 7, from, to));

    /* The statistics over such a span, as the column a query would give */
    ecuio_recording_stats_t st = cli::no_stats();
    ecuio_recording_column_t col = cli::column_of_stats(st);
    EXPECT_EQ(col.samples, 0u);
    st.samples = 12;
    st.lowest = -3.5;
    st.highest = 41;
    st.mean = 7;
    col = cli::column_of_stats(st);
    EXPECT_EQ(col.samples, 12u);
    EXPECT_EQ(col.lowest, -3.5);
    EXPECT_EQ(col.highest, 41);
    EXPECT_EQ(col.flags, 0u);
    st.samples = uint64_t{UINT32_MAX} + 5;
    EXPECT_EQ(cli::column_of_stats(st).samples, UINT32_MAX)
        << "a count past the column's room is its most, never wrapped";
}

TEST(CliViewTest, TheLastColumnHoldsTheAxisEnd)
{
    /* Columns under a second: every query stops short of the end */
    cli::Axis narrow;
    ASSERT_TRUE(cli::make_axis(0, 4000000, 8, narrow));
    cli::Window w;
    ASSERT_TRUE(cli::window_of(narrow, 0, w));
    EXPECT_TRUE(cli::query_leaves_end(w, narrow));
    ASSERT_TRUE(cli::window_of(narrow, -250000, w));
    EXPECT_TRUE(cli::query_leaves_end(w, narrow)) << "ending inside a second";

    /* Columns of a second or more: a query ending inside a second takes
     * that second whole, one ending on a second leaves the end out */
    cli::Axis wide;
    ASSERT_TRUE(cli::make_axis(0, 40000000, 8, wide));
    ASSERT_TRUE(cli::window_of(wide, 0, w));
    ASSERT_EQ(w.t1_us, 40000000u);
    EXPECT_TRUE(cli::query_leaves_end(w, wide));
    ASSERT_TRUE(cli::window_of(wide, -250000, w));
    ASSERT_EQ(w.t1_us, 40250000u);
    EXPECT_FALSE(cli::query_leaves_end(w, wide));

    /* The axis ending inside the column across a recording's start: that
     * one query's own width decides */
    ASSERT_TRUE(cli::window_of(wide, 39500000, w));
    ASSERT_TRUE(w.partial);
    ASSERT_EQ(w.count, 1u);
    ASSERT_EQ(w.t1_us, 500000u);
    EXPECT_TRUE(cli::query_leaves_end(w, wide)) << "half a second, read raw";
    ASSERT_TRUE(cli::window_of(wide, 37500000, w));
    ASSERT_EQ(w.count, 1u);
    ASSERT_EQ(w.t1_us, 2500000u);
    EXPECT_FALSE(cli::query_leaves_end(w, wide));
    ASSERT_TRUE(cli::window_of(wide, 38000000, w));
    ASSERT_EQ(w.t1_us, 2000000u);
    EXPECT_TRUE(cli::query_leaves_end(w, wide));

    /* The end's samples into the last column */
    ecuio_recording_column_t into{};
    ecuio_recording_column_t more{};
    cli::add_column(into, more);
    EXPECT_EQ(into.samples, 0u);
    more.samples = 2;
    more.lowest = 10;
    more.highest = 30;
    cli::add_column(into, more);
    EXPECT_EQ(into.samples, 2u);
    EXPECT_EQ(into.lowest, 10);
    EXPECT_EQ(into.highest, 30);
    more.samples = 1;
    more.lowest = more.highest = 20;
    cli::add_column(into, more);
    EXPECT_EQ(into.samples, 3u);
    EXPECT_EQ(into.lowest, 10) << "inside the range: it stays";
    EXPECT_EQ(into.highest, 30);
    more.lowest = more.highest = 45;
    cli::add_column(into, more);
    EXPECT_EQ(into.samples, 4u);
    EXPECT_EQ(into.highest, 45);
    more.lowest = more.highest = -5;
    cli::add_column(into, more);
    EXPECT_EQ(into.lowest, -5);
    EXPECT_EQ(into.highest, 45);
    more.samples = 0;
    more.lowest = more.highest = 99;
    cli::add_column(into, more);
    EXPECT_EQ(into.samples, 5u) << "an empty column adds nothing";
    EXPECT_EQ(into.highest, 45);
    into.samples = UINT32_MAX - 1;
    more.samples = 7;
    cli::add_column(into, more);
    EXPECT_EQ(into.samples, UINT32_MAX);
}

TEST(CliViewTest, OneMicrosecondsStatisticsComeFromItsColumn)
{
    ecuio_recording_column_t col{};
    ecuio_recording_stats_t st = cli::stats_of_instant(col, false, 0);
    EXPECT_EQ(st.samples, 0u);
    EXPECT_TRUE(std::isnan(st.last));
    EXPECT_TRUE(std::isnan(st.lowest));
    EXPECT_TRUE(std::isnan(st.mean));
    EXPECT_EQ(st.lowest_us, ECUIO_RECORDING_NONE);
    EXPECT_EQ(st.highest_us, ECUIO_RECORDING_NONE);

    col.samples = 1;
    col.lowest = col.highest = 50;
    st = cli::stats_of_instant(col, false, 7);
    EXPECT_EQ(st.samples, 1u);
    EXPECT_EQ(st.first, 50);
    EXPECT_EQ(st.last, 50);
    EXPECT_EQ(st.lowest, 50);
    EXPECT_EQ(st.highest, 50);
    EXPECT_EQ(st.mean, 50);
    EXPECT_EQ(st.lowest_us, 7u);
    EXPECT_EQ(st.highest_us, 7u);

    /* Two unlike samples in one microsecond: which came last isn't told */
    col.samples = 2;
    col.highest = 54;
    st = cli::stats_of_instant(col, false, 0);
    EXPECT_EQ(st.samples, 2u);
    EXPECT_EQ(st.lowest, 50);
    EXPECT_EQ(st.highest, 54);
    EXPECT_EQ(st.mean, 52);
    EXPECT_TRUE(std::isnan(st.first));
    EXPECT_TRUE(std::isnan(st.last));
    col.samples = 3;
    st = cli::stats_of_instant(col, false, 0);
    EXPECT_TRUE(std::isnan(st.mean)) << "three unlike samples have no mean";
    EXPECT_EQ(st.highest, 54);

    /* A text signal's samples alone */
    col.samples = 1;
    col.lowest = col.highest = 50;
    st = cli::stats_of_instant(col, true, 0);
    EXPECT_EQ(st.samples, 1u);
    EXPECT_TRUE(std::isnan(st.last));
    EXPECT_TRUE(std::isnan(st.lowest));
    EXPECT_TRUE(std::isnan(st.mean));
    EXPECT_EQ(st.lowest_us, ECUIO_RECORDING_NONE);
}

TEST(CliViewTest, AFailedReadsErrorLineSaysItsOwnCause)
{
    EXPECT_STREQ(cli::read_error_detail(ECUIO_ERR_RECORDING_CHANGED),
                 "the recording changed while it was read");
    EXPECT_EQ(cli::read_error_detail(ECUIO_ERR_OUT_OF_MEMORY), nullptr)
        << "core's own detail, never the changed file's words";
    EXPECT_EQ(cli::read_error_detail(ECUIO_ERR_INVALID_ARGUMENT), nullptr);
}

TEST(CliViewTest, AColumnsMeanComesFromItsRangeWhereThatGivesIt)
{
    ecuio_recording_column_t col{};
    double mean = 7;
    ASSERT_TRUE(cli::mean_of_column(col, mean));
    EXPECT_TRUE(std::isnan(mean)) << "an empty column has none";
    col.samples = 1;
    col.lowest = col.highest = 12.5;
    ASSERT_TRUE(cli::mean_of_column(col, mean));
    EXPECT_EQ(mean, 12.5);
    col.samples = 40;
    ASSERT_TRUE(cli::mean_of_column(col, mean));
    EXPECT_EQ(mean, 12.5) << "samples all alike";
    col.samples = 2;
    col.highest = 14.5;
    ASSERT_TRUE(cli::mean_of_column(col, mean));
    EXPECT_EQ(mean, 13.5);
    col.samples = 3;
    EXPECT_FALSE(cli::mean_of_column(col, mean))
        << "three unlike samples must be read";
}

TEST(CliViewTest, SecondsAreSignedAndToTheMicrosecond)
{
    int64_t us = 0;
    ASSERT_TRUE(cli::parse_seconds("-5", us));
    EXPECT_EQ(us, -5000000);
    ASSERT_TRUE(cli::parse_seconds("1.0000015", us));
    EXPECT_EQ(us, 1000002);
    ASSERT_TRUE(cli::parse_seconds("0", us));
    EXPECT_EQ(us, 0);
    for (const char* bad : {"", "soon", "5s", "nan", "1e10", "-1e10"}) {
        EXPECT_FALSE(cli::parse_seconds(bad, us)) << bad;
    }
}

TEST(CliViewTest, ASpanIsTwoSecondsInOrder)
{
    cli::Span s;
    ASSERT_TRUE(cli::parse_span("-1.5:2", s));
    EXPECT_EQ(s.from_us, -1500000);
    EXPECT_EQ(s.to_us, 2000000);
    ASSERT_TRUE(cli::parse_span("0:0.000001", s));
    EXPECT_EQ(s.to_us, 1);
    for (const char* bad :
         {"", "5", "5:", ":5", "5:3", "5:5", "a:b", "1:2:3", "1;2"}) {
        EXPECT_FALSE(cli::parse_span(bad, s)) << bad;
    }
}

TEST(CliViewTest, AStatsLineHoldsTheSpanAndNullsWhatIsntThere)
{
    char buf[1024];
    ecuio_recording_signal_t rpm{};
    std::strcpy(rpm.name, "RPM");
    std::strcpy(rpm.key, "7E8:410C:2.0:16");
    cli::Span span;
    span.from_us = -1000000;
    span.to_us = 2500000;
    ecuio_recording_stats_t st = cli::no_stats();
    st.samples = 3;
    st.first = 812.5;
    st.last = 900;
    st.lowest = 800;
    st.lowest_us = 400000;
    st.highest = HUGE_VAL;
    st.highest_us = 2000000;
    st.mean = HUGE_VAL;
    cli::Line l(buf, sizeof buf);
    cli::stats_line(l, 1, rpm, span, st);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"stats\",\"recording\":1,\"signal\":\"RPM\","
              "\"key\":\"7E8:410C:2.0:16\","
              "\"from_us\":-1000000,\"to_us\":2500000,\"samples\":3,"
              "\"first\":812.5,\"last\":900,\"lowest\":800,"
              "\"lowest_us\":400000,\"highest\":null,\"highest_us\":2000000,"
              "\"mean\":null}");
    /* A recording older than format 4 keeps no key: null */
    rpm.key[0] = 0;
    cli::Line e(buf, sizeof buf);
    cli::stats_line(e, 0, rpm, span, cli::no_stats());
    EXPECT_EQ(
        std::string(e.data(), e.size()),
        "{\"type\":\"stats\",\"recording\":0,\"signal\":\"RPM\",\"key\":null,"
        "\"from_us\":-1000000,\"to_us\":2500000,\"samples\":0,"
        "\"first\":null,\"last\":null,\"lowest\":null,"
        "\"lowest_us\":null,\"highest\":null,\"highest_us\":null,"
        "\"mean\":null}");
}

TEST(CliViewTest, AStretchLevelIsAKeyAndAValueInItsUnits)
{
    ecuio_stretch_rules_t r = cli::stretch_rules(ECUIO_STRETCH_PULLS);
    EXPECT_EQ(r.kinds, ECUIO_STRETCH_PULLS);
    EXPECT_EQ(r.throttle, UINT32_MAX);
    EXPECT_EQ(r.engine_speed, UINT32_MAX);
    ASSERT_TRUE(cli::parse_stretch_level("pull-throttle=85", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("pull-load=70.5", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("pull-hold=1.5", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("pull-gap=0.75", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("cruise-hold=20", false, r));
    ASSERT_TRUE(cli::parse_stretch_level("cruise-throttle-band=5", true, r));
    ASSERT_TRUE(cli::parse_stretch_level("pull-rise=600", true, r));
    EXPECT_EQ(r.pull_min_rise, 600.0) << "rpm has no unit system";
    ASSERT_TRUE(cli::parse_stretch_level("pull-gain=12", false, r));
    EXPECT_EQ(r.pull_min_gain, 12.0);
    ASSERT_TRUE(cli::parse_stretch_level("pull-gain=5", true, r));
    EXPECT_DOUBLE_EQ(r.pull_min_gain, 8.04672) << "5 mph in km/h";
    EXPECT_EQ(r.pull_level, 85.0);
    EXPECT_EQ(r.pull_load_level, 70.5);
    EXPECT_EQ(r.pull_hold_us, 1500000u);
    EXPECT_EQ(r.pull_gap_us, 750000u);
    EXPECT_EQ(r.cruise_hold_us, 20000000u);
    EXPECT_EQ(r.cruise_throttle_band, 5.0) << "a percentage has no unit system";

    /* Speeds in km/h, or mph under imperial: 40 mph reaches core as km/h */
    ASSERT_TRUE(cli::parse_stretch_level("cruise-floor=40", true, r));
    EXPECT_DOUBLE_EQ(r.cruise_floor, 64.37376);
    ASSERT_TRUE(cli::parse_stretch_level("cruise-speed-band=2", true, r));
    EXPECT_DOUBLE_EQ(r.cruise_speed_band, 3.218688);
    ASSERT_TRUE(cli::parse_stretch_level("cruise-floor=60", false, r));
    EXPECT_EQ(r.cruise_floor, 60.0);
    ASSERT_TRUE(cli::parse_stretch_level("pull-gap=0", false, r));
    EXPECT_EQ(r.pull_gap_us, 0u) << "0 takes core's default";

    for (const char* bad :
         {"pull-gap", "pull-gap=", "pull-gap=-1", "pull-gap=soon", "=1",
          "pull-speed=1", "cruise-floor=1e10"}) {
        EXPECT_FALSE(cli::parse_stretch_level(bad, false, r)) << bad;
    }
}

TEST(CliViewTest, ARoleAndAKindAreOneOfTheirWords)
{
    uint32_t role = 0;
    std::string name;
    ASSERT_TRUE(cli::parse_role("speed=Vehicle Speed", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_VEHICLE_SPEED);
    EXPECT_EQ(name, "Vehicle Speed");
    ASSERT_TRUE(cli::parse_role("throttle=Pedal=D", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_THROTTLE);
    EXPECT_EQ(name, "Pedal=D") << "the name is all after the first =";
    ASSERT_TRUE(cli::parse_role("load=Load", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_ENGINE_LOAD);
    ASSERT_TRUE(cli::parse_role("rpm=RPM", role, name));
    EXPECT_EQ(role, ECUIO_RECORDING_ROLE_ENGINE_SPEED);
    for (const char* bad : {"speed", "speed=", "brake=Pedal", "=RPM"}) {
        EXPECT_FALSE(cli::parse_role(bad, role, name)) << bad;
    }
    ecuio_stretch_rules_t r = cli::stretch_rules(0);
    cli::role_field(r, ECUIO_RECORDING_ROLE_ENGINE_LOAD) = 7;
    EXPECT_EQ(r.engine_load, 7u);
    EXPECT_EQ(r.throttle, UINT32_MAX);

    uint32_t kinds = 0;
    ASSERT_TRUE(cli::parse_stretch_kinds("pulls", kinds));
    EXPECT_EQ(kinds, ECUIO_STRETCH_PULLS);
    ASSERT_TRUE(cli::parse_stretch_kinds("cruises", kinds));
    EXPECT_EQ(kinds, ECUIO_STRETCH_CRUISES);
    ASSERT_TRUE(cli::parse_stretch_kinds("all", kinds));
    EXPECT_EQ(kinds, ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES);
    EXPECT_FALSE(cli::parse_stretch_kinds("pull", kinds));

    /* --pull-end sets or clears its flag alone */
    uint32_t flags = 0x40;
    ASSERT_TRUE(cli::parse_pull_end("top-speed", flags));
    EXPECT_EQ(flags, 0x40u | ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED);
    ASSERT_TRUE(cli::parse_pull_end("lift", flags));
    EXPECT_EQ(flags, 0x40u);
    for (const char* bad : {"", "Lift", "top", "top_speed", "speed"}) {
        EXPECT_FALSE(cli::parse_pull_end(bad, flags)) << bad;
    }
    EXPECT_EQ(flags, 0x40u) << "a refused word changes nothing";
}

TEST(CliViewTest, TheRolesAndStretchLinesSayWhatWasFoundAndWith)
{
    char buf[1024];
    ecuio_recording_signal_t speed{};
    std::strcpy(speed.name, "Vehicle Speed");
    ecuio_recording_signal_t rpm{};
    std::strcpy(rpm.name, "RPM");
    const ecuio_recording_signal_t* by_role[4] = {nullptr, nullptr, &speed,
                                                  &rpm};
    cli::Line l(buf, sizeof buf);
    cli::roles_line(l, 1, by_role, cli::stretch_rules(ECUIO_STRETCH_PULLS));
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"roles\",\"recording\":1,\"throttle\":null,"
              "\"engine_load\":null,\"vehicle_speed\":\"Vehicle Speed\","
              "\"engine_speed\":\"RPM\",\"cruise_floor\":56.32704,"
              "\"cruise_speed_band\":2,\"pull_gain\":10,"
              "\"pull_end\":\"lift\"}")
        << "the defaults core takes, in km/h";
    ecuio_stretch_rules_t set = cli::stretch_rules(ECUIO_STRETCH_PULLS);
    set.pull_min_gain = 8.04672;
    cli::Line gl(buf, sizeof buf);
    cli::roles_line(gl, 0, by_role, set);
    EXPECT_NE(std::string(gl.data(), gl.size()).find("\"pull_gain\":8.04672,"),
              std::string::npos);
    set.flags = ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED;
    cli::Line tl(buf, sizeof buf);
    cli::roles_line(tl, 0, by_role, set);
    EXPECT_NE(
        std::string(tl.data(), tl.size()).find("\"pull_end\":\"top-speed\"}"),
        std::string::npos);

    ecuio_recording_stretch_t s{};
    s.kind = ECUIO_RECORDING_STRETCH_PULL;
    s.from_us = 14000000;
    s.to_us = 19500000;
    s.start = 40;
    s.end = 113.5;
    s.flags = ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED |
              ECUIO_RECORDING_STRETCH_CUT_END;
    std::strcpy(rpm.key, "7E8:410C:2.0:16");
    cli::Line sl(buf, sizeof buf);
    cli::stretch_line(sl, 0, s, rpm, -14600000);
    EXPECT_EQ(std::string(sl.data(), sl.size()),
              "{\"type\":\"stretch\",\"recording\":0,\"kind\":\"pull\","
              "\"from_us\":-600000,\"to_us\":4900000,\"length_us\":5500000,"
              "\"signal\":\"RPM\",\"key\":\"7E8:410C:2.0:16\","
              "\"start\":40,\"end\":113.5,"
              "\"by_load\":false,\"by_engine_speed\":true,"
              "\"no_throttle\":false,\"cut_start\":false,\"cut_end\":true}");
    s.kind = ECUIO_RECORDING_STRETCH_CRUISE;
    cli::Line cl(buf, sizeof buf);
    cli::stretch_line(cl, 0, s, speed, 0);
    EXPECT_NE(std::string(cl.data(), cl.size())
                  .find("\"signal\":\"Vehicle Speed\",\"key\":null,"),
              std::string::npos)
        << "no key in a recording older than format 4";
    EXPECT_NE(std::string(cl.data(), cl.size()).find("\"kind\":\"cruise\""),
              std::string::npos);
}

TEST(CliViewTest, TheAxisRoundsOutToWholeColumns)
{
    cli::Axis a;
    ASSERT_TRUE(cli::make_axis(-5000000, 10000000, 15, a));
    EXPECT_EQ(a.column_us, 1000000);
    EXPECT_EQ(a.to_us, 10000000);
    ASSERT_TRUE(cli::make_axis(0, 10, 3, a));
    EXPECT_EQ(a.column_us, 4);
    EXPECT_EQ(a.to_us, 12);
    EXPECT_FALSE(cli::make_axis(5, 5, 3, a));
    EXPECT_FALSE(cli::make_axis(0, 10, 0, a));
}

TEST(CliViewTest, AWindowCoversWhatTheRecordingHoldsOnce)
{
    cli::Axis a;
    cli::Window w;
    /* Unaligned: the whole axis, one query */
    ASSERT_TRUE(cli::make_axis(0, 10000000, 10, a));
    ASSERT_TRUE(cli::window_of(a, 0, w));
    EXPECT_EQ(w.first, 0u);
    EXPECT_EQ(w.count, 10u);
    EXPECT_FALSE(w.partial);
    EXPECT_EQ(w.t0_us, 0u);
    EXPECT_EQ(w.t1_us, 10000000u);

    /* Starting 3.5 s into the axis, wide columns: half a second of column
     * 3 holds no whole second, so the query starts at column 4, which
     * takes second 0 (it starts before the query) */
    ASSERT_TRUE(cli::window_of(a, 3500000, w));
    EXPECT_FALSE(w.partial);
    EXPECT_EQ(w.first, 4u);
    EXPECT_EQ(w.count, 6u);
    EXPECT_EQ(w.t0_us, 500000u);
    /* 3 s columns: the column across its start ends at its last whole
     * second, and the second across the split goes to the next */
    ASSERT_TRUE(cli::make_axis(0, 30000000, 10, a));
    ASSERT_TRUE(cli::window_of(a, 4500000, w));
    EXPECT_TRUE(w.partial);
    EXPECT_EQ(w.first, 1u);
    EXPECT_EQ(w.count, 9u);
    EXPECT_EQ(w.partial_t1_us, 1000000u);
    EXPECT_EQ(w.t0_us, 1500000u);
    EXPECT_EQ(w.t1_us, 25500000u);

    /* Narrow columns: the raw rows, split exactly */
    ASSERT_TRUE(cli::make_axis(0, 1000000, 10, a)); /* 100 ms */
    ASSERT_TRUE(cli::window_of(a, 250000, w));
    EXPECT_TRUE(w.partial);
    EXPECT_EQ(w.first, 2u);
    EXPECT_EQ(w.count, 8u);
    EXPECT_EQ(w.partial_t1_us, 50000u);
    EXPECT_EQ(w.t0_us, 50000u);

    /* On a column boundary: no partial column */
    ASSERT_TRUE(cli::window_of(a, 300000, w));
    EXPECT_FALSE(w.partial);
    EXPECT_EQ(w.first, 3u);
    EXPECT_EQ(w.t0_us, 0u);

    /* The axis ends inside the column across its start */
    ASSERT_TRUE(cli::window_of(a, 950000, w));
    EXPECT_TRUE(w.partial);
    EXPECT_EQ(w.first, 9u);
    EXPECT_EQ(w.count, 1u);
    EXPECT_EQ(w.partial_t1_us, 50000u);

    /* All of the axis before it, or the axis ending where it starts */
    EXPECT_FALSE(cli::window_of(a, 1000000, w));
    EXPECT_FALSE(cli::window_of(a, 5000000, w));
    /* Started before the axis: a later part of it */
    ASSERT_TRUE(cli::window_of(a, -2000000, w));
    EXPECT_EQ(w.first, 0u);
    EXPECT_EQ(w.t0_us, 2000000u);
    EXPECT_EQ(w.t1_us, 3000000u);
}

/* The opened line's conditions: null with no set and no line; the set's
 * hash, whether it is there and the same, and the events read, skipped,
 * cut and, last, lost by the recorder. A cond line is stream's condition
 * line with the recording's time and held added. */
TEST(CliViewTest, ARecordingsConditionsAreNamedAndEachEventIsALine)
{
    char buf[1024];
    ecuio_recording_info_t info{};
    info.format = ECUIO_RECORDING_FORMAT_ENUMS;
    const ecuio_recording_signals_t no_file{};
    ecuio_recording_conditions_t cs{};
    const auto conditions_of = [&] {
        cli::Line l(buf, sizeof buf);
        cli::opened_line(l, 0, info, no_file, cs);
        const std::string line(l.data(), l.size());
        const size_t at = line.find(",\"conditions\":");
        return at == std::string::npos ? line : line.substr(at);
    };
    EXPECT_EQ(conditions_of(), ",\"conditions\":null}");
    info.condition_events = 2;
    EXPECT_EQ(conditions_of(),
              ",\"conditions\":{\"sha256\":null,\"present\":false,"
              "\"matches\":false,\"events\":2,\"lines_skipped\":0,"
              "\"cut\":false,\"events_lost\":0}}")
        << "lines with no set named or present";
    info.condition_events = 0;
    cs.lines_skipped = 1;
    EXPECT_EQ(conditions_of(),
              ",\"conditions\":{\"sha256\":null,\"present\":false,"
              "\"matches\":false,\"events\":0,\"lines_skipped\":1,"
              "\"cut\":false,\"events_lost\":0}}")
        << "only a line that doesn't parse";
    cs.lines_skipped = 0;
    cs.events_lost = 3;
    EXPECT_EQ(conditions_of(),
              ",\"conditions\":{\"sha256\":null,\"present\":false,"
              "\"matches\":false,\"events\":0,\"lines_skipped\":0,"
              "\"cut\":false,\"events_lost\":3}}")
        << "only a drop line of lost events";
    cs.flags = ECUIO_RECORDING_CONDITIONS_NAMED |
               ECUIO_RECORDING_CONDITIONS_PRESENT |
               ECUIO_RECORDING_CONDITIONS_MATCH;
    std::memset(cs.sha256, 'b', 64);
    cs.lines_skipped = 3;
    cs.events_lost = UINT64_MAX;
    info.condition_events = 1048577;
    info.flags = ECUIO_RECORDING_INFO_CONDITIONS_CUT;
    EXPECT_EQ(conditions_of(),
              ",\"conditions\":{\"sha256\":\"" + std::string(64, 'b') +
                  "\",\"present\":true,\"matches\":true,\"events\":1048577,"
                  "\"lines_skipped\":3,\"cut\":true,"
                  "\"events_lost\":18446744073709551615}}");
    cs.flags = ECUIO_RECORDING_CONDITIONS_PRESENT;
    info.flags = 0;
    EXPECT_NE(conditions_of().find("\"sha256\":null,\"present\":true,"
                                   "\"matches\":false"),
              std::string::npos)
        << "present, not named";

    ecuio_recording_condition_event_t e{};
    e.host_us = 7251000;
    e.timestamp_us = 7250000;
    e.value = 111;
    e.state = ECUIO_CONDITION_ON;
    e.flags = ECUIO_CONDITION_FIRED | ECUIO_CONDITION_HELD;
    e.term = 1;
    std::memcpy(e.key, "coolant_high", 13);
    char line[cli::kRecordedConditionLineBytes];
    cli::Line c(line, sizeof line);
    cli::recorded_condition_line(c, 2, e);
    EXPECT_EQ(std::string(c.data(), c.size()),
              "{\"type\":\"condition\",\"recording\":2,\"host_us\":7251000,"
              "\"t_us\":7250000,\"key\":\"coolant_high\",\"state\":\"on\","
              "\"fired\":true,\"stale\":false,\"held\":true,\"value\":111,"
              "\"term\":1}");
    e.state = ECUIO_CONDITION_OFF;
    e.flags = ECUIO_CONDITION_STALE;
    std::memset(e.key, 'k', sizeof e.key - 1);
    cli::Line d(line, sizeof line);
    cli::recorded_condition_line(d, 0, e);
    EXPECT_TRUE(d.finish()) << "a key of the most bytes fits";
    EXPECT_NE(std::string(d.data(), d.size())
                  .find("\"key\":\"" + std::string(31, 'k') +
                        "\",\"state\":\"off\",\"fired\":false,\"stale\":true,"
                        "\"held\":false"),
              std::string::npos);
}

TEST(CliViewTest, TheLinesCarryWhatTheReaderHolds)
{
    char buf[1024];
    ecuio_recording_info_t info{};
    info.format = ECUIO_RECORDING_FORMAT_OLDER;
    info.flags = ECUIO_RECORDING_INFO_NO_SUMMARY;
    info.duration_us = 30000000;
    info.signal_count = 2;
    info.rows_torn = 1;
    info.records_dropped = 4;
    const ecuio_recording_signals_t no_file{};
    const ecuio_recording_conditions_t no_set{};
    cli::Line o(buf, sizeof buf);
    cli::opened_line(o, 1, info, no_file, no_set);
    EXPECT_EQ(std::string(o.data(), o.size()),
              "{\"type\":\"opened\",\"recording\":1,\"format\":\"older\","
              "\"duration_us\":30000000,\"utc_start_us\":null,\"signals\":2,"
              "\"markers\":0,\"summary\":false,\"rows_torn\":1,"
              "\"records_dropped\":4,\"notes_dropped\":0,"
              "\"signal_file\":null,\"conditions\":null}");
    /* The header's file: its hash, there and the same; or there unnamed */
    ecuio_recording_signals_t named{};
    named.flags = ECUIO_RECORDING_SIGNALS_NAMED |
                  ECUIO_RECORDING_SIGNALS_PRESENT |
                  ECUIO_RECORDING_SIGNALS_MATCH;
    std::memset(named.sha256, 'a', 64);
    cli::Line f(buf, sizeof buf);
    cli::opened_line(f, 1, info, named, no_set);
    const std::string line(f.data(), f.size());
    EXPECT_NE(line.find(",\"signal_file\":{\"sha256\":\"" +
                        std::string(64, 'a') +
                        "\",\"present\":true,\"matches\":true},"
                        "\"conditions\":null}"),
              std::string::npos)
        << line;
    ecuio_recording_signals_t unnamed{};
    unnamed.flags = ECUIO_RECORDING_SIGNALS_PRESENT;
    cli::Line u(buf, sizeof buf);
    cli::opened_line(u, 1, info, unnamed, no_set);
    const std::string unnamed_line(u.data(), u.size());
    EXPECT_NE(unnamed_line.find(",\"signal_file\":{\"sha256\":null,\"present\":"
                                "true,\"matches\":false},"),
              std::string::npos)
        << unnamed_line;
    EXPECT_STREQ(cli::format_word(ECUIO_RECORDING_FORMAT_ENUMS), "core")
        << "format 3 is core's, its enums on their signal lines";
    EXPECT_STREQ(cli::format_word(ECUIO_RECORDING_FORMAT_CORE), "core");
    EXPECT_STREQ(cli::format_word(ECUIO_RECORDING_FORMAT_KEYS), "core")
        << "format 4 is core's too, each signal under its key";
    EXPECT_STREQ(cli::format_word(99), "unknown");

    ecuio_recording_signal_t s{};
    std::strcpy(s.name, "RPM");
    std::strcpy(s.key, "7E8:410C:2.0:16");
    std::strcpy(s.unit, "rpm");
    s.plot_rank = 1;
    s.group = ECUIO_SIGNAL_GROUP_ENGINE;
    s.samples = 2;
    s.lowest = 800;
    s.highest = 2512.5;
    s.first_us = 100;
    s.last_us = 200;
    cli::Line n(buf, sizeof buf);
    cli::signal_line(n, 0, 3, s);
    EXPECT_EQ(
        std::string(n.data(), n.size()),
        "{\"type\":\"signal\",\"recording\":0,\"index\":3,\"name\":\"RPM\","
        "\"key\":\"7E8:410C:2.0:16\","
        "\"unit\":\"rpm\",\"text\":false,\"decimals\":0,\"rank\":1,"
        "\"group\":\"Engine\",\"samples\":2,\"lowest\":800,"
        "\"highest\":2512.5,\"first_us\":100,\"last_us\":200,"
        "\"rate_hz\":10000}");
    ecuio_recording_signal_t none = s;
    none.samples = 0;
    none.first_us = ECUIO_RECORDING_NONE;
    none.last_us = ECUIO_RECORDING_NONE;
    cli::Line e(buf, sizeof buf);
    cli::signal_line(e, 0, 3, none);
    EXPECT_NE(std::string(e.data(), e.size())
                  .find("\"lowest\":null,\"highest\":null,\"first_us\":null,"
                        "\"last_us\":null,\"rate_hz\":null}"),
              std::string::npos);
    ecuio_recording_signal_t vin{};
    std::strcpy(vin.name, "VIN");
    vin.flags = ECUIO_RECORDING_SIGNAL_TEXT;
    vin.samples = 1;
    vin.group = 200; /* a group this core doesn't know: "" */
    cli::Line t(buf, sizeof buf);
    cli::signal_line(t, 0, 4, vin);
    EXPECT_NE(std::string(t.data(), t.size()).find("\"group\":\"\","),
              std::string::npos);
    EXPECT_EQ(std::string(t.data(), t.size()).find("lowest"), std::string::npos)
        << "a text signal has no range";
    EXPECT_NE(std::string(t.data(), t.size()).find("\"rate_hz\":null"),
              std::string::npos)
        << "one sample has no rate";

    /* An enum (format 3): "enum" and its labels after "text", a signal
     * file's "values" shape, then the rest as a number's; a label's
     * quotes escaped, as any string's */
    ecuio_recording_signal_t fuel{};
    std::strcpy(fuel.name, "Fuel");
    fuel.flags = ECUIO_RECORDING_SIGNAL_ENUM;
    fuel.samples = 2;
    fuel.lowest = 1;
    fuel.highest = 4;
    fuel.first_us = 100;
    fuel.last_us = 200;
    ecuio_enum_entry_t labels[2] = {};
    labels[0].raw = 1;
    std::strcpy(labels[0].label, "Open loop, cold");
    labels[1].raw = 4294967295u;
    std::strcpy(labels[1].label, "Say \"fault\"");
    cli::Line en(buf, sizeof buf);
    cli::signal_line(en, 0, 5, fuel, labels, 2);
    EXPECT_EQ(
        std::string(en.data(), en.size()),
        "{\"type\":\"signal\",\"recording\":0,\"index\":5,\"name\":\"Fuel\","
        "\"key\":null,"
        "\"unit\":\"\",\"text\":false,\"enum\":true,\"labels\":{\"1\":"
        "\"Open loop, cold\",\"4294967295\":\"Say \\\"fault\\\"\"},"
        "\"decimals\":0,\"rank\":0,\"group\":\"Other\",\"samples\":2,"
        "\"lowest\":1,\"highest\":4,\"first_us\":100,\"last_us\":200,"
        "\"rate_hz\":10000}");
    cli::Line bare(buf, sizeof buf);
    cli::signal_line(bare, 0, 5, fuel);
    EXPECT_NE(std::string(bare.data(), bare.size())
                  .find("\"enum\":true,\"labels\":{},\"decimals\""),
              std::string::npos)
        << "an enum with no labels has an empty object";
    cli::Line number(buf, sizeof buf);
    cli::signal_line(number, 0, 3, s);
    EXPECT_EQ(std::string(number.data(), number.size()).find("enum"),
              std::string::npos)
        << "a number's line is as it was";
    EXPECT_GE(cli::labels_bytes(256), 256u * (12 + 6 * 27))
        << "room for 256 labels, every byte escaped";

    cli::Line al(buf, sizeof buf);
    cli::align_line(al, 1, "rise:RPM:4000", true, 24600000, -24600000);
    EXPECT_EQ(std::string(al.data(), al.size()),
              "{\"type\":\"align\",\"recording\":1,\"on\":\"rise:RPM:4000\","
              "\"found\":true,\"host_us\":24600000,\"offset_us\":-24600000}");
    cli::Line nf(buf, sizeof buf);
    cli::align_line(nf, 0, "marker:pull", false, 0, 0);
    EXPECT_EQ(std::string(nf.data(), nf.size()),
              "{\"type\":\"align\",\"recording\":0,\"on\":\"marker:pull\","
              "\"found\":false,\"host_us\":null,\"offset_us\":null}");

    /* Columns from the third on: the others empty */
    cli::Window w;
    w.first = 2;
    w.count = 2;
    ecuio_recording_column_t cols[2] = {};
    cols[0].samples = 3;
    cols[0].lowest = 1;
    cols[0].highest = 2.5;
    const double means[4] = {NAN, NAN, 1.75, NAN};
    cli::Line c(buf, sizeof buf);
    cli::columns_line(c, 0, s, w, 4, cols, means);
    EXPECT_EQ(std::string(c.data(), c.size()),
              "{\"type\":\"columns\",\"recording\":0,\"signal\":\"RPM\","
              "\"key\":\"7E8:410C:2.0:16\","
              "\"samples\":[0,0,3,0],\"lowest\":[null,null,1,null],"
              "\"highest\":[null,null,2.5,null],"
              "\"mean\":[null,null,1.75,null]}");
    /* A later recording's line: its mean less the first's, by column */
    const double delta[4] = {NAN, NAN, -0.25, NAN};
    cli::Line cd(buf, sizeof buf);
    cli::columns_line(cd, 2, s, w, 4, cols, means, delta, 1);
    EXPECT_EQ(std::string(cd.data(), cd.size()),
              "{\"type\":\"columns\",\"recording\":2,\"signal\":\"RPM\","
              "\"key\":\"7E8:410C:2.0:16\","
              "\"samples\":[0,0,3,0],\"lowest\":[null,null,1,null],"
              "\"highest\":[null,null,2.5,null],"
              "\"mean\":[null,null,1.75,null],\"delta_from\":1,"
              "\"delta\":[null,null,-0.25,null]}");
    EXPECT_GE(cli::columns_line_bytes(10000), 10000u * 5 * 26)
        << "room for five entries a column";
    cli::Line ct(buf, sizeof buf);
    cli::columns_line(ct, 0, vin, w, 2, nullptr);
    EXPECT_EQ(
        std::string(ct.data(), ct.size()),
        "{\"type\":\"columns\",\"recording\":0,\"signal\":\"VIN\",\"key\":null,"
        "\"samples\":[0,0]}");

    cli::Line at(buf, sizeof buf);
    cli::at_begin(at, 1, -2000000);
    cli::at_value(at, true, s, 300, "2512.5");
    cli::at_value(at, false, vin, ECUIO_RECORDING_NONE, "");
    cli::at_end(at);
    EXPECT_EQ(
        std::string(at.data(), at.size()),
        "{\"type\":\"at\",\"recording\":1,\"t_us\":-2000000,\"values\":"
        "[{\"signal\":\"RPM\",\"key\":\"7E8:410C:2.0:16\",\"host_us\":300,"
        "\"text\":\"2512.5\"},"
        "{\"signal\":\"VIN\",\"key\":null,\"host_us\":null,\"text\":\"\"}]}");
}

TEST(CliInfoTest, RateDropIsAMultipleOverOne)
{
    uint32_t x100 = 0;
    ASSERT_TRUE(cli::parse_rate_drop("4", x100));
    EXPECT_EQ(x100, 400u);
    ASSERT_TRUE(cli::parse_rate_drop("1.5", x100));
    EXPECT_EQ(x100, 150u);
    ASSERT_TRUE(cli::parse_rate_drop("1.01", x100));
    EXPECT_EQ(x100, 101u) << "the check's least";
    for (const char* bad :
         {"1", "1.004", "0.5", "0", "-4", "x", "", "4x", "inf", "1e9"}) {
        EXPECT_FALSE(cli::parse_rate_drop(bad, x100)) << bad;
    }
}
