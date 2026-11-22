// File/layer: tests/test_sandbox.cpp | UNIT: core/src/wasm_sandbox.h
// Testing: SandboxTest, Qt-free, every tree (build-win32's x86 core too)
//
// A module's and its instances' life in the sandbox a plugin's code runs
// in (ADR-0066 section 2.2). Every module is built here by
// tests/wasm_builder.h, most of them hostile: an endless loop, a memory
// that grows, a trap of each kind. The check's cases are test_wasm_check.cpp's, and what a budget
// counts test_sandbox_budget.cpp's.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ecuio/signals.h" /* ECUIO_MAX_CHANNELS */
#include "sandbox_cases.h"
#include "wasm_builder.h"
#include "wasm_check.h"
#include "wasm_sandbox.h"

namespace {

using namespace wasm_builder;
using namespace sandbox_cases;
using ecuio_wasm::call_end;
using ecuio_wasm::wanted_export;

/* One module with an export of each kind the cases call, in this order */
enum : size_t {
    ADD,
    SPIN,
    COUNTDOWN,
    PEEK,
    POKE,
    TRAP,
    OUTSIDE,
    RECURSE,
    GROW,
    PAGES,
    GROW_ON
};
const wanted_export kAll[] = {
    {"add", "ii:i"}, {"spin", ":"},   {"countdown", "i:"}, {"peek", ":i"},
    {"poke", ":"},   {"trap", ":"},   {"outside", ":i"},   {"recurse", ":"},
    {"grow", ":i"},  {"pages", ":i"}, {"grow_on", ":i"},
};
constexpr size_t kAllCount = sizeof kAll / sizeof kAll[0];

module everything()
{
    module m;
    m.memory_min = 1;
    m.memory_max = 1;
    m.funcs.push_back(add_func("add"));
    m.funcs.push_back(spin_func("spin"));
    m.funcs.push_back(countdown_func("countdown"));
    m.funcs.push_back(plain_func("peek", {I32_CONST, 0, I32_LOAD, 2, 0}));
    m.funcs.push_back(store_func("poke", 7));
    m.funcs.push_back(plain_func("trap", {UNREACHABLE}, false));
    /* address -1: the last byte and three past the memory's end */
    m.funcs.push_back(plain_func("outside", {I32_CONST, 0x7F, I32_LOAD, 2, 0}));
    m.funcs.push_back(
        plain_func("recurse", {CALL, static_cast<uint8_t>(RECURSE)}, false));
    m.funcs.push_back(plain_func("grow", {I32_CONST, 1, MEMORY_GROW, 0}));
    m.funcs.push_back(plain_func("pages", {MEMORY_SIZE, 0}));
    /* what a grow answered, plus 43: 42 after a grow that failed */
    m.funcs.push_back(plain_func(
        "grow_on", {I32_CONST, 1, MEMORY_GROW, 0, I32_CONST, 43, I32_ADD}));
    return m;
}

/* good() of `pages` pages with n more functions that do nothing, of two
 * types: four bytes of module each */
module padded(uint32_t n, uint32_t pages = 1)
{
    module m = good();
    m.memory_min = pages;
    m.memory_max = static_cast<int32_t>(pages);
    func empty;  /* () -> () */
    func taking; /* (i32) -> () */
    taking.params = {I32};
    m.funcs.reserve(n + 1);
    for (uint32_t k = 0; k < n; ++k) {
        m.funcs.push_back(k % 2 == 0 ? empty : taking);
    }
    return m;
}

/* 200,000 functions: under 1 MiB of module, past what a load may take */
module costly()
{
    return padded(200000);
}

/* The runtime is up and its start's allocations are behind us */
void warm()
{
    Made first(everything(), kAll, kAllCount);
    ASSERT_EQ(first.status, ECUIO_OK) << first.why;
}

TEST(SandboxTest, AnInstanceRunsItsExports)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    const ecuio_wasm::call_result r = m.call(ADD, {40, 2});
    EXPECT_EQ(r.end, call_end::returned);
    EXPECT_EQ(r.value, 42u);
    EXPECT_EQ(m.call(COUNTDOWN, {100}).end, call_end::returned);
    EXPECT_FALSE(ecuio_wasm::instance_dead(m.i));
    EXPECT_STREQ(ecuio_wasm::instance_trap(m.i), "");
}

TEST(SandboxTest, AnEndlessLoopEndsAtItsBudgetAndTheInstanceIsDead)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    EXPECT_EQ(m.call(SPIN).end, call_end::budget);
    EXPECT_TRUE(ecuio_wasm::instance_dead(m.i));
    EXPECT_NE(
        std::string(ecuio_wasm::instance_trap(m.i)).find("instruction limit"),
        std::string::npos);
    /* Nothing runs in it again: a call into a stopped instance is what
     * WAMR's tracker reports as corrupting it */
    EXPECT_EQ(m.call(ADD, {1, 2}).end, call_end::dead);
    EXPECT_EQ(m.call(SPIN).end, call_end::dead);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, 0, 1), nullptr);
}

TEST(SandboxTest, EachKindOfTrapEndsTheInstance)
{
    for (size_t which : {TRAP, OUTSIDE, RECURSE}) {
        Made m(everything(), kAll, kAllCount);
        ASSERT_EQ(m.status, ECUIO_OK) << m.why;
        EXPECT_EQ(m.call(which).end, call_end::trapped) << kAll[which].name;
        EXPECT_TRUE(ecuio_wasm::instance_dead(m.i));
        EXPECT_STRNE(ecuio_wasm::instance_trap(m.i), "");
        EXPECT_EQ(m.call(ADD, {1, 2}).end, call_end::dead);
    }
}

/* Each call has the whole budget: one that left a count behind would
 * fail the second of two calls that each need nearly all of it. Past the
 * turns, a call needs a few units to leave, and the first one the bytes
 * of the countdown's block, scanned for its end. */
TEST(SandboxTest, TheBudgetIsEachCallsOwn)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    const uint32_t turns = 100;
    const uint32_t enough = turns * COUNTDOWN_TURN + 48;
    for (int k = 0; k < 3; ++k) {
        EXPECT_EQ(m.call(COUNTDOWN, {turns}, enough).end, call_end::returned)
            << k;
    }
    EXPECT_EQ(m.call(COUNTDOWN, {turns}, turns * COUNTDOWN_TURN - 1).end,
              call_end::budget);
}

TEST(SandboxTest, ACallThatDoesntFitItsExportRunsNothing)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    EXPECT_EQ(m.call(kAllCount).end, call_end::no_function);
    EXPECT_EQ(m.call(ADD, {1}).end, call_end::no_function)
        << "one argument of two";
    EXPECT_EQ(m.call(SPIN, {1}).end, call_end::no_function);
    EXPECT_FALSE(ecuio_wasm::instance_dead(m.i));
    EXPECT_EQ(m.call(ADD, {1, 2}).value, 3u);
}

TEST(SandboxTest, TheMemoryIsReadAndWrittenInsideItsBounds)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    const uint32_t page = 65536;
    uint8_t* base = ecuio_wasm::instance_memory(m.i, 0, page);
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, page - 4, 4), base + page - 4);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, page, 0), base + page);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, 1, page), nullptr);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, page, 1), nullptr);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, page + 1, 0), nullptr);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, 0xFFFFFFFFu, 2), nullptr);
    EXPECT_EQ(ecuio_wasm::instance_memory(m.i, 2, 0xFFFFFFFFu), nullptr);

    /* The host's write is the guest's read, and the guest's the host's */
    const uint8_t word[4] = {0x78, 0x56, 0x34, 0x12};
    std::memcpy(base, word, sizeof word);
    EXPECT_EQ(m.call(PEEK).value, 0x12345678u);
    EXPECT_EQ(m.call(POKE).end, call_end::returned);
    EXPECT_EQ(base[0], 7);
}

/* The memory is the size the module states: a grow answers -1 and its
 * call goes on, no trap, and the worker that calls never allocates for one */
TEST(SandboxTest, AGrowFailsAndItsCallGoesOn)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    const uint64_t before = ecuio_wasm::runtime_allocations();
    const ecuio_wasm::call_result grown = m.call(GROW);
    EXPECT_EQ(grown.end, call_end::returned);
    EXPECT_EQ(grown.value, 0xFFFFFFFFu) << "memory.grow's failure";
    const ecuio_wasm::call_result on = m.call(GROW_ON);
    EXPECT_EQ(on.end, call_end::returned);
    EXPECT_EQ(on.value, 42u) << "the instructions after the grow ran";
    EXPECT_FALSE(ecuio_wasm::instance_dead(m.i));
    EXPECT_EQ(m.call(PAGES).value, 1u);
    EXPECT_EQ(m.call(ADD, {40, 2}).value, 42u);
    EXPECT_EQ(ecuio_wasm::runtime_allocations(), before);
}

TEST(SandboxTest, ACallAllocatesNothing)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    const uint64_t before = ecuio_wasm::runtime_allocations();
    for (uint32_t k = 0; k < 1000; ++k) {
        ASSERT_EQ(m.call(ADD, {k, 1}).value, k + 1);
        ASSERT_EQ(m.call(COUNTDOWN, {10}).end, call_end::returned);
    }
    EXPECT_EQ(m.call(SPIN, {}, 5000).end, call_end::budget);
    EXPECT_EQ(ecuio_wasm::runtime_allocations(), before);
}

/* Sixteen pages, the most a module's memory may be */
module largest()
{
    module m = everything();
    m.memory_min = ecuio_wasm::MEMORY_MAX_PAGES;
    m.memory_max = ecuio_wasm::MEMORY_MAX_PAGES;
    return m;
}

/* The module's memory and its stack are counted with the rest */
TEST(SandboxTest, AnInstanceFreesWhatItsMakingTook)
{
    warm();
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    {
        Made m(everything(), kAll, kAllCount);
        ASSERT_EQ(m.status, ECUIO_OK) << m.why;
        const size_t one_page = ecuio_wasm::runtime_bytes_in_use() - before;
        EXPECT_GT(one_page, 65536u + ecuio_wasm::STACK_BYTES);
        EXPECT_EQ(m.call(SPIN).end, call_end::budget);

        Made large(largest(), kAll, kAllCount);
        ASSERT_EQ(large.status, ECUIO_OK) << large.why;
        EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use() - before,
                  2 * one_page + 15u * 65536u)
            << "fifteen pages more than the first";
    }
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), before) << "a dead one too";
}

/* A new instance never sees what an earlier one, or the host, left in
 * the heap: the runtime leaves a module's memory as the allocator gave it */
TEST(SandboxTest, AnInstancesMemoryStartsZeroed)
{
    const uint32_t size = ecuio_wasm::MEMORY_MAX_PAGES * 65536u;
    for (int round = 0; round < 3; ++round) {
        Made m(largest(), kAll, kAllCount);
        ASSERT_EQ(m.status, ECUIO_OK) << m.why;
        uint8_t* base = ecuio_wasm::instance_memory(m.i, 0, size);
        ASSERT_NE(base, nullptr);
        size_t nonzero = 0;
        for (uint32_t k = 0; k < size; ++k) {
            nonzero += base[k] != 0;
        }
        EXPECT_EQ(nonzero, 0u) << "round " << round;
        EXPECT_EQ(m.call(PEEK).value, 0u);
        std::memset(base, 0xA5, size); /* for the next round's block */
    }
}

/* Every instance alive counts toward one bound: past it a making is out
 * of memory, and room given back is room again */
TEST(SandboxTest, AllInstancesTogetherStayUnderTheRuntimesBound)
{
    warm();
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    const bytes code = largest().build();
    std::vector<ecuio_wasm::instance*> made;
    char why[ecuio_wasm::REFUSAL_MAX] = {};
    ecuio_status_t status = ECUIO_OK;
    while (made.size() < 200) { /* each over 1 MiB: far past the bound */
        ecuio_wasm::instance* i = nullptr;
        status = ecuio_wasm::make_instance(code.data(), code.size(), kAll,
                                           kAllCount, &i, why);
        if (status != ECUIO_OK) {
            break;
        }
        made.push_back(i);
        ASSERT_LE(ecuio_wasm::runtime_bytes_in_use(),
                  ecuio_wasm::RUNTIME_BYTES_MAX);
    }
    EXPECT_EQ(status, ECUIO_ERR_OUT_OF_MEMORY) << why;
    EXPECT_STREQ(why, "");
    ASSERT_GT(made.size(), 32u) << "a session's most ECUs fit";
    EXPECT_GT(ecuio_wasm::runtime_bytes_in_use(),
              ecuio_wasm::RUNTIME_BYTES_MAX - (2u << 20))
        << "the refused one was the first that didn't fit";

    ecuio_wasm::free_instance(made.back());
    made.pop_back();
    ecuio_wasm::instance* again = nullptr;
    EXPECT_EQ(ecuio_wasm::make_instance(code.data(), code.size(), kAll,
                                        kAllCount, &again, why),
              ECUIO_OK)
        << why;
    made.push_back(again);
    const ecuio_wasm::call_result sum = ecuio_wasm::call_instance(
        made.front(), ADD, std::vector<uint32_t>{40, 2}.data(), 2,
        ecuio_wasm::BUDGET_DEFAULT);
    EXPECT_EQ(sum.value, 42u);

    for (ecuio_wasm::instance* i : made) {
        ecuio_wasm::free_instance(i);
    }
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), before);
}

/* The check's refusal comes before the runtime is asked anything */
TEST(SandboxTest, AModuleTheCheckRefusesNeverReachesTheRuntime)
{
    warm();
    const uint64_t before = ecuio_wasm::runtime_allocations();
    module m = everything();
    m.funcs.push_back(spin_func(""));
    m.start = static_cast<int32_t>(m.funcs.size() - 1);
    Made refused(m, kAll, kAllCount);
    EXPECT_EQ(refused.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(refused.i, nullptr);
    EXPECT_STREQ(refused.why,
                 "the module has a start function, which would run "
                 "before any limit is set");
    EXPECT_EQ(ecuio_wasm::runtime_allocations(), before);
}

TEST(SandboxTest, AModuleTheRuntimeRefusesSaysItsWords)
{
    warm();
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    module m = good();
    m.funcs.push_back(
        plain_func("bad", {I32_ADD})); /* adds with nothing to add */
    Made refused(m, kAdd, 1);
    EXPECT_EQ(refused.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(refused.i, nullptr);
    EXPECT_EQ(
        std::string(refused.why).rfind("the runtime refused the module: ", 0),
        0u)
        << refused.why;
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), before);
}

/* A module under 1 MiB whose load would take more than a load may: out
 * of memory is the answer, with everything given back. A function costs
 * the runtime least where pointers are 4 bytes, and this many pass the
 * bound there too. */
TEST(SandboxTest, AModuleTooCostlyToMakeIsOutOfMemory)
{
    warm();
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    const module m = costly();
    const bytes code = m.build();
    ASSERT_LE(code.size(), ecuio_wasm::MODULE_MAX_BYTES)
        << "the check would refuse it";
    ASSERT_EQ(refusal(code), "");
    Made refused(m, kAdd, 1);
    EXPECT_EQ(refused.status, ECUIO_ERR_OUT_OF_MEMORY) << refused.why;
    EXPECT_EQ(refused.i, nullptr);
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), before);
}

/* A function may declare nearly 2^32 locals, and the runtime asks for a
 * block that large: refused whole, on a core whose sizes are 32 bits too,
 * where the block's size plus its head wraps to a few bytes. Which count
 * lands on the wrap depends on the runtime's own record, so each of the
 * last 1,024 is tried. */
TEST(SandboxTest, AFunctionDeclaringFourGibOfLocalsIsOutOfMemory)
{
    warm();
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    size_t starved = 0;
    for (uint32_t locals = 0xFFFFFC00u; locals != 0; ++locals) {
        module m = good();
        m.funcs[0].i32_locals = locals;
        const bytes code = m.build();
        ASSERT_EQ(refusal(code), "") << "the check never reads a function";
        Made refused(m, kAdd, 1);
        ASSERT_EQ(refused.i, nullptr) << locals;
        ASSERT_TRUE(refused.status == ECUIO_ERR_OUT_OF_MEMORY ||
                    refused.status == ECUIO_ERR_LOAD_FAILED)
            << locals << ": " << refused.status;
        starved += refused.status == ECUIO_ERR_OUT_OF_MEMORY ? 1 : 0;
    }
    EXPECT_GE(starved, 512u) << "most reach the allocator, which refuses";
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), before);
}

/* The runtime writes a call's results over its argument cells, as many as
 * the export's type has, so a want is held to what a call carries */
TEST(SandboxTest, AWantedTypeACallCantCarryIsRefused)
{
    warm();
    const uint64_t before = ecuio_wasm::runtime_allocations();
    for (const char* type :
         {":ii", ":I", "I:", "f:", "ii:F", "iiiii:", "ii", ":iiiii", "iI:i"}) {
        const wanted_export wants[] = {{"add", type}};
        Made refused(good(), wants, 1);
        EXPECT_EQ(refused.status, ECUIO_ERR_INVALID_ARGUMENT) << type;
        EXPECT_EQ(refused.i, nullptr) << type;
        EXPECT_STREQ(refused.why,
                     "wanted export 0 has a type a call can't carry")
            << type;
    }
    const wanted_export second[] = {{"add", "ii:i"}, {"add", ":ii"}};
    Made refused(good(), second, 2);
    EXPECT_EQ(refused.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(refused.why, "wanted export 1 has a type a call can't carry");
    EXPECT_EQ(ecuio_wasm::runtime_allocations(), before)
        << "judged before the module";

    module four = good();
    func wide;
    wide.params = {I32, I32, I32, I32};
    wide.results = {I32};
    wide.body = {LOCAL_GET, 3};
    wide.exported = "last";
    four.funcs.push_back(wide);
    const wanted_export most[] = {{"last", "iiii:i"}};
    Made made(four, most, 1);
    ASSERT_EQ(made.status, ECUIO_OK) << made.why;
    EXPECT_EQ(made.call(0, {1, 2, 3, 4}).value, 4u) << "ARGS_MAX is allowed";
}

/* Two ECUs of one plugin are two instances of one loaded module: what
 * ends one leaves the other as it was */
TEST(SandboxTest, ADeadInstanceLeavesAnotherOfItsModuleAnswering)
{
    Loaded l(everything(), kAll, kAllCount);
    ASSERT_EQ(l.status, ECUIO_OK) << l.why;
    Made a(l);
    Made b(l);
    ASSERT_EQ(a.status, ECUIO_OK) << a.why;
    ASSERT_EQ(b.status, ECUIO_OK) << b.why;
    EXPECT_EQ(a.call(POKE).end, call_end::returned);
    EXPECT_EQ(a.call(SPIN).end, call_end::budget);
    ASSERT_TRUE(ecuio_wasm::instance_dead(a.i));

    EXPECT_FALSE(ecuio_wasm::instance_dead(b.i));
    EXPECT_STREQ(ecuio_wasm::instance_trap(b.i), "");
    const ecuio_wasm::call_result sum = b.call(ADD, {40, 2});
    EXPECT_EQ(sum.end, call_end::returned);
    EXPECT_EQ(sum.value, 42u);
    EXPECT_EQ(b.call(PEEK).value, 0u) << "the other's memory is its own";
    EXPECT_EQ(b.call(COUNTDOWN, {100}).end, call_end::returned);
    EXPECT_EQ(a.call(ADD, {40, 2}).end, call_end::dead);
}

/* The runtime knows a module's exported malloc and free and would run
 * them, with no limit, to allocate in the guest for the host. Core never
 * asks it to: these two never return, so a call of either at the making
 * or around an export's call hangs the case. */
TEST(SandboxTest, AnExportedMallocAndFreeAreNeverRun)
{
    module m = everything();
    func endless_malloc =
        spin_func("malloc"); /* (i32) -> i32, as the runtime looks for */
    endless_malloc.params = {I32};
    endless_malloc.results = {I32};
    endless_malloc.body.insert(endless_malloc.body.end(), {I32_CONST, 0});
    func endless_free = spin_func("free"); /* (i32) -> () */
    endless_free.params = {I32};
    m.funcs.push_back(endless_malloc);
    m.funcs.push_back(endless_free);

    Made made(m, kAll, kAllCount);
    ASSERT_EQ(made.status, ECUIO_OK) << made.why;
    const ecuio_wasm::call_result sum = made.call(ADD, {40, 2});
    EXPECT_EQ(sum.end, call_end::returned);
    EXPECT_EQ(sum.value, 42u);
    EXPECT_EQ(made.call(COUNTDOWN, {100}).end, call_end::returned);
    EXPECT_EQ(made.call(POKE).end, call_end::returned);
    EXPECT_EQ(made.call(PEEK).value, 7u);
    EXPECT_FALSE(ecuio_wasm::instance_dead(made.i));
}

/* The runtime writes its warnings to standard output. None reaches it:
 * a front end's output is its own (havne-cli's is lines of JSON). The
 * first module has a custom section with a NUL in its name: the check
 * reads no custom section, and the runtime warns of the name and
 * refuses it. */
TEST(SandboxTest, NothingAModuleDoesPrints)
{
    warm();
    module named = good();
    named.tail = raw_section(0, {0x03, 'a', 0x00, 'b'});
    const module too_costly = costly();

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    ecuio_status_t named_status = ECUIO_OK;
    std::string named_why;
    ecuio_status_t costly_status = ECUIO_OK;
    call_end ends[4] = {};
    {
        Made refused(named, kAdd, 1);
        named_status = refused.status;
        named_why = refused.why;
        Made starved(too_costly, kAdd, 1);
        costly_status = starved.status;
        size_t n = 0;
        for (size_t which : {SPIN, TRAP, OUTSIDE, RECURSE}) {
            Made m(everything(), kAll, kAllCount);
            ends[n++] = m.call(which).end;
        }
    }
    const std::string out = testing::internal::GetCapturedStdout();
    const std::string err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(out, "");
    EXPECT_EQ(err, "");
    EXPECT_EQ(named_status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(named_why.rfind("the runtime refused the module: ", 0), 0u)
        << "the runtime, not the check, met the name: " << named_why;
    EXPECT_EQ(costly_status, ECUIO_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(ends[0], call_end::budget);
    EXPECT_EQ(ends[1], call_end::trapped);
    EXPECT_EQ(ends[2], call_end::trapped);
    EXPECT_EQ(ends[3], call_end::trapped);
}

/* Made at attach on the caller's thread, called on the worker */
TEST(SandboxTest, AnInstanceMadeOnOneThreadRunsOnAnother)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    ecuio_wasm::call_result sum;
    ecuio_wasm::call_result deep;
    std::thread worker([&] {
        sum = m.call(ADD, {40, 2});
        deep = m.call(RECURSE);
    });
    worker.join();
    EXPECT_EQ(sum.value, 42u);
    EXPECT_EQ(deep.end, call_end::trapped)
        << "the worker's own stack bounds it";
}

/* Instances of two modules: two sessions' workers */
TEST(SandboxTest, TwoInstancesOfTwoModulesRunAtOnce)
{
    Made a(everything(), kAll, kAllCount);
    Made b(everything(), kAll, kAllCount);
    ASSERT_EQ(a.status, ECUIO_OK) << a.why;
    ASSERT_EQ(b.status, ECUIO_OK) << b.why;
    uint32_t sums[2] = {};
    auto run = [&](Made& m, uint32_t* sum) {
        for (uint32_t k = 0; k < 2000; ++k) {
            *sum += m.call(ADD, {k, 1}).value;
            m.call(COUNTDOWN, {50});
        }
    };
    std::thread first(run, std::ref(a), &sums[0]);
    std::thread second(run, std::ref(b), &sums[1]);
    first.join();
    second.join();
    EXPECT_EQ(sums[0], 2000u * 2001u / 2u);
    EXPECT_EQ(sums[1], sums[0]);
}

/* A session loads its plugin's module once and makes an instance of it
 * for each ECU: the second instance costs no second load, and the module
 * stays until its last instance goes, whoever lets go first */
TEST(SandboxTest, AModuleIsLoadedOnceForItsInstances)
{
    warm();
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    const bytes code = largest().build();
    char why[ecuio_wasm::REFUSAL_MAX] = {};
    ecuio_wasm::loaded* m = nullptr;
    ASSERT_EQ(ecuio_wasm::load_module(code.data(), code.size(), kAll, kAllCount,
                                      &m, why),
              ECUIO_OK)
        << why;
    const size_t load = ecuio_wasm::runtime_bytes_in_use() - before;
    EXPECT_GE(load, code.size()) << "the module's bytes, kept for the runtime";
    ecuio_wasm::instance* a = nullptr;
    ecuio_wasm::instance* b = nullptr;
    ASSERT_EQ(ecuio_wasm::make_instance(m, &a, why), ECUIO_OK) << why;
    const size_t one = ecuio_wasm::runtime_bytes_in_use() - before - load;
    EXPECT_GT(one,
              ecuio_wasm::MEMORY_MAX_PAGES * 65536u + ecuio_wasm::STACK_BYTES);
    ASSERT_EQ(ecuio_wasm::make_instance(m, &b, why), ECUIO_OK) << why;
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use() - before, load + 2 * one);

    ecuio_wasm::free_module(m); /* the loader lets go first */
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use() - before, load + 2 * one)
        << "held by its instances";
    const uint32_t args[2] = {40, 2};
    EXPECT_EQ(
        ecuio_wasm::call_instance(a, ADD, args, 2, ecuio_wasm::BUDGET_DEFAULT)
            .value,
        42u);
    ecuio_wasm::free_instance(a);
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use() - before, load + one);
    EXPECT_EQ(
        ecuio_wasm::call_instance(b, ADD, args, 2, ecuio_wasm::BUDGET_DEFAULT)
            .value,
        42u);
    ecuio_wasm::free_instance(b);
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), before);

    EXPECT_EQ(ecuio_wasm::make_instance(nullptr, &a, why),
              ECUIO_ERR_NULL_ARGUMENT);
}

/* The instances of one module share its code alone: each its own memory
 * and its own globals */
TEST(SandboxTest, TwoInstancesOfOneModuleShareNoMemoryOrGlobal)
{
    module m = everything();
    m.i32_globals = 1;
    m.funcs.push_back(plain_func("bump", {GLOBAL_GET, 0, I32_CONST, 1, I32_ADD,
                                          GLOBAL_SET, 0, GLOBAL_GET, 0}));
    const wanted_export wants[] = {
        {"poke", ":"}, {"peek", ":i"}, {"bump", ":i"}};
    enum : size_t { POKED, PEEKED, BUMPED };
    Loaded l(m, wants, 3);
    ASSERT_EQ(l.status, ECUIO_OK) << l.why;
    Made a(l);
    Made b(l);
    ASSERT_EQ(a.status, ECUIO_OK) << a.why;
    ASSERT_EQ(b.status, ECUIO_OK) << b.why;
    EXPECT_EQ(a.call(BUMPED).value, 1u);
    EXPECT_EQ(a.call(BUMPED).value, 2u);
    EXPECT_EQ(a.call(BUMPED).value, 3u);
    EXPECT_EQ(b.call(BUMPED).value, 1u) << "the global is each instance's";
    EXPECT_EQ(a.call(POKED).end, call_end::returned);
    EXPECT_EQ(a.call(PEEKED).value, 7u);
    EXPECT_EQ(b.call(PEEKED).value, 0u) << "and the memory";
    uint8_t* mine = ecuio_wasm::instance_memory(a.i, 0, 4);
    uint8_t* theirs = ecuio_wasm::instance_memory(b.i, 0, 4);
    ASSERT_NE(mine, nullptr);
    ASSERT_NE(theirs, nullptr);
    EXPECT_NE(mine, theirs);
}

/* The most functions a module of `pages` pages holds while its load and
 * one instance of it both fit their bounds: what an instance costs
 * differs by pointer width, so it is found, not named */
uint32_t most_functions(uint32_t pages)
{
    const auto fits = [pages](uint32_t n) {
        Loaded l(padded(n, pages), kAdd, 1);
        Made i(l);
        return l.status == ECUIO_OK && i.status == ECUIO_OK;
    };
    uint32_t lo = 0;        /* fits */
    uint32_t hi = 1u << 17; /* doesn't */
    EXPECT_TRUE(fits(lo));
    EXPECT_FALSE(fits(hi));
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        (fits(mid) ? lo : hi) = mid;
    }
    return lo;
}

/* The costliest module a session can bind, by either bound: its load and
 * an instance for each of a session's most ECUs fit the runtime's bound
 * together, and every instance answers */
TEST(SandboxTest, ThirtyTwoInstancesOfTheCostliestModuleAnswer)
{
    warm();
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    for (const uint32_t pages : {1u, ecuio_wasm::MEMORY_MAX_PAGES}) {
        const uint32_t n = most_functions(pages);
        ASSERT_GT(n, 10000u) << pages << " pages";
        Made past(padded(n + 1, pages), kAdd, 1);
        EXPECT_EQ(past.status, ECUIO_ERR_OUT_OF_MEMORY) << pages << " pages";

        Loaded l(padded(n, pages), kAdd, 1);
        ASSERT_EQ(l.status, ECUIO_OK) << l.why;
        std::vector<std::unique_ptr<Made>> ecus;
        for (uint32_t c = 0; c < ECUIO_MAX_CHANNELS; ++c) {
            ecus.push_back(std::make_unique<Made>(l));
            ASSERT_EQ(ecus.back()->status, ECUIO_OK)
                << pages << " pages, ECU " << c << ": " << ecus.back()->why;
        }
        EXPECT_LE(ecuio_wasm::runtime_bytes_in_use(),
                  ecuio_wasm::RUNTIME_BYTES_MAX);
        for (uint32_t c = 0; c < ECUIO_MAX_CHANNELS; ++c) {
            const ecuio_wasm::call_result sum = ecus[c]->call(0, {40, c});
            EXPECT_EQ(sum.end, call_end::returned) << c;
            EXPECT_EQ(sum.value, 40u + c) << c;
        }
    }
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), before);
}

} // namespace
