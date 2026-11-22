// File/layer: tests/test_signal_file_support.h | C++17, Qt-free, every platform
// Testing: UNIT: the helpers of test_signal_file*.cpp (test_signals)

#pragma once

#include <gtest/gtest.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include "computed.h"
#include "request_engine.h" /* core/src: obd2's request count */

#include <cctype>
#include <clocale>
#include <cstring>
#include <memory>
#include <string>

namespace {

struct FileFree {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};
using File = std::unique_ptr<ecuio_signal_file_t, FileFree>;

inline File load(const std::string& json, ecuio_status_t* status = nullptr)
{
    ecuio_signal_file_t* f = reinterpret_cast<ecuio_signal_file_t*>(1);
    const ecuio_status_t s = ecuio_signal_file_load(json.data(), json.size(), &f);
    if (status != nullptr) {
        *status = s;
    }
    if (s != ECUIO_OK) {
        EXPECT_EQ(f, nullptr) << "out is NULL on a failure";
    }
    return File(f);
}

/* The detail a refused file leaves */
inline std::string refusal(const std::string& json)
{
    ecuio_status_t s = ECUIO_OK;
    File f = load(json, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT) << json;
    return ecuio_last_error_detail();
}

/* Whether text holds words */
inline bool has(const std::string& text, const char* words)
{
    return text.find(words) != std::string::npos;
}

/* ADR-0051 section 2's example */
const char* const kExample = R"json({
  "havne_signals": 1,
  "name": "Daily driver extras",
  "notes": "TFT checked against the gauge",
  "source": { "name": "Written by hand for the daily driver" },
  "extends": "obd2",
  "ecus": [
    { "id": "pcm", "rx": "7E8" },
    { "id": "tcm", "rx": "7E9" }
  ],
  "signals": [
    {
      "key": "tft",
      "name": "Trans Fluid Temp",
      "ecu": "tcm",
      "read": { "service": "22", "number": "1E1C" },
      "field": { "byte": 0, "bits": 16, "order": "be", "signed": false },
      "scale": 1, "offset": -40,
      "unit": "°C", "decimals": 0, "group": "engine"
    },
    {
      "key": "fuel_status",
      "name": "Fuel System 1",
      "ecu": "pcm",
      "read": { "service": "01", "number": "03" },
      "field": { "byte": 0, "bits": 8 },
      "kind": "enum",
      "values": { "1": "Open loop, cold", "2": "Closed loop", "4": "Open loop, load" },
      "group": "fuel"
    },
    {
      "key": "cal_id",
      "name": "TCM Calibration",
      "ecu": "tcm",
      "read": { "service": "22", "number": "F188" },
      "field": { "byte": 0 },
      "kind": "text",
      "group": "diagnostics"
    },
    {
      "key": "boost",
      "name": "Boost",
      "kind": "computed",
      "expr": "obd2.map - obd2.baro",
      "max_age": { "obd2.baro": 30 },
      "unit": "kPa", "decimals": 0, "group": "air", "plot": 1
    }
  ]
})json";

/* A minimal file around one signal's JSON, on one ECU */
inline std::string one(const std::string& signal, const std::string& ecus =
                                               R"([{"id":"pcm","rx":"7E8"}])")
{
    return std::string(R"({"havne_signals":1,"name":"T","source":{"name":"t"},)") +
           R"("ecus":)" + ecus + R"(,"signals":[)" + signal + "]}";
}

const char* const kNumber =
    R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0}})";

} // namespace
