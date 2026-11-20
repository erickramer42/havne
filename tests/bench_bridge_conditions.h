// File/layer: tests/bench_bridge_conditions.h | Qt6 Core, benchmark, C++17
// Testing: none of its own; part of bench_bridge (tests/bench_bridge.cpp)
// The two condition sets the load and decode runs judge, and their attach.

#pragma once

#include <cstdint>
#include <string>
#include "ecuio/conditions.h"
#include "ecuio/internal.h"

namespace {

// A typical set: 8 conditions, 12 terms on obd2's signals, with clears
// and holds
std::string typicalConditions()
{
    return R"({"havne_conditions":1,"conditions":[
 {"key":"c1","name":"C1","when":{"signal":["obd2.rpm"],"unit":"RPM","above":3000,"clear":2800}},
 {"key":"c2","name":"C2","when":{"signal":["obd2.coolant"],"unit":"°C","above":105,"clear":100},"for":2},
 {"key":"c3","name":"C3","when":{"all":[
   {"signal":["obd2.throttle"],"unit":"%","above":80,"clear":70},
   {"signal":["obd2.rpm"],"unit":"RPM","above":2500}]},"for":0.5},
 {"key":"c4","name":"C4","when":{"any":[
   {"signal":["obd2.load"],"unit":"%","above":90},
   {"signal":["obd2.maf"],"unit":"g/s","above":100}]}},
 {"key":"c5","name":"C5","when":{"signal":["obd2.speed"],"unit":"km/h","above":130}},
 {"key":"c6","name":"C6","when":{"all":[
   {"signal":["obd2.iat"],"unit":"°C","above":60},
   {"signal":["obd2.coolant"],"unit":"°C","above":100}]}},
 {"key":"c7","name":"C7","when":{"signal":["obd2.module_voltage"],"unit":"V","below":12},"for":1},
 {"key":"c8","name":"C8","when":{"all":[
   {"signal":["obd2.speed"],"unit":"km/h","above":100},
   {"signal":["obd2.throttle"],"unit":"%","above":50}]},"clear_for":1}]})";
}

// The largest set: 64 conditions of 4 terms, on engine speed, throttle and
// load, which the fixture carries, with levels the run crosses
std::string largestConditions()
{
    std::string all = R"({"havne_conditions":1,"conditions":[)";
    for (int i = 0; i < 64; ++i) {
        const std::string n = std::to_string(i);
        all +=
            std::string(i == 0 ? "" : ",") + R"({"key":"c)" + n +
            R"(","name":"C)" + n + R"(","when":{"all":[)" +
            R"({"signal":["obd2.rpm"],"unit":"RPM","above":)" +
            std::to_string(600 + 10 * i) + "}," +
            R"({"signal":["obd2.throttle"],"unit":"%","above":)" +
            std::to_string(i) + "}," +
            R"({"signal":["obd2.rpm"],"unit":"RPM","below":9000,"clear":9100},)" +
            R"({"signal":["obd2.load"],"unit":"%","differs":255}]},"for":0.5})";
    }
    return all + "]}";
}

// Attaches a condition file's JSON to the session
void attachConditions(ecuio_session_t* session, const std::string& json)
{
    ecuio_condition_set_t* set = nullptr;
    if (ecuio_condition_set_load(json.data(), json.size(), &set) != ECUIO_OK ||
        ecuio_session_set_conditions(session, set, nullptr) != ECUIO_OK) {
        std::fprintf(stderr, "the bench's conditions didn't attach: %s\n",
                     ecuio_last_error_detail());
        std::exit(1);
    }
    ecuio_condition_set_free(set);
}

void countEvents(const ecuio_condition_event_t*, uint32_t count, void* user)
{
    *static_cast<uint64_t*>(user) += count;
}

} // namespace
