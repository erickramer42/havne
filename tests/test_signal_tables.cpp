// File/layer: tests/test_signal_tables.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalTablesTest, ComputedTest (test_signals)

#include <gtest/gtest.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include "computed.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

ecuio_signal_def_t number_def(uint16_t id, uint32_t message_id,
                              uint16_t start = 0)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.kind = ECUIO_VALUE_UINT;
    d.message_id = message_id;
    d.start_byte = start;
    d.bit_length = 8;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "N%u", static_cast<unsigned>(id));
    return d;
}

ecuio_signal_def_t enum_def(uint16_t id, uint16_t first, uint16_t count)
{
    ecuio_signal_def_t d = number_def(id, 0x123, 1);
    d.kind = ECUIO_VALUE_ENUM;
    d.table_first = first;
    d.table_count = count;
    return d;
}

ecuio_signal_def_t computed_def(uint16_t id, uint16_t program)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.flags = ECUIO_DEF_FLAG_COMPUTED;
    d.scale = 1.0;
    d.table_first = program;
    std::snprintf(d.name, sizeof d.name, "C%u", static_cast<unsigned>(id));
    return d;
}

ecuio_enum_entry_t entry(uint32_t raw, const char* label)
{
    ecuio_enum_entry_t e{};
    e.raw = raw;
    std::snprintf(e.label, sizeof e.label, "%s", label);
    return e;
}

/* input 0 minus input 1, ages as given */
ecuio_computed_t difference(uint16_t a, uint16_t b, uint32_t age_b = 0)
{
    ecuio_computed_t c{};
    c.input_ids[0] = a;
    c.input_ids[1] = b;
    c.max_age_us[1] = age_b;
    c.input_count = 2;
    c.step_count = 3;
    c.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
    c.steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
    c.steps[2] = ECUIO_COMPUTED_SUBTRACT;
    return c;
}

/* A set with two numbers on 0x123 and 0x124, an enum of two labels
 * and one computed def (their difference): valid as built */
struct Tables {
    ecuio_signal_def_t defs[4];
    ecuio_enum_entry_t enums[2];
    ecuio_computed_t computed[1];
    ecuio_signal_set_t set{};

    Tables() { fresh(); }
    Tables(const Tables&) = delete;   /* the set points into this one */
    Tables& operator=(const Tables&) = delete;

    /* Back to the valid set, as built */
    void fresh()
    {
        set = ecuio_signal_set_t{};
        defs[0] = number_def(1, 0x123);
        defs[1] = number_def(2, 0x124);
        defs[2] = enum_def(3, 0, 2);
        defs[3] = computed_def(4, 0);
        enums[0] = entry(1, "Open loop");
        enums[1] = entry(2, "Closed loop");
        computed[0] = difference(1, 2);
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 4;
        set.defs = defs;
        set.enum_count = 2;
        set.enums = enums;
        set.computed_count = 1;
        set.computed = computed;
    }

    ecuio_validation_t check() const
    {
        ecuio_validation_t v{};
        ecuio_validate_signal_set(&set, &v);
        return v;
    }
};

} // namespace

TEST(SignalTablesTest, AnEnumDecodesItsRawNumberAndFindsItsLabel)
{
    const Tables t;
    ASSERT_EQ(t.check().code, ECUIO_VALIDATE_OK);
    const ecuio_signal_def_t& e = t.defs[2];
    const uint8_t payload[2] = {0x00, 0x02};
    double v = -1.0;
    ASSERT_EQ(ecuio_decode_signal(&e, payload, sizeof payload, &v),
              ECUIO_DECODE_OK);
    EXPECT_EQ(v, 2.0);
    EXPECT_STREQ(ecuio_enum_label(&t.set, &e, 1), "Open loop");
    EXPECT_STREQ(ecuio_enum_label(&t.set, &e, 2), "Closed loop");
    EXPECT_EQ(ecuio_enum_label(&t.set, &e, 0), nullptr) << "no label: its number";
    EXPECT_EQ(ecuio_enum_label(&t.set, &e, 3), nullptr);
    EXPECT_EQ(ecuio_enum_label(&t.set, &t.defs[0], 1), nullptr) << "not an enum";
    EXPECT_EQ(ecuio_enum_label(nullptr, &e, 1), nullptr);
    EXPECT_EQ(ecuio_enum_label(&t.set, nullptr, 1), nullptr);

    /* A binary search over each def's own range: a third label, one
     * def reading only the first two */
    ecuio_enum_entry_t three[3] = {entry(1, "A"), entry(5, "B"), entry(9, "C")};
    ecuio_signal_set_t s = t.set;
    s.enums = three;
    s.enum_count = 3;
    const ecuio_signal_def_t all = enum_def(3, 0, 3);
    const ecuio_signal_def_t two = enum_def(3, 0, 2);
    const ecuio_signal_def_t last = enum_def(3, 2, 1);
    EXPECT_STREQ(ecuio_enum_label(&s, &all, 9), "C");
    EXPECT_STREQ(ecuio_enum_label(&s, &all, 5), "B");
    EXPECT_EQ(ecuio_enum_label(&s, &two, 9), nullptr);
    EXPECT_STREQ(ecuio_enum_label(&s, &last, 9), "C");
    EXPECT_EQ(ecuio_enum_label(&s, &last, 1), nullptr);
}

TEST(SignalTablesTest, AnEnumIsUnscaledAndHoldsAtMost256Labels)
{
    ecuio_signal_def_t d = enum_def(1, 0, 0);
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK);
    d.scale = 0.5;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_ENUM);
    d = enum_def(1, 0, 0);
    d.offset = -40.0;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_ENUM);
    d = enum_def(1, 0, ECUIO_ENUM_MAX_ENTRIES);
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK);
    d.table_count = ECUIO_ENUM_MAX_ENTRIES + 1;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_ENUM);

    /* The field's rules are a number's */
    d = enum_def(1, 0, 0);
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.bit_length = 4;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BIG_ENDIAN);
    d = enum_def(1, 0, 0);
    d.bit_length = 0;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_BIT_LENGTH);
}

TEST(SignalTablesTest, AComputedDefReadsNoFrame)
{
    const ecuio_signal_def_t good = computed_def(1, 0);
    EXPECT_EQ(ecuio_validate_signal(&good), ECUIO_VALIDATE_OK);

    /* Each field that would read a frame, set, refuses it */
    const auto refused = [](void (*set)(ecuio_signal_def_t&), const char* what) {
        ecuio_signal_def_t d = computed_def(1, 0);
        set(d);
        EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_COMPUTED) << what;
    };
    refused([](ecuio_signal_def_t& d) { d.message_id = 0x7E8; }, "message_id");
    refused([](ecuio_signal_def_t& d) { d.mux_offset = 1; }, "mux_offset");
    refused([](ecuio_signal_def_t& d) { d.mux_length = 1; }, "mux_length");
    refused([](ecuio_signal_def_t& d) { d.mux_key = 1; }, "mux_key");
    refused([](ecuio_signal_def_t& d) { d.start_byte = 1; }, "start_byte");
    refused([](ecuio_signal_def_t& d) { d.bit_offset = 1; }, "bit_offset");
    refused([](ecuio_signal_def_t& d) { d.bit_length = 8; }, "bit_length");
    refused([](ecuio_signal_def_t& d) { d.byte_length = 1; }, "byte_length");
    refused([](ecuio_signal_def_t& d) { d.scale = 2.0; }, "scale");
    refused([](ecuio_signal_def_t& d) { d.offset = 1.0; }, "offset");
    refused([](ecuio_signal_def_t& d) { d.table_count = 1; }, "table_count");
    refused([](ecuio_signal_def_t& d) { d.kind = ECUIO_VALUE_INT; }, "kind");
    refused([](ecuio_signal_def_t& d) { d.byte_order = ECUIO_BYTE_ORDER_BE; },
            "byte_order");

    /* The one flag: any other bit is reserved */
    ecuio_signal_def_t d = computed_def(1, 0);
    d.flags |= 0x2u;
    EXPECT_EQ(ecuio_validate_signal(&d), ECUIO_VALIDATE_ERR_RESERVED);

    /* No frame carries it, so no decode and no frame applies, an ID 0
     * frame included */
    const uint8_t payload[8] = {};
    double v = 0.0;
    EXPECT_EQ(ecuio_decode_signal(&good, payload, sizeof payload, &v),
              ECUIO_DECODE_ERR_UNSUPPORTED);
    ecuio_frame_t f{};
    f.data_length = 8;
    EXPECT_EQ(ecuio_def_applies_to_frame(&good, &f), 0);
}

TEST(SignalTablesTest, TheTablesAreNullOnlyWhenEmpty)
{
    Tables t;
    t.set.enums = nullptr;
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_ERR_TABLES_NULL);
    EXPECT_EQ(t.check().index, UINT32_MAX);
    t.fresh();
    t.set.computed = nullptr;
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_ERR_TABLES_NULL);

    /* Past what a def's uint16_t table_first reaches: refused before
     * any entry is read */
    t.fresh();
    t.set.enum_count = ECUIO_SET_MAX_TABLE + 1;
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_ERR_TABLES_NULL);
    t.fresh();
    t.set.computed_count = ECUIO_SET_MAX_TABLE + 1;
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_ERR_TABLES_NULL);

    /* None at all: a layout 3 set's members, restated as 4 */
    ecuio_signal_set_t plain{};
    plain.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    plain.count = 2;
    plain.defs = t.defs;
    EXPECT_EQ(ecuio_validate_signal_set(&plain, nullptr), ECUIO_VALIDATE_OK);
}

TEST(SignalTablesTest, EachDefsRangeLiesInItsTable)
{
    Tables t;
    t.defs[2].table_count = 3;   /* one past the two entries */
    ecuio_validation_t v = t.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_TABLE_RANGE);
    EXPECT_EQ(v.index, 2u) << "the def";
    t.fresh();
    t.defs[2].table_first = 1;   /* entries 1 and 2: one past */
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_ERR_TABLE_RANGE);
    t.fresh();
    t.defs[2].table_first = 2;
    t.defs[2].table_count = 0;   /* an empty range at the end is in range */
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_OK);
    t.fresh();
    t.defs[3].table_first = 1;   /* the only program is 0 */
    v = t.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_TABLE_RANGE);
    EXPECT_EQ(v.index, 3u);

    /* A number's table fields stay reserved */
    t.fresh();
    t.defs[0].table_first = 1;
    v = t.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_RESERVED);
    EXPECT_EQ(v.index, 0u);
}

TEST(SignalTablesTest, EachLabelIsShortTerminatedAndNoFormula)
{
    const auto bad = [](void (*spoil)(ecuio_enum_entry_t&), const char* what) {
        Tables t;
        spoil(t.enums[1]);
        const ecuio_validation_t v = t.check();
        EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_ENUM_ENTRY) << what;
        EXPECT_EQ(v.index, 1u) << what << ": the entry";
    };
    bad([](ecuio_enum_entry_t& e) { std::memset(e.label, 0, sizeof e.label); },
        "empty");
    bad([](ecuio_enum_entry_t& e) { std::memset(e.label, 'x', sizeof e.label); },
        "not terminated");
    bad([](ecuio_enum_entry_t& e) { e.label[sizeof e.label - 1] = 'x'; },
        "a byte after the NUL");
    for (const char* start : {"=1+1", "+A", "-A", "@A", "\tA", "\rA"}) {
        Tables t;
        t.enums[1] = entry(2, start);
        EXPECT_EQ(t.check().code, ECUIO_VALIDATE_ERR_ENUM_ENTRY) << int(start[0]);
    }
    /* 27 bytes is the longest */
    Tables t;
    std::memset(t.enums[1].label, 'x', ECUIO_ENUM_LABEL_MAX);
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_OK);
}

TEST(SignalTablesTest, OneDefsRawValuesStrictlyAscend)
{
    Tables t;
    t.enums[1].raw = 1;   /* the same as the first */
    ecuio_validation_t v = t.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_ENUM_ENTRY);
    EXPECT_EQ(v.index, 1u);
    t.fresh();
    t.enums[0].raw = 3;   /* 3 then 2 */
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_ERR_ENUM_ENTRY);
    /* Two defs' ranges are apart: a second def starting lower is fine */
    t.fresh();
    t.defs[2].table_count = 1;
    t.defs[1] = enum_def(2, 1, 1);
    t.enums[1].raw = 0;
    EXPECT_EQ(t.check().code, ECUIO_VALIDATE_OK);
}

TEST(SignalTablesTest, AProgramMustRunAndReadNumbersOfTheSet)
{
    Tables t;
    t.computed[0].steps[2] = 0x30;   /* no such step */
    ecuio_validation_t v = t.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_PROGRAM);
    EXPECT_EQ(v.index, 0u) << "the program";

    const auto input = [](uint16_t id, const char* what) {
        Tables t;
        t.computed[0].input_ids[1] = id;
        const ecuio_validation_t v = t.check();
        EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_COMPUTED_INPUT) << what;
        EXPECT_EQ(v.index, 0u) << what;
    };
    input(99, "not a def of the set");
    Tables text;
    text.defs[1].kind = ECUIO_VALUE_TEXT;
    text.defs[1].bit_length = 0;
    EXPECT_EQ(text.check().code, ECUIO_VALIDATE_ERR_COMPUTED_INPUT) << "a text def";
    Tables en;
    en.computed[0].input_ids[1] = 3;
    EXPECT_EQ(en.check().code, ECUIO_VALIDATE_OK) << "an enum's number is a number";
}

namespace {

/* Number defs 1 and 2, then computed defs 101 to 100 + depth, each
 * reading the one below (101 reads def 1) plus def 2: a chain depth
 * deep. reversed lists the computed defs and their programs top first. */
struct Chain {
    ecuio_signal_def_t defs[2 + 16];
    ecuio_computed_t computed[16];
    ecuio_signal_set_t set{};

    explicit Chain(uint32_t depth, bool reversed = false)
    {
        defs[0] = number_def(1, 0x123);
        defs[1] = number_def(2, 0x124);
        for (uint32_t k = 0; k < depth; ++k) {
            /* level k + 1, in the place reversed puts it */
            const uint32_t at = reversed ? depth - 1 - k : k;
            const uint16_t below = k == 0 ? 1 : static_cast<uint16_t>(100 + k);
            defs[2 + at] = computed_def(static_cast<uint16_t>(101 + k),
                                        static_cast<uint16_t>(at));
            computed[at] = difference(below, 2, 0);
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 2 + depth;
        set.defs = defs;
        set.computed_count = depth;
        set.computed = computed;
    }
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    ecuio_validation_t check() const
    {
        ecuio_validation_t v{};
        ecuio_validate_signal_set(&set, &v);
        return v;
    }
};

} // namespace

/* ADR-0062: a computed def may read another, ECUIO_COMPUTED_MAX_DEPTH
 * deep at most, listed in any order */
TEST(SignalTablesTest, AChainPassesToItsDepthInAnyOrder)
{
    for (const bool reversed : {false, true}) {
        const Chain at(ECUIO_COMPUTED_MAX_DEPTH, reversed);
        EXPECT_EQ(at.check().code, ECUIO_VALIDATE_OK) << reversed;
        const Chain past(ECUIO_COMPUTED_MAX_DEPTH + 1, reversed);
        const ecuio_validation_t v = past.check();
        EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_COMPUTED_CHAIN) << reversed;
        /* only the top level is past the depth: its program */
        EXPECT_EQ(v.index, reversed ? 0u : ECUIO_COMPUTED_MAX_DEPTH)
            << reversed;
        EXPECT_EQ(v.reserved[0], 0u);
    }
}

TEST(SignalTablesTest, ACircleIsRefusedAtTheFirstProgramItLeavesUnsettled)
{
    /* 101 reads 102 and 102 reads 101 */
    Chain two(2);
    two.computed[0].input_ids[0] = 102;
    ecuio_validation_t v = two.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_COMPUTED_CHAIN);
    EXPECT_EQ(v.index, 0u);

    /* one reading itself */
    Chain self(1);
    self.computed[0].input_ids[0] = 101;
    v = self.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_COMPUTED_CHAIN);
    EXPECT_EQ(v.index, 0u);

    /* 101 reads the circle of 102 and 103, listed first: it is the first
     * program left unsettled */
    Chain reader(3);
    reader.computed[0].input_ids[0] = 102;
    reader.computed[1].input_ids[0] = 103;
    reader.computed[2].input_ids[0] = 102;
    v = reader.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_COMPUTED_CHAIN);
    EXPECT_EQ(v.index, 0u);
}

TEST(SignalTablesTest, AComputedInputTakesNoAgeBound)
{
    Chain c(2);
    c.computed[1].max_age_us[0] = 1000000; /* 102's input 101 */
    const ecuio_validation_t v = c.check();
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_COMPUTED_INPUT);
    EXPECT_EQ(v.index, 1u);
    c.computed[1].max_age_us[0] = 0;
    c.computed[1].max_age_us[1] = 1000000; /* def 2, a number: a bound */
    EXPECT_EQ(c.check().code, ECUIO_VALIDATE_OK);
}

/* The walk a focus takes: a computed def's leaves, down a whole chain,
 * the defs between them marked too, nothing else */
TEST(SignalTablesTest, TheLeavesWalkReachesTheNumbersUnderAChain)
{
    for (const bool reversed : {false, true}) {
        const Chain c(ECUIO_COMPUTED_MAX_DEPTH, reversed);
        ASSERT_EQ(c.check().code, ECUIO_VALIDATE_OK);
        uint8_t marks[2 + 16] = {};
        const uint32_t top = reversed ? 0u : ECUIO_COMPUTED_MAX_DEPTH - 1;
        marks[2 + top] = 1;
        ecuio_computed_mark_leaves(&c.set, marks);
        for (uint32_t i = 0; i < c.set.count; ++i) {
            EXPECT_EQ(marks[i], 1u) << "def " << i << " reversed " << reversed;
        }
        uint8_t none[2 + 16] = {};
        none[0] = 1; /* a number def marks nothing more */
        ecuio_computed_mark_leaves(&c.set, none);
        for (uint32_t i = 1; i < c.set.count; ++i) {
            EXPECT_EQ(none[i], 0u) << i;
        }
    }
    const Chain c(3);
    uint8_t mid[5] = {0, 0, 0, 1, 0}; /* 102: 101, defs 1 and 2, not 103 */
    ecuio_computed_mark_leaves(&c.set, mid);
    EXPECT_EQ(mid[0], 1u);
    EXPECT_EQ(mid[1], 1u);
    EXPECT_EQ(mid[2], 1u);
    EXPECT_EQ(mid[4], 0u);
}

TEST(ComputedTest, TheCheckerAcceptsAProgramThatRunsToOneValue)
{
    ecuio_computed_t c = difference(1, 2);
    EXPECT_TRUE(ecuio_computed_check(c));

    /* (in0 + 1.5) * in1 / 2 */
    ecuio_computed_t p{};
    p.input_ids[0] = 1;
    p.input_ids[1] = 2;
    p.input_count = 2;
    p.constant_count = 2;
    p.constants[0] = 1.5;
    p.constants[1] = 2.0;
    const uint8_t steps[] = {ECUIO_COMPUTED_PUSH_INPUT | 0u,
                             ECUIO_COMPUTED_PUSH_CONSTANT | 0u,
                             ECUIO_COMPUTED_ADD,
                             ECUIO_COMPUTED_PUSH_INPUT | 1u,
                             ECUIO_COMPUTED_MULTIPLY,
                             ECUIO_COMPUTED_PUSH_CONSTANT | 1u,
                             ECUIO_COMPUTED_DIVIDE};
    p.step_count = sizeof steps;
    std::memcpy(p.steps, steps, sizeof steps);
    EXPECT_TRUE(ecuio_computed_check(p));
    const double in[4] = {8.5, 3.0, 0.0, 0.0};
    double out = 0.0;
    ASSERT_TRUE(ecuio_computed_run(p, in, &out));
    EXPECT_DOUBLE_EQ(out, 15.0);

    /* The longest: 17 operands and 16 operations */
    ecuio_computed_t longest{};
    longest.input_ids[0] = 1;
    longest.input_count = 1;
    for (uint32_t i = 0; i < 17; ++i) {
        longest.steps[i] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
    }
    for (uint32_t i = 17; i < ECUIO_COMPUTED_MAX_STEPS; ++i) {
        longest.steps[i] = ECUIO_COMPUTED_ADD;
    }
    longest.step_count = ECUIO_COMPUTED_MAX_STEPS;
    EXPECT_TRUE(ecuio_computed_check(longest));
    const double one[4] = {2.0, 0.0, 0.0, 0.0};
    ASSERT_TRUE(ecuio_computed_run(longest, one, &out));
    EXPECT_DOUBLE_EQ(out, 34.0);
}

TEST(ComputedTest, TheCheckerRefusesAProgramThatDoesntRun)
{
    const auto refused = [](void (*spoil)(ecuio_computed_t&), const char* what) {
        ecuio_computed_t c = difference(1, 2);
        c.constant_count = 1;
        c.constants[0] = 2.0;
        ASSERT_TRUE(ecuio_computed_check(c));
        spoil(c);
        EXPECT_FALSE(ecuio_computed_check(c)) << what;
    };
    refused([](ecuio_computed_t& c) { c.input_count = 0; }, "no input");
    refused([](ecuio_computed_t& c) { c.input_count = 5; }, "five inputs");
    refused([](ecuio_computed_t& c) { c.step_count = 0; }, "no step");
    refused([](ecuio_computed_t& c) { c.step_count = ECUIO_COMPUTED_MAX_STEPS + 1; },
            "34 steps");
    refused([](ecuio_computed_t& c) { c.constant_count = 17; }, "17 constants");
    refused([](ecuio_computed_t& c) { c.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 2u; },
            "an input past the count");
    refused([](ecuio_computed_t& c) { c.steps[0] = ECUIO_COMPUTED_PUSH_CONSTANT | 1u; },
            "a constant past the count");
    refused([](ecuio_computed_t& c) { c.steps[2] = 0x2B; },
            "an unknown operation");
    refused([](ecuio_computed_t& c) { c.steps[1] = ECUIO_COMPUTED_ADD; },
            "an operation on one value");
    refused([](ecuio_computed_t& c) {
        c.step_count = 2;
        c.steps[2] = 0;
    }, "two values left");
    refused([](ecuio_computed_t& c) { c.steps[3] = ECUIO_COMPUTED_ADD; },
            "a step past the count");
    refused([](ecuio_computed_t& c) { c.constants[0] = std::nan(""); }, "a NaN");
    refused([](ecuio_computed_t& c) {
        c.constants[0] = std::numeric_limits<double>::infinity();
    }, "an infinity");
    refused([](ecuio_computed_t& c) { c.constants[1] = 1.0; },
            "a constant past the count");
    refused([](ecuio_computed_t& c) { c.constants[1] = -0.0; },
            "-0.0 past the count");
    refused([](ecuio_computed_t& c) { c.input_ids[2] = 7; }, "an id past the count");
    refused([](ecuio_computed_t& c) { c.max_age_us[2] = 200000; },
            "an age past the count");
    refused([](ecuio_computed_t& c) { c.max_age_us[1] = ECUIO_COMPUTED_MIN_AGE_US - 1; },
            "an age under 0.1 s");
    refused([](ecuio_computed_t& c) { c.max_age_us[1] = ECUIO_COMPUTED_MAX_AGE_US + 1; },
            "an age over 60 s");
    refused([](ecuio_computed_t& c) { c.reserved0 = 1; }, "reserved0");
    refused([](ecuio_computed_t& c) { c.reserved[3] = 1; }, "reserved");

    ecuio_computed_t c = difference(1, 2, ECUIO_COMPUTED_MIN_AGE_US);
    EXPECT_TRUE(ecuio_computed_check(c)) << "0.1 s";
    c.max_age_us[1] = ECUIO_COMPUTED_MAX_AGE_US;
    EXPECT_TRUE(ecuio_computed_check(c)) << "60 s";
}

TEST(ComputedTest, ADivisionByZeroOrAnInfinityGivesNoValue)
{
    ecuio_computed_t c = difference(1, 2);
    c.steps[2] = ECUIO_COMPUTED_DIVIDE;
    ASSERT_TRUE(ecuio_computed_check(c));
    double out = 7.0;
    const double by_zero[4] = {1.0, 0.0, 0.0, 0.0};
    EXPECT_FALSE(ecuio_computed_run(c, by_zero, &out));
    EXPECT_EQ(out, 7.0) << "untouched";
    const double fine[4] = {1.0, 4.0, 0.0, 0.0};
    ASSERT_TRUE(ecuio_computed_run(c, fine, &out));
    EXPECT_DOUBLE_EQ(out, 0.25);

    c.steps[2] = ECUIO_COMPUTED_MULTIPLY;
    const double huge[4] = {1e308, 1e308, 0.0, 0.0};
    EXPECT_FALSE(ecuio_computed_run(c, huge, &out)) << "an overflow";

    c.steps[2] = ECUIO_COMPUTED_SUBTRACT;
    const double boost[4] = {140.0, 101.0, 0.0, 0.0};
    ASSERT_TRUE(ecuio_computed_run(c, boost, &out));
    EXPECT_DOUBLE_EQ(out, 39.0) << "the second from the top minus the top";
}

// A step that isn't finite gives no value, though a later step would make
// it finite again
TEST(ComputedTest, AStepThatIsntFiniteGivesNoValue)
{
    const auto program = [](std::initializer_list<uint8_t> steps) {
        ecuio_computed_t p{};
        p.input_ids[0] = 1;
        p.input_count = 1;
        p.constant_count = 1;
        p.constants[0] = 1.0;
        p.step_count = static_cast<uint8_t>(steps.size());
        size_t k = 0;
        for (const uint8_t s : steps) {
            p.steps[k++] = s;
        }
        EXPECT_TRUE(ecuio_computed_check(p));
        return p;
    };
    const uint8_t one = ECUIO_COMPUTED_PUSH_CONSTANT | 0u;
    const uint8_t a = ECUIO_COMPUTED_PUSH_INPUT | 0u;
    /* 1 / (1 / a) */
    const ecuio_computed_t inverse =
        program({one, one, a, ECUIO_COMPUTED_DIVIDE, ECUIO_COMPUTED_DIVIDE});
    /* 1 / (a * a) */
    const ecuio_computed_t square =
        program({one, a, a, ECUIO_COMPUTED_MULTIPLY, ECUIO_COMPUTED_DIVIDE});
    double out = -1.0;
    const double zero[4] = {0.0, 0.0, 0.0, 0.0};
    EXPECT_FALSE(ecuio_computed_run(inverse, zero, &out)) << out;
    const double huge[4] = {1e200, 0.0, 0.0, 0.0};
    EXPECT_FALSE(ecuio_computed_run(square, huge, &out)) << out;
    const double two[4] = {2.0, 0.0, 0.0, 0.0};
    ASSERT_TRUE(ecuio_computed_run(inverse, two, &out));
    EXPECT_DOUBLE_EQ(out, 2.0);
    ASSERT_TRUE(ecuio_computed_run(square, two, &out));
    EXPECT_DOUBLE_EQ(out, 0.25);
}
