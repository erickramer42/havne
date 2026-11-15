// File/layer: tests/test_computed.cpp | C++17, Qt-free, every platform
// Testing: UNIT: ComputedTest (test_signals)
//
// The computed program's machine (core/src/computed.h): each step's
// result, the no-value rule, the typed checker, and a table of results
// as bytes that every compiler and the x86 build must give alike.

#include <gtest/gtest.h>
#include <ecuio/signals.h>
#include "computed.h"

#include <cfenv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double kNone = std::numeric_limits<double>::quiet_NaN();

constexpr uint8_t in(unsigned k)
{
    return static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_INPUT | k);
}

constexpr uint8_t k(unsigned c)
{
    return static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_CONSTANT | c);
}

constexpr uint8_t op(unsigned step)
{
    return static_cast<uint8_t>(step);
}

/* A program over four inputs (ids 1 to 4) with these steps and constants */
ecuio_computed_t program(std::initializer_list<uint8_t> steps,
                         std::initializer_list<double> constants = {})
{
    ecuio_computed_t p{};
    p.input_count = ECUIO_COMPUTED_MAX_INPUTS;
    for (uint16_t i = 0; i < ECUIO_COMPUTED_MAX_INPUTS; ++i) {
        p.input_ids[i] = static_cast<uint16_t>(i + 1);
    }
    p.step_count = static_cast<uint8_t>(steps.size());
    size_t n = 0;
    for (const uint8_t s : steps) {
        p.steps[n++] = s;
    }
    p.constant_count = static_cast<uint8_t>(constants.size());
    n = 0;
    for (const double c : constants) {
        p.constants[n++] = c;
    }
    return p;
}

std::string hex_of(double v)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    char text[24];
    std::snprintf(text, sizeof text, "0x%016" PRIX64, bits);
    return text;
}

/* The result's bytes, or "none" */
std::string run(const ecuio_computed_t& p, double a = 0.0, double b = 0.0,
                double c = 0.0, double d = 0.0)
{
    const double inputs[4] = {a, b, c, d};
    double out = -1.0;
    if (!ecuio_computed_run(p, inputs, &out)) {
        return out == -1.0 ? "none" : "none, but out written";
    }
    return hex_of(out);
}

std::string bits(double v)
{
    return hex_of(v);
}

/* if(p, 1, 0) over a program's truth, with 0 and 1 as constants 0 and 1 */
const uint8_t kOne = k(1);
const uint8_t kZero = k(0);

// Each step's result, and the order of its operands: a is the second
// from the top
TEST(ComputedTest, EachNumberStepGivesItsResult)
{
    const auto two = [](unsigned step, double a, double b) {
        return run(program({in(0), in(1), op(step)}), a, b);
    };
    const auto one = [](unsigned step, double a) {
        return run(program({in(0), op(step)}), a);
    };
    EXPECT_EQ(two(ECUIO_COMPUTED_ADD, 7, 2), bits(9));
    EXPECT_EQ(two(ECUIO_COMPUTED_SUBTRACT, 7, 2), bits(5));
    EXPECT_EQ(two(ECUIO_COMPUTED_MULTIPLY, 7, 2), bits(14));
    EXPECT_EQ(two(ECUIO_COMPUTED_DIVIDE, 7, 2), bits(3.5));
    EXPECT_EQ(two(ECUIO_COMPUTED_MIN, 7, -2), bits(-2));
    EXPECT_EQ(two(ECUIO_COMPUTED_MIN, -2, 7), bits(-2));
    EXPECT_EQ(two(ECUIO_COMPUTED_MAX, 7, -2), bits(7));
    EXPECT_EQ(two(ECUIO_COMPUTED_MAX, -2, 7), bits(7));
    EXPECT_EQ(one(ECUIO_COMPUTED_ABS, -2.5), bits(2.5));
    EXPECT_EQ(one(ECUIO_COMPUTED_ABS, 2.5), bits(2.5));
    EXPECT_EQ(one(ECUIO_COMPUTED_SQRT, 6.25), bits(2.5));
    EXPECT_EQ(one(ECUIO_COMPUTED_SQRT, 0.0), bits(0.0));
    EXPECT_EQ(one(ECUIO_COMPUTED_SQRT, -1e-300), "none") << "below zero";
    EXPECT_EQ(one(ECUIO_COMPUTED_ROUND, 2.4), bits(2));
    EXPECT_EQ(one(ECUIO_COMPUTED_ROUND, 2.5), bits(3));
    EXPECT_EQ(one(ECUIO_COMPUTED_ROUND, -2.5), bits(-3));
    EXPECT_EQ(one(ECUIO_COMPUTED_FLOOR, -2.5), bits(-3));
    EXPECT_EQ(one(ECUIO_COMPUTED_FLOOR, 2.5), bits(2));
    EXPECT_EQ(one(ECUIO_COMPUTED_CEIL, -2.5), bits(-2));
    EXPECT_EQ(one(ECUIO_COMPUTED_CEIL, 2.1), bits(3));
}

// min and max compare, so of -0 and +0 the first is kept, never the C
// library's choice
TEST(ComputedTest, MinAndMaxKeepTheFirstOfTwoEqualValues)
{
    const auto two = [](unsigned step, double a, double b) {
        return run(program({in(0), in(1), op(step)}), a, b);
    };
    EXPECT_EQ(two(ECUIO_COMPUTED_MIN, -0.0, 0.0), bits(-0.0));
    EXPECT_EQ(two(ECUIO_COMPUTED_MIN, 0.0, -0.0), bits(0.0));
    EXPECT_EQ(two(ECUIO_COMPUTED_MAX, -0.0, 0.0), bits(-0.0));
    EXPECT_EQ(two(ECUIO_COMPUTED_MAX, 0.0, -0.0), bits(0.0));
}

TEST(ComputedTest, RoundTakesAHalfAwayFromZeroInEveryRoundingMode)
{
    const ecuio_computed_t p = program({in(0), op(ECUIO_COMPUTED_ROUND)});
    const int before = std::fegetround();
    for (const int mode :
         {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        ASSERT_EQ(std::fesetround(mode), 0);
        EXPECT_EQ(run(p, 2.5), bits(3)) << mode;
        EXPECT_EQ(run(p, -2.5), bits(-3)) << mode;
        EXPECT_EQ(run(p, 3.5), bits(4)) << mode;
        EXPECT_EQ(run(p, 0.49999999999999994), bits(0)) << mode;
    }
    std::fesetround(before);
}

TEST(ComputedTest, EachComparisonGivesATruth)
{
    const auto compare = [](unsigned step, double a, double b) {
        return run(program({in(0), in(1), op(step), kOne, kZero,
                            op(ECUIO_COMPUTED_IF)},
                           {0.0, 1.0}),
                   a, b);
    };
    const std::string yes = bits(1);
    const std::string no = bits(0);
    struct Row {
        unsigned step;
        const char* name;
        const char* below; /* 1 against 2 */
        const char* equal; /* 2 against 2 */
        const char* above; /* 3 against 2 */
    };
    const Row rows[] = {
        {ECUIO_COMPUTED_LESS, "<", "y", "n", "n"},
        {ECUIO_COMPUTED_LESS_EQUAL, "<=", "y", "y", "n"},
        {ECUIO_COMPUTED_GREATER, ">", "n", "n", "y"},
        {ECUIO_COMPUTED_GREATER_EQUAL, ">=", "n", "y", "y"},
        {ECUIO_COMPUTED_EQUAL, "==", "n", "y", "n"},
        {ECUIO_COMPUTED_NOT_EQUAL, "!=", "y", "n", "y"},
    };
    for (const Row& r : rows) {
        const auto want = [&](const char* yn) { return *yn == 'y' ? yes : no; };
        EXPECT_EQ(compare(r.step, 1, 2), want(r.below)) << r.name;
        EXPECT_EQ(compare(r.step, 2, 2), want(r.equal)) << r.name;
        EXPECT_EQ(compare(r.step, 3, 2), want(r.above)) << r.name;
        EXPECT_EQ(compare(r.step, kNone, 2), "none") << r.name;
        EXPECT_EQ(compare(r.step, 2, kNone), "none") << r.name;
    }
    EXPECT_EQ(compare(ECUIO_COMPUTED_EQUAL, -0.0, 0.0), yes)
        << "exactly, as IEEE 754 compares";
    EXPECT_EQ(compare(ECUIO_COMPUTED_EQUAL, 0.1 + 0.2, 0.3), no) << "exactly";
}

// A NaN input has no value, and so has every step it reaches, whatever
// the step
TEST(ComputedTest, AStepWithNoValueInGivesNoValueOut)
{
    const unsigned twos[] = {ECUIO_COMPUTED_ADD,      ECUIO_COMPUTED_SUBTRACT,
                             ECUIO_COMPUTED_MULTIPLY, ECUIO_COMPUTED_DIVIDE,
                             ECUIO_COMPUTED_MIN,      ECUIO_COMPUTED_MAX};
    for (const unsigned s : twos) {
        const ecuio_computed_t p = program({in(0), in(1), op(s)});
        EXPECT_EQ(run(p, kNone, 2), "none") << s;
        EXPECT_EQ(run(p, 2, kNone), "none") << s;
        EXPECT_NE(run(p, 4, 2), "none") << s;
    }
    const unsigned ones[] = {ECUIO_COMPUTED_ABS, ECUIO_COMPUTED_SQRT,
                             ECUIO_COMPUTED_ROUND, ECUIO_COMPUTED_FLOOR,
                             ECUIO_COMPUTED_CEIL};
    for (const unsigned s : ones) {
        const ecuio_computed_t p = program({in(0), op(s)});
        EXPECT_EQ(run(p, kNone), "none") << s;
        EXPECT_EQ(run(p, -std::numeric_limits<double>::infinity()), "none")
            << s << ": a result that isn't finite";
        EXPECT_NE(run(p, 4), "none") << s;
    }
    /* not(p) */
    const ecuio_computed_t negated =
        program({in(0), kZero, op(ECUIO_COMPUTED_GREATER),
                 op(ECUIO_COMPUTED_NOT), kOne, kZero, op(ECUIO_COMPUTED_IF)},
                {0.0, 1.0});
    EXPECT_EQ(run(negated, 1), bits(0));
    EXPECT_EQ(run(negated, -1), bits(1));
    EXPECT_EQ(run(negated, kNone), "none");

    /* A format 1 program: any input without a value leaves it without one */
    const ecuio_computed_t boost =
        program({in(0), in(1), op(ECUIO_COMPUTED_SUBTRACT)});
    EXPECT_EQ(run(boost, 140, 101), bits(39));
    EXPECT_EQ(run(boost, 140, kNone), "none");
    /* No value comes back finite: 0 * none is none, not 0 */
    const ecuio_computed_t times_zero =
        program({in(0), kZero, op(ECUIO_COMPUTED_MULTIPLY)}, {0.0});
    EXPECT_EQ(run(times_zero, kNone), "none");
}

// An infinite input is a value, as in format 1: a step's result is
// judged, and the program's
TEST(ComputedTest, AnInfiniteInputIsAValueWhoseResultIsJudged)
{
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(
        run(program({kOne, in(0), op(ECUIO_COMPUTED_DIVIDE)}, {0.0, 1.0}), inf),
        bits(0))
        << "1 / inf, format 1's answer";
    EXPECT_EQ(run(program({in(0), in(1), op(ECUIO_COMPUTED_MIN)}), inf, 2),
              bits(2));
    EXPECT_EQ(run(program({in(0), in(1), op(ECUIO_COMPUTED_MAX)}), inf, 2),
              "none");
    EXPECT_EQ(run(program({in(0), in(1), op(ECUIO_COMPUTED_ADD)}), inf, 2),
              "none");
    EXPECT_EQ(run(program({in(0)}), inf), "none")
        << "a program ending infinite";
    EXPECT_EQ(run(program({in(0), kZero, op(ECUIO_COMPUTED_GREATER), kOne,
                           kZero, op(ECUIO_COMPUTED_IF)},
                          {0.0, 1.0}),
                  inf),
              bits(1))
        << "inf > 0";
    EXPECT_EQ(run(program({in(0), kZero, op(ECUIO_COMPUTED_GREATER), in(0),
                           kZero, op(ECUIO_COMPUTED_IF)},
                          {0.0}),
                  inf),
              "none")
        << "if picking an infinite input ends infinite";
}

// if(p, a, b): no value when p has none or when the branch p picks has
// none; the other branch's value, or lack of one, changes nothing
TEST(ComputedTest, IfIgnoresTheBranchItDoesntTake)
{
    /* if(in0 > 0, in1 * 60 / in0, 0): air per revolution */
    const ecuio_computed_t air =
        program({in(0), kZero, op(ECUIO_COMPUTED_GREATER), in(1), k(1),
                 op(ECUIO_COMPUTED_MULTIPLY), in(0), op(ECUIO_COMPUTED_DIVIDE),
                 kZero, op(ECUIO_COMPUTED_IF)},
                {0.0, 60.0});
    ASSERT_TRUE(ecuio_computed_check(air));
    EXPECT_EQ(run(air, 0, 12.3), bits(0))
        << "a zero divisor in the branch not taken";
    EXPECT_EQ(run(air, 800, 12.3), bits(12.3 * 60 / 800));
    EXPECT_EQ(run(air, 800, kNone), "none") << "the branch taken has no value";
    EXPECT_EQ(run(air, 0, kNone), bits(0))
        << "an old input in the branch not taken";
    EXPECT_EQ(run(air, kNone, 12.3), "none") << "the truth has no value";

    /* if(in0 == 0, 0, in1 / in2): a quiet signal in the branch not taken */
    const ecuio_computed_t gear =
        program({in(0), kZero, op(ECUIO_COMPUTED_EQUAL), kZero, in(1), in(2),
                 op(ECUIO_COMPUTED_DIVIDE), op(ECUIO_COMPUTED_IF)},
                {0.0});
    ASSERT_TRUE(ecuio_computed_check(gear));
    EXPECT_EQ(run(gear, 0, 50, kNone), bits(0)) << "never seen, not taken";
    EXPECT_EQ(run(gear, 0, 50, 0), bits(0)) << "a zero divisor, not taken";
    EXPECT_EQ(run(gear, 3, 50, 2), bits(25));
    EXPECT_EQ(run(gear, 3, 50, kNone), "none");
    EXPECT_EQ(run(gear, 3, 50, 0), "none") << "a zero divisor, taken";

    /* if(in0 > 0, sqrt(in1), in2): sqrt below zero, not taken */
    const ecuio_computed_t root =
        program({in(0), kZero, op(ECUIO_COMPUTED_GREATER), in(1),
                 op(ECUIO_COMPUTED_SQRT), in(2), op(ECUIO_COMPUTED_IF)},
                {0.0});
    EXPECT_EQ(run(root, -1, -4, 7), bits(7));
    EXPECT_EQ(run(root, 1, -4, 7), "none");
    EXPECT_EQ(run(root, 1, 4, kNone), bits(2));

    /* A step after the if still sees no value: if(...) + in3 */
    const ecuio_computed_t after =
        program({in(0), kZero, op(ECUIO_COMPUTED_GREATER), in(1), in(2),
                 op(ECUIO_COMPUTED_IF), in(3), op(ECUIO_COMPUTED_ADD)},
                {0.0});
    EXPECT_EQ(run(after, 1, 5, kNone, 1), bits(6));
    EXPECT_EQ(run(after, 1, 5, kNone, kNone), "none");
    EXPECT_EQ(run(after, -1, kNone, 5, 1), bits(6));
}

// all is false when any part is false and has no value when none is false
// and one has none; any mirrors it with true
TEST(ComputedTest, AllAndAnyJoinAsAConditionsTermsDo)
{
    /* Each truth is in > 0: 1 true, -1 false, NaN no value */
    const double T = 1;
    const double F = -1;
    const double N = kNone;
    const auto join = [](unsigned step, double p, double q) {
        return run(program({in(0), kZero, op(ECUIO_COMPUTED_GREATER), in(1),
                            kZero, op(ECUIO_COMPUTED_GREATER), op(step), kOne,
                            kZero, op(ECUIO_COMPUTED_IF)},
                           {0.0, 1.0}),
                   p, q);
    };
    const std::string t = bits(1);
    const std::string f = bits(0);
    const std::string n = "none";
    struct Row {
        double p;
        double q;
        const std::string& all;
        const std::string& any;
    };
    const Row rows[] = {
        {T, T, t, t}, {T, F, f, t}, {F, T, f, t}, {F, F, f, f}, {T, N, n, t},
        {N, T, n, t}, {F, N, f, n}, {N, F, f, n}, {N, N, n, n},
    };
    for (const Row& r : rows) {
        EXPECT_EQ(join(ECUIO_COMPUTED_ALL, r.p, r.q), r.all)
            << r.p << " " << r.q;
        EXPECT_EQ(join(ECUIO_COMPUTED_ANY, r.p, r.q), r.any)
            << r.p << " " << r.q;
    }
}

/* Whether a program lacks a value with some inputs gone (ADR-0062
 * section 3): each row's inputs gone one at a time as the rules give it,
 * and the ones gone only together; then held to the machine: whatever the
 * set of inputs gone, a program said to lack a value gives none for every
 * value of the others, and one input gone alone that isn't gives a value
 * for some. A program the steps can't run to one value lacks one with any
 * input gone. */
TEST(ComputedTest, AProgramLacksAValueWithTheInputsItCantDoWithout)
{
    /* in(i) > 0, then the rest */
    const auto is_pos = [](unsigned i, std::initializer_list<uint8_t> rest) {
        std::vector<uint8_t> v = {in(i), kZero, op(ECUIO_COMPUTED_GREATER)};
        v.insert(v.end(), rest.begin(), rest.end());
        return v;
    };
    struct Row {
        const char* what;
        std::vector<uint8_t> steps;
        uint8_t alone;    /* bit k: input k gone alone leaves no value */
        uint8_t together; /* a set gone together that does, or 0 */
    };
    const uint8_t IF = op(ECUIO_COMPUTED_IF);
    const uint8_t DIV = op(ECUIO_COMPUTED_DIVIDE);
    const std::vector<Row> rows = {
        {"a", {in(0)}, 0x1, 0},
        {"a constant", {kOne}, 0x0, 0},
        {"a + b", {in(0), in(1), op(ECUIO_COMPUTED_ADD)}, 0x3, 0},
        {"abs(c)", {in(2), op(ECUIO_COMPUTED_ABS)}, 0x4, 0},
        {"if(a > 0, b, c)", is_pos(0, {in(1), in(2), IF}), 0x1, 0x6},
        {"if(a > 0, b, b)", is_pos(0, {in(1), in(1), IF}), 0x3, 0},
        {"if(a > 0, b + c, b)",
         is_pos(0, {in(1), in(2), op(ECUIO_COMPUTED_ADD), in(1), IF}), 0x3, 0},
        {"if(not(a > 0), b, b * c)",
         is_pos(0, {op(ECUIO_COMPUTED_NOT), in(1), in(1), in(2),
                    op(ECUIO_COMPUTED_MULTIPLY), IF}),
         0x3, 0},
        {"if(all(a > 0, b > 0), 1, 0)",
         is_pos(0, {in(1), kZero, op(ECUIO_COMPUTED_GREATER),
                    op(ECUIO_COMPUTED_ALL), kOne, kZero, IF}),
         0x0, 0x3},
        {"if(any(a > 0, b > 0), 1, 0)",
         is_pos(0, {in(1), kZero, op(ECUIO_COMPUTED_GREATER),
                    op(ECUIO_COMPUTED_ANY), kOne, kZero, IF}),
         0x0, 0x3},
        {"if(s > 0, a / s, b / s)",
         is_pos(0, {in(1), in(0), DIV, in(2), in(0), DIV, IF}), 0x1, 0x6},
        {"if(a > b, c, 0) + d",
         {in(0), in(1), op(ECUIO_COMPUTED_GREATER), in(2), kZero, IF, in(3),
          op(ECUIO_COMPUTED_ADD)},
         0xB,
         0},
    };
    const double grid[] = {-1.0, 0.0, 2.0};
    for (const Row& r : rows) {
        SCOPED_TRACE(r.what);
        ecuio_computed_t p = program({}, {0.0, 1.0});
        for (const uint8_t s : r.steps) {
            p.steps[p.step_count++] = s;
        }
        ASSERT_TRUE(ecuio_computed_check(p));
        EXPECT_FALSE(ecuio_computed_lacks(p, 0));
        for (unsigned k = 0; k < ECUIO_COMPUTED_MAX_INPUTS; ++k) {
            EXPECT_EQ(ecuio_computed_lacks(p, static_cast<uint8_t>(1u << k)),
                      ((r.alone >> k) & 1u) != 0)
                << "input " << k << " alone";
        }
        if (r.together != 0) {
            EXPECT_TRUE(ecuio_computed_lacks(p, r.together)) << "together";
        }
        for (unsigned gone = 1; gone < 16; ++gone) {
            bool some_value = false;
            for (unsigned c = 0; c < 81; ++c) {
                double inputs[4] = {grid[c % 3], grid[c / 3 % 3],
                                    grid[c / 9 % 3], grid[c / 27 % 3]};
                for (unsigned k = 0; k < 4; ++k) {
                    if (((gone >> k) & 1u) != 0) {
                        inputs[k] = kNone;
                    }
                }
                double out = 0.0;
                some_value = ecuio_computed_run(p, inputs, &out) || some_value;
            }
            if (ecuio_computed_lacks(p, static_cast<uint8_t>(gone))) {
                EXPECT_FALSE(some_value) << "gone " << gone;
            } else if ((gone & (gone - 1)) == 0) {
                EXPECT_TRUE(some_value) << "gone " << gone << " alone";
            }
        }
    }

    ecuio_computed_t none = program({});
    EXPECT_TRUE(ecuio_computed_lacks(none, 0x8)) << "no steps";
    EXPECT_FALSE(ecuio_computed_lacks(none, 0)) << "no steps, none gone";
    ecuio_computed_t two = program({in(0), in(1)});
    EXPECT_TRUE(ecuio_computed_lacks(two, 0x4)) << "two values left";
    ecuio_computed_t under = program({in(0), op(ECUIO_COMPUTED_ADD)});
    EXPECT_TRUE(ecuio_computed_lacks(under, 0x2)) << "too few values";
    ecuio_computed_t unlisted = program({in(0), 0x2F});
    EXPECT_TRUE(ecuio_computed_lacks(unlisted, 0x2)) << "a step unlisted";
    ecuio_computed_t past = program({in(0)});
    past.input_count = 1;
    past.steps[0] = in(1);
    EXPECT_TRUE(ecuio_computed_lacks(past, 0x1))
        << "an index past the count, its one input gone";
    EXPECT_FALSE(ecuio_computed_lacks(past, 0x2))
        << "an index past the count, a bit past the inputs";
}

// Each listed step in the smallest program its types allow
TEST(ComputedTest, TheCheckerAcceptsEveryListedStepWithItsTypes)
{
    const auto truth = [](unsigned step) {
        return program({in(0), kZero, op(ECUIO_COMPUTED_GREATER), in(1), kZero,
                        op(ECUIO_COMPUTED_GREATER), op(step), kOne, kZero,
                        op(ECUIO_COMPUTED_IF)},
                       {0.0, 1.0});
    };
    for (unsigned s = ECUIO_COMPUTED_ADD; s <= ECUIO_COMPUTED_MAX; ++s) {
        EXPECT_TRUE(ecuio_computed_check(program({in(0), in(1), op(s)}))) << s;
    }
    for (unsigned s = ECUIO_COMPUTED_ABS; s <= ECUIO_COMPUTED_CEIL; ++s) {
        EXPECT_TRUE(ecuio_computed_check(program({in(0), op(s)}))) << s;
    }
    for (unsigned s = ECUIO_COMPUTED_LESS; s <= ECUIO_COMPUTED_NOT_EQUAL; ++s) {
        EXPECT_TRUE(ecuio_computed_check(
            program({in(0), in(1), op(s), kOne, kZero, op(ECUIO_COMPUTED_IF)},
                    {0.0, 1.0})))
            << s;
    }
    EXPECT_TRUE(ecuio_computed_check(truth(ECUIO_COMPUTED_ALL)));
    EXPECT_TRUE(ecuio_computed_check(truth(ECUIO_COMPUTED_ANY)));
    EXPECT_TRUE(ecuio_computed_check(
        program({in(0), kZero, op(ECUIO_COMPUTED_GREATER),
                 op(ECUIO_COMPUTED_NOT), kOne, kZero, op(ECUIO_COMPUTED_IF)},
                {0.0, 1.0})));
    EXPECT_TRUE(
        ecuio_computed_check(program({in(0), kZero, op(ECUIO_COMPUTED_GREATER),
                                      in(0), in(1), op(ECUIO_COMPUTED_IF)},
                                     {0.0})));
}

// Each refusal beside the program it was made from, which passes, so the
// refusal is the type's alone
TEST(ComputedTest, TheCheckerRefusesEachWrongType)
{
    const uint8_t gt = op(ECUIO_COMPUTED_GREATER);
    const uint8_t IF = op(ECUIO_COMPUTED_IF);
    const uint8_t truth[] = {in(0), kZero, gt};
    struct Case {
        const char* what;
        ecuio_computed_t wrong;
        ecuio_computed_t right;
    };
    const Case cases[] = {
        {"+ given a truth below",
         program({truth[0], truth[1], truth[2], in(1), op(ECUIO_COMPUTED_ADD)},
                 {0.0, 1.0}),
         program({in(0), kZero, op(ECUIO_COMPUTED_SUBTRACT), in(1),
                  op(ECUIO_COMPUTED_ADD)},
                 {0.0, 1.0})},
        {"max given a truth on top",
         program({in(1), truth[0], truth[1], truth[2], op(ECUIO_COMPUTED_MAX)},
                 {0.0, 1.0}),
         program({in(1), in(0), kZero, op(ECUIO_COMPUTED_MIN),
                  op(ECUIO_COMPUTED_MAX)},
                 {0.0, 1.0})},
        {"abs given a truth",
         program({truth[0], truth[1], truth[2], op(ECUIO_COMPUTED_ABS)},
                 {0.0, 1.0}),
         program({in(0), op(ECUIO_COMPUTED_ABS)}, {0.0, 1.0})},
        {"< given a truth",
         program({truth[0], truth[1], truth[2], in(1), op(ECUIO_COMPUTED_LESS),
                  kOne, kZero, IF},
                 {0.0, 1.0}),
         program({in(0), in(1), op(ECUIO_COMPUTED_LESS), kOne, kZero, IF},
                 {0.0, 1.0})},
        {"all given a number first",
         program({in(1), truth[0], truth[1], truth[2], op(ECUIO_COMPUTED_ALL),
                  kOne, kZero, IF},
                 {0.0, 1.0}),
         program({in(1), kZero, gt, truth[0], truth[1], truth[2],
                  op(ECUIO_COMPUTED_ALL), kOne, kZero, IF},
                 {0.0, 1.0})},
        {"any given a number on top",
         program({truth[0], truth[1], truth[2], in(1), op(ECUIO_COMPUTED_ANY),
                  kOne, kZero, IF},
                 {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], in(1), kZero, gt,
                  op(ECUIO_COMPUTED_ANY), kOne, kZero, IF},
                 {0.0, 1.0})},
        {"not given a number",
         program({in(0), op(ECUIO_COMPUTED_NOT), kOne, kZero, IF}, {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], op(ECUIO_COMPUTED_NOT), kOne,
                  kZero, IF},
                 {0.0, 1.0})},
        {"if given a number where a truth should be",
         program({in(0), kOne, kZero, IF}, {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], kOne, kZero, IF}, {0.0, 1.0})},
        {"if given a truth as its first branch",
         program({truth[0], truth[1], truth[2], truth[0], truth[1], truth[2],
                  kZero, IF},
                 {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], in(0), kZero, IF}, {0.0, 1.0})},
        {"if given a truth as its second branch",
         program({truth[0], truth[1], truth[2], kZero, truth[0], truth[1],
                  truth[2], IF},
                 {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], kZero, in(0), IF}, {0.0, 1.0})},
        {"a truth at the end",
         program({in(0), in(1), op(ECUIO_COMPUTED_LESS)}, {0.0, 1.0}),
         program({in(0), in(1), op(ECUIO_COMPUTED_MIN)}, {0.0, 1.0})},
        {"a truth at the end, after not",
         program({truth[0], truth[1], truth[2], op(ECUIO_COMPUTED_NOT)},
                 {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], op(ECUIO_COMPUTED_NOT), in(0),
                  in(1), IF},
                 {0.0, 1.0})},
        {"a truth left below the result",
         program({truth[0], truth[1], truth[2], in(0)}, {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], in(0), in(1), IF}, {0.0, 1.0})},
        {"if given two values",
         program({truth[0], truth[1], truth[2], in(0), IF}, {0.0, 1.0}),
         program({truth[0], truth[1], truth[2], in(0), in(0), IF}, {0.0, 1.0})},
        {"abs given none", program({op(ECUIO_COMPUTED_ABS), in(0)}, {0.0, 1.0}),
         program({in(0), op(ECUIO_COMPUTED_ABS)}, {0.0, 1.0})},
    };
    for (const Case& c : cases) {
        EXPECT_FALSE(ecuio_computed_check(c.wrong)) << c.what;
        EXPECT_TRUE(ecuio_computed_check(c.right)) << c.what << ", made right";
    }
}

// The step values are a list: anything else past the pushes is refused,
// whatever its place would allow
TEST(ComputedTest, TheCheckerRefusesEveryStepValueNotListed)
{
    const auto listed = [](unsigned s) {
        return (s >= ECUIO_COMPUTED_ADD && s <= ECUIO_COMPUTED_CEIL) ||
               (s >= ECUIO_COMPUTED_LESS && s <= ECUIO_COMPUTED_NOT_EQUAL) ||
               (s >= ECUIO_COMPUTED_ALL && s <= ECUIO_COMPUTED_IF);
    };
    int unlisted = 0;
    for (unsigned s = 0x20; s <= 0xFF; ++s) {
        ecuio_computed_shape shape{};
        EXPECT_EQ(ecuio_computed_shape_of(op(s), &shape), listed(s)) << s;
        if (listed(s)) {
            continue;
        }
        ++unlisted;
        EXPECT_FALSE(ecuio_computed_check(program({in(0), op(s)}))) << s;
        EXPECT_FALSE(ecuio_computed_check(program({in(0), in(1), op(s)}))) << s;
        EXPECT_FALSE(ecuio_computed_check(program(
            {in(0), kZero, op(ECUIO_COMPUTED_GREATER), in(0), in(1), op(s)},
            {0.0})))
            << s;
        double out = 0.0;
        const double inputs[4] = {1, 2, 3, 4};
        EXPECT_FALSE(
            ecuio_computed_run(program({in(0), in(1), op(s)}), inputs, &out))
            << s << ": the machine refuses it too";
    }
    EXPECT_EQ(unlisted, 0xE0 - 21) << "21 listed operations";
}

// A checked program holds at most half its steps, rounded up, at once
// (a search over every typed program of 33 steps found none deeper):
// 17 values, here with each number step on the way down
TEST(ComputedTest, TheDeepestStackRuns)
{
    ecuio_computed_t deep = program({});
    deep.constant_count = 2;
    deep.constants[0] = 0.0;
    deep.constants[1] = 1.0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < 17; ++i) {
        deep.steps[n++] = in(i % 4);
    }
    const uint8_t down[] = {
        op(ECUIO_COMPUTED_MAX),      op(ECUIO_COMPUTED_MIN),
        op(ECUIO_COMPUTED_ADD),      op(ECUIO_COMPUTED_SUBTRACT),
        op(ECUIO_COMPUTED_MULTIPLY), op(ECUIO_COMPUTED_DIVIDE)};
    for (uint32_t i = 0; i < 16; ++i) {
        deep.steps[n++] = down[i % 6];
    }
    deep.step_count = static_cast<uint8_t>(n);
    ASSERT_EQ(n, ECUIO_COMPUTED_MAX_STEPS);
    ASSERT_TRUE(ecuio_computed_check(deep));
    EXPECT_NE(run(deep, 1, 2, 3, 4), "none");
}

// The machine checks its own bounds, so a program the checker would
// refuse returns no value and reads and writes nothing past its stack
TEST(ComputedTest, AnUncheckedProgramStaysWithinItsStack)
{
    const double inputs[4] = {1, 2, 3, 4};
    double out = 7.0;
    ecuio_computed_t p{};
    p.step_count = 255;
    std::memset(p.steps, k(15), sizeof p.steps);
    EXPECT_FALSE(ecuio_computed_run(p, inputs, &out)) << "36 pushes, 33 run";
    std::memset(p.steps, in(15), sizeof p.steps);
    EXPECT_FALSE(ecuio_computed_run(p, inputs, &out))
        << "an input past the four";
    std::memset(p.steps, op(ECUIO_COMPUTED_IF), sizeof p.steps);
    EXPECT_FALSE(ecuio_computed_run(p, inputs, &out)) << "pops from nothing";
    p = program({in(0), in(1), op(ECUIO_COMPUTED_IF)});
    EXPECT_FALSE(ecuio_computed_run(p, inputs, &out)) << "if over two";
    p = program({in(0), in(1)});
    EXPECT_FALSE(ecuio_computed_run(p, inputs, &out)) << "two left";
    EXPECT_EQ(out, 7.0) << "untouched";
}

// The same inputs give the same bytes on every platform: values chosen
// where a compiler's shortcut would show (a fused multiply-add, flushed
// subnormals, a library's sqrt or min), and each new step. Worked out
// in Python, whose floats are IEEE 754 doubles with a correctly rounded
// sqrt.
TEST(ComputedTest, TheSameBitsOnEveryPlatform)
{
    const uint8_t ADD = op(ECUIO_COMPUTED_ADD);
    const uint8_t SUB = op(ECUIO_COMPUTED_SUBTRACT);
    const uint8_t MUL = op(ECUIO_COMPUTED_MULTIPLY);
    const uint8_t DIV = op(ECUIO_COMPUTED_DIVIDE);
    const uint8_t MIN = op(ECUIO_COMPUTED_MIN);
    const uint8_t MAX = op(ECUIO_COMPUTED_MAX);
    const uint8_t SQRT = op(ECUIO_COMPUTED_SQRT);
    const uint8_t GT = op(ECUIO_COMPUTED_GREATER);
    const uint8_t IF = op(ECUIO_COMPUTED_IF);
    const double tiny = std::numeric_limits<double>::denorm_min();
    struct Row {
        const char* what;
        ecuio_computed_t p;
        double in[4];
        const char* bits;
    };
    const Row rows[] = {
        {"0.1 + 0.2",
         program({in(0), in(1), ADD}),
         {0.1, 0.2},
         "0x3FD3333333333334"},
        {"1 / 3", program({in(0), in(1), DIV}), {1, 3}, "0x3FD5555555555555"},
        {"0.1 * 10 - 1, never fused",
         program({in(0), in(1), MUL, in(2), ADD}),
         {0.1, 10, -1},
         "0x0000000000000000"},
        {"sqrt(2)", program({in(0), SQRT}), {2}, "0x3FF6A09E667F3BCD"},
        {"sqrt of the least subnormal",
         program({in(0), SQRT}),
         {tiny},
         "0x1E60000000000000"},
        {"the least subnormal / 2, ties to even",
         program({in(0), in(1), DIV}),
         {tiny, 2},
         "0x0000000000000000"},
        {"three least subnormals / 2, ties to even",
         program({in(0), in(1), DIV}),
         {3 * tiny, 2},
         "0x0000000000000002"},
        {"1e-160 * 1e-160, a subnormal",
         program({in(0), in(1), MUL}),
         {1e-160, 1e-160},
         "0x00000000000007E8"},
        {"round(2.5)",
         program({in(0), op(ECUIO_COMPUTED_ROUND)}),
         {2.5},
         "0x4008000000000000"},
        {"round(-0.5)",
         program({in(0), op(ECUIO_COMPUTED_ROUND)}),
         {-0.5},
         "0xBFF0000000000000"},
        {"round(-0.4)",
         program({in(0), op(ECUIO_COMPUTED_ROUND)}),
         {-0.4},
         "0x8000000000000000"},
        {"round(0.49999999999999994)",
         program({in(0), op(ECUIO_COMPUTED_ROUND)}),
         {0.49999999999999994},
         "0x0000000000000000"},
        {"floor(-0)",
         program({in(0), op(ECUIO_COMPUTED_FLOOR)}),
         {-0.0},
         "0x8000000000000000"},
        {"ceil(-0.5)",
         program({in(0), op(ECUIO_COMPUTED_CEIL)}),
         {-0.5},
         "0x8000000000000000"},
        {"min(-0, +0)",
         program({in(0), in(1), MIN}),
         {-0.0, 0.0},
         "0x8000000000000000"},
        {"max(+0, -0)",
         program({in(0), in(1), MAX}),
         {0.0, -0.0},
         "0x0000000000000000"},
        {"abs(-0)",
         program({in(0), op(ECUIO_COMPUTED_ABS)}),
         {-0.0},
         "0x0000000000000000"},
        {"sqrt(-0)", program({in(0), SQRT}), {-0.0}, "0x8000000000000000"},
        {"1e308 * 10, an overflow",
         program({in(0), in(1), MUL}),
         {1e308, 10},
         "none"},
        {"sqrt(x * x + y * y), an overflow on the way",
         program({in(0), in(0), MUL, in(1), in(1), MUL, ADD, SQRT}),
         {3e200, 4e200},
         "none"},
        {"sqrt(x * x + y * y)",
         program({in(0), in(0), MUL, in(1), in(1), MUL, ADD, SQRT}),
         {0.3, 0.4},
         "0x3FE0000000000000"},
        {"if(rpm > 0, maf * 60 / rpm, 0)",
         program({in(0), k(0), GT, in(1), k(1), MUL, in(0), DIV, k(0), IF},
                 {0.0, 60.0}),
         {800, 12.3},
         "0x3FED851EB851EB85"},
        {"if(rpm > 0, maf * 60 / rpm, 0) at 0 rpm",
         program({in(0), k(0), GT, in(1), k(1), MUL, in(0), DIV, k(0), IF},
                 {0.0, 60.0}),
         {0, 12.3},
         "0x0000000000000000"},
        {"max of four",
         program({in(0), in(1), MAX, in(2), MAX, in(3), MAX}),
         {3.25, -1, 7.5, 7.25},
         "0x401E000000000000"},
        {"clamp(x, 0, 100)",
         program({in(0), k(0), MAX, k(1), MIN}, {0.0, 100.0}),
         {104.7},
         "0x4059000000000000"},
        {"1 / x at an infinite x, format 1",
         program({in(1), in(0), DIV}),
         {std::numeric_limits<double>::infinity(), 1},
         "0x0000000000000000"},
        {"(boost + baro) / baro, format 1",
         program({in(0), in(1), ADD, in(1), DIV}),
         {139.7, 101.3},
         "0x40030856EF116FF4"},
        {"map - baro with baro past its bound, format 1",
         program({in(0), in(1), SUB}),
         {139.7, kNone},
         "none"},
    };
    for (const Row& r : rows) {
        ASSERT_TRUE(ecuio_computed_check(r.p)) << r.what;
        EXPECT_EQ(run(r.p, r.in[0], r.in[1], r.in[2], r.in[3]), r.bits)
            << r.what;
    }
}

} // namespace
