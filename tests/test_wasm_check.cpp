// File/layer: tests/test_wasm_check.cpp | UNIT: core/src/wasm_check.h
// Testing: WasmCheckTest (test_sandbox), Qt-free, every tree (build-win32's x86 core too)
//
// The check that judges a module before the runtime sees it (ADR-0066
// section 2.2). Every module is built here by tests/wasm_builder.h, most
// of them hostile: a start function, an import, a memory that grows, a
// section repeated or cut.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "sandbox_cases.h"
#include "wasm_builder.h"
#include "wasm_check.h"

namespace {

using namespace wasm_builder;
using namespace sandbox_cases;
using ecuio_wasm::wanted_export;

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

/* The runtime looks an export up by its name as a C string, so a name
 * with a NUL in it could pass for the name before the NUL */
TEST(WasmCheckTest, AnExportNameHoldingANulIsRefused)
{
    for (const std::string& text :
         {std::string("__post_instantiate\0x", 20), std::string("add\0", 4),
          std::string("\0", 1)}) {
        module m = good();
        m.funcs.push_back(store_func(text, 7));
        EXPECT_EQ(refusal(m), "the module exports a name that holds a NUL");
    }
}

TEST(WasmCheckTest, AWantedExportIsExportedOnce)
{
    module twice = good();
    twice.funcs.push_back(add_func("add"));
    EXPECT_EQ(refusal(twice), "the module exports add twice");

    module other = good();
    other.funcs.push_back(add_func("sum"));
    other.funcs.push_back(add_func("sum"));
    EXPECT_EQ(refusal(other), "")
        << "an unwanted name is the runtime's to judge";
}

/* The runtime compares each function type with every one before it */
TEST(WasmCheckTest, TheFunctionTypesAreCounted)
{
    auto types = [](uint32_t count) {
        bytes code = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        bytes content;
        leb(content, count);
        for (uint32_t k = 0; k < count; ++k) {
            content.insert(content.end(), {0x60, 0x00, 0x00});
        }
        section(code, 1, content);
        return code;
    };
    EXPECT_EQ(refusal(types(ecuio_wasm::TYPES_MAX + 1)),
              "the module states 1025 function types: 1024 are the most");
    EXPECT_EQ(refusal(types(ecuio_wasm::TYPES_MAX)),
              "the module has 0 memories: it needs exactly one")
        << "the cap itself is allowed";

    bytes claimed = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    bytes count_only;
    leb(count_only, 0xFFFFFFFFu);
    section(claimed, 1, count_only);
    EXPECT_EQ(refusal(claimed),
              "the module states 4294967295 function types: 1024 are the "
              "most")
        << "the count is the module's claim, judged before any type";
}

/* A count is the module's own claim: a function section that claims 2^32
 * entries and holds none is walked to its end, not to the claim */
TEST(WasmCheckTest, ACountPastItsSectionCostsNoMoreThanTheSection)
{
    bytes code = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    section(code, 1, {0x01, 0x60, 0x02, I32, I32, 0x01, I32});
    bytes claim;
    leb(claim, 0xFFFFFFFFu);
    section(code, 3, claim);
    section(code, 5, {0x01, 0x01, 0x01, 0x01});
    bytes exports = {0x01};
    name(exports, "add");
    exports.push_back(0x00);
    leb(exports, 0xFFFFFFFEu);
    section(code, 7, exports);

    const auto began = std::chrono::steady_clock::now();
    const std::string why = refusal(code);
    const auto took = std::chrono::steady_clock::now() - began;
    EXPECT_EQ(why, "the module's export add isn't a function of type "
                   "(i32, i32) -> i32");
    EXPECT_LT(took, std::chrono::seconds(1))
        << "walking to the claimed index is over four thousand million steps";
}

/* The runtime scans a block's code for its end, so a function's size is
 * the most one instruction can make it read */
TEST(WasmCheckTest, AFunctionsCodeIsHeldToTheCap)
{
    /* a body is its locals' count (one byte here), its code and its end */
    auto sized = [](uint32_t size) {
        module m = good();
        m.funcs.push_back(plain_func("", bytes(size - 2, NOP), false));
        return m;
    };
    EXPECT_EQ(refusal(sized(ecuio_wasm::BODY_MAX_BYTES)), "")
        << "the cap itself is allowed";
    EXPECT_EQ(refusal(sized(ecuio_wasm::BODY_MAX_BYTES + 1)),
              "function 1's code is 65537 bytes: 64 KiB is the most");

    /* three blocks of 340 KB in one function: a module of under 1 MiB */
    module huge = good();
    huge.funcs[0].body = bytes(3 * 339968, NOP);
    ASSERT_LE(huge.build().size(), ecuio_wasm::MODULE_MAX_BYTES);
    EXPECT_EQ(refusal(huge),
              "function 0's code is 1019906 bytes: 64 KiB is the most");

    /* a code section that claims a second body it doesn't hold */
    bytes code = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    section(code, 10, {0x02, 0x02, 0x00, END});
    EXPECT_EQ(refusal(code), "the module is cut short or malformed");
}

} // namespace
