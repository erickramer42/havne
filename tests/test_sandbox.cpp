// File/layer: tests/test_sandbox.cpp | UNIT: core/src/wasm_check.h and wasm_sandbox.h
// Testing: WasmCheckTest, SandboxTest, Qt-free, every tree (build-win32's x86 core too)
//
// The sandbox a plugin's code runs in (ADR-0066 section 2.2): the check
// that judges a module before the runtime sees it, and an instance's
// life. Every module is built here by tests/wasm_builder.h, most of them
// hostile: an endless loop, a start function, an import, a memory that
// grows, a trap of each kind.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "wasm_builder.h"
#include "wasm_check.h"
#include "wasm_sandbox.h"

namespace {

using namespace wasm_builder;
using ecuio_wasm::call_end;
using ecuio_wasm::wanted_export;

/* A well-formed module with one export, add: (i32, i32) -> i32 */
module good()
{
    module m;
    m.memory_min = 1;
    m.memory_max = 1;
    m.funcs.push_back(add_func("add"));
    return m;
}

const wanted_export kAdd[] = {{"add", "ii:i"}};

/* A section's bytes: its id, its size, its content */
bytes raw_section(uint8_t id, const bytes& content)
{
    bytes out;
    section(out, id, content);
    return out;
}

std::string refusal(const bytes& code, const wanted_export* wants = kAdd,
                    size_t count = 1)
{
    const ecuio_wasm::check_result r =
        ecuio_wasm::check_module(code.data(), code.size(), wants, count);
    EXPECT_EQ(r.ok, r.why[0] == '\0') << r.why;
    EXPECT_EQ(std::strchr(r.why, '\n'), nullptr) << "a refusal is one line";
    return r.why;
}

std::string refusal(const module& m, const wanted_export* wants = kAdd,
                    size_t count = 1)
{
    return refusal(m.build(), wants, count);
}

TEST(WasmCheckTest, AWellFormedModuleWithItsExportPasses)
{
    EXPECT_EQ(refusal(good()), "");
    EXPECT_EQ(refusal(good(), nullptr, 0), "") << "no export wanted";
}

TEST(WasmCheckTest, WhatWouldRunBeforeALimitIsRefused)
{
    module m = good();
    m.funcs.push_back(store_func("", 7));
    m.start = 1;
    EXPECT_EQ(refusal(m),
              "the module has a start function, which would run before "
              "any limit is set");
    for (const char* name :
         {"__post_instantiate", "__wasm_call_ctors", "_initialize"}) {
        module e = good();
        e.funcs.push_back(store_func(name, 7));
        EXPECT_EQ(refusal(e), std::string("the module exports ") + name +
                                  ", which would run before any limit is set");
    }
}

TEST(WasmCheckTest, AnImportIsRefusedByName)
{
    module m = good();
    m.imports.push_back({"env", "clock"});
    EXPECT_EQ(refusal(m),
              "the module imports env.clock: a plugin's module imports "
              "nothing");
}

TEST(WasmCheckTest, TheMemoryMustStateItsWholeSizeWithinTheCap)
{
    module none = good();
    none.memory = false;
    EXPECT_EQ(refusal(none), "the module has 0 memories: it needs exactly one");

    module open = good();
    open.memory_max = -1;
    EXPECT_EQ(refusal(open),
              "the module's memory states no maximum (minimum 1): a "
              "plugin's is one fixed size (link with --max-memory equal "
              "to --initial-memory)");

    module grows = good();
    grows.memory_max = 2;
    EXPECT_EQ(refusal(grows),
              "the module's memory grows from 1 to 2 pages: a plugin's "
              "is one fixed size (link with --max-memory equal to "
              "--initial-memory)");

    module large = good();
    large.memory_min = 17;
    large.memory_max = 17;
    EXPECT_EQ(refusal(large),
              "the module's memory is 17 pages: 1 to 16 (1 MiB) are "
              "allowed");

    module empty = good();
    empty.memory_min = 0;
    empty.memory_max = 0;
    EXPECT_EQ(refusal(empty),
              "the module's memory is 0 pages: 1 to 16 (1 MiB) are "
              "allowed");

    module shared = good();
    shared.memory_flags = 0x03;
    EXPECT_EQ(refusal(shared),
              "the module's memory is shared or 64-bit, which a "
              "plugin's can't be");

    module full = good();
    full.memory_min = 16;
    full.memory_max = 16;
    EXPECT_EQ(refusal(full), "") << "the cap itself is allowed";
}

TEST(WasmCheckTest, AWantedExportMustBeAFunctionOfItsType)
{
    const wanted_export missing[] = {{"begin", "ii:i"}};
    EXPECT_EQ(refusal(good(), missing, 1), "the module lacks the export begin");

    const wanted_export other_result[] = {{"add", "ii:"}};
    EXPECT_EQ(
        refusal(good(), other_result, 1),
        "the module's export add isn't a function of type (i32, i32) -> ()");
    const wanted_export fewer[] = {{"add", "i:i"}};
    EXPECT_EQ(refusal(good(), fewer, 1),
              "the module's export add isn't a function of type (i32) -> i32");
    const wanted_export wider[] = {{"add", "iI:i"}};
    EXPECT_EQ(
        refusal(good(), wider, 1),
        "the module's export add isn't a function of type (i32, i64) -> i32");

    module m = good();
    m.memory_export = "begin"; /* the name, on a memory */
    EXPECT_EQ(
        refusal(m, missing, 1),
        "the module's export begin isn't a function of type (i32, i32) -> i32");
}

/* A section repeated or out of its place is judged all the same: the
 * runtime may refuse such a module, but this check never relies on it */
TEST(WasmCheckTest, ARepeatedOrMisplacedSectionHidesNothing)
{
    module late_start = good();
    late_start.tail = raw_section(8, {0x00});
    EXPECT_EQ(refusal(late_start),
              "the module has a start function, which would run "
              "before any limit is set");

    module late_import = good();
    bytes one = {0x01};
    name(one, "env");
    name(one, "f");
    one.insert(one.end(), {0x00, 0x00});
    late_import.tail = raw_section(2, one);
    EXPECT_EQ(refusal(late_import),
              "the module imports env.f: a plugin's module "
              "imports nothing");

    module second_memory = good();
    second_memory.tail = raw_section(5, {0x01, 0x01, 0x01, 0x01});
    EXPECT_EQ(refusal(second_memory),
              "the module has 2 memories: it needs exactly one");

    module second_exports = good();
    second_exports.tail = raw_section(7, {0x00});
    EXPECT_EQ(refusal(second_exports), "the module has two export sections");
}

TEST(WasmCheckTest, WhatIsNotAModuleOfABoundedShapeIsRefused)
{
    EXPECT_EQ(refusal(bytes{0x00, 0x61, 0x73, 0x6D, 0x02, 0x00, 0x00, 0x00}),
              "not a WebAssembly module of version 1");
    EXPECT_EQ(refusal(bytes{'{', '}'}),
              "not a WebAssembly module of version 1");

    bytes large = good().build();
    large.resize(ecuio_wasm::MODULE_MAX_BYTES + 1, 0);
    EXPECT_EQ(refusal(large), "the module is 1048577 bytes: 1 MiB is the most");

    module data_count = good();
    data_count.tail = raw_section(12, {0x00});
    EXPECT_EQ(refusal(data_count),
              "the module has a section of kind 12, which a "
              "plugin's can't have");

    /* A section whose size is a LEB128 number past 32 bits */
    module long_number = good();
    long_number.tail = {0x00, 0x80, 0x80, 0x80, 0x80, 0x10};
    EXPECT_EQ(refusal(long_number), "the module is cut short or malformed");
}

/* Cut anywhere, a module is refused, but for the one cut that leaves
 * every section the check reads whole (the code section alone gone, which
 * the runtime then refuses): never a read past its end, which the
 * sanitizer build would report */
TEST(WasmCheckTest, AModuleCutAnywhereIsJudgedWithinItsBytes)
{
    const bytes whole = good().build();
    size_t passed = 0;
    for (size_t n = 0; n < whole.size(); ++n) {
        const bytes cut(whole.begin(),
                        whole.begin() + static_cast<std::ptrdiff_t>(n));
        if (refusal(cut).empty()) {
            ++passed;
        }
    }
    EXPECT_EQ(passed, 1u)
        << "the cut before the code section, which the check never reads";
    EXPECT_EQ(refusal(whole), "");
}

/* ---- The sandbox ---- */

/* An instance and what its making said; freed at the end */
struct Made {
    ecuio_wasm::instance* i = nullptr;
    ecuio_status_t status = ECUIO_OK;
    char why[ecuio_wasm::REFUSAL_MAX] = {};

    Made(const module& m, const wanted_export* wants, size_t count)
    {
        const bytes code = m.build();
        status = ecuio_wasm::make_instance(code.data(), code.size(), wants,
                                           count, &i, why);
    }
    ~Made() { ecuio_wasm::free_instance(i); }
    Made(const Made&) = delete;
    Made& operator=(const Made&) = delete;

    ecuio_wasm::call_result call(size_t index, std::vector<uint32_t> args = {},
                                 uint32_t budget = ecuio_wasm::BUDGET_DEFAULT)
    {
        return ecuio_wasm::call_instance(
            i, index, args.data(), static_cast<uint32_t>(args.size()), budget);
    }
};

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

/* good() with 200,000 more functions that do nothing, of two types:
 * four bytes of module each, so under 1 MiB */
module costly()
{
    module m = good();
    func empty;  /* () -> () */
    func taking; /* (i32) -> () */
    taking.params = {I32};
    m.funcs.reserve(200001);
    for (int k = 0; k < 100000; ++k) {
        m.funcs.push_back(empty);
        m.funcs.push_back(taking);
    }
    return m;
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
 * fail the second of two calls that each need nearly all of it */
TEST(SandboxTest, TheBudgetIsEachCallsOwn)
{
    Made m(everything(), kAll, kAllCount);
    ASSERT_EQ(m.status, ECUIO_OK) << m.why;
    const uint32_t turns = 100;
    const uint32_t enough = turns * COUNTDOWN_TURN + 16;
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

/* A module under 1 MiB whose making would take more than an instance may:
 * out of memory is the answer, with everything given back. A function
 * costs the runtime least where pointers are 4 bytes, and this many pass
 * the bound there too. */
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

/* Two ECUs of one plugin are two instances of the same bytes: what ends
 * one leaves the other as it was */
TEST(SandboxTest, ADeadInstanceLeavesAnotherOfItsModuleAnswering)
{
    Made a(everything(), kAll, kAllCount);
    Made b(everything(), kAll, kAllCount);
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
 * first module's export has a NUL in its name, which the runtime warns
 * of and refuses. */
TEST(SandboxTest, NothingAModuleDoesPrints)
{
    warm();
    module named = good();
    named.funcs.push_back(plain_func(std::string("a\0b", 3), {I32_CONST, 1}));
    const module too_costly = costly();

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    ecuio_status_t named_status = ECUIO_OK;
    ecuio_status_t costly_status = ECUIO_OK;
    call_end ends[4] = {};
    {
        Made refused(named, kAdd, 1);
        named_status = refused.status;
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
    EXPECT_EQ(named_status, ECUIO_ERR_LOAD_FAILED)
        << "the runtime refuses the name";
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

TEST(SandboxTest, TwoInstancesRunAtOnce)
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

} // namespace
