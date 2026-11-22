// File/layer: tests/test_csv_guess_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_csv_guess*.cpp (test_core)

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "csv_guess.h"
#include "csv_import.h"
#include "json.h"
#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "temp_name.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;
namespace csv = ecuio_csv_import;
namespace guess = ecuio_map_guess;
namespace json = ecuio_json;

namespace {

/* ---- in memory ---- */

/* The guess over text, its JSON parsed */
struct Guessed {
    bool ok = false;
    std::string why;
    guess::result r;
    json::Value v;
    explicit Guessed(const std::string& text, const char* unit = nullptr,
                     bool cut = false)
    {
        ok = guess::guess(reinterpret_cast<const unsigned char*>(text.data()),
                          text.size(), cut, unit, r, &why);
        if (!ok) {
            return;
        }
        json::Error e;
        EXPECT_TRUE(json::parse(r.text.data(), r.text.size(), &v, &e))
            << e.to_string() << "\n"
            << r.text;
    }
    const json::Value& map() const { return *v.find("map"); }
    std::string map_text() const
    {
        return r.text.substr(r.map_offset, r.map_length);
    }
    /* A map key's text, or "" */
    std::string key(const char* k) const
    {
        const json::Value* x = map().find(k);
        return x != nullptr && x->type == json::Type::string ? x->text : "";
    }
    int64_t number(const char* k) const
    {
        const json::Value* x = map().find(k);
        return x != nullptr ? x->whole : -1;
    }
    std::string time(const char* k) const
    {
        const json::Value* t = map().find("time");
        const json::Value* x = t != nullptr ? t->find(k) : nullptr;
        return x != nullptr ? x->text : "";
    }
    bool time_has_unit() const { return map().find("time")->find("unit"); }
    const std::vector<json::Value>& signals() const
    {
        return map().find("signals")->items;
    }
    /* A signal's key by its column, or "-" for no signal, "" for none */
    std::string sig(const std::string& column, const char* k) const
    {
        for (const json::Value& s : signals()) {
            if (s.find("column")->text == column) {
                const json::Value* x = s.find(k);
                if (x == nullptr) {
                    return "";
                }
                return x->type == json::Type::string ? x->text
                                                     : std::to_string(x->whole);
            }
        }
        return "-";
    }
    std::vector<std::string> missing() const
    {
        std::vector<std::string> out;
        if (const json::Value* m = map().find("missing")) {
            for (const json::Value& x : m->items) {
                out.push_back(x.text);
            }
        }
        return out;
    }
    /* The columns list's why for a header text, "used" when used */
    std::string column(const std::string& c) const
    {
        for (const json::Value& x : v.find("columns")->items) {
            if (x.find("column")->text == c) {
                return x.find("used")->boolean ? "used" : x.find("why")->text;
            }
        }
        return "not listed";
    }
    std::vector<std::string> notes() const
    {
        std::vector<std::string> out;
        for (const json::Value& x : v.find("notes")->items) {
            out.push_back(x.text);
        }
        return out;
    }
    bool noted(const std::string& words) const
    {
        for (const std::string& n : notes()) {
            if (n.find(words) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

inline std::string refused(const std::string& text, const char* unit = nullptr)
{
    Guessed g(text, unit);
    EXPECT_FALSE(g.ok) << g.r.text;
    EXPECT_FALSE(g.why.empty());
    return g.why;
}

inline std::string notes_of(const Guessed& g)
{
    std::string all;
    for (const std::string& n : g.notes()) {
        all += n + "\n";
    }
    return all;
}

const std::string kSimple = "Time (s),Speed (km/h),Gear\n"
                            "0,0.0,N\n"
                            "0.5,1.25,1\n"
                            "1.0,2.5,1\n";

} // namespace
