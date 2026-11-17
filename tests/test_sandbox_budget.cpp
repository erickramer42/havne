// File/layer: tests/test_sandbox_budget.cpp | UNIT: core/src/wasm_sandbox.h
// Testing: SandboxTest, Qt-free, every tree (build-win32's x86 core too)

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#include "sandbox_cases.h"
#include "wasm_builder.h"
#include "wasm_check.h"
#include "wasm_sandbox.h"

namespace {

using namespace wasm_builder;
using namespace sandbox_cases;
using ecuio_wasm::call_end;
using ecuio_wasm::wanted_export;

/* ---- What a budget counts ----
 *
 * One unit for each instruction, and for the work an instruction does
 * beyond itself (third_party/wamr-patches/0001): each byte the runtime
 * scans to find a block's end, each record it walks for a br_table, each
 * 16 stack cells it zeroes or copies. Each case compares the least budget
 * a module's call returns under with that of the same module less the
 * work, so the numbers hold on every machine. */

const wanted_export kRun[] = {{"run", ":"}};

/* A module whose one export, run, is this code */
module running(bytes body)
{
    module m;
    m.memory_min = 1;
    m.memory_max = 1;
    m.funcs.push_back(plain_func("run", std::move(body), false));
    return m;
}

void append(bytes& to, uint32_t times, std::initializer_list<uint8_t> code)
{
    for (uint32_t k = 0; k < times; ++k) {
        to.insert(to.end(), code);
    }
}

/* The least budget under which m's run returns, 0 when none up to 2^24
 * does. Each try is a new instance: a call that runs out ends its own. */
uint32_t least_budget(const module& m)
{
    uint32_t low = 1;
    uint32_t high = 1u << 24;
    {
        Made most(m, kRun, 1);
        if (most.status != ECUIO_OK ||
            most.call(0, {}, high).end != call_end::returned) {
            return 0;
        }
    }
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        Made made(m, kRun, 1);
        if (made.call(0, {}, mid).end == call_end::returned) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low;
}

/* run: an if that isn't taken, over `skipped` bytes of code */
module skipping_if(uint32_t skipped)
{
    bytes body = {I32_CONST, 0, IF, VOID_BLOCK};
    body.insert(body.end(), skipped, NOP);
    body.push_back(END);
    return running(body);
}

TEST(SandboxTest, ABlockScannedForItsEndCostsItsBytes)
{
    const uint32_t plain = least_budget(skipping_if(0));
    ASSERT_NE(plain, 0u);
    EXPECT_EQ(least_budget(skipping_if(1000)), plain + 1000);
    EXPECT_EQ(least_budget(skipping_if(50000)), plain + 50000);

    /* the same through a branch out of a block whose end wasn't known */
    auto leaving = [](uint32_t skipped) {
        bytes body = {BLOCK, VOID_BLOCK, BR, 0};
        body.insert(body.end(), skipped, NOP);
        body.push_back(END);
        return running(body);
    };
    const uint32_t left = least_budget(leaving(0));
    ASSERT_NE(left, 0u);
    EXPECT_EQ(least_budget(leaving(1000)), left + 1000);
    EXPECT_EQ(least_budget(leaving(50000)), left + 50000);
}

/* Charged work ends a call exactly as instructions do: one unit short,
 * the instance is dead with the runtime's words for a limit. A block
 * found once is in the runtime's cache, and costs nothing again. */
TEST(SandboxTest, ChargedWorkEndsACallAsInstructionsDo)
{
    const module m = skipping_if(50000);
    const uint32_t least = least_budget(m);
    ASSERT_GT(least, 50000u) << "the scan alone is 50,000 units";

    /* a charge far past what is left stops the count at zero: below it
     * the runtime would read no limit at all */
    Made far_short(m, kRun, 1);
    EXPECT_EQ(far_short.call(0, {}, 100).end, call_end::budget);
    EXPECT_TRUE(ecuio_wasm::instance_dead(far_short.i));

    Made short_by_one(m, kRun, 1);
    EXPECT_EQ(short_by_one.call(0, {}, least - 1).end, call_end::budget);
    EXPECT_TRUE(ecuio_wasm::instance_dead(short_by_one.i));
    EXPECT_NE(std::string(ecuio_wasm::instance_trap(short_by_one.i))
                  .find("instruction limit exceeded"),
              std::string::npos);
    EXPECT_EQ(short_by_one.call(0, {}, least).end, call_end::dead);

    Made found(m, kRun, 1);
    EXPECT_EQ(found.call(0, {}, least).end, call_end::returned);
    EXPECT_EQ(found.call(0, {}, least - 50000).end, call_end::returned)
        << "the block's end is known now";
}

TEST(SandboxTest, ACallCostsTheCellsOfItsFrame)
{
    /* run pushes the parameters and calls a function with these locals */
    auto calling = [](uint32_t params, uint32_t locals) {
        bytes body;
        append(body, params, {I32_CONST, 0});
        body.insert(body.end(), {CALL, 1});
        module m = running(body);
        func callee;
        callee.params.assign(params, I32);
        callee.i32_locals = locals;
        m.funcs.push_back(callee);
        return m;
    };
    const uint32_t plain = least_budget(calling(0, 0));
    ASSERT_NE(plain, 0u);
    EXPECT_EQ(least_budget(calling(0, 15)), plain) << "under one unit";
    EXPECT_EQ(least_budget(calling(0, 16)), plain + 1);
    EXPECT_EQ(least_budget(calling(0, 8000)), plain + 8000 / 16);
    /* each parameter is an instruction to push, and a cell of the frame */
    EXPECT_EQ(least_budget(calling(4000, 0)), plain + 4000 + 4000 / 16);
    EXPECT_EQ(least_budget(calling(3000, 3000)), plain + 3000 + 6000 / 16);
}

TEST(SandboxTest, ValuesAReturnOrABranchCarriesCostTheirCells)
{
    /* run calls a function that returns `values` i32s, and drops them */
    auto returning = [](uint32_t values, bool by_return) {
        bytes body = {CALL, 1};
        body.insert(body.end(), values, DROP);
        module m = running(body);
        func callee;
        callee.results.assign(values, I32);
        append(callee.body, values, {I32_CONST, 0});
        if (by_return) {
            callee.body.push_back(RETURN);
        }
        m.funcs.push_back(callee);
        return m;
    };
    for (bool by_return : {false, true}) {
        const uint32_t plain = least_budget(returning(0, by_return));
        ASSERT_NE(plain, 0u);
        /* a push and a drop for each value, and its cell copied */
        EXPECT_EQ(least_budget(returning(4000, by_return)),
                  plain + 2 * 4000 + 4000 / 16)
            << by_return;
    }

    /* run leaves a block with `values` i32s by a branch, and drops them.
     * The block's type is the second function's, which nothing calls. */
    auto branching = [](uint32_t values) {
        bytes body = {BLOCK, static_cast<uint8_t>(values == 0 ? 0 : 1)};
        append(body, values, {I32_CONST, 0});
        body.insert(body.end(), {BR, 0, END});
        body.insert(body.end(), values, DROP);
        module m = running(body);
        func typed;
        typed.results.assign(values, I32);
        append(typed.body, values, {I32_CONST, 0});
        m.funcs.push_back(typed);
        return m;
    };
    const uint32_t plain = least_budget(branching(0));
    ASSERT_NE(plain, 0u);
    /* a push and a drop for each value, its two bytes of code scanned for
     * the block's end, and its cell copied */
    EXPECT_EQ(least_budget(branching(4000)),
              plain + 2 * 4000 + 2 * 4000 + 4000 / 16);
}

/* A branch to a block the runtime already has in its cache finds the end
 * without a scan, and costs nothing. run's function calls itself `depth`
 * deep inside a block, so every level enters the block before any has
 * left it: the deepest scans it, and each level above finds it. The cache
 * keeps two blocks for each of its 64 sets, by the block's address. */
TEST(SandboxTest, ABranchToABlockAlreadyFoundCostsNothing)
{
    /* `lead` nops move the block by a byte; with `second`, a block 64
     * bytes before it is scanned first and takes their set's first place */
    auto nested = [](uint32_t skipped, uint32_t lead, bool second) {
        const uint8_t depth = 10;
        module m = running({I32_CONST, depth, CALL, 1});
        func again; /* (left: i32) -> () */
        again.params = {I32};
        again.body.assign(lead, NOP);
        if (second) {
            again.body.insert(again.body.end(),
                              {BLOCK, VOID_BLOCK, BR, 0, END});
            again.body.insert(again.body.end(), 59, NOP);
        }
        again.body.insert(again.body.end(),
                          {BLOCK, VOID_BLOCK, LOCAL_GET, 0, IF, VOID_BLOCK,
                           LOCAL_GET, 0, I32_CONST, 1, I32_SUB, CALL, 1, END,
                           BR, 0});
        again.body.insert(again.body.end(), skipped, NOP);
        again.body.push_back(END);
        m.funcs.push_back(again);
        return m;
    };
    for (bool second : {false, true}) {
        for (uint32_t lead : {0u, 1u}) {
            const uint32_t plain = least_budget(nested(0, lead, second));
            ASSERT_NE(plain, 0u);
            EXPECT_EQ(least_budget(nested(1000, lead, second)), plain + 1000)
                << "one scan, not one for each level: lead " << lead
                << ", second " << second;
        }
    }
}

/* The budget counts the work of the instructions this build of the
 * runtime knows. Those of the features left out do work a module sizes
 * that nothing counts (a memory.fill of the whole memory is one
 * instruction), so the runtime must refuse each: third_party/wamr_port.c
 * fails a build that turns one on beside metering. Each body here is
 * valid with its feature on, so a switch turned on fails this case.
 * Exception handling and GC need sources that are not kept: no switch
 * alone builds them. */
TEST(SandboxTest, AnInstructionTheBudgetCantCountIsRefused)
{
    const struct {
        const char* what;
        bytes body;
    } uncounted[] = {
        {"memory.fill",
         {I32_CONST, 0, I32_CONST, 0, I32_CONST, 0, 0xFC, 0x0B, 0x00}},
        {"memory.copy",
         {I32_CONST, 0, I32_CONST, 0, I32_CONST, 0, 0xFC, 0x0A, 0x00, 0x00}},
        /* reference types: the table instructions need a table, which
         * no module here has; ref.null stands for the feature */
        {"ref.null", {0xD0, 0x70, DROP}},
        {"return_call", {0x12, 0x00}},
    };
    for (const auto& u : uncounted) {
        const module m = running(u.body);
        ASSERT_EQ(refusal(m.build(), kRun, 1), "")
            << u.what << ": the check judges no instruction";
        Made made(m, kRun, 1);
        EXPECT_EQ(made.status, ECUIO_ERR_LOAD_FAILED) << u.what;
        EXPECT_EQ(made.i, nullptr) << u.what;
        EXPECT_EQ(
            std::string(made.why).rfind("the runtime refused the module: ", 0),
            0u)
            << u.what << ": " << made.why;
    }
}

/* A br_table with a depth past 255 is kept in a list the runtime walks
 * each time one runs: one unit for each record looked at. With `tables`
 * of them run once each, the walks are 1, 2, ... records long. */
TEST(SandboxTest, ABrTableCostsTheRecordsWalkedToFindIt)
{
    auto tabled = [](uint32_t tables) {
        bytes body;
        append(body, 256, {BLOCK, VOID_BLOCK});
        /* index 1 of a table of one: its default, the block it is in */
        append(body, tables,
               {BLOCK, VOID_BLOCK, I32_CONST, 1, BR_TABLE, 0x01, 0x80, 0x02,
                0x00, END});
        append(body, 256, {END});
        return running(body);
    };
    const uint32_t one = least_budget(tabled(1));
    const uint32_t two = least_budget(tabled(2));
    ASSERT_NE(one, 0u);
    ASSERT_GT(two, one + 2);
    const uint32_t each = two - one - 2; /* less the second's walk of two */
    EXPECT_EQ(least_budget(tabled(40)), one + 39 * each + (40 * 41 / 2 - 1));
}

/* The most a module that passes the check can make one instruction scan:
 * three ifs whose blocks share a set of the runtime's cache, so each is
 * scanned again on every turn. Uncharged, the default budget ran for
 * seconds here and for minutes with larger blocks; charged, it ends in
 * the time of about a million bytes read. */
TEST(SandboxTest, TheDearestScannerEndsAtItsBudgetInTime)
{
    bytes body = {LOOP, VOID_BLOCK};
    for (int k = 0; k < 3; ++k) {
        /* four bytes of head, the nops and an end: a multiple of 64 */
        body.insert(body.end(), {I32_CONST, 0, IF, VOID_BLOCK});
        body.insert(body.end(), 21755, NOP);
        body.push_back(END);
    }
    body.insert(body.end(), {BR, 0, END});
    const module m = running(body);
    ASSERT_EQ(refusal(m.build(), kRun, 1), "") << "within the body cap";

    Made made(m, kRun, 1);
    ASSERT_EQ(made.status, ECUIO_OK) << made.why;
    const auto began = std::chrono::steady_clock::now();
    const call_end end = made.call(0).end;
    const auto took = std::chrono::steady_clock::now() - began;
    EXPECT_EQ(end, call_end::budget);
    EXPECT_LT(took, std::chrono::seconds(2));
}

} // namespace
