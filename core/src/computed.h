// File/layer: core/src/computed.h | C++17, Qt-free, every platform, pure
// Testing: UNIT: ComputedTest (test_signals); CoreTest (test_core)
//
// A computed def's program (ADR-0051 section 5, ecuio_computed_t in
// signals.h): the checker the validator runs once, and the stack
// machine the worker runs per frame, which allocates nothing.

#ifndef ECUIO_COMPUTED_H
#define ECUIO_COMPUTED_H

#include <ecuio/signals.h>

#include <math.h>

/* Whether a step is a push of an input or a constant, and its index */
inline bool ecuio_computed_is_push(uint8_t step, uint8_t kind)
{
    return (step & 0xF0u) == kind;
}

/* Whether program runs to one value: its counts in range, every step
 * known, every index within its count, two values under every
 * operation, one at the end, its constants finite, its age bounds in
 * range, and every byte past a count, and the reserved ones, zero. The
 * inputs' ids are the set's to check. */
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
    uint32_t depth = 0;
    for (uint32_t i = 0; i < steps; ++i) {
        const uint8_t s = program.steps[i];
        if (ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_INPUT)) {
            if ((s & 0x0Fu) >= inputs) {
                return false;
            }
            ++depth;
        } else if (ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_CONSTANT)) {
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

/* Runs a checked program over its inputs' values: true with *out
 * written, or false for a result that isn't finite (a division by zero
 * gives one), which gives no value. The stack's bounds are checked again,
 * so a program that never passed the checker can't reach past it. */
inline bool ecuio_computed_run(const ecuio_computed_t& program,
                               const double inputs[ECUIO_COMPUTED_MAX_INPUTS],
                               double* out)
{
    /* Steps of one push or one operation each, ending at one value, so
     * the stack is never deeper than half the steps rounded up */
    constexpr uint32_t kDepth = (ECUIO_COMPUTED_MAX_STEPS + 1) / 2;
    double stack[kDepth];
    uint32_t depth = 0;
    const uint32_t steps = program.step_count <= ECUIO_COMPUTED_MAX_STEPS
                               ? program.step_count
                               : ECUIO_COMPUTED_MAX_STEPS;
    for (uint32_t i = 0; i < steps; ++i) {
        const uint8_t s = program.steps[i];
        const uint32_t index = s & 0x0Fu;
        if (ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_INPUT) ||
            ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_CONSTANT)) {
            const bool input =
                ecuio_computed_is_push(s, ECUIO_COMPUTED_PUSH_INPUT);
            if (depth >= kDepth ||
                index >= (input ? ECUIO_COMPUTED_MAX_INPUTS
                                : ECUIO_COMPUTED_MAX_CONSTANTS)) {
                return false;
            }
            stack[depth++] = input ? inputs[index] : program.constants[index];
            continue;
        }
        if (depth < 2) {
            return false;
        }
        const double b = stack[--depth];
        const double a = stack[depth - 1];
        double r = 0.0;
        switch (s) {
        case ECUIO_COMPUTED_ADD:
            r = a + b;
            break;
        case ECUIO_COMPUTED_SUBTRACT:
            r = a - b;
            break;
        case ECUIO_COMPUTED_MULTIPLY:
            r = a * b;
            break;
        default:
            r = a / b;
            break;
        }
        /* A step that isn't finite (a division by zero, an overflow)
         * gives no value, even where a later step would make it finite
         * again: 1 / (1 / 0) is no value, not 0 */
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

#endif /* ECUIO_COMPUTED_H */
