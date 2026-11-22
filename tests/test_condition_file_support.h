// File/layer: tests/test_condition_file_support.h | C++17, Qt-free, every platform
// Testing: UNIT: the helpers of test_condition_file*.cpp (test_signals)

#pragma once

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>
#include "condition_file.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace {

struct SetFree {
    void operator()(ecuio_condition_set_t* s) const
    {
        ecuio_condition_set_free(s);
    }
};
using Set = std::unique_ptr<ecuio_condition_set_t, SetFree>;

struct FileFree {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};
using File = std::unique_ptr<ecuio_signal_file_t, FileFree>;

inline Set load(const std::string& json, ecuio_status_t* status = nullptr)
{
    ecuio_condition_set_t* s = reinterpret_cast<ecuio_condition_set_t*>(1);
    const ecuio_status_t st =
        ecuio_condition_set_load(json.data(), json.size(), &s);
    if (status != nullptr) {
        *status = st;
    } else {
        EXPECT_EQ(st, ECUIO_OK) << ecuio_last_error_detail() << "\n" << json;
    }
    if (st != ECUIO_OK) {
        EXPECT_EQ(s, nullptr) << "out is NULL on a failure";
    }
    return Set(s);
}

/* The detail a refused file leaves */
inline std::string refusal(const std::string& json)
{
    ecuio_status_t s = ECUIO_OK;
    Set set = load(json, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT) << json;
    return ecuio_last_error_detail();
}

/* ADR-0063 section 5's example */
const char* const kExample = R"json({
  "havne_conditions": 1,
  "name": "Daily driver",
  "checks": { "gap": 1, "rate_drop": 10, "settle": 2 },
  "conditions": [
    {
      "key": "coolant_high",
      "name": "Coolant high",
      "when": { "signal": ["obd2.coolant"], "unit": "°C",
                "above": 110, "clear": 105, "max_age": 10 },
      "for": 2,
      "alert": { "level": "danger", "sound": true, "banner": true },
      "mark": true
    },
    {
      "key": "pull",
      "name": "Full throttle",
      "when": { "all": [
        { "signal": ["obd2.throttle"], "unit": "%", "above": 80, "clear": 70 },
        { "signal": ["id:1001", "id:7003"], "unit": "RPM", "above": 2500 }
      ] },
      "for": 0.5,
      "record": { "before": 5, "after": 5 },
      "mark": true
    },
    {
      "key": "link_fault",
      "name": "Link fault",
      "when": { "link": "not_ok" },
      "record": { "before": 10, "after": 10 }
    },
    {
      "key": "module_slow",
      "name": "A module slowed down",
      "when": { "check": "rate_drop" },
      "alert": { "level": "warning", "banner": true },
      "mark": true
    }
  ]
})json";

/* A file around its conditions' JSON */
inline std::string file_of(const std::string& conditions)
{
    return R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
}

/* A condition "c" around the JSON of its when, and of what follows it */
inline std::string when(const std::string& w, const std::string& rest = "")
{
    return file_of(R"({"key":"c","name":"C","when":)" + w + rest + "}");
}

/* A signal term on obd2's coolant, above 100 deg C, with more keys */
inline std::string coolant(const std::string& more = "")
{
    return std::string(R"({"signal":["obd2.coolant"],"unit":")") +
           "\xC2\xB0"
           "C" +
           R"(","above":100)" + more + "}";
}

inline ecuio_condition_info_t info_of(const Set& s, uint32_t i)
{
    ecuio_condition_info_t info{};
    EXPECT_EQ(ecuio_condition_set_get(s.get(), i, &info), ECUIO_OK);
    return info;
}

inline ecuio_condition_term_t term_of(const Set& s, uint32_t condition, uint32_t term)
{
    ecuio_condition_term_t t{};
    EXPECT_EQ(ecuio_condition_set_get_term(s.get(), condition, term, &t),
              ECUIO_OK);
    return t;
}

inline const ecuio_signal_set_t* profile(const char* name)
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup(name, &set), ECUIO_OK) << name;
    return set;
}

/* A signal file extending obd2: a number in psi, an enumeration, a text
 * and a computed signal */
const char* const kSignals = R"json({
  "havne_signals": 2,
  "name": "Condition signals",
  "source": { "name": "Written for Havne's tests" },
  "extends": "obd2",
  "ecus": [ { "id": "pcm", "rx": "7E8" } ],
  "signals": [
    { "key": "oil_pressure", "name": "Oil Pressure",
      "read": { "service": "22", "number": "D100" }, "field": { "byte": 0 },
      "unit": "psi" },
    { "key": "gear", "name": "Gear", "kind": "enum",
      "read": { "service": "22", "number": "D101" }, "field": { "byte": 0 },
      "values": { "0": "Park", "1": "First" } },
    { "key": "part", "name": "Part", "kind": "text",
      "read": { "service": "22", "number": "F187" }, "field": { "byte": 0 } },
    { "key": "ratio", "name": "Ratio", "kind": "computed",
      "expr": "oil_pressure / 2" },
    { "key": "count", "name": "Count",
      "read": { "service": "22", "number": "D102" }, "field": { "byte": 0 } }
  ]
})json";

inline File signals(const std::string& json = kSignals)
{
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(json.data(), json.size(), &f), ECUIO_OK)
        << ecuio_last_error_detail();
    return File(f);
}

} // namespace
