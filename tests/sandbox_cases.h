// File/layer: tests/sandbox_cases.h | test helper: what the sandbox's test files share
// Testing: used by test_wasm_check.cpp, test_sandbox.cpp and test_sandbox_budget.cpp
//
// A well-formed module and its wanted export, the check's refusal as a
// string, and a loaded module and an instance that free themselves.

#ifndef ECUIO_TESTS_SANDBOX_CASES_H
#define ECUIO_TESTS_SANDBOX_CASES_H

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wasm_builder.h"
#include "wasm_check.h"
#include "wasm_sandbox.h"

namespace sandbox_cases {

using namespace wasm_builder;
using ecuio_wasm::wanted_export;

/* A well-formed module with one export, add: (i32, i32) -> i32 */
inline module good()
{
    module m;
    m.memory_min = 1;
    m.memory_max = 1;
    m.funcs.push_back(add_func("add"));
    return m;
}

inline const wanted_export kAdd[] = {{"add", "ii:i"}};

/* A section's bytes: its id, its size, its content */
inline bytes raw_section(uint8_t id, const bytes& content)
{
    bytes out;
    section(out, id, content);
    return out;
}

inline std::string refusal(const bytes& code, const wanted_export* wants = kAdd,
                    size_t count = 1)
{
    const ecuio_wasm::check_result r =
        ecuio_wasm::check_module(code.data(), code.size(), wants, count);
    EXPECT_EQ(r.ok, r.why[0] == '\0') << r.why;
    EXPECT_EQ(std::strchr(r.why, '\n'), nullptr) << "a refusal is one line";
    return r.why;
}

inline std::string refusal(const module& m, const wanted_export* wants = kAdd,
                    size_t count = 1)
{
    return refusal(m.build(), wants, count);
}

/* A loaded module and what its load said; let go at the end */
struct Loaded {
    ecuio_wasm::loaded* m = nullptr;
    ecuio_status_t status = ECUIO_OK;
    char why[ecuio_wasm::REFUSAL_MAX] = {};

    Loaded(const module& code, const wanted_export* wants, size_t count)
    {
        const bytes built = code.build();
        status = ecuio_wasm::load_module(built.data(), built.size(), wants,
                                         count, &m, why);
    }
    ~Loaded() { ecuio_wasm::free_module(m); }
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
};

/* An instance and what its making said; freed at the end */
struct Made {
    ecuio_wasm::instance* i = nullptr;
    ecuio_status_t status = ECUIO_OK;
    char why[ecuio_wasm::REFUSAL_MAX] = {};

    /* of a module of its own */
    Made(const module& m, const wanted_export* wants, size_t count)
    {
        const bytes code = m.build();
        status = ecuio_wasm::make_instance(code.data(), code.size(), wants,
                                           count, &i, why);
    }
    /* of a loaded module */
    explicit Made(const Loaded& l)
    {
        status = ecuio_wasm::make_instance(l.m, &i, why);
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

} // namespace sandbox_cases

#endif // ECUIO_TESTS_SANDBOX_CASES_H
