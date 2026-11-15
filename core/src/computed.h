// File/layer: core/src/computed.h | C++17, Qt-free, every platform, pure
// Testing: UNIT: ComputedTest, SignalTablesTest (test_signals); CoreTest
//          (test_core); FUZZ: fuzz_computed
//
// A computed def's program (ADR-0051 section 5, ecuio_computed_t in
// signals.h): the checker the validator runs once, and the stack
// machine the worker runs per frame, which allocates nothing. Every
// step is exact or correctly rounded under IEEE 754, so a program gives
// the same bits on every platform; min and max are comparisons here,
// never fmin and fmax, whose answer for -0 against +0 is the library's.
// And the walk from a computed def to the number defs under it.

#ifndef ECUIO_COMPUTED_H
#define ECUIO_COMPUTED_H

#include <ecuio/signals.h>

#include <math.h>

/* Whether a step is a push of an input or a constant, and its index */
inline bool ecuio_computed_is_push(uint8_t step, uint8_t kind)
{
    return (step & 0xF0u) == kind;
}

/* What an operation takes and gives: takes values off the top, bit k of
 * truths set when the k-th of them from the deepest is a truth */
struct ecuio_computed_shape {
    uint8_t takes;
    uint8_t truths;
    bool gives_truth;
};

/* An operation's shape; false for a push or a step value not listed */
inline bool ecuio_computed_shape_of(uint8_t step, ecuio_computed_shape* out)
{
    if (step >= ECUIO_COMPUTED_ADD && step <= ECUIO_COMPUTED_MAX) {
        *out = {2, 0x0u, false};
    } else if (step >= ECUIO_COMPUTED_ABS && step <= ECUIO_COMPUTED_CEIL) {
        *out = {1, 0x0u, false};
    } else if (step >= ECUIO_COMPUTED_LESS &&
               step <= ECUIO_COMPUTED_NOT_EQUAL) {
        *out = {2, 0x0u, true};
    } else if (step == ECUIO_COMPUTED_ALL || step == ECUIO_COMPUTED_ANY) {
        *out = {2, 0x3u, true};
    } else if (step == ECUIO_COMPUTED_NOT) {
        *out = {1, 0x1u, true};
    } else if (step == ECUIO_COMPUTED_IF) {
        *out = {3, 0x1u, false};
    } else {
        return false;
    }
    return true;
}

/* Whether program runs to one number: its counts in range, every step
 * listed, every index within its count, each operation given the
 * values and types it takes, one number at the end, its constants
 * finite, its age bounds in range, and every byte past a count, and the
 * reserved ones, zero. The inputs' ids are the set's to check. */
inline bool ecuio_computed_check(const ecuio_computed_t& program)
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
        /* Zero after the count is +0.0: -0.0's sign bit is a byte that
         * isn't zero */
        if (i >= constants ? (c != 0.0 || signbit(c)) : !isfinite(c)) {
            return false;
        }
    }
    for (uint32_t i = steps; i < sizeof program.steps; ++i) {
        if (program.steps[i] != 0) {
            return false;
        }
    }
    /* Bit d set: the value at depth d is a truth. At most one value per
     * step, so 33 bits. */
    uint64_t truths = 0;
    uint32_t depth = 0;
    for (uint32_t i = 0; i < steps; ++i) {
        const uint8_t s = program.steps[i];
        if (ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_INPUT) ||
            ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_CONSTANT)) {
            const bool input =
                ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_INPUT);
            if ((s & 0x0Fu) >= (input ? inputs : constants)) {
                return false;
            }
            truths &= ~(uint64_t{1} << depth);
            ++depth;
            continue;
        }
        ecuio_computed_shape shape{};
        if (!ecuio_computed_shape_of(s, &shape) || depth < shape.takes) {
            return false;
        }
        depth -= shape.takes;
        for (uint32_t k = 0; k < shape.takes; ++k) {
            const bool truth = ((truths >> (depth + k)) & 1u) != 0;
            if (truth != (((shape.truths >> k) & 1u) != 0)) {
                return false;
            }
        }
        truths &= ~(uint64_t{1} << depth);
        truths |= uint64_t{shape.gives_truth} << depth;
        ++depth;
    }
    return depth == 1 && (truths & 1u) == 0;
}

/* Runs a checked program: true with *out written, false for no value.
 * A NaN input has no value (the decoder passes NaN for one past its age
 * bound or never seen); an infinite one is a value, as in format 1,
 * where 1 / inf is 0. A number step whose result isn't finite has no
 * value, and so has a program that ends with one. Stack bounds are
 * checked again, so an unchecked program can't reach past it. */
inline bool ecuio_computed_run(const ecuio_computed_t& program,
                               const double inputs[ECUIO_COMPUTED_MAX_INPUTS],
                               double* out)
{
    struct slot {
        double value;
        bool has;
    };
    /* A step pushes one value at most, so the stack can't outgrow the
     * steps; a checked program reaches half of them, rounded up */
    slot stack[ECUIO_COMPUTED_MAX_STEPS];
    uint32_t depth = 0;
    const uint32_t steps = program.step_count <= ECUIO_COMPUTED_MAX_STEPS
                               ? program.step_count
                               : ECUIO_COMPUTED_MAX_STEPS;
    for (uint32_t i = 0; i < steps; ++i) {
        const uint8_t s = program.steps[i];
        const uint32_t index = s & 0x0Fu;
        if (ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_INPUT)) {
            if (index >= ECUIO_COMPUTED_MAX_INPUTS) {
                return false;
            }
            stack[depth++] = {inputs[index], !isnan(inputs[index])};
            continue;
        }
        if (ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_CONSTANT)) {
            const double c = program.constants[index];
            stack[depth++] = {c, isfinite(c)};
            continue;
        }
        ecuio_computed_shape shape{};
        if (!ecuio_computed_shape_of(s, &shape) || depth < shape.takes) {
            return false;
        }
        depth -= shape.takes;
        const slot* x = &stack[depth];
        slot r{0.0, false};
        if (s == ECUIO_COMPUTED_IF) {
            /* The branch not taken changes nothing */
            if (x[0].has) {
                r = x[0].value != 0.0 ? x[1] : x[2];
            }
        } else if (s == ECUIO_COMPUTED_ALL || s == ECUIO_COMPUTED_ANY) {
            /* A part that settles it wins over a part with no value */
            const double settles = s == ECUIO_COMPUTED_ALL ? 0.0 : 1.0;
            if ((x[0].has && x[0].value == settles) ||
                (x[1].has && x[1].value == settles)) {
                r = {settles, true};
            } else if (x[0].has && x[1].has) {
                r = {1.0 - settles, true};
            }
        } else if (x[0].has && (shape.takes < 2 || x[1].has)) {
            const double a = x[0].value;
            const double b = shape.takes < 2 ? 0.0 : x[1].value;
            double v = 0.0;
            switch (s) {
            case ECUIO_COMPUTED_ADD:
                v = a + b;
                break;
            case ECUIO_COMPUTED_SUBTRACT:
                v = a - b;
                break;
            case ECUIO_COMPUTED_MULTIPLY:
                v = a * b;
                break;
            case ECUIO_COMPUTED_DIVIDE:
                v = a / b;
                break;
            case ECUIO_COMPUTED_MIN:
                v = b < a ? b : a;
                break;
            case ECUIO_COMPUTED_MAX:
                v = b > a ? b : a;
                break;
            case ECUIO_COMPUTED_ABS:
                v = fabs(a);
                break;
            case ECUIO_COMPUTED_SQRT:
                /* NaN below zero, so no value; sqrt(-0) is -0 */
                v = a < 0.0 ? NAN : sqrt(a);
                break;
            case ECUIO_COMPUTED_ROUND:
                /* Half away from zero, whatever the rounding mode */
                v = round(a);
                break;
            case ECUIO_COMPUTED_FLOOR:
                v = floor(a);
                break;
            case ECUIO_COMPUTED_CEIL:
                v = ceil(a);
                break;
            case ECUIO_COMPUTED_LESS:
                v = a < b ? 1.0 : 0.0;
                break;
            case ECUIO_COMPUTED_LESS_EQUAL:
                v = a <= b ? 1.0 : 0.0;
                break;
            case ECUIO_COMPUTED_GREATER:
                v = a > b ? 1.0 : 0.0;
                break;
            case ECUIO_COMPUTED_GREATER_EQUAL:
                v = a >= b ? 1.0 : 0.0;
                break;
            case ECUIO_COMPUTED_EQUAL:
                v = a == b ? 1.0 : 0.0;
                break;
            case ECUIO_COMPUTED_NOT_EQUAL:
                v = a != b ? 1.0 : 0.0;
                break;
            default: /* ECUIO_COMPUTED_NOT */
                v = a == 0.0 ? 1.0 : 0.0;
                break;
            }
            /* A step that isn't finite has no value even if a later one
             * would make it finite again: 1 / (1 / 0) is no value */
            r = {v, isfinite(v)};
        }
        stack[depth++] = r;
    }
    if (depth != 1 || !stack[0].has || !isfinite(stack[0].value)) {
        return false;
    }
    *out = stack[0].value;
    return true;
}

/* Marks in place (a byte per def of set) the defs each marked computed
 * def reads, and theirs in turn, down to the number defs under it
 * (ADR-0062): what a focus on a computed signal keeps asked. A marked
 * def stays marked. At most ECUIO_COMPUTED_MAX_DEPTH passes, no
 * recursion and no allocation; a set the validator passed is walked
 * whole. */
inline void ecuio_computed_mark_leaves(const ecuio_signal_set_t* set,
                                       uint8_t* marks)
{
    if (set == nullptr || set->defs == nullptr || marks == nullptr) {
        return;
    }
    for (uint32_t pass = 0; pass < ECUIO_COMPUTED_MAX_DEPTH; ++pass) {
        bool more = false;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (marks[i] == 0 || (d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 ||
                d.table_first >= set->computed_count) {
                continue;
            }
            const ecuio_computed_t& c = set->computed[d.table_first];
            for (uint32_t k = 0; k < c.input_count; ++k) {
                for (uint32_t j = 0; j < set->count; ++j) {
                    if (set->defs[j].id == c.input_ids[k] && marks[j] == 0) {
                        marks[j] = 1;
                        more = true;
                    }
                }
            }
        }
        if (!more) {
            break;
        }
    }
}

#endif /* ECUIO_COMPUTED_H */
