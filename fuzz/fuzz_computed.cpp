// File/layer: fuzz/fuzz_computed.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: arbitrary computed programs and inputs against the
//          checker and the stack machine (core/src/computed.h)
//
// The first byte picks the program's form: odd, the input's bytes are
// the struct as they come; even, a program built from them, each step
// byte mapped to a push or a listed operation (format 1's four alone
// with bit 1 set), so most pass the checker. Then four inputs, a byte
// saying which have no value; a value or constant is a small number,
// eight raw bytes or one of the edges (infinities, zeros, subnormals,
// the largest doubles, halves), where the machines could differ. The
// checker never faults and two calls agree; any program runs within
// its stack and two runs agree to the bit; a checked one gives no value
// or a finite one; and a program of format 1's steps alone is judged
// and run as the format 1 machine, kept here as the reference, does.
// Last, the program over a file's set (ecuio_signal_file_evaluate and
// ecuio_signal_file_expr_leaves, ADR-0062): its inputs named as the
// file's signals by a byte, each number given a value and an age; the
// two calls refuse alike, a refused program is never one the checker
// passed with every input a number, and one reading numbers alone gives
// what the machine gives over the values within their bounds.

#include "computed.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

/* Format 1's checker and machine as they were before format 2: four
 * operations, two values each, no value only from a step that isn't
 * finite */
bool ref_check(const ecuio_computed_t& program)
{
    const uint32_t inputs = program.input_count;
    const uint32_t steps = program.step_count;
    const uint32_t constants = program.constant_count;
    if (inputs < 1 || inputs > ECUIO_COMPUTED_MAX_INPUTS || steps < 1 ||
        steps > ECUIO_COMPUTED_MAX_STEPS ||
        constants > ECUIO_COMPUTED_MAX_CONSTANTS || program.reserved0 != 0) {
        return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (program.reserved[i] != 0) {
            return false;
        }
    }
    for (uint32_t i = 0; i < ECUIO_COMPUTED_MAX_INPUTS; ++i) {
        const uint32_t age = program.max_age_us[i];
        if (i >= inputs) {
            if (program.input_ids[i] != 0 || age != 0) {
                return false;
            }
        } else if (age != 0 && (age < ECUIO_COMPUTED_MIN_AGE_US ||
                                age > ECUIO_COMPUTED_MAX_AGE_US)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < ECUIO_COMPUTED_MAX_CONSTANTS; ++i) {
        const double c = program.constants[i];
        if (i >= constants ? (c != 0.0 || signbit(c)) : !isfinite(c)) {
            return false;
        }
    }
    for (uint32_t i = steps; i < sizeof program.steps; ++i) {
        if (program.steps[i] != 0) {
            return false;
        }
    }
    uint32_t depth = 0;
    for (uint32_t i = 0; i < steps; ++i) {
        const uint8_t s = program.steps[i];
        if ((s & 0xF0u) == ECUIO_COMPUTED_PUSH_INPUT) {
            if ((s & 0x0Fu) >= inputs) {
                return false;
            }
            ++depth;
        } else if ((s & 0xF0u) == ECUIO_COMPUTED_PUSH_CONSTANT) {
            if ((s & 0x0Fu) >= constants) {
                return false;
            }
            ++depth;
        } else if (s >= ECUIO_COMPUTED_ADD && s <= ECUIO_COMPUTED_DIVIDE) {
            if (depth < 2) {
                return false;
            }
            --depth;
        } else {
            return false;
        }
    }
    return depth == 1;
}

/* Runs a program ref_check passed */
bool ref_run(const ecuio_computed_t& program, const double inputs[4],
             double* out)
{
    double stack[(ECUIO_COMPUTED_MAX_STEPS + 1) / 2];
    uint32_t depth = 0;
    for (uint32_t i = 0; i < program.step_count; ++i) {
        const uint8_t s = program.steps[i];
        if ((s & 0xF0u) == ECUIO_COMPUTED_PUSH_INPUT) {
            stack[depth++] = inputs[s & 0x0Fu];
            continue;
        }
        if ((s & 0xF0u) == ECUIO_COMPUTED_PUSH_CONSTANT) {
            stack[depth++] = program.constants[s & 0x0Fu];
            continue;
        }
        const double b = stack[--depth];
        const double a = stack[depth - 1];
        double r = s == ECUIO_COMPUTED_ADD        ? a + b
                   : s == ECUIO_COMPUTED_SUBTRACT ? a - b
                   : s == ECUIO_COMPUTED_MULTIPLY ? a * b
                                                  : a / b;
        if (!isfinite(r)) {
            return false;
        }
        stack[depth - 1] = r;
    }
    if (depth != 1 || !isfinite(stack[0])) {
        return false;
    }
    *out = stack[0];
    return true;
}

/* Whether every step is a push or one of format 1's four operations */
bool format1_steps(const ecuio_computed_t& program)
{
    for (uint32_t i = 0; i < program.step_count && i < sizeof program.steps;
         ++i) {
        const uint8_t s = program.steps[i];
        if (s > ECUIO_COMPUTED_DIVIDE) {
            return false;
        }
    }
    return true;
}

const uint8_t kOperations[] = {
    ECUIO_COMPUTED_ADD,
    ECUIO_COMPUTED_SUBTRACT,
    ECUIO_COMPUTED_MULTIPLY,
    ECUIO_COMPUTED_DIVIDE,
    ECUIO_COMPUTED_MIN,
    ECUIO_COMPUTED_MAX,
    ECUIO_COMPUTED_ABS,
    ECUIO_COMPUTED_SQRT,
    ECUIO_COMPUTED_ROUND,
    ECUIO_COMPUTED_FLOOR,
    ECUIO_COMPUTED_CEIL,
    ECUIO_COMPUTED_LESS,
    ECUIO_COMPUTED_LESS_EQUAL,
    ECUIO_COMPUTED_GREATER,
    ECUIO_COMPUTED_GREATER_EQUAL,
    ECUIO_COMPUTED_EQUAL,
    ECUIO_COMPUTED_NOT_EQUAL,
    ECUIO_COMPUTED_ALL,
    ECUIO_COMPUTED_ANY,
    ECUIO_COMPUTED_NOT,
    ECUIO_COMPUTED_IF,
};

/* Where IEEE 754's corners are: an infinite input is format 1's value */
const double kEdges[] = {
    INFINITY,
    -INFINITY,
    0.0,
    -0.0,
    4.9406564584124654e-324,
    -4.9406564584124654e-324,
    1.7976931348623157e308,
    -1.7976931348623157e308,
    1e-300,
    1e300,
    0.5,
    -0.5,
    2.5,
    NAN,
};

/* The next byte, 0 past the end */
struct Reader {
    const uint8_t* p;
    size_t left;
    uint8_t byte()
    {
        if (left == 0) {
            return 0;
        }
        --left;
        return *p++;
    }
    double number()
    {
        uint8_t b[8];
        for (uint8_t& x : b) {
            x = byte();
        }
        double v = 0.0;
        memcpy(&v, b, sizeof v);
        return v;
    }
    /* An edge, eight raw bytes or a small number, by the next byte */
    double value()
    {
        const uint8_t b = byte();
        if ((b & 0xC0u) == 0xC0u) {
            return kEdges[(b & 0x3Fu) % (sizeof kEdges / sizeof kEdges[0])];
        }
        if ((b & 0x80u) != 0) {
            return number();
        }
        return static_cast<double>(static_cast<int8_t>(b << 1)) / 4;
    }
};

/* A program from the bytes: counts, then each step byte as a push (low
 * bits the index, sometimes past its count) or a listed operation (one
 * of format 1's four with format1), sometimes an unlisted value */
ecuio_computed_t built(Reader& r, bool format1)
{
    const uint32_t operations = format1 ? 4u : sizeof kOperations;
    ecuio_computed_t p{};
    p.input_count = static_cast<uint8_t>(1 + r.byte() % 4);
    for (uint32_t i = 0; i < p.input_count; ++i) {
        p.input_ids[i] = static_cast<uint16_t>(i + 1);
    }
    p.step_count =
        static_cast<uint8_t>(1 + r.byte() % ECUIO_COMPUTED_MAX_STEPS);
    p.constant_count =
        static_cast<uint8_t>(r.byte() % (ECUIO_COMPUTED_MAX_CONSTANTS + 1));
    for (uint32_t i = 0; i < p.step_count; ++i) {
        const uint8_t b = r.byte();
        switch (b % 4) {
        case 0:
            p.steps[i] = static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_INPUT |
                                              ((b >> 2) % 5));
            break;
        case 1:
            p.steps[i] = static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_CONSTANT |
                                              ((b >> 2) & 0x0Fu));
            break;
        default:
            p.steps[i] = b == 0xFF ? static_cast<uint8_t>(0x2B + (i % 5))
                                   : kOperations[(b >> 2) % operations];
            break;
        }
    }
    for (uint32_t i = 0; i < p.constant_count; ++i) {
        p.constants[i] = r.value();
    }
    return p;
}

bool same_bits(double a, double b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

/* Four numbers, a text, a computed signal over two of them and one over
 * that: every kind a program's input can name */
const ecuio_signal_file_t* the_file()
{
    static ecuio_signal_file_t* file = nullptr;
    if (file == nullptr) {
        static const char kText[] =
            "{\"havne_signals\":2,\"name\":\"F\",\"source\":{\"name\":\"f\"},"
            "\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],\"signals\":["
            "{\"key\":\"a\",\"name\":\"A\",\"read\":{\"service\":\"22\","
            "\"number\":\"D100\"},\"field\":{\"byte\":0}},"
            "{\"key\":\"b\",\"name\":\"B\",\"read\":{\"service\":\"22\","
            "\"number\":\"D101\"},\"field\":{\"byte\":0}},"
            "{\"key\":\"c\",\"name\":\"C\",\"read\":{\"service\":\"22\","
            "\"number\":\"D102\"},\"field\":{\"byte\":0}},"
            "{\"key\":\"d\",\"name\":\"D\",\"read\":{\"service\":\"22\","
            "\"number\":\"D103\"},\"field\":{\"byte\":0}},"
            "{\"key\":\"t\",\"name\":\"T\",\"kind\":\"text\",\"read\":"
            "{\"service\":\"22\",\"number\":\"D104\"},\"field\":{\"byte\":0}},"
            "{\"key\":\"e\",\"name\":\"E\",\"kind\":\"computed\","
            "\"expr\":\"a / b\",\"max_age\":{\"b\":1}},"
            "{\"key\":\"f\",\"name\":\"F\",\"kind\":\"computed\","
            "\"expr\":\"if(e > c, e, d)\"}]}";
        if (ecuio_signal_file_load(kText, sizeof kText - 1, &file) !=
            ECUIO_OK) {
            abort();
        }
    }
    return file;
}

/* A program reading numbers alone gives the machine's own answer over
 * the values within their bounds */
void same_as_the_machine(const ecuio_computed_t& p,
                         const ecuio_eval_input_t* given,
                         const ecuio_eval_result_t& out)
{
    double in[ECUIO_COMPUTED_MAX_INPUTS] = {};
    for (uint32_t k = 0; k < p.input_count; ++k) {
        in[k] = NAN;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint64_t bound =
                p.max_age_us[k] != 0 ? p.max_age_us[k] : ECUIO_VALUE_STALE_US;
            if (given[i].def_id == p.input_ids[k] && given[i].age_us <= bound) {
                in[k] = given[i].value;
            }
        }
    }
    double want = 0.0;
    const bool ran = ecuio_computed_run(p, in, &want);
    const bool has = (out.flags & ECUIO_EVAL_VALUE) != 0;
    if (ran != has || (ran && !same_bits(want, out.value))) {
        abort();
    }
}

/* p over the file's set: see the file's opening comment */
void over_a_file(ecuio_computed_t p, bool checked, Reader& r)
{
    const ecuio_signal_file_t* file = the_file();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    /* The first four defs are the numbers; a set bit names any def */
    const uint8_t any = r.byte();
    for (uint32_t k = 0; k < ECUIO_COMPUTED_MAX_INPUTS; ++k) {
        if (k >= p.input_count && (any & 0x80u) == 0) {
            continue; /* past the count the IDs stay as they came */
        }
        const uint32_t at = ((any >> k) & 1u) != 0 ? r.byte() % set->count
                                                   : (k + r.byte()) % 4u;
        p.input_ids[k] = set->defs[at].id;
    }
    checked = ecuio_computed_check(p);
    static const uint64_t kAges[] = {0,       99999,   100000,  1000000,
                                     1000001, 3000000, 3000001, 60000001};
    ecuio_eval_input_t given[4];
    for (uint32_t i = 0; i < 4; ++i) {
        given[i] = ecuio_eval_input_t{};
        given[i].def_id = set->defs[i].id;
        given[i].value = r.value();
        given[i].age_us = kAges[r.byte() % (sizeof kAges / sizeof kAges[0])];
    }
    uint16_t leaves[8] = {};
    uint32_t count = 0;
    const ecuio_status_t listed =
        ecuio_signal_file_expr_leaves(file, &p, leaves, 8, &count);
    ecuio_eval_result_t out{};
    out.struct_size = sizeof out;
    const ecuio_status_t worked =
        ecuio_signal_file_evaluate(file, &p, given, 4, &out);
    if (listed != worked ||
        (worked != ECUIO_OK && worked != ECUIO_ERR_INVALID_ARGUMENT)) {
        abort();
    }
    bool numbers = true;
    for (uint32_t k = 0; k < p.input_count && k < ECUIO_COMPUTED_MAX_INPUTS;
         ++k) {
        bool number = false;
        for (uint32_t i = 0; i < 4; ++i) {
            number = number || p.input_ids[k] == set->defs[i].id;
        }
        numbers = numbers && number;
    }
    if (worked != ECUIO_OK) {
        if (checked && numbers) {
            abort(); /* nothing left to refuse */
        }
        return;
    }
    if (!checked || count > 4 || out.struct_size != sizeof out) {
        abort();
    }
    const bool has = (out.flags & ECUIO_EVAL_VALUE) != 0;
    if (has ? (!isfinite(out.value) || out.why != ECUIO_EVAL_WHY_NONE)
            : (out.why < ECUIO_EVAL_WHY_MISSING ||
               out.why > ECUIO_EVAL_WHY_NO_NUMBER)) {
        abort();
    }
    if (numbers) {
        same_as_the_machine(p, given, out);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1) {
        return 0;
    }
    Reader r{data + 1, size - 1};
    ecuio_computed_t p{};
    if ((data[0] & 1u) != 0) {
        uint8_t raw[sizeof p];
        for (uint8_t& b : raw) {
            b = r.byte();
        }
        memcpy(&p, raw, sizeof p);
    } else {
        p = built(r, (data[0] & 2u) != 0);
    }
    double inputs[4];
    const uint8_t missing = r.byte();
    for (uint32_t i = 0; i < 4; ++i) {
        inputs[i] = (missing >> i) & 1u ? NAN : r.value();
    }

    const bool checked = ecuio_computed_check(p);
    if (ecuio_computed_check(p) != checked) {
        abort();
    }

    /* Any program, checked or not, runs within its stack, alike twice */
    double out1 = 0.0;
    double out2 = 0.0;
    const bool ran1 = ecuio_computed_run(p, inputs, &out1);
    const bool ran2 = ecuio_computed_run(p, inputs, &out2);
    if (ran1 != ran2 || (ran1 && !same_bits(out1, out2))) {
        abort();
    }
    if (checked && ran1 && !isfinite(out1)) {
        abort();
    }

    /* Format 1's steps alone: judged and run as before */
    if (format1_steps(p)) {
        if (ref_check(p) != checked) {
            abort();
        }
        if (checked) {
            double ref = 0.0;
            const bool ref_ran = ref_run(p, inputs, &ref);
            if (ref_ran != ran1 || (ref_ran && !same_bits(ref, out1))) {
                abort();
            }
        }
    }
    over_a_file(p, checked, r);
    return 0;
}
