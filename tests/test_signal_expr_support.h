// File/layer: tests/test_signal_expr_support.h | C++17, Qt-free, every platform
// Testing: UNIT: the helpers of test_signal_expr*.cpp (test_signals)

#pragma once

#include <gtest/gtest.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>

#include <atomic>
#include <cstdio>
#include <cstring>
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

inline File load(const std::string& json)
{
    ecuio_signal_file_t* f = nullptr;
    if (ecuio_signal_file_load(json.data(), json.size(), &f) != ECUIO_OK) {
        EXPECT_EQ(f, nullptr);
    }
    return File(f);
}

/* A computed signal's JSON */
inline std::string computed(const std::string& key, const std::string& expr,
                     const std::string& more = "")
{
    return "{\"key\":\"" + key + "\",\"name\":\"N " + key +
           "\",\"kind\":\"computed\",\"expr\":\"" + expr + "\"" + more + "}";
}

/* Four numbers, a to d (named A to D), then signals as given; head goes
 * before "signals" ("\"extends\":\"obd2\",") */
inline std::string file_of(int format, const std::vector<std::string>& signals = {},
                    const std::string& head = "")
{
    std::string s = "{\"havne_signals\":" + std::to_string(format) +
                    ",\"name\":\"T\",\"source\":{\"name\":\"t\"}," + head +
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

/* What a check answered */
struct Checked {
    ecuio_status_t status = ECUIO_OK;
    ecuio_expr_check_t out{};
    std::string detail;
};

inline Checked check(const ecuio_signal_file_t* f, const char* key,
              const std::string& expr)
{
    Checked c;
    std::memset(&c.out, 0xA5, sizeof c.out);
    c.out.struct_size = sizeof c.out;
    c.status =
        ecuio_signal_file_check_expr(f, key, expr.data(), expr.size(), &c.out);
    c.detail = ecuio_last_error_detail();
    if (c.status == ECUIO_OK || c.out.at != 0) {
        EXPECT_EQ(c.out.struct_size, sizeof c.out);
        EXPECT_EQ(c.out.flags, 0u);
        for (const uint32_t r : c.out.reserved) {
            EXPECT_EQ(r, 0u);
        }
    }
    if (c.status == ECUIO_OK) {
        EXPECT_EQ(c.out.at, 0u) << expr;
        EXPECT_EQ(c.detail, "") << expr;
    } else if (c.out.at != 0) {
        const ecuio_computed_t none{};
        EXPECT_EQ(c.out.format, 0u) << expr;
        EXPECT_EQ(c.out.depth, 0u) << expr;
        EXPECT_EQ(std::memcmp(&c.out.program, &none, sizeof none), 0) << expr;
    }
    return c;
}

/* The words a refused load leaves, without their place in the file */
inline std::string load_words(const std::string& json)
{
    File f = load(json);
    EXPECT_FALSE(f) << json;
    const std::string detail = ecuio_last_error_detail();
    const size_t colon = detail.find(": ");
    EXPECT_EQ(detail.compare(0, 5, "line "), 0) << detail;
    return colon == std::string::npos ? detail : detail.substr(colon + 2);
}

inline const ecuio_computed_t& program_of(const ecuio_signal_file_t* f,
                                   const char* key)
{
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key);
    return ecuio_signal_file_set(f)->computed[def->table_first];
}

/* The name f's expressions read def by, or "!" and the refusal's detail */
inline std::string ref_of(const ecuio_signal_file_t* f, const ecuio_signal_def_t* def)
{
    char name[ECUIO_EXPR_REF_LEN];
    std::memset(name, 'x', sizeof name);
    if (ecuio_signal_file_expr_ref(f, def, name, sizeof name) != ECUIO_OK) {
        EXPECT_EQ(name[0], '\0');
        return std::string("!") + ecuio_last_error_detail();
    }
    return name;
}

/* The def of f's set an expression reads as name */
inline const ecuio_signal_def_t* def_read_as(const ecuio_signal_file_t* f,
                                      const std::string& name)
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    for (uint32_t i = 0; i < set->count; ++i) {
        if (ref_of(f, &set->defs[i]) == name) {
            return &set->defs[i];
        }
    }
    return nullptr;
}

} // namespace
