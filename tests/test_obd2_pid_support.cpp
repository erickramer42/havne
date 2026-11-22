// File/layer: tests/test_obd2_pid_support.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

namespace {

/* The first vehicle's support report: the bitmaps at 00, 20 and 40
 * (docs/bench-sessions/2026-10-22-vehicle.md), from the engine ECU */
ecuio_channel_support_t first_vehicle()
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    return car;
}

/* The obd2 set's unsupported ids against reports */
std::vector<uint16_t> unsupported(const ecuio_channel_support_t* reports,
                                  uint32_t n)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> ids(set->count);
    const uint32_t count = ecuio_unsupported_signals(
        *set, reports, n, ids.data(), static_cast<uint32_t>(ids.size()));
    EXPECT_LE(count, ids.size());
    ids.resize(count);
    return ids;
}

} // namespace

/* ADR-0039 section 6: a Service 01 signal is unsupported when its ECU's
 * answered bitmap clears its PID's bit; the rule from the bitmaps'
 * bytes: PID p is supported when bit 0x80 >> ((p - 1) % 8) of byte
 * (p - 1) / 8 is set. Ranges never answered (past 0x60 here) and PID 0
 * judge nothing */
TEST(PidSupportTest, TheBitmapsMarkTheirClearPidsUnsupported)
{
    const ecuio_channel_support_t car = first_vehicle();
    const uint8_t* bitmaps = car.pid_bitmap;
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> expected;
    int pastTheRanges = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const unsigned pid =
            (set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) != 0
                ? 0x100u /* a computed def has no PID */
                : set->defs[i].mux_key & 0xFFu;
        if (pid <= 0x60 &&
            (bitmaps[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) == 0) {
            expected.push_back(set->defs[i].id);
        }
        pastTheRanges += pid > 0x60 ? 1 : 0;
    }
    ASSERT_GT(expected.size(), 1u);
    ASSERT_GT(pastTheRanges, 0);
    EXPECT_EQ(unsupported(&car, 1), expected);

    EXPECT_TRUE(ecuio_pid_marked_unsupported(car, 0x5C));
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x0C));
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x61))
        << "a range never answered";
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x00));

    /* A rescan that finds 0x5C takes its signal off */
    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    const std::vector<uint16_t> after = unsupported(&rescan, 1);
    EXPECT_EQ(after.size(), expected.size() - 1);
    for (uint16_t id : after) {
        EXPECT_NE(id, ECUIO_OBD2_PID_SIG_ID(0x5C, 0));
    }
}

/* A complete walk whose chain ended early (0x40's link to 0x60 clear)
 * says every later PID unsupported, as J1979 reads the clear link: the
 * set's PIDs past 0x60 are listed with the clear ones before it */
TEST(PidSupportTest, AChainEndedEarlyMarksEveryLaterPidUnsupported)
{
    ecuio_channel_support_t car = first_vehicle();
    car.flags = ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE;
    car.pid_bitmap[(0x60 - 1) / 8] &= ~(0x80u >> ((0x60 - 1) % 8));
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> expected;
    int pastTheChain = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        if ((set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue; /* none of obd2's computed inputs is past 0x60 */
        }
        const unsigned pid = set->defs[i].mux_key & 0xFFu;
        if (pid > 0x60 ||
            (car.pid_bitmap[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) == 0) {
            expected.push_back(set->defs[i].id);
        }
        pastTheChain += pid > 0x60 ? 1 : 0;
    }
    ASSERT_GT(pastTheChain, 5);
    EXPECT_EQ(unsupported(&car, 1), expected);
    EXPECT_TRUE(ecuio_pid_marked_unsupported(car, 0x61));
    EXPECT_TRUE(ecuio_pid_marked_unsupported(car, 0xA6));
    EXPECT_TRUE(ecuio_pid_marked_unsupported(car, 0xFF));
    EXPECT_TRUE(ecuio_pid_marked_unsupported(car, 0x5C))
        << "clear in 40's answered bitmap";
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x0C))
        << "set in 00's answered bitmap";
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x00));

    /* A chain whose last answered link is set judges nothing past it: the
     * walk ended for another reason than the ECU's word */
    ecuio_channel_support_t linked = car;
    linked.pid_bitmap[(0x60 - 1) / 8] |= 0x80u >> ((0x60 - 1) % 8);
    EXPECT_FALSE(ecuio_pid_marked_unsupported(linked, 0x61));
    EXPECT_FALSE(ecuio_pid_marked_unsupported(linked, 0xA6));
    /* A range below an answered one stays as its own bitmap says */
    ecuio_channel_support_t gap = car;
    gap.ranges = 0x5; /* 00 and 40 answered, 20 not */
    EXPECT_FALSE(ecuio_pid_marked_unsupported(gap, 0x21))
        << "00's link to 20 is set";
}

/* A walk that never finished (PROBED alone) judges no range it didn't
 * answer, however its last answered link reads */
TEST(PidSupportTest, AnUnfinishedWalkJudgesNoRangeItDidntAnswer)
{
    ecuio_channel_support_t car = first_vehicle();
    ASSERT_EQ(car.flags & ECUIO_SUPPORT_COMPLETE, 0u);
    car.pid_bitmap[(0x60 - 1) / 8] &= ~(0x80u >> ((0x60 - 1) % 8));
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0x61));
    EXPECT_FALSE(ecuio_pid_marked_unsupported(car, 0xA6));
    for (uint16_t id : unsupported(&car, 1)) {
        const ecuio_signal_def_t* d = find_def(ecuio_obd2_signal_set(), id);
        ASSERT_NE(d, nullptr);
        EXPECT_LE(d->mux_key & 0xFFu, 0x60u) << id;
    }
}

/* The first vehicle's sheet counts 44 data PIDs to 0x60 (0x20 and 0x40,
 * the chain's links, and 0x60 left out); the bitmaps cover what their
 * ranges answered */
TEST(PidSupportTest, TheCountLeavesOutTheChainsOwnPids)
{
    const ecuio_channel_support_t car = first_vehicle();
    EXPECT_EQ(ecuio_supported_pid_count(car.pid_bitmap), 44u);
    uint8_t chain[32] = {};
    chain[(0x20 - 1) / 8] = 0x01; /* 0x20 */
    chain[(0x40 - 1) / 8] = 0x01; /* 0x40 */
    EXPECT_EQ(ecuio_supported_pid_count(chain), 0u);
    chain[0] = 0x80;  /* 0x01 */
    chain[31] = 0x02; /* 0xFF */
    EXPECT_EQ(ecuio_supported_pid_count(chain), 2u);
    EXPECT_EQ(ecuio_pids_through(car.ranges), 0x60u);
    EXPECT_EQ(ecuio_pids_through(0x3F), 0xC0u);
    EXPECT_EQ(ecuio_pids_through(0x1), 0x20u);
    EXPECT_EQ(ecuio_pids_through(0), 0u);
}

/* Only the report of the channel a def's message ID names judges it,
 * and one with nothing learned judges nothing; a set with no Service 01
 * keys has none; the count is all of them, the first capacity copied */
TEST(PidSupportTest, OnlyTheDefsOwnEcusReportJudgesIt)
{
    const ecuio_channel_support_t car = first_vehicle();
    ecuio_channel_support_t other = car;
    other.rx_id = 0x7E9;
    ecuio_channel_support_t nothing = car;
    nothing.generation = 0;
    const ecuio_channel_support_t neither[2] = {other, nothing};
    EXPECT_TRUE(unsupported(neither, 2).empty());
    EXPECT_TRUE(unsupported(nullptr, 0).empty());
    const ecuio_channel_support_t both[2] = {other, car};
    EXPECT_EQ(unsupported(both, 2), unsupported(&car, 1));

    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    EXPECT_EQ(ecuio_unsupported_signals(*mock, &car, 1, nullptr, 0), 0u);

    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const uint32_t all = ecuio_unsupported_signals(*set, &car, 1, nullptr, 0);
    ASSERT_GT(all, 2u);
    uint16_t two[3] = {0, 0, 0xFFFF};
    EXPECT_EQ(ecuio_unsupported_signals(*set, &car, 1, two, 2), all);
    EXPECT_EQ(two[0], unsupported(&car, 1)[0]);
    EXPECT_EQ(two[1], unsupported(&car, 1)[1]);
    EXPECT_EQ(two[2], 0xFFFF) << "nothing past the capacity";
}

namespace {

/* A def keyed 41 <PID> on rx */
ecuio_signal_def_t pid_def(uint16_t id, uint8_t pid, uint32_t rx = 0x7E8)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 2;
    d.mux_key = 0x4100u | pid;
    return d;
}

/* A computed def running the set's program at index program */
ecuio_signal_def_t computed_over(uint16_t id, uint16_t program)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.flags = ECUIO_DEF_FLAG_COMPUTED;
    d.table_first = program;
    return d;
}

ecuio_computed_t program_of(std::initializer_list<uint16_t> inputs)
{
    ecuio_computed_t p{};
    for (uint16_t id : inputs) {
        p.input_ids[p.input_count++] = id;
    }
    return p;
}

/* The ids a set's defs and programs give against the channels' reports */
std::vector<uint16_t>
unsupported_of(const std::vector<ecuio_signal_def_t>& defs,
               const std::vector<ecuio_computed_t>& programs,
               const std::vector<ecuio_channel_support_t>& reports)
{
    ecuio_signal_set_t set{};
    set.defs = defs.data();
    set.count = static_cast<uint32_t>(defs.size());
    set.computed = programs.data();
    set.computed_count = static_cast<uint32_t>(programs.size());
    std::vector<uint16_t> ids(defs.size());
    const uint32_t count = ecuio_unsupported_signals(
        set, reports.data(), static_cast<uint32_t>(reports.size()), ids.data(),
        static_cast<uint32_t>(ids.size()));
    EXPECT_LE(count, ids.size());
    ids.resize(count);
    return ids;
}

/* Against one report */
std::vector<uint16_t>
unsupported_of(const std::vector<ecuio_signal_def_t>& defs,
               const std::vector<ecuio_computed_t>& programs,
               const ecuio_channel_support_t& report)
{
    return unsupported_of(defs, programs,
                          std::vector<ecuio_channel_support_t>{report});
}

} // namespace

/* A computed def can't be computed without each input, so it is listed
 * when one of them is, in the set's order, through a chain of computed
 * defs; one whose inputs are all supported, or not judged (a report that
 * has learned nothing, another ECU's, a range never answered), isn't */
TEST(PidSupportTest, AComputedSignalFollowsItsInputs)
{
    const ecuio_channel_support_t car = first_vehicle();
    ASSERT_TRUE(ecuio_pid_marked_unsupported(car, 0x5C));
    ASSERT_FALSE(ecuio_pid_marked_unsupported(car, 0x0C));

    /* obd2's Boost reads MAP (0B) and the barometric pressure (33): the
     * first vehicle has both, a car without the second has no Boost */
    const std::vector<uint16_t> with = unsupported(&car, 1);
    EXPECT_EQ(std::count(with.begin(), with.end(), ECUIO_OBD2_SIG_BOOST_ID), 0);
    ecuio_channel_support_t no_baro = car;
    no_baro.pid_bitmap[(0x33 - 1) / 8] &=
        static_cast<uint8_t>(~(0x80u >> ((0x33 - 1) % 8)));
    const std::vector<uint16_t> without = unsupported(&no_baro, 1);
    EXPECT_EQ(without.size(), with.size() + 2) << "baro and Boost";
    ASSERT_FALSE(without.empty());
    EXPECT_EQ(without.back(), ECUIO_OBD2_SIG_BOOST_ID) << "the set's last def";

    /* 10 reads a PID the car has, 11 one it lacks; 20 over 10; 21 over
     * both; 22 over 21 (a chain); 23 over 22 */
    const std::vector<ecuio_signal_def_t> defs = {
        computed_over(22, 2), pid_def(10, 0x0C),    pid_def(11, 0x5C),
        computed_over(20, 0), computed_over(21, 1), computed_over(23, 3)};
    const std::vector<ecuio_computed_t> programs = {
        program_of({10}), program_of({10, 11}), program_of({21}),
        program_of({22})};
    EXPECT_EQ(unsupported_of(defs, programs, car),
              (std::vector<uint16_t>{22, 11, 21, 23}));

    /* Nothing judged: no input is listed, so no computed def is */
    ecuio_channel_support_t nothing = car;
    nothing.generation = 0;
    EXPECT_TRUE(unsupported_of(defs, programs, nothing).empty());
    ecuio_channel_support_t other = car;
    other.rx_id = 0x7E9;
    EXPECT_TRUE(unsupported_of(defs, programs, other).empty());
    const std::vector<ecuio_signal_def_t> past = {pid_def(11, 0x61),
                                                  computed_over(20, 0)};
    EXPECT_TRUE(unsupported_of(past, {program_of({11})}, car).empty())
        << "a range never answered";

    /* Inputs on two ECUs, one judged and one not: 30 (a PID the car
     * lacks) and 31 (one it has) on the engine ECU, 32 on another whose
     * report has learned nothing. 40 over 30 and 32 is listed by 30; 41
     * over 31 and 32 has no input listed. Once the second ECU's report
     * says it lacks 32's PID, 41 goes too. */
    const std::vector<ecuio_signal_def_t> two = {
        pid_def(30, 0x5C), pid_def(31, 0x0C), pid_def(32, 0x5C, 0x7E9),
        computed_over(40, 0), computed_over(41, 1)};
    const std::vector<ecuio_computed_t> across = {program_of({30, 32}),
                                                  program_of({31, 32})};
    ecuio_channel_support_t second = car;
    second.rx_id = 0x7E9;
    second.generation = 0;
    EXPECT_EQ(unsupported_of(two, across, {car, second}),
              (std::vector<uint16_t>{30, 40}));
    EXPECT_EQ(unsupported_of(two, across, {second, car}),
              (std::vector<uint16_t>{30, 40}))
        << "whichever report comes first";
    EXPECT_EQ(unsupported_of(two, across, car), (std::vector<uint16_t>{30, 40}))
        << "no report for the second ECU";
    second.generation = 1;
    EXPECT_EQ(unsupported_of(two, across, {car, second}),
              (std::vector<uint16_t>{30, 32, 40, 41}));

    /* What the set doesn't hold judges nothing: a program past its table,
     * an input it lacks; programs naming each other end, unlisted */
    const std::vector<ecuio_signal_def_t> loose = {
        pid_def(11, 0x5C), computed_over(20, 5), computed_over(21, 0),
        computed_over(22, 1), computed_over(23, 2)};
    EXPECT_EQ(
        unsupported_of(
            loose, {program_of({99}), program_of({23}), program_of({22})}, car),
        (std::vector<uint16_t>{11}));
    /* A ring reached from a listed input still lists what reads it */
    EXPECT_EQ(unsupported_of(
                  loose,
                  {program_of({99}), program_of({23, 11}), program_of({22})},
                  car),
              (std::vector<uint16_t>{11, 22, 23}));
    /* No table at all */
    ecuio_signal_set_t bare{};
    bare.defs = loose.data();
    bare.count = static_cast<uint32_t>(loose.size());
    uint16_t ids[8] = {};
    EXPECT_EQ(ecuio_unsupported_signals(bare, &car, 1, ids, 8), 1u);
    EXPECT_EQ(ids[0], 11);
}

namespace {

/* A program over inputs with these steps and constants */
ecuio_computed_t program_of(std::initializer_list<uint16_t> inputs,
                            std::initializer_list<uint8_t> steps,
                            std::initializer_list<double> constants = {})
{
    ecuio_computed_t p = program_of(inputs);
    for (uint8_t s : steps) {
        p.steps[p.step_count++] = s;
    }
    for (double c : constants) {
        p.constants[p.constant_count++] = c;
    }
    return p;
}

constexpr uint8_t kIn0 = ECUIO_COMPUTED_PUSH_INPUT | 0u;
constexpr uint8_t kIn1 = ECUIO_COMPUTED_PUSH_INPUT | 1u;
constexpr uint8_t kZero = ECUIO_COMPUTED_PUSH_CONSTANT | 0u;

} // namespace

/* An input the program can give a value without (a branch an if may not
 * take, a part of an all or an any) lists nothing alone; one it can't
 * (the predicate, both branches, a part of a sum) lists it, and so do
 * inputs it can't do without together; a chain over each follows: 10
 * reads the PID the car has, 11 the one it lacks */
TEST(PidSupportTest, AComputedSignalListedOnlyByAnInputItCantDoWithout)
{
    const ecuio_channel_support_t car = first_vehicle();
    ASSERT_FALSE(ecuio_pid_marked_unsupported(car, 0x0C));
    ASSERT_TRUE(ecuio_pid_marked_unsupported(car, 0x5C));
    const std::vector<ecuio_signal_def_t> defs = {
        pid_def(10, 0x0C),    pid_def(11, 0x5C),    computed_over(20, 0),
        computed_over(21, 1), computed_over(22, 2), computed_over(23, 3),
        computed_over(24, 4), computed_over(25, 5), computed_over(26, 6),
        computed_over(27, 7)};
    const std::vector<ecuio_computed_t> programs = {
        /* 20: if(a > 0, b / a, 0) */
        program_of({10, 11},
                   {kIn0, kZero, ECUIO_COMPUTED_GREATER, kIn1, kIn0,
                    ECUIO_COMPUTED_DIVIDE, kZero, ECUIO_COMPUTED_IF},
                   {0.0}),
        /* 21: if(b > 0, a, 0) */
        program_of({10, 11},
                   {kIn1, kZero, ECUIO_COMPUTED_GREATER, kIn0, kZero,
                    ECUIO_COMPUTED_IF},
                   {0.0}),
        /* 22: if(a > 0, b, b * 2) */
        program_of({10, 11},
                   {kIn0, kZero, ECUIO_COMPUTED_GREATER, kIn1, kIn1,
                    ECUIO_COMPUTED_PUSH_CONSTANT | 1u, ECUIO_COMPUTED_MULTIPLY,
                    ECUIO_COMPUTED_IF},
                   {0.0, 2.0}),
        /* 23: if(any(a > 0, b > 0), 1, 0) */
        program_of({10, 11},
                   {kIn0, kZero, ECUIO_COMPUTED_GREATER, kIn1, kZero,
                    ECUIO_COMPUTED_GREATER, ECUIO_COMPUTED_ANY,
                    ECUIO_COMPUTED_PUSH_CONSTANT | 1u, kZero,
                    ECUIO_COMPUTED_IF},
                   {0.0, 1.0}),
        /* 24: if(all(a > 0, b > 0), 1, 0) */
        program_of({10, 11},
                   {kIn0, kZero, ECUIO_COMPUTED_GREATER, kIn1, kZero,
                    ECUIO_COMPUTED_GREATER, ECUIO_COMPUTED_ALL,
                    ECUIO_COMPUTED_PUSH_CONSTANT | 1u, kZero,
                    ECUIO_COMPUTED_IF},
                   {0.0, 1.0}),
        /* 25: a + b, as format 1 writes it */
        program_of({10, 11}, {kIn0, kIn1, ECUIO_COMPUTED_ADD}),
        /* 26 over 20, 27 over 21 */
        program_of({20}, {kIn0}), program_of({21}, {kIn0})};
    for (const ecuio_computed_t& p : programs) {
        ASSERT_TRUE(ecuio_computed_check(p));
    }
    EXPECT_EQ(unsupported_of(defs, programs, car),
              (std::vector<uint16_t>{11, 21, 22, 25, 27}));

    /* Inputs it can do without one at a time but not together: 12 reads
     * a second PID the car lacks; 30 is if(any(b > 0, c > 0), 1, 0) over
     * 11 and 12, 31 if(a > 0, b / a, c / a) over 10, 11 and 12 */
    uint8_t lacked = 0;
    for (uint8_t pid = 0x01; pid < 0x5C && lacked == 0; ++pid) {
        if (pid % 0x20 != 0 && ecuio_pid_marked_unsupported(car, pid)) {
            lacked = pid;
        }
    }
    ASSERT_NE(lacked, 0);
    const std::vector<ecuio_signal_def_t> both = {
        pid_def(10, 0x0C), pid_def(11, 0x5C), pid_def(12, lacked),
        computed_over(30, 0), computed_over(31, 1)};
    const std::vector<ecuio_computed_t> together = {
        program_of({11, 12},
                   {kIn0, kZero, ECUIO_COMPUTED_GREATER, kIn1, kZero,
                    ECUIO_COMPUTED_GREATER, ECUIO_COMPUTED_ANY,
                    ECUIO_COMPUTED_PUSH_CONSTANT | 1u, kZero,
                    ECUIO_COMPUTED_IF},
                   {0.0, 1.0}),
        program_of({10, 11, 12},
                   {kIn0, kZero, ECUIO_COMPUTED_GREATER, kIn1, kIn0,
                    ECUIO_COMPUTED_DIVIDE, ECUIO_COMPUTED_PUSH_INPUT | 2u, kIn0,
                    ECUIO_COMPUTED_DIVIDE, ECUIO_COMPUTED_IF},
                   {0.0})};
    for (const ecuio_computed_t& p : together) {
        ASSERT_TRUE(ecuio_computed_check(p));
    }
    EXPECT_EQ(unsupported_of(both, together, car),
              (std::vector<uint16_t>{11, 12, 30, 31}));
}

/* A chain as deep as the validator takes is followed to its end: 100
 * reads the PID the car lacks, each next def reads the one before */
TEST(PidSupportTest, AChainAsDeepAsTheValidatorTakesIsFollowedToItsEnd)
{
    const ecuio_channel_support_t car = first_vehicle();
    ASSERT_TRUE(ecuio_pid_marked_unsupported(car, 0x5C));
    std::vector<ecuio_signal_def_t> defs = {pid_def(11, 0x5C)};
    std::vector<ecuio_computed_t> programs;
    std::vector<uint16_t> want = {11};
    for (uint16_t k = 0; k < ECUIO_COMPUTED_MAX_DEPTH; ++k) {
        const uint16_t id = static_cast<uint16_t>(100 + k);
        defs.push_back(computed_over(id, k));
        programs.push_back(program_of(
            {k == 0 ? uint16_t{11} : static_cast<uint16_t>(id - 1)}));
        want.push_back(id);
    }
    EXPECT_EQ(unsupported_of(defs, programs, car), want);
}
