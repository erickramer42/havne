// File/layer: tests/test_wamr_build.cpp | UNIT: the vendored WAMR as third_party/wamr.cmake builds it
// Testing: WamrBuildTest, Qt-free, every tree (build-win32's x86 core too)
//
// What core relies on from the vendored runtime, asked of WAMR's own API
// with no core code between: a module runs, the instruction limit counts
// every instruction under the dispatch this build uses, a start function
// runs at instantiate with no limit (which is why core refuses a module
// that has one before WAMR sees it), and a module's memory comes from the
// embedder's allocator, as it gives it. A switch in wamr.cmake that
// changes any of these fails here on every platform.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wasm_builder.h"
#include "wasm_export.h"

namespace {

using wasm_builder::bytes;

/* The runtime, up for one test; WAMR's own allocator calls go to malloc,
 * a module's memory filled with kFill and its sizes kept */
class WamrBuildTest : public ::testing::Test
{
protected:
    static constexpr int kFill = 0xA5;
    static std::vector<unsigned int>& memories()
    {
        static std::vector<unsigned int> sizes;
        return sizes;
    }

    void SetUp() override
    {
        RuntimeInitArgs init;
        std::memset(&init, 0, sizeof init);
        /* WAMR holds its allocator's functions as void pointers */
        init.mem_alloc_type = Alloc_With_Allocator;
        init.mem_alloc_option.allocator.malloc_func =
            reinterpret_cast<void*>(&take);
        init.mem_alloc_option.allocator.realloc_func =
            reinterpret_cast<void*>(&retake);
        init.mem_alloc_option.allocator.free_func =
            reinterpret_cast<void*>(&give);
        memories().clear();
        ASSERT_TRUE(wasm_runtime_full_init(&init));
    }

    void TearDown() override { wasm_runtime_destroy(); }

private:
    /* The signatures WASM_MEM_ALLOC_WITH_USAGE gives the allocator */
    static void* take(mem_alloc_usage_t usage, unsigned int n)
    {
        void* p = std::malloc(n);
        if (usage == Alloc_For_LinearMemory && p != nullptr) {
            memories().push_back(n);
            std::memset(p, kFill, n);
        }
        return p;
    }
    static void* retake(mem_alloc_usage_t, bool, void* p, unsigned int n)
    {
        return std::realloc(p, n);
    }
    static void give(mem_alloc_usage_t, void* p) { std::free(p); }
};

/* One module loaded and instantiated; freed in reverse at the end. WAMR
 * reads the module's bytes in place, so they live as long as it does. */
struct Instance {
    bytes code;
    char why[128] = {};
    wasm_module_t module = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t env = nullptr;

    explicit Instance(const wasm_builder::module& m) : code(m.build())
    {
        module = wasm_runtime_load(
            code.data(), static_cast<uint32_t>(code.size()), why, sizeof why);
        if (module != nullptr) {
            inst =
                wasm_runtime_instantiate(module, 16 * 1024, 0, why, sizeof why);
        }
        if (inst != nullptr) {
            env = wasm_runtime_create_exec_env(inst, 16 * 1024);
        }
    }

    ~Instance()
    {
        if (env != nullptr) {
            wasm_runtime_destroy_exec_env(env);
        }
        if (inst != nullptr) {
            wasm_runtime_deinstantiate(inst);
        }
        if (module != nullptr) {
            wasm_runtime_unload(module);
        }
    }

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    /* Calls an export under limit instructions; false with the exception
     * in why when it didn't return */
    bool call(const char* name, int limit, uint32_t* args, uint32_t count)
    {
        wasm_function_inst_t f = wasm_runtime_lookup_function(inst, name);
        if (f == nullptr) {
            std::snprintf(why, sizeof why, "no export %s", name);
            return false;
        }
        wasm_runtime_set_instruction_count_limit(env, limit);
        if (wasm_runtime_call_wasm(env, f, count, args)) {
            return true;
        }
        const char* e = wasm_runtime_get_exception(inst);
        std::snprintf(why, sizeof why, "%s", e != nullptr ? e : "no exception");
        return false;
    }
};

/* Whether countdown(n) returns under limit instructions, on an instance
 * of its own: one that hit the limit is never called again */
bool counts_down(uint32_t n, int limit)
{
    wasm_builder::module m;
    m.funcs.push_back(wasm_builder::countdown_func("countdown"));
    Instance i(m);
    if (i.env == nullptr) {
        ADD_FAILURE() << i.why;
        return false;
    }
    uint32_t args[1] = {n};
    return i.call("countdown", limit, args, 1);
}

/* The fewest instructions countdown(n) returns under */
int fewest(uint32_t n)
{
    int low = 0;       /* fails */
    int high = 100000; /* returns */
    EXPECT_TRUE(counts_down(n, high));
    while (high - low > 1) {
        const int mid = low + (high - low) / 2;
        if (counts_down(n, mid)) {
            high = mid;
        } else {
            low = mid;
        }
    }
    return high;
}

TEST_F(WamrBuildTest, AModuleLoadsAndItsFunctionRuns)
{
    wasm_builder::module m;
    m.funcs.push_back(wasm_builder::add_func("add"));
    Instance i(m);
    ASSERT_NE(i.env, nullptr) << i.why;
    uint32_t args[2] = {40, 2};
    ASSERT_TRUE(i.call("add", 1000, args, 2)) << i.why;
    EXPECT_EQ(args[0], 42u);
}

TEST_F(WamrBuildTest, AnEndlessLoopStopsAtTheInstructionLimit)
{
    wasm_builder::module m;
    m.funcs.push_back(wasm_builder::spin_func("spin"));
    Instance i(m);
    ASSERT_NE(i.env, nullptr) << i.why;
    EXPECT_FALSE(i.call("spin", 100000, nullptr, 0));
    EXPECT_NE(std::string(i.why).find("instruction limit exceeded"),
              std::string::npos)
        << i.why;
}

/* Ten more turns of the loop cost exactly ten turns' instructions, so
 * every instruction counts one: a dispatch that skipped the check (the
 * fast interpreter's switch at this tag) or counted by blocks fails it */
TEST_F(WamrBuildTest, TheLimitCountsEveryInstruction)
{
    const int ten = fewest(10);
    const int twenty = fewest(20);
    EXPECT_EQ(twenty - ten,
              static_cast<int>(10 * wasm_builder::COUNTDOWN_TURN));
    EXPECT_FALSE(counts_down(20, twenty - 1));
}

/* WAMR runs a start function inside instantiate, on an exec env of its
 * own that no limit can be set on. Core's check before load refuses a
 * module with one; this pins the behavior that check exists for. */
TEST_F(WamrBuildTest, AStartFunctionRunsAtInstantiateBeforeAnyLimitIsSet)
{
    wasm_builder::module m;
    m.funcs.push_back(wasm_builder::store_func("", 7));
    m.start = 0;
    Instance i(m);
    ASSERT_NE(i.env, nullptr) << i.why;
    uint8_t first = 0;
    const uint8_t* memory =
        static_cast<const uint8_t*>(wasm_runtime_addr_app_to_native(i.inst, 0));
    ASSERT_NE(memory, nullptr);
    first = memory[0];
    EXPECT_EQ(first, 7);
}

/* Core's caps count what its allocator hands out, so a module's memory
 * must be asked of it (WAMR maps pages of its own otherwise), and core
 * zeroes that memory because WAMR leaves it as the allocator gave it */
TEST_F(WamrBuildTest, AModulesMemoryComesFromTheAllocatorAsGiven)
{
    wasm_builder::module m;
    m.memory_min = 3;
    m.memory_max = 3;
    m.funcs.push_back(wasm_builder::add_func("add"));
    Instance i(m);
    ASSERT_NE(i.env, nullptr) << i.why;
    ASSERT_EQ(memories().size(), 1u);
    EXPECT_EQ(memories()[0], 3u * 65536u);
    const uint8_t* memory =
        static_cast<const uint8_t*>(wasm_runtime_addr_app_to_native(i.inst, 0));
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(memory[0], kFill);
    EXPECT_EQ(memory[3 * 65536 - 1], kFill);
}

} // namespace
