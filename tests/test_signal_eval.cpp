// File/layer: tests/test_signal_eval.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalExprTest (test_signals)
//
// A program worked out over a file's set from given values (ADR-0062):
// ecuio_signal_file_evaluate held to the frame decoder's own values, the
// number defs under a program, and what each says when it has no value.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include "frame_decoder.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

struct FileFree {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};
using File = std::unique_ptr<ecuio_signal_file_t, FileFree>;

File load(const std::string& json)
{
    ecuio_signal_file_t* f = nullptr;
    if (ecuio_signal_file_load(json.data(), json.size(), &f) != ECUIO_OK) {
        EXPECT_EQ(f, nullptr);
    }
    return File(f);
}

std::string computed(const std::string& key, const std::string& expr,
                     const std::string& more = "")
{
    return "{\"key\":\"" + key + "\",\"name\":\"N " + key +
           "\",\"kind\":\"computed\",\"expr\":\"" + expr + "\"" + more + "}";
}

/* Four one-byte numbers, a to d, on DIDs D100 to D103 of 7E8, then the
 * signals given */
std::string file_of(const std::vector<std::string>& signals,
                    const std::string& head = "")
{
    std::string s = "{\"havne_signals\":2,\"name\":\"T\","
                    "\"source\":{\"name\":\"t\"}," +
                    head +
                    "\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],\"signals\":[";
    const char* leaves = "abcd";
    for (int i = 0; i < 4; ++i) {
        char one[160];
        std::snprintf(
            one, sizeof one,
            "%s{\"key\":\"%c\",\"name\":\"%c\",\"read\":{\"service\":\"22\","
            "\"number\":\"D10%d\"},\"field\":{\"byte\":0}}",
            i == 0 ? "" : ",", leaves[i], leaves[i] - 'a' + 'A', i);
        s += one;
    }
    for (const std::string& one : signals) {
        s += "," + one;
    }
    return s + "]}";
}

uint16_t id_of(const ecuio_signal_file_t* f, const char* key)
{
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key);
    return def != nullptr ? def->id : uint16_t{0};
}

const ecuio_computed_t& program_of(const ecuio_signal_file_t* f,
                                   const char* key)
{
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key);
    return ecuio_signal_file_set(f)->computed[def->table_first];
}

/* An expression of f compiled as a new signal's */
ecuio_computed_t compiled(const ecuio_signal_file_t* f, const std::string& expr)
{
    ecuio_expr_check_t out{};
    out.struct_size = sizeof out;
    EXPECT_EQ(ecuio_signal_file_check_expr(f, nullptr, expr.data(), expr.size(),
                                           &out),
              ECUIO_OK)
        << expr << ": " << ecuio_last_error_detail();
    return out.program;
}

ecuio_eval_input_t input(uint16_t id, double value, uint64_t age_us = 0)
{
    ecuio_eval_input_t in{};
    in.def_id = id;
    in.value = value;
    in.age_us = age_us;
    return in;
}

/* What an evaluation answered, its reserved room checked */
ecuio_eval_result_t evaluated(const ecuio_signal_file_t* f,
                              const ecuio_computed_t& program,
                              const std::vector<ecuio_eval_input_t>& inputs)
{
    ecuio_eval_result_t out;
    std::memset(&out, 0xA5, sizeof out);
    out.struct_size = sizeof out;
    const ecuio_status_t st = ecuio_signal_file_evaluate(
        f, &program, inputs.empty() ? nullptr : inputs.data(),
        static_cast<uint32_t>(inputs.size()), &out);
    EXPECT_EQ(st, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(out.struct_size, sizeof out);
    EXPECT_EQ(out.reserved0, 0u);
    for (const uint32_t r : out.reserved) {
        EXPECT_EQ(r, 0u);
    }
    EXPECT_EQ(out.flags & ~ECUIO_EVAL_VALUE, 0u);
    if (out.flags & ECUIO_EVAL_VALUE) {
        EXPECT_EQ(out.why, ECUIO_EVAL_WHY_NONE);
        EXPECT_EQ(out.leaf, 0u);
        EXPECT_EQ(out.bound_us, 0u);
        EXPECT_TRUE(std::isfinite(out.value));
    } else {
        EXPECT_NE(out.why, ECUIO_EVAL_WHY_NONE);
        EXPECT_EQ(out.value, 0.0);
        EXPECT_EQ(out.leaf != 0, out.why == ECUIO_EVAL_WHY_MISSING ||
                                     out.why == ECUIO_EVAL_WHY_STALE);
        EXPECT_EQ(out.bound_us != 0, out.why == ECUIO_EVAL_WHY_STALE);
    }
    return out;
}

/* The number defs under a program, through the two-call contract */
std::vector<uint16_t> leaves_of(const ecuio_signal_file_t* f,
                                const ecuio_computed_t& program)
{
    uint32_t count = 99;
    EXPECT_EQ(ecuio_signal_file_expr_leaves(f, &program, nullptr, 0, &count),
              ECUIO_OK)
        << ecuio_last_error_detail();
    std::vector<uint16_t> ids(count + 1, 0xFFFF);
    uint32_t again = 0;
    EXPECT_EQ(
        ecuio_signal_file_expr_leaves(f, &program, ids.data(), count, &again),
        ECUIO_OK);
    EXPECT_EQ(again, count);
    EXPECT_EQ(ids[count], 0xFFFF) << "nothing past the capacity";
    ids.resize(count);
    return ids;
}

/* A reply of 7E8 carrying one of the four DIDs' byte */
ecuio_frame_t reply(int leaf, uint8_t byte, uint64_t at_us)
{
    ecuio_frame_t f{};
    f.arbitration_id = 0x7E8;
    f.timestamp_us = at_us;
    f.data_length = 4;
    f.data[0] = 0x62;
    f.data[1] = 0xD1;
    f.data[2] = static_cast<uint8_t>(leaf);
    f.data[3] = byte;
    return f;
}

std::string bits(double v)
{
    uint64_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    char text[24];
    std::snprintf(text, sizeof text, "%016llx",
                  static_cast<unsigned long long>(b));
    return text;
}

const uint64_t kSecond = 1000000;

} // namespace

// The evaluation is the decoder's: a set with chains, age bounds, a
// division that meets zero, a root below zero and an if() decoded frame by
// frame, and at every frame each computed def one of the frame's values
// feeds worked out from the same values and ages: a value where the
// decoder gave one, with the same bits, and none where it gave none
TEST(SignalExprTest, AnEvaluationGivesTheDecodersValueAtEveryFrame)
{
    File f = load(file_of({
        computed("sum", "a + b", ",\"max_age\":{\"a\":1}"),
        computed("ratio", "sum / (c - 5)"),
        computed("pick", "if(a > 100, d, ratio) * 1.5",
                 ",\"max_age\":{\"d\":0.5}"),
        computed("root", "sqrt(c - 10) + pick"),
        computed("wide", "max(a, b) - min(sum, c)",
                 ",\"max_age\":{\"b\":10,\"c\":60}"),
        computed("top", "root + wide + ratio"),
    }));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const char* const keys[] = {"sum", "ratio", "pick", "root", "wide", "top"};
    const uint16_t leaf_ids[4] = {id_of(f.get(), "a"), id_of(f.get(), "b"),
                                  id_of(f.get(), "c"), id_of(f.get(), "d")};
    ecuio_frame_decoder decoder;
    decoder.prepare(set, false);

    /* Steps that land on, just past and far past each bound */
    const uint64_t steps[] = {
        1,           1000,        250000,      500000,          500001,
        kSecond,     kSecond + 1, 3 * kSecond, 3 * kSecond + 1, 10 * kSecond,
        61 * kSecond};
    /* Bytes that meet the zero divisor (5), the negative root (under 10)
     * and both sides of the if (100) */
    const uint8_t values[] = {0, 5, 5, 9, 10, 11, 50, 100, 101, 200, 255};
    std::mt19937 rng(20261116u);
    uint64_t now = 0;
    bool seen[4] = {};
    double newest[4] = {};
    uint64_t when[4] = {};
    uint32_t with_value = 0;
    uint32_t without = 0;
    std::map<uint32_t, uint32_t> whys;
    for (int n = 0; n < 4000; ++n) {
        now += steps[rng() % (sizeof steps / sizeof steps[0])];
        const int leaf = static_cast<int>(rng() % 4);
        const uint8_t byte = values[rng() % sizeof values];
        const ecuio_frame_t frame = reply(leaf, byte, now);
        const uint32_t count = decoder.decode(&frame);
        ASSERT_GE(count, 1u);
        ASSERT_EQ(decoder.values[0].signal_id, leaf_ids[leaf]);
        seen[leaf] = true;
        newest[leaf] = decoder.values[0].value;
        when[leaf] = now;
        std::vector<ecuio_eval_input_t> inputs;
        for (int k = 0; k < 4; ++k) {
            if (seen[k]) {
                inputs.push_back(input(leaf_ids[k], newest[k], now - when[k]));
            }
        }
        for (const char* key : keys) {
            const ecuio_computed_t& program = program_of(f.get(), key);
            /* The decoder gives a computed value only at a frame that
             * feeds it */
            bool fed = false;
            for (const uint16_t id : leaves_of(f.get(), program)) {
                fed = fed || id == leaf_ids[leaf];
            }
            const ecuio_value_t* given = nullptr;
            for (uint32_t v = 1; v < count; ++v) {
                if (decoder.values[v].signal_id == id_of(f.get(), key)) {
                    given = &decoder.values[v];
                }
            }
            ASSERT_TRUE(fed || given == nullptr) << key;
            if (!fed) {
                continue;
            }
            const ecuio_eval_result_t out = evaluated(f.get(), program, inputs);
            ASSERT_EQ((out.flags & ECUIO_EVAL_VALUE) != 0, given != nullptr)
                << key << " at frame " << n << ", why " << out.why;
            if (given != nullptr) {
                ASSERT_EQ(bits(out.value), bits(given->value)) << key;
                ++with_value;
            } else {
                ++without;
                ++whys[out.why];
            }
        }
    }
    /* Each outcome was met often enough to mean something */
    EXPECT_GT(with_value, 1000u);
    EXPECT_GT(without, 1000u);
    EXPECT_GT(whys[ECUIO_EVAL_WHY_MISSING], 0u);
    EXPECT_GT(whys[ECUIO_EVAL_WHY_STALE], 100u);
    EXPECT_GT(whys[ECUIO_EVAL_WHY_NO_NUMBER], 100u);
}

// With no value, the result says why: the first leaf read without one,
// and the bound a stale one is past; with every leaf in, the steps
TEST(SignalExprTest, AnEvaluationWithNoValueSaysWhy)
{
    File f = load(file_of({
        computed("sum", "a + b", ",\"max_age\":{\"a\":1}"),
        computed("over", "sum / (c - 5)"),
    }));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const uint16_t a = id_of(f.get(), "a");
    const uint16_t b = id_of(f.get(), "b");
    const uint16_t c = id_of(f.get(), "c");
    const uint16_t d = id_of(f.get(), "d");
    const ecuio_computed_t& sum = program_of(f.get(), "sum");
    const ecuio_computed_t& over = program_of(f.get(), "over");

    ecuio_eval_result_t out =
        evaluated(f.get(), sum, {input(a, 2), input(b, 3)});
    EXPECT_EQ(out.flags, ECUIO_EVAL_VALUE);
    EXPECT_EQ(out.value, 5.0);
    /* Missing: none given, the first read; then the second */
    out = evaluated(f.get(), sum, {});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, a);
    out = evaluated(f.get(), sum, {input(a, 2)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, b);
    /* A NaN is no value given; an infinity is a value, judged by the steps */
    out = evaluated(
        f.get(), sum,
        {input(a, std::numeric_limits<double>::quiet_NaN()), input(b, 3)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, a);
    /* And it is the first given for its def: a second one doesn't count */
    out = evaluated(f.get(), sum,
                    {input(a, std::numeric_limits<double>::quiet_NaN()),
                     input(a, 2), input(b, 3)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, a);
    out = evaluated(
        f.get(), sum,
        {input(a, std::numeric_limits<double>::infinity()), input(b, 3)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_NO_NUMBER);
    /* Stale: a's own bound of a second, b's default of three; a value at
     * its bound counts */
    out = evaluated(f.get(), sum,
                    {input(a, 2, kSecond), input(b, 3, 3 * kSecond)});
    EXPECT_EQ(out.value, 5.0);
    out = evaluated(f.get(), sum, {input(a, 2, kSecond + 1), input(b, 3)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_STALE);
    EXPECT_EQ(out.leaf, a);
    EXPECT_EQ(out.bound_us, kSecond);
    out = evaluated(f.get(), sum, {input(a, 2), input(b, 3, 3 * kSecond + 1)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_STALE);
    EXPECT_EQ(out.leaf, b);
    EXPECT_EQ(out.bound_us, uint64_t{ECUIO_VALUE_STALE_US});
    /* Through a chain the leaves keep the bounds of the programs that
     * read them, and the first without a value is named */
    out = evaluated(f.get(), over,
                    {input(a, 2, kSecond + 1), input(b, 3), input(c, 7)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_STALE);
    EXPECT_EQ(out.leaf, a);
    out = evaluated(f.get(), over, {input(a, 2), input(b, 3)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, c);
    out = evaluated(f.get(), over, {input(a, 2), input(b, 3), input(c, 7)});
    EXPECT_EQ(out.value, 2.5);
    /* Every leaf in and still no number: the steps */
    out = evaluated(f.get(), over, {input(a, 2), input(b, 3), input(c, 5)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_NO_NUMBER);
    EXPECT_EQ(out.leaf, 0u);

    /* An if's branch not taken needs no value, and a leaf only it reads
     * is not why */
    const ecuio_computed_t pick = compiled(f.get(), "if(a > 10, b, c)");
    out = evaluated(f.get(), pick, {input(a, 11), input(b, 3)});
    EXPECT_EQ(out.value, 3.0);
    out = evaluated(f.get(), pick, {input(a, 9), input(b, 3)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, c);
    /* The rule is the first leaf read without a value, whatever branch
     * wanted it */
    out = evaluated(f.get(), pick, {input(a, 9), input(c, 4)});
    EXPECT_EQ(out.value, 4.0);

    /* What the program doesn't read is passed by; of two for one def the
     * first counts */
    out = evaluated(f.get(), sum,
                    {input(d, 99), input(60000, 1), input(a, 2), input(a, 50),
                     input(b, 3), input(b, 70, 10 * kSecond)});
    EXPECT_EQ(out.value, 5.0);
    /* A caller's own age bound on a checked program */
    ecuio_computed_t aged = compiled(f.get(), "a * 2");
    EXPECT_EQ(evaluated(f.get(), aged, {input(a, 4, 3 * kSecond)}).value, 8.0);
    aged.max_age_us[0] = 100000;
    out = evaluated(f.get(), aged, {input(a, 4, 100001)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_STALE);
    EXPECT_EQ(out.bound_us, 100000u);
    EXPECT_EQ(evaluated(f.get(), aged, {input(a, 4, 100000)}).value, 8.0);
}

// The number defs under a program, each once, in the order they are
// first read through any chain; a base set's program the same
TEST(SignalExprTest, ALeafIsEachNumberDefUnderAProgram)
{
    File f = load(file_of(
        {
            computed("left", "c + a"),
            computed("right", "a * d"),
            computed("both", "right - left + b"),
        },
        "\"extends\":\"obd2\","));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const uint16_t a = id_of(f.get(), "a");
    const uint16_t b = id_of(f.get(), "b");
    const uint16_t c = id_of(f.get(), "c");
    const uint16_t d = id_of(f.get(), "d");
    EXPECT_EQ(leaves_of(f.get(), program_of(f.get(), "left")),
              (std::vector<uint16_t>{c, a}));
    EXPECT_EQ(leaves_of(f.get(), program_of(f.get(), "both")),
              (std::vector<uint16_t>{a, d, c, b}));
    EXPECT_EQ(leaves_of(f.get(), compiled(f.get(), "both + left + d")),
              (std::vector<uint16_t>{a, d, c, b}));
    EXPECT_EQ(leaves_of(f.get(), compiled(f.get(), "obd2.rpm / b")),
              (std::vector<uint16_t>{ECUIO_OBD2_SIG_RPM_ID, b}));

    /* obd2's Boost, a program of the set's own with no expression */
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_def_t* boost = nullptr;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == ECUIO_OBD2_SIG_BOOST_ID) {
            boost = &set->defs[i];
        }
    }
    ASSERT_NE(boost, nullptr);
    const ecuio_computed_t& program = set->computed[boost->table_first];
    const std::vector<uint16_t> under = leaves_of(f.get(), program);
    ASSERT_EQ(under.size(), 2u);
    ecuio_eval_result_t out = evaluated(
        f.get(), program, {input(under[0], 180), input(under[1], 100)});
    EXPECT_EQ(out.value, 80.0);

    /* The count alone, and room for fewer than there are */
    uint32_t count = 0;
    uint16_t two[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    EXPECT_EQ(ecuio_signal_file_expr_leaves(
                  f.get(), &program_of(f.get(), "both"), two, 2, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(two[0], a);
    EXPECT_EQ(two[1], d);
    EXPECT_EQ(two[2], 0xFFFF);
}

// A program a caller made is judged before it is run: what the set's
// validator would refuse of it in this set is refused here, in words
TEST(SignalExprTest, AProgramTheSetWouldRefuseIsRefused)
{
    const std::string text =
        R"({"key":"t","name":"T","kind":"text","read":{"service":"22",)"
        R"("number":"D110"},"field":{"byte":0}})";
    const std::string hex =
        R"({"key":"h","name":"H","kind":"hex","read":{"service":"22",)"
        R"("number":"D111"},"field":{"byte":0}})";
    std::vector<std::string> signals = {text, hex, computed("e", "a + 1")};
    for (int k = 1; k < 8; ++k) {
        signals.push_back(
            computed("e" + std::to_string(k),
                     (k == 1 ? std::string("e") : "e" + std::to_string(k - 1)) +
                         " + 1"));
    }
    File f = load(file_of(signals));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_computed_t good = compiled(f.get(), "a + e");
    const auto refused = [&](const ecuio_computed_t& program) {
        ecuio_eval_result_t out;
        std::memset(&out, 0xA5, sizeof out);
        out.struct_size = sizeof out;
        const ecuio_eval_result_t before = out;
        const ecuio_eval_input_t in = input(id_of(f.get(), "a"), 1);
        EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &program, &in, 1, &out),
                  ECUIO_ERR_INVALID_ARGUMENT);
        const std::string why = ecuio_last_error_detail();
        EXPECT_EQ(std::memcmp(&out, &before, sizeof out), 0) << why;
        uint32_t count = 77;
        uint16_t ids[8];
        EXPECT_EQ(
            ecuio_signal_file_expr_leaves(f.get(), &program, ids, 8, &count),
            ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_EQ(count, 77u);
        EXPECT_EQ(why, ecuio_last_error_detail());
        return why;
    };
    ecuio_computed_t p = good;
    p.steps[p.step_count - 1] = 0xFF;
    EXPECT_EQ(refused(p), "the program fails the validator's check of its "
                          "counts, steps, constants and age bounds");
    p = good;
    p.input_count = 0;
    EXPECT_EQ(refused(p), "the program fails the validator's check of its "
                          "counts, steps, constants and age bounds");
    p = good;
    p.reserved[0] = 1;
    EXPECT_EQ(refused(p), "the program fails the validator's check of its "
                          "counts, steps, constants and age bounds");
    p = good;
    p.input_ids[0] = 12345;
    EXPECT_EQ(refused(p), "the program reads ID 12345, which no def of the "
                          "file's set has");
    p = good;
    p.input_ids[0] = id_of(f.get(), "t");
    EXPECT_EQ(refused(p), "the program reads 'T', which is text");
    p.input_ids[0] = id_of(f.get(), "h");
    EXPECT_EQ(refused(p), "the program reads 'H', which is text");
    p = good;
    p.max_age_us[1] = 1000000;
    EXPECT_EQ(refused(p), "the program keeps an age bound on 'N e', a computed "
                          "signal: its own inputs carry their ages");
    p = good;
    p.max_age_us[0] = 99;
    EXPECT_EQ(refused(p), "the program fails the validator's check of its "
                          "counts, steps, constants and age bounds");
    p = good;
    p.input_ids[1] = id_of(f.get(), "e7");
    EXPECT_EQ(refused(p), "the program sits 9 computed signals deep: a chain "
                          "goes 8 deep at most");
    p.input_ids[1] = id_of(f.get(), "e6");
    EXPECT_EQ(evaluated(f.get(), p, {input(id_of(f.get(), "a"), 1)}).value,
              1.0 + 8.0);
    /* The deepest input counts, wherever it is among them */
    p = compiled(f.get(), "e + e6");
    EXPECT_EQ(evaluated(f.get(), p, {input(id_of(f.get(), "a"), 1)}).value,
              2.0 + 8.0);
    p.input_ids[1] = id_of(f.get(), "e7");
    EXPECT_EQ(refused(p), "the program sits 9 computed signals deep: a chain "
                          "goes 8 deep at most");

    /* The arguments themselves */
    ecuio_eval_result_t out{};
    out.struct_size = sizeof out;
    const ecuio_eval_input_t in = input(id_of(f.get(), "a"), 1);
    uint32_t count = 0;
    EXPECT_EQ(ecuio_signal_file_evaluate(nullptr, &good, &in, 1, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), nullptr, &in, 1, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &good, nullptr, 1, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &good, &in, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &good, nullptr, 0, &out),
              ECUIO_OK);
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    out.struct_size = sizeof out - 1;
    EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &good, &in, 1, &out),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the result's struct_size is 47, under its 48 bytes");
    EXPECT_EQ(ecuio_signal_file_expr_leaves(nullptr, &good, nullptr, 0, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_signal_file_expr_leaves(f.get(), nullptr, nullptr, 0, &count),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_signal_file_expr_leaves(f.get(), &good, nullptr, 0, nullptr),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_expr_leaves(f.get(), &good, nullptr, 2, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    /* A larger struct, a later core's caller */
    struct Grown {
        ecuio_eval_result_t result;
        uint32_t later[2];
    } grown;
    std::memset(&grown, 0xA5, sizeof grown);
    grown.result.struct_size = sizeof grown;
    EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &good, &in, 1, &grown.result),
              ECUIO_OK);
    EXPECT_EQ(grown.result.struct_size, sizeof(ecuio_eval_result_t));
    EXPECT_EQ(grown.result.value, 3.0);
    EXPECT_EQ(grown.later[0], 0xA5A5A5A5u);
}

// A diamond of chains, each def read by four above it, eight levels deep
TEST(SignalExprTest, ADiamondOfChainsWorksOutToItsValue)
{
    std::vector<std::string> signals;
    const auto key = [](int level, int k) {
        return "l" + std::to_string(level) + "_" + std::to_string(k);
    };
    for (int level = 1; level <= 7; ++level) {
        for (int k = 0; k < 4; ++k) {
            std::string expr;
            for (int j = 0; j < 4; ++j) {
                expr += (j == 0 ? "" : " + ") +
                        (level == 1 ? std::string(1, static_cast<char>('a' + j))
                                    : key(level - 1, j));
            }
            signals.push_back(computed(key(level, k), expr));
        }
    }
    File f = load(file_of(signals));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_computed_t top =
        compiled(f.get(), key(7, 0) + " + " + key(7, 1) + " + " + key(7, 2) +
                              " + " + key(7, 3));
    EXPECT_EQ(leaves_of(f.get(), top).size(), 4u);
    const ecuio_eval_result_t out = evaluated(
        f.get(), top,
        {input(id_of(f.get(), "a"), 1), input(id_of(f.get(), "b"), 1),
         input(id_of(f.get(), "c"), 1), input(id_of(f.get(), "d"), 1)});
    /* 4 at the leaves' sum, times 4 a level over eight levels */
    EXPECT_EQ(out.value, 4.0 * 16384.0);
}

// The four calls only read the file: two threads over one file agree
// with one
TEST(SignalExprTest, TwoThreadsEvaluateOneFileAtOnce)
{
    File f = load(file_of(
        {
            computed("sum", "a + b", ",\"max_age\":{\"a\":1}"),
            computed("over", "sum / (c - 5)"),
        },
        "\"extends\":\"obd2\","));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_computed_t& over = program_of(f.get(), "over");
    const std::vector<uint16_t> want = leaves_of(f.get(), over);
    const std::string expr = "max(over, obd2.rpm) - d";
    std::atomic<int> wrong{0};
    const auto run = [&](int turn) {
        for (int i = 0; i < 400; ++i) {
            ecuio_expr_check_t checked{};
            checked.struct_size = sizeof checked;
            if (ecuio_signal_file_check_expr(f.get(), "sum", expr.data(),
                                             expr.size(), &checked) !=
                    ECUIO_ERR_INVALID_ARGUMENT ||
                std::string(ecuio_last_error_detail()).find("in a circle") ==
                    std::string::npos) {
                ++wrong;
            }
            if (ecuio_signal_file_check_expr(f.get(), nullptr, expr.data(),
                                             expr.size(),
                                             &checked) != ECUIO_OK ||
                checked.depth != 3) {
                ++wrong;
            }
            char name[ECUIO_EXPR_REF_LEN];
            const ecuio_signal_def_t& def = set->defs[(i + turn) % set->count];
            (void)ecuio_signal_file_expr_ref(f.get(), &def, name, sizeof name);
            uint16_t ids[8] = {};
            uint32_t count = 0;
            if (ecuio_signal_file_expr_leaves(f.get(), &over, ids, 8, &count) !=
                    ECUIO_OK ||
                count != want.size() ||
                std::memcmp(ids, want.data(), count * sizeof ids[0]) != 0) {
                ++wrong;
            }
            const double c = 6 + (i + turn) % 5;
            const ecuio_eval_input_t in[3] = {input(id_of(f.get(), "a"), 2),
                                              input(id_of(f.get(), "b"), 3),
                                              input(id_of(f.get(), "c"), c)};
            ecuio_eval_result_t out{};
            out.struct_size = sizeof out;
            if (ecuio_signal_file_evaluate(f.get(), &over, in, 3, &out) !=
                    ECUIO_OK ||
                out.flags != ECUIO_EVAL_VALUE || out.value != 5.0 / (c - 5)) {
                ++wrong;
            }
        }
    };
    std::thread one(run, 0);
    std::thread two(run, 3);
    one.join();
    two.join();
    EXPECT_EQ(wrong.load(), 0);
}

// An input's flags and reserved word are zero: the room is a later
// core's, so this one refuses what it doesn't know, at any input given
TEST(SignalExprTest, AnInputWithAFlagOrAReservedWordSetIsRefused)
{
    File f = load(file_of({}));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_computed_t sum = compiled(f.get(), "a + b");
    const uint16_t a = id_of(f.get(), "a");
    const uint16_t b = id_of(f.get(), "b");
    ecuio_eval_result_t out;
    for (int which = 0; which < 4; ++which) {
        ecuio_eval_input_t in[2] = {input(a, 2), input(b, 3)};
        if (which < 2) {
            in[which].flags = 1u << 31;
        } else {
            in[which - 2].reserved0 = 1;
        }
        std::memset(&out, 0xA5, sizeof out);
        out.struct_size = sizeof out;
        const ecuio_eval_result_t before = out;
        EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &sum, in, 2, &out),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << which;
        EXPECT_EQ(std::memcmp(&out, &before, sizeof out), 0) << which;
        EXPECT_EQ(ecuio_last_error_detail(),
                  "the evaluation's input at index " +
                      std::to_string(which % 2) +
                      " has a nonzero flags or reserved0: no flag is defined");
    }
    /* An input past the count is not looked at */
    ecuio_eval_input_t in[2] = {input(a, 2), input(b, 3)};
    in[1].flags = 1;
    out = ecuio_eval_result_t{};
    out.struct_size = sizeof out;
    EXPECT_EQ(ecuio_signal_file_evaluate(f.get(), &sum, in, 1, &out), ECUIO_OK);
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, b);
}

// The leaf named is that of the first read without a value, in the order
// the reads are made: a leaf fresh within one program's bound can be
// stale at a later read within another's
TEST(SignalExprTest, TheLeafNamedIsThatOfTheFirstReadWithoutAValue)
{
    File f =
        load(file_of({computed("c1", "a + 0", ",\"max_age\":{\"a\":60}")}));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_computed_t p = compiled(f.get(), "c1 + b + a");
    const uint16_t a = id_of(f.get(), "a");
    const uint16_t b = id_of(f.get(), "b");
    EXPECT_EQ(leaves_of(f.get(), p), (std::vector<uint16_t>{a, b}));
    /* c1 reads a within its minute, then b is missing, though a comes
     * before b among the leaves and is stale where the program reads it */
    ecuio_eval_result_t out =
        evaluated(f.get(), p, {input(a, 2, 10 * kSecond)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_MISSING);
    EXPECT_EQ(out.leaf, b);
    out = evaluated(f.get(), p, {input(a, 2, 10 * kSecond), input(b, 3)});
    EXPECT_EQ(out.why, ECUIO_EVAL_WHY_STALE);
    EXPECT_EQ(out.leaf, a);
    EXPECT_EQ(out.bound_us, uint64_t{ECUIO_VALUE_STALE_US});
    out = evaluated(f.get(), p, {input(a, 2, 3 * kSecond), input(b, 3)});
    EXPECT_EQ(out.value, 7.0);
}
