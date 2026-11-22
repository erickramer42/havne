// File/layer: tests/test_recording_stretches.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingStretchesTest (test_core)

#include "test_recording_support.h"

// ---- The navigation aids (ADR-0056's appendix "the navigation aids") -----

namespace {

/* A signal of a written drive: its name, unit, rank and value by ms */
struct Track {
    const char* name;
    const char* unit;
    unsigned rank;
    double (*at)(uint64_t ms);
};

/* Every track sampled each 100 ms from 0 to end_ms */
void write_drive(const Folder& f, const std::vector<Track>& tracks,
                 uint64_t end_ms)
{
    std::vector<std::string> lines;
    for (const Track& t : tracks) {
        lines.push_back(std::string(t.name) + ",number," + t.unit + ",1," +
                        std::to_string(t.rank));
    }
    f.write("events.log", events_v2(lines));
    std::string csv = std::string(kBom) + rows::kSignalsHeader + "\n";
    for (uint64_t ms = 0; ms <= end_ms; ms += 100) {
        for (const Track& t : tracks) {
            csv += std::to_string(ms * 1000) + "," + t.name + ",";
            rows::append_value(csv, t.at(ms));
            csv += std::string(",") + t.unit + "\n";
        }
    }
    f.write("signals.csv", csv);
}

/* A pull from 5 s (40 to 110 km/h by 10 s, then the limiter's flat speed
 * to 12 s) and a cruise from 20 s to 45 s at 100 km/h, 30 km/h between */
double drive_speed(uint64_t ms)
{
    if (ms < 5000) {
        return 40.0;
    }
    if (ms < 10000) {
        return 40.0 + static_cast<double>(ms - 5000) * 0.014;
    }
    if (ms < 12000) {
        return 110.0;
    }
    if (ms >= 20000 && ms < 45000) {
        return (ms / 100) % 2 == 0 ? 99.6 : 100.4;
    }
    return 30.0;
}

double drive_throttle(uint64_t ms)
{
    if (ms >= 5000 && ms < 12000) {
        return 95.0;
    }
    if (ms >= 20000 && ms < 45000) {
        return (ms / 100) % 2 == 0 ? 19.0 : 21.0;
    }
    return 10.0;
}

double drive_rpm(uint64_t ms)
{
    return 1000.0 + drive_speed(ms) * 40.0;
}

/* Both kinds, the pull ending at top speed, which the drive fixture's
 * expectations below assume (AFlagClearEndsAPullAtTheLiftAndTheFlagAtTopSpeed
 * reads both ends) */
ecuio_stretch_rules_t both_kinds()
{
    ecuio_stretch_rules_t r{};
    r.kinds = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    r.flags = ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED;
    r.throttle = UINT32_MAX;
    r.engine_load = UINT32_MAX;
    r.vehicle_speed = UINT32_MAX;
    r.engine_speed = UINT32_MAX;
    return r;
}

std::vector<ecuio_recording_stretch_t>
stretches_of(const Reader& r, const ecuio_stretch_rules_t& rules, uint64_t from,
             uint64_t to)
{
    uint32_t n = 99;
    EXPECT_EQ(ecuio_recording_stretches(r.r, &rules, from, to, nullptr, 0, &n),
              ECUIO_OK);
    std::vector<ecuio_recording_stretch_t> out(n);
    uint32_t again = 0;
    EXPECT_EQ(
        ecuio_recording_stretches(r.r, &rules, from, to, out.data(), n, &again),
        ECUIO_OK);
    EXPECT_EQ(again, n);
    return out;
}

uint32_t role_of(const Reader& r, uint32_t role)
{
    uint32_t at = 7;
    EXPECT_EQ(ecuio_recording_role_signal(r.r, role, &at), ECUIO_OK);
    return at;
}

/* A road log's shape: every signal read in one poll loop at 3 to 4 Hz,
 * its steps uneven. The instants the cases measure from are sampled
 * exactly, the loop's own sample within 100 ms of one left out. */
const uint64_t kLogInstants[] = {12000, 13300, 20000, 24000, 24925, 28000,
                                 34000, 37000, 38400, 41000, 43200, 47000};

std::vector<uint64_t> log_times(uint64_t end_ms)
{
    static const uint64_t kSteps[] = {250, 330, 290, 310, 270};
    std::vector<uint64_t> out(std::begin(kLogInstants), std::end(kLogInstants));
    size_t n = 0;
    for (uint64_t ms = 0; ms <= end_ms; ms += kSteps[n++ % 5]) {
        bool near = false;
        for (uint64_t at : kLogInstants) {
            near = near || (ms > at ? ms - at : at - ms) < 100;
        }
        if (!near) {
            out.push_back(ms);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void write_log(const Folder& f, const std::vector<Track>& tracks,
               uint64_t end_ms)
{
    std::vector<std::string> lines;
    for (const Track& t : tracks) {
        lines.push_back(std::string(t.name) + ",number," + t.unit + ",1," +
                        std::to_string(t.rank));
    }
    f.write("events.log", events_v2(lines));
    std::string csv = std::string(kBom) + rows::kSignalsHeader + "\n";
    for (uint64_t ms : log_times(end_ms)) {
        for (const Track& t : tracks) {
            csv += std::to_string(ms * 1000) + "," + t.name + ",";
            rows::append_value(csv, t.at(ms));
            csv += std::string(",") + t.unit + "\n";
        }
    }
    f.write("signals.csv", csv);
}

/* The value at ms on the line through knots, held past its ends */
double along(const std::vector<std::pair<uint64_t, double>>& knots, uint64_t ms)
{
    if (ms <= knots.front().first) {
        return knots.front().second;
    }
    for (size_t i = 1; i < knots.size(); ++i) {
        if (ms <= knots[i].first) {
            const double span =
                static_cast<double>(knots[i].first - knots[i - 1].first);
            return knots[i - 1].second +
                   (knots[i].second - knots[i - 1].second) *
                       static_cast<double>(ms - knots[i - 1].first) / span;
        }
    }
    return knots.back().second;
}

/* The log's drive. Part throttle through 3000 rpm from 4 s to 9 s; a
 * stab at the throttle from 12 s to 13.3 s (+8 km/h, +180 rpm); a pull
 * through an upshift from 20 s to 28 s, the lift between its gears 925 ms
 * from its first sample to the next at the level, engine speed ending
 * under where it began; a pull from 34 s whose lift is 1.4 s; and 2.2 s
 * after that one's throttle closes, a pull in top gear from 43.2 s to
 * 47 s (+15 km/h, +2000 rpm). */
double log_throttle(uint64_t ms)
{
    if (ms >= 12000 && ms <= 13300) {
        return 90.0;
    }
    if ((ms >= 20000 && ms < 24000) || (ms >= 24925 && ms <= 28000) ||
        (ms >= 34000 && ms < 37000) || (ms >= 38400 && ms < 41000) ||
        (ms >= 43200 && ms <= 47000)) {
        return 95.0;
    }
    if (ms >= 4000 && ms < 9000) {
        return 45.0;
    }
    return (ms >= 24000 && ms < 24925) || (ms >= 37000 && ms < 38400) ? 20.0
                                                                      : 12.0;
}

double log_speed(uint64_t ms)
{
    return along({{4000, 50.0},
                  {9000, 65.0},
                  {12000, 65.0},
                  {13300, 73.0},
                  {20000, 90.0},
                  {24000, 112.0},
                  {24925, 114.0},
                  {28000, 130.0},
                  {34000, 80.0},
                  {37000, 100.0},
                  {38400, 101.0},
                  {41000, 118.0},
                  {43200, 120.0},
                  {47000, 135.0}},
                 ms);
}

double log_rpm(uint64_t ms)
{
    return along({{4000, 2400.0},
                  {9000, 3300.0},
                  {12000, 2500.0},
                  {13300, 2680.0},
                  {19000, 2700.0},
                  {20000, 5500.0},
                  {24000, 6500.0},
                  {24925, 4600.0},
                  {28000, 5200.0},
                  {34000, 3000.0},
                  {37000, 5800.0},
                  {38400, 4000.0},
                  {41000, 5200.0},
                  {43200, 4300.0},
                  {47000, 6300.0}},
                 ms);
}

const uint64_t kLogEndMs = 52000;

ecuio_stretch_rules_t pulls_only()
{
    ecuio_stretch_rules_t r = both_kinds();
    r.kinds = ECUIO_STRETCH_PULLS;
    return r;
}

} // namespace

TEST(RecordingStretchesTest, EachRoleIsPickedByItsUnitAndName)
{
    Folder f;
    f.write(
        "events.log",
        events_v2({"Commanded Throttle,number,%,1,0",
                   "Relative Throttle Position,number,%,1,0",
                   "Throttle Position,number,%,1,0",
                   "Accelerator Pedal D,number,%,1,3",
                   "Engine Load,number,%,1,0", "Wheel Speed,number,MPH,1,0",
                   "Engine Speed,number,RPM,0,0", "Speed Note,text,km/h,0,0"}));
    f.write("signals.csv", std::string(kBom) + rows::kSignalsHeader +
                               "\n"
                               "1000,Commanded Throttle,1,%\n"
                               "1000,Relative Throttle Position,1,%\n"
                               "1000,Throttle Position,1,%\n"
                               "1000,Accelerator Pedal D,1,%\n"
                               "1000,Engine Load,1,%\n"
                               "1000,Wheel Speed,1,MPH\n"
                               "1000,Engine Speed,800,RPM\n"
                               "1000,Speed Note,x,km/h\n");
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_THROTTLE),
              r.index_of("Accelerator Pedal D"))
        << "the ranked one, neither relative nor commanded";
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_ENGINE_LOAD),
              r.index_of("Engine Load"));
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_VEHICLE_SPEED),
              r.index_of("Wheel Speed"))
        << "mph in any case, never a text signal";
    EXPECT_EQ(role_of(r, ECUIO_RECORDING_ROLE_ENGINE_SPEED),
              r.index_of("Engine Speed"));

    uint32_t at = 0;
    EXPECT_EQ(ecuio_recording_role_signal(r.r, 0, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_role_signal(r.r, 5, &at),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recording_role_signal(nullptr, 1, &at),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_role_signal(r.r, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    Folder none;
    write_span_folder(none);
    Reader n(none.utf8());
    ASSERT_EQ(n.rc, ECUIO_OK);
    EXPECT_EQ(role_of(n, ECUIO_RECORDING_ROLE_THROTTLE), UINT32_MAX);
    EXPECT_EQ(role_of(n, ECUIO_RECORDING_ROLE_VEHICLE_SPEED), UINT32_MAX);
}

TEST(RecordingStretchesTest, ADrivesPullAndCruiseAreFoundInTimeOrder)
{
    Folder f;
    write_drive(f,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t speed = r.index_of("Vehicle Speed");
    const std::vector<ecuio_recording_stretch_t> all =
        stretches_of(r, both_kinds(), 0, 60000000);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_EQ(all[0].signal, speed);
    EXPECT_EQ(all[0].from_us, 5000000u);
    EXPECT_EQ(all[0].to_us, 10000000u) << "the limiter's flat speed ends it";
    EXPECT_EQ(all[0].start, 40.0);
    EXPECT_EQ(all[0].end, 110.0);
    EXPECT_EQ(all[0].flags, 0u);
    EXPECT_EQ(all[1].kind, ECUIO_RECORDING_STRETCH_CRUISE);
    EXPECT_EQ(all[1].signal, speed);
    EXPECT_EQ(all[1].from_us, 20000000u);
    EXPECT_EQ(all[1].to_us, 44900000u);
    EXPECT_NEAR(all[1].start, 100.0, 1e-9);
    EXPECT_EQ(all[1].start, all[1].end);
    EXPECT_EQ(all[1].flags, 0u);
    EXPECT_EQ(all[1].reserved0, 0u);
    EXPECT_EQ(all[1].reserved[1], 0u);

    /* One kind at a time */
    ecuio_stretch_rules_t pulls = both_kinds();
    pulls.kinds = ECUIO_STRETCH_PULLS;
    EXPECT_EQ(stretches_of(r, pulls, 0, 60000000).size(), 1u);

    /* A range cuts each stretch reaching past it and measures it again */
    const std::vector<ecuio_recording_stretch_t> cut =
        stretches_of(r, both_kinds(), 7000000, 30000000);
    ASSERT_EQ(cut.size(), 2u);
    EXPECT_EQ(cut[0].from_us, 7000000u);
    EXPECT_EQ(cut[0].flags, ECUIO_RECORDING_STRETCH_CUT_START);
    EXPECT_EQ(cut[0].start, 68.0);
    EXPECT_EQ(cut[1].to_us, 30000000u);
    EXPECT_EQ(cut[1].flags, ECUIO_RECORDING_STRETCH_CUT_END);
    EXPECT_NEAR(cut[1].start, 100.0, 0.01);

    /* The capacity copies the first; the count is all */
    ecuio_recording_stretch_t one{};
    uint32_t n = 0;
    ecuio_stretch_rules_t rules = both_kinds();
    ASSERT_EQ(ecuio_recording_stretches(r.r, &rules, 0, 60000000, &one, 1, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(one.kind, ECUIO_RECORDING_STRETCH_PULL);

    /* A level the user sets: 96 % finds no pull */
    rules.pull_level = 96.0;
    EXPECT_EQ(stretches_of(r, rules, 0, 60000000).size(), 1u);
}

/* Where a pull ends: the drive's pull runs onto the limiter, its speed flat
 * from 10 s while the throttle stays down to 12 s */
TEST(RecordingStretchesTest, AFlagClearEndsAPullAtTheLiftAndTheFlagAtTopSpeed)
{
    Folder f;
    write_drive(f,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_stretch_rules_t lift = both_kinds();
    lift.kinds = ECUIO_STRETCH_PULLS;
    lift.flags = 0;
    ecuio_stretch_rules_t top = lift;
    top.flags = ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED;

    const auto at_lift = stretches_of(r, lift, 0, 60000000);
    ASSERT_EQ(at_lift.size(), 1u);
    EXPECT_EQ(at_lift[0].from_us, 5000000u);
    EXPECT_EQ(at_lift[0].to_us, 11900000u) << "the last sample at the level";
    EXPECT_EQ(at_lift[0].start, 40.0);
    EXPECT_EQ(at_lift[0].end, 110.0);
    const auto at_top = stretches_of(r, top, 0, 60000000);
    ASSERT_EQ(at_top.size(), 1u);
    EXPECT_EQ(at_top[0].from_us, 5000000u);
    EXPECT_EQ(at_top[0].to_us, 10000000u) << "the first sample at the top";

    /* A range ending inside the limiter cuts either end there and says so */
    const auto cut_lift = stretches_of(r, lift, 0, 11000000);
    ASSERT_EQ(cut_lift.size(), 1u);
    EXPECT_EQ(cut_lift[0].to_us, 11000000u);
    EXPECT_EQ(cut_lift[0].flags, ECUIO_RECORDING_STRETCH_CUT_END);
    const auto cut_top = stretches_of(r, top, 0, 11000000);
    ASSERT_EQ(cut_top.size(), 1u);
    EXPECT_EQ(cut_top[0].to_us, 10000000u);
    EXPECT_EQ(cut_top[0].flags, 0u) << "inside the range: not cut";

    /* A recording with no speed signal has only the lift to end a pull */
    Folder g;
    write_drive(g,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader q(g.utf8());
    ASSERT_EQ(q.rc, ECUIO_OK) << ecuio_last_error_detail();
    const auto by_rpm_lift = stretches_of(q, lift, 0, 60000000);
    const auto by_rpm_top = stretches_of(q, top, 0, 60000000);
    ASSERT_EQ(by_rpm_lift.size(), 1u);
    ASSERT_EQ(by_rpm_top.size(), 1u);
    EXPECT_EQ(by_rpm_lift[0].to_us, by_rpm_top[0].to_us);
    EXPECT_EQ(by_rpm_lift[0].flags, ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
}

namespace {

/* A cancel that stops at its nth ask (0: never), counting its asks */
struct StopAt {
    int at = 0;
    int asked = 0;
    static int ask(void* user)
    {
        auto* s = static_cast<StopAt*>(user);
        return ++s->asked == s->at ? 1 : 0;
    }
};

} // namespace

/* stretches_with asks its cancel once the arguments pass and before each
 * second's rows: never stopping gives the plain call's stretches, and a
 * stop at each ask in turn ends the call there, CANCELED with a count of
 * 0 and the detail's words, asking no more */
TEST(RecordingStretchesTest, ACancelStopsTheSearchAtItsNextSecond)
{
    Folder f;
    write_drive(f,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_stretch_rules_t rules = both_kinds();
    const std::vector<ecuio_recording_stretch_t> plain =
        stretches_of(r, rules, 0, 60000000);
    ASSERT_EQ(plain.size(), 2u);

    StopAt never;
    ecuio_recording_stretch_t got[4] = {};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_recording_stretches_with(r.r, &rules, 0, 60000000,
                                             StopAt::ask, &never, got, 4, &n),
              ECUIO_OK);
    ASSERT_EQ(n, 2u);
    for (uint32_t k = 0; k < n; ++k) {
        EXPECT_EQ(got[k].from_us, plain[k].from_us);
        EXPECT_EQ(got[k].to_us, plain[k].to_us);
    }
    EXPECT_EQ(never.asked, 62) << "once, then each of the 61 seconds";

    for (int at = 1; at <= never.asked; ++at) {
        StopAt stop{at};
        n = 99;
        ASSERT_EQ(ecuio_recording_stretches_with(
                      r.r, &rules, 0, 60000000, StopAt::ask, &stop, got, 4, &n),
                  ECUIO_ERR_CANCELED)
            << "at " << at;
        EXPECT_EQ(n, 0u);
        EXPECT_EQ(stop.asked, at) << "asked again after its stop";
        EXPECT_STREQ(ecuio_last_error_detail(), "the search was canceled");
    }

    /* A refusal comes before any ask; a NULL cancel is the plain call */
    StopAt refused;
    ecuio_stretch_rules_t none = rules;
    none.kinds = 0;
    EXPECT_EQ(ecuio_recording_stretches_with(r.r, &none, 0, 60000000,
                                             StopAt::ask, &refused, got, 4, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(refused.asked, 0);
    ASSERT_EQ(ecuio_recording_stretches_with(r.r, &rules, 0, 60000000, nullptr,
                                             nullptr, got, 4, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 2u);
}

/* find_with asks its cancel once the arguments pass and before each
 * second whose rows it reads, never one its summary passes by: a stop at
 * each ask ends the search CANCELED with nothing found */
TEST(RecordingStretchesTest, ACancelStopsAFindAtItsNextSecond)
{
    Folder f;
    write_drive(f,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_find_t rising = threshold(
        r.index_of("Vehicle Speed"), 100, 0, ECUIO_RECORDING_FIND_RISING);
    const uint64_t plain = r.find(rising, 0);
    ASSERT_EQ(plain, 9300000u);

    StopAt never;
    uint64_t at = 0;
    ASSERT_EQ(
        ecuio_recording_find_with(r.r, &rising, 0, StopAt::ask, &never, &at),
        ECUIO_OK);
    EXPECT_EQ(at, plain);
    EXPECT_EQ(never.asked, 3)
        << "once, then second 0 that arms it and second 9 that fires";
    for (int k = 1; k <= never.asked; ++k) {
        StopAt stop{k};
        at = 0;
        EXPECT_EQ(
            ecuio_recording_find_with(r.r, &rising, 0, StopAt::ask, &stop, &at),
            ECUIO_ERR_CANCELED);
        EXPECT_EQ(at, ECUIO_RECORDING_NONE);
        EXPECT_EQ(stop.asked, k);
        EXPECT_STREQ(ecuio_last_error_detail(), "the search was canceled");
    }

    /* An engine start and a marker ask once before they look */
    ecuio_recording_find_t start{};
    start.kind = ECUIO_RECORDING_FIND_ENGINE_START;
    ecuio_recording_find_t marker{};
    marker.kind = ECUIO_RECORDING_FIND_MARKER;
    marker.label_utf8 = "lap";
    for (const ecuio_recording_find_t* what : {&start, &marker}) {
        StopAt once;
        EXPECT_EQ(
            ecuio_recording_find_with(r.r, what, 0, StopAt::ask, &once, &at),
            ECUIO_OK);
        EXPECT_GE(once.asked, 1);
        StopAt first{1};
        EXPECT_EQ(
            ecuio_recording_find_with(r.r, what, 0, StopAt::ask, &first, &at),
            ECUIO_ERR_CANCELED);
        EXPECT_EQ(at, ECUIO_RECORDING_NONE);
        EXPECT_EQ(first.asked, 1);
    }

    /* A refusal comes before any ask; a NULL cancel is the plain call */
    StopAt refused;
    ecuio_recording_find_t bad = rising;
    bad.flags = 1;
    EXPECT_EQ(
        ecuio_recording_find_with(r.r, &bad, 0, StopAt::ask, &refused, &at),
        ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(refused.asked, 0);
    ASSERT_EQ(ecuio_recording_find_with(r.r, &rising, 0, nullptr, nullptr, &at),
              ECUIO_OK);
    EXPECT_EQ(at, plain);
}

TEST(RecordingStretchesTest, TheFallbacksAreFlagged)
{
    Folder load;
    write_drive(load,
                {{"Engine Load", "%", 0, drive_throttle},
                 {"Vehicle Speed", "km/h", 0, drive_speed}},
                60000);
    Reader l(load.utf8());
    ASSERT_EQ(l.rc, ECUIO_OK);
    const std::vector<ecuio_recording_stretch_t> by_load =
        stretches_of(l, both_kinds(), 0, 60000000);
    ASSERT_EQ(by_load.size(), 2u);
    EXPECT_EQ(by_load[0].flags, ECUIO_RECORDING_STRETCH_BY_LOAD);
    EXPECT_EQ(by_load[1].flags, ECUIO_RECORDING_STRETCH_NO_THROTTLE);

    Folder rpm;
    write_drive(rpm,
                {{"Throttle Position", "%", 0, drive_throttle},
                 {"RPM", "rpm", 0, drive_rpm}},
                60000);
    Reader e(rpm.utf8());
    ASSERT_EQ(e.rc, ECUIO_OK);
    const std::vector<ecuio_recording_stretch_t> by_rpm =
        stretches_of(e, both_kinds(), 0, 60000000);
    ASSERT_EQ(by_rpm.size(), 1u) << "no speed: no cruise";
    EXPECT_EQ(by_rpm[0].flags, ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
    EXPECT_EQ(by_rpm[0].signal, e.index_of("RPM"));
    EXPECT_EQ(by_rpm[0].to_us, 11900000u) << "engine speed keeps the whole run";
}

TEST(RecordingStretchesTest, TheFloorIsThirtyFiveMphInEitherUnit)
{
    /* 1 mph is 1.609344 km/h exactly, so the default is 35 mph to the bit */
    ecuio_display_value_t shown{};
    ASSERT_EQ(ecuio_display_convert("km/h", 2,
                                    ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR,
                                    ECUIO_UNITS_IMPERIAL, &shown),
              ECUIO_DISPLAY_OK);
    EXPECT_NEAR(shown.value, 35.0, 1e-12);
    EXPECT_STREQ(shown.unit, "mph");

    struct Case {
        const char* unit;
        double (*speed)(uint64_t);
        bool found;
    };
    const Case cases[] = {
        {"km/h", [](uint64_t) { return 50.0; }, false},
        {"km/h", [](uint64_t) { return 60.0; }, true},
        {"mph", [](uint64_t) { return 30.0; }, false},
        {"mph", [](uint64_t) { return 40.0; }, true},
    };
    for (const Case& c : cases) {
        Folder f;
        write_drive(f, {{"Vehicle Speed", c.unit, 0, c.speed}}, 15000);
        Reader r(f.utf8());
        ASSERT_EQ(r.rc, ECUIO_OK);
        EXPECT_EQ(stretches_of(r, both_kinds(), 0, 15000000).size(),
                  c.found ? 1u : 0u)
            << c.speed(0) << " " << c.unit;
    }
}

TEST(RecordingStretchesTest, StretchesRefuseRulesTheyCantRun)
{
    Folder f;
    write_span_folder(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK);
    uint32_t n = 0;
    ecuio_recording_stretch_t one{};
    const ecuio_stretch_rules_t good = both_kinds();
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 0, 1, nullptr, 0, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 0u) << "no throttle, load or speed: nothing found";
    EXPECT_EQ(ecuio_recording_stretches(nullptr, &good, 0, 1, nullptr, 0, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, nullptr, 0, 1, nullptr, 0, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 0, 1, nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 0, 1, nullptr, 1, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_stretches(r.r, &good, 5, 5, &one, 1, &n),
              ECUIO_ERR_INVALID_ARGUMENT);

    const auto refused = [&](void (*spoil)(ecuio_stretch_rules_t&),
                             const char* why) {
        ecuio_stretch_rules_t bad = both_kinds();
        spoil(bad);
        EXPECT_EQ(ecuio_recording_stretches(r.r, &bad, 0, 1, &one, 1, &n),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << why;
    };
    refused([](ecuio_stretch_rules_t& b) { b.kinds = 0; }, "no kind");
    refused([](ecuio_stretch_rules_t& b) { b.kinds = 0x4; }, "unknown kind");
    refused([](ecuio_stretch_rules_t& b) { b.flags = 0x2; }, "an unknown flag");
    refused([](ecuio_stretch_rules_t& b) { b.flags |= 0x80000000u; },
            "an unknown flag beside the known one");
    refused([](ecuio_stretch_rules_t& b) { b.reserved[0] = 1; }, "reserved");
    refused([](ecuio_stretch_rules_t& b) { b.reserved[1] = 1; }, "reserved");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_rise = -1; },
            "a negative rise");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_rise = NAN; },
            "a NaN rise");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_gain = -0.5; },
            "a negative gain");
    refused([](ecuio_stretch_rules_t& b) { b.pull_min_gain = NAN; },
            "a NaN gain");
    refused([](ecuio_stretch_rules_t& b) { b.pull_level = -1; }, "negative");
    refused([](ecuio_stretch_rules_t& b) { b.cruise_speed_band = NAN; }, "NaN");
    refused([](ecuio_stretch_rules_t& b) { b.cruise_floor = -0.5; },
            "negative floor");
    refused([](ecuio_stretch_rules_t& b) { b.vehicle_speed = 99; },
            "an index past the signals");
    ecuio_stretch_rules_t text = both_kinds();
    text.throttle = r.index_of("VIN");
    EXPECT_EQ(ecuio_recording_stretches(r.r, &text, 0, 1, &one, 1, &n),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a text signal in a role";
}

TEST(RecordingStretchesTest, ARoadLogsPullsAreFoundAndItsStabIsnt)
{
    Folder f;
    write_log(f,
              {{"Throttle Position", "%", 0, log_throttle},
               {"Vehicle Speed", "km/h", 0, log_speed},
               {"RPM", "rpm", 0, log_rpm}},
              kLogEndMs);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint64_t end = kLogEndMs * 1000 + 1;
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, pulls_only(), 0, end);
    ASSERT_EQ(found.size(), 3u)
        << "neither the part throttle nor the stab is a pull";

    /* The 925 ms lift between two gears stays inside its pull */
    EXPECT_EQ(found[0].from_us, 20000000u);
    EXPECT_EQ(found[0].to_us, 28000000u);
    EXPECT_EQ(found[0].start, 90.0);
    EXPECT_EQ(found[0].end, 130.0);
    EXPECT_EQ(found[0].flags, 0u);

    /* So does a 1.4 s one; the pull ends at the last sample before its
     * throttle closes at 41 s */
    EXPECT_EQ(found[1].from_us, 34000000u);
    EXPECT_GE(found[1].to_us, 40600000u);
    EXPECT_LT(found[1].to_us, 41000000u);

    /* 2.2 s on, the next pull is its own: in top gear, 15 km/h gained */
    EXPECT_EQ(found[2].from_us, 43200000u);
    EXPECT_EQ(found[2].to_us, 47000000u);
    EXPECT_EQ(found[2].start, 120.0);
    EXPECT_EQ(found[2].end, 135.0);

    /* Where the gap's default sits: a second splits the 1.4 s lift, and
     * 2.5 s joins the two pulls 2.2 s apart */
    ecuio_stretch_rules_t rules = pulls_only();
    rules.pull_gap_us = 1000000;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 4u);
    rules.pull_gap_us = 2500000;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 2u);
    rules.pull_gap_us = ECUIO_STRETCH_DEFAULT_PULL_GAP_US;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* The rise the user sets: 150 rpm lets the stab in (+180), 2500 keeps
     * only the pull that climbs 2800 */
    rules = pulls_only();
    rules.pull_min_rise = 150.0;
    const std::vector<ecuio_recording_stretch_t> with_stab =
        stretches_of(r, rules, 0, end);
    ASSERT_EQ(with_stab.size(), 4u);
    EXPECT_EQ(with_stab[0].from_us, 12000000u);
    EXPECT_EQ(with_stab[0].to_us, 13300000u);
    EXPECT_EQ(with_stab[0].end - with_stab[0].start, 8.0);
    rules.pull_min_rise = 2500.0;
    const std::vector<ecuio_recording_stretch_t> steep =
        stretches_of(r, rules, 0, end);
    ASSERT_EQ(steep.size(), 1u);
    EXPECT_EQ(steep[0].from_us, 34000000u);
    rules.pull_min_rise = ECUIO_STRETCH_DEFAULT_PULL_RISE;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* With engine speed in the log, the gain is no floor at all: one past
     * the top-gear pull's 15 km/h drops nothing */
    rules = pulls_only();
    rules.pull_min_gain = 16.0;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* A range cutting the top-gear pull at 45.95 s leaves over its hold
     * and under 730 rpm to climb, counting from the sample in force there:
     * what is left is no pull; from 45.2 s, over 900 rpm, it is */
    EXPECT_EQ(stretches_of(r, pulls_only(), 45950000, end).size(), 0u);
    const std::vector<ecuio_recording_stretch_t> cut =
        stretches_of(r, pulls_only(), 45200000, end);
    ASSERT_EQ(cut.size(), 1u);
    EXPECT_EQ(cut[0].flags, ECUIO_RECORDING_STRETCH_CUT_START);
}

TEST(RecordingStretchesTest, WithNoEngineSpeedAPullGainsItsSpeed)
{
    Folder f;
    write_log(f,
              {{"Throttle Position", "%", 0, log_throttle},
               {"Vehicle Speed", "km/h", 0, log_speed}},
              kLogEndMs);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint64_t end = kLogEndMs * 1000 + 1;
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, pulls_only(), 0, end);
    ASSERT_EQ(found.size(), 3u) << "the stab's 8 km/h is under the gain";
    EXPECT_EQ(found[0].from_us, 20000000u);
    EXPECT_EQ(found[2].from_us, 43200000u);

    /* The gain the user sets: 5 km/h lets the stab in, 16 drops the pull
     * in top gear, which is why engine speed's climb is the floor
     * wherever the log has it */
    ecuio_stretch_rules_t rules = pulls_only();
    rules.pull_min_gain = 5.0;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 4u);
    rules.pull_min_gain = 16.0;
    const std::vector<ecuio_recording_stretch_t> fast =
        stretches_of(r, rules, 0, end);
    ASSERT_EQ(fast.size(), 2u);
    EXPECT_EQ(fast[1].from_us, 34000000u);
    rules.pull_min_gain = ECUIO_STRETCH_DEFAULT_PULL_GAIN;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* The rise is engine speed's: with none it changes nothing */
    rules = pulls_only();
    rules.pull_min_rise = 1e6;
    EXPECT_EQ(stretches_of(r, rules, 0, end).size(), 3u);

    /* The gain is in km/h: the same numbers in mph make the stab's 8 mph
     * 12.9 km/h, past it */
    Folder m;
    write_log(m,
              {{"Throttle Position", "%", 0, log_throttle},
               {"Vehicle Speed", "mph", 0, log_speed}},
              kLogEndMs);
    Reader mph(m.utf8());
    ASSERT_EQ(mph.rc, ECUIO_OK);
    EXPECT_EQ(stretches_of(mph, pulls_only(), 0, end).size(), 4u);
    rules = pulls_only();
    rules.pull_min_gain = 13.0;
    EXPECT_EQ(stretches_of(mph, rules, 0, end).size(), 3u);
}

TEST(RecordingStretchesTest, APullThroughAnUpshiftCountsByEngineSpeedAlone)
{
    Folder f;
    write_log(f,
              {{"Throttle Position", "%", 0, log_throttle},
               {"RPM", "rpm", 0, log_rpm}},
              kLogEndMs);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, pulls_only(), 0, kLogEndMs * 1000 + 1);
    ASSERT_EQ(found.size(), 3u) << "the stab climbs 180 rpm";
    for (const ecuio_recording_stretch_t& s : found) {
        EXPECT_EQ(s.flags, ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED);
    }
    EXPECT_EQ(found[0].from_us, 20000000u);
    EXPECT_EQ(found[0].to_us, 28000000u);
    EXPECT_EQ(found[0].start, 5500.0);
    EXPECT_EQ(found[0].end, 5200.0)
        << "it ends under where it began, having climbed 1000 rpm";
}

TEST(RecordingStretchesTest, TheMockDriveCyclesPullsAndCruiseAreFound)
{
    /* One loop of the mock's drive cycle, each tick's frame decoded as a
     * recording of the mock holds it (whole km/h, the throttle's bytes) */
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &set), ECUIO_OK);
    const uint16_t wanted[] = {ECUIO_MOCK_SIG_RPM_ID, ECUIO_MOCK_SIG_SPEED_ID,
                               ECUIO_MOCK_SIG_THROTTLE_ID,
                               ECUIO_MOCK_SIG_LOAD_ID};
    std::vector<const ecuio_signal_def_t*> defs;
    std::vector<std::string> lines;
    for (uint16_t id : wanted) {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].id == id) {
                defs.push_back(&set->defs[i]);
                lines.push_back(std::string(set->defs[i].name) + ",number," +
                                set->defs[i].unit + ",1,0");
            }
        }
    }
    ASSERT_EQ(defs.size(), 4u);
    Folder f;
    f.write("events.log", events_v2(lines));
    std::string csv = std::string(kBom) + rows::kSignalsHeader + "\n";
    std::map<int, std::pair<uint64_t, uint64_t>> phases;
    ecuio_mock::DriveCycle dc;
    while (dc.loops < 1) {
        dc.step();
        const uint64_t us = dc.tick * 50000;
        auto& span = phases[static_cast<int>(dc.phase)];
        if (span.first == 0) {
            span.first = us;
        }
        span.second = us;
        uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN];
        ecuio_mock::encode_payload(dc, payload);
        for (const ecuio_signal_def_t* d : defs) {
            double v = 0;
            ASSERT_EQ(ecuio_decode_signal(d, payload,
                                          ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &v),
                      0);
            csv += std::to_string(us) + "," + d->name + ",";
            rows::append_value(csv, v);
            csv += std::string(",") + d->unit + "\n";
        }
    }
    f.write("signals.csv", csv);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const std::vector<ecuio_recording_stretch_t> found =
        stretches_of(r, both_kinds(), 0, r.info().duration_us + 1);
    using ecuio_mock::Phase;
    const auto span = [&](Phase p) { return phases[static_cast<int>(p)]; };
    const auto within = [](const ecuio_recording_stretch_t& s, uint64_t lo,
                           uint64_t hi) {
        return s.from_us >= lo && s.to_us <= hi;
    };
    ASSERT_EQ(found.size(), 4u);
    for (const ecuio_recording_stretch_t& s : found) {
        EXPECT_EQ(s.flags, 0u);
    }

    /* Each gear's pull ends where the limiter's flat speed begins: in
     * whole km/h, up to a tick before rpm meets the limit */
    const uint64_t tick = 50000;
    EXPECT_EQ(found[0].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(found[0], span(Phase::Pull2nd).first,
                       span(Phase::Limiter2nd).first));
    EXPECT_GE(found[0].to_us + tick, span(Phase::Pull2nd).second);
    EXPECT_EQ(found[1].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(found[1], span(Phase::Pull3rd).first,
                       span(Phase::Limiter3rd).first));
    EXPECT_GE(found[1].to_us + tick, span(Phase::Pull3rd).second);
    EXPECT_GT(found[1].end, found[0].end) << "third gear runs faster";

    /* The cruise in fourth, at its 100 km/h */
    EXPECT_EQ(found[2].kind, ECUIO_RECORDING_STRETCH_CRUISE);
    EXPECT_TRUE(within(found[2], span(Phase::Cruise4th).first,
                       span(Phase::Cruise4th).second + 100000));
    EXPECT_GE(found[2].to_us - found[2].from_us,
              ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US);
    EXPECT_NEAR(found[2].start, 100.0, 2.0);

    /* The quarter mile is one pull once the clutch locks (the throttle is
     * feathered under the level while it slips): every upshift's lift
     * (about 0.55 s under the level) inside the gap. It ends at its top
     * speed, which can be read a tick past the line, the pedal still down */
    EXPECT_EQ(found[3].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(found[3], span(Phase::DragRun).first,
                       span(Phase::DragRun).second + tick));
    EXPECT_GT(found[3].to_us - found[3].from_us, 10000000u);

    /* The two-step's launch at a standstill is no pull: a full-throttle
     * run of its own, 1.7 s before the quarter mile's, creeping 2 km/h
     * while engine speed climbs some 540 rpm onto the two-step. A rise of
     * 400 rpm lets it in. */
    ecuio_stretch_rules_t low_rise = both_kinds();
    low_rise.pull_min_rise = 400.0;
    const std::vector<ecuio_recording_stretch_t> with_launch =
        stretches_of(r, low_rise, 0, r.info().duration_us + 1);
    ASSERT_EQ(with_launch.size(), 5u);
    EXPECT_EQ(with_launch[3].kind, ECUIO_RECORDING_STRETCH_PULL);
    EXPECT_TRUE(within(with_launch[3], span(Phase::Launch).first,
                       span(Phase::DragRun).first));
    EXPECT_LT(with_launch[3].end - with_launch[3].start, 5.0);

    /* A gap of 500 ms splits the quarter mile at each of its three
     * upshifts */
    ecuio_stretch_rules_t short_gap = both_kinds();
    short_gap.pull_gap_us = 500000;
    EXPECT_EQ(stretches_of(r, short_gap, 0, r.info().duration_us + 1).size(),
              7u);
}
