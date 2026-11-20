// File/layer: tests/test_csv_import_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_csv_import*.cpp (test_core)

#pragma once

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "csv_import.h"
#include "ecuio/core.h"
#include "ecuio/internal.h" /* ecuio_import_test_between_passes */
#include "ecuio/recording.h"
#include "partial_left.h"
#include "temp_name.h"

void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;
namespace csv = ecuio_csv_import;

namespace {

/* ---- in memory ---- */

struct Sample {
    uint64_t us;
    size_t signal;
    bool text;
    double number;
    std::string value;
};

struct Collect {
    std::vector<Sample> got;
    bool number(uint64_t us, size_t s, double v)
    {
        got.push_back({us, s, false, v, {}});
        return true;
    }
    bool text(uint64_t us, size_t s, const std::string& v)
    {
        got.push_back({us, s, true, 0, v});
        return true;
    }
    std::string failure() const { return "never"; }
};

/* The map parsed and the CSV run through it */
struct Pass {
    bool ok = false;
    std::string why;
    csv::column_map map;
    csv::outcome out;
    std::vector<Sample> got;
    Pass(const std::string& map_json, const std::string& text)
    {
        if (!csv::parse_map(map_json.data(), map_json.size(), map, &why)) {
            return;
        }
        csv::memory_source src{
            reinterpret_cast<const unsigned char*>(text.data()), text.size()};
        Collect c;
        ok = csv::run(src, map, c, out, &why);
        got = std::move(c.got);
    }
};

/* A map of a time column and signals, the rest of its keys in extra */
inline std::string map_of(const std::string& time_column, const std::string& unit,
                   const std::string& signals, const std::string& extra = "")
{
    return "{\"havne_column_map\": 1, \"time\": {\"column\": \"" + time_column +
           "\", \"unit\": \"" + unit + "\"}, \"signals\": [" + signals + "]" +
           (extra.empty() ? "" : ", " + extra) + "}";
}

const std::string kRpm =
    R"({"column": "Engine Speed", "name": "RPM", "unit": "rpm"})";
const std::string kGear =
    R"({"column": "Gear", "name": "Gear", "kind": "text"})";

inline std::string refused(const std::string& map_json, const std::string& text)
{
    Pass p(map_json, text);
    EXPECT_FALSE(p.ok) << text;
    EXPECT_FALSE(p.why.empty());
    return p.why;
}

} // namespace
