// File/layer: tests/bench_bridge_decode.h | Qt6 Core, benchmark, C++17
// Testing: none of its own; part of bench_bridge (tests/bench_bridge.cpp)
// --decode: core's decode with computed signals, a chain and conditions.

#pragma once

#include <QFile>
#include <QString>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "ecuio/conditions.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "bench_bridge_common.h"
#include "bench_bridge_conditions.h"

namespace {

// --decode: core's decode a frame with computed signals (ADR-0062): the
// fixture's frames dispatched (ecuio_session_dispatch_callbacks) through
// a session never started, under obd2 alone, obd2 with eight computed
// signals each reading its PIDs (format 1), and obd2 with a chain of
// eight, each reading the one before (format 2, ECUIO_COMPUTED_MAX_DEPTH
// deep). Each round times every set once, in turn; the table gives each
// set's median, fastest and slowest. A core that refuses a set's file
// says so in its row.
//
// Then conditions (ADR-0063 section 3): obd2 with a typical set attached
// (8 conditions, 12 terms) and the largest (64 conditions of 4 terms,
// every term on a signal the fixture carries), each judged after the
// decode; and the two rows with no values consumer, where a frame is
// decoded only because a set is attached. Last, core's own replay worker
// looping at full speed with a values consumer: its frames a second with
// no set, the typical one and the largest, alternated.

const char* const kFlatFile = R"({"havne_signals": 1, "name": "Bench flat",
 "source": {"name": "bench_bridge"}, "extends": "obd2", "signals": [
 {"key": "f1", "name": "F1", "kind": "computed", "expr": "obd2.rpm / 100"},
 {"key": "f2", "name": "F2", "kind": "computed", "expr": "obd2.rpm / 100 + obd2.load"},
 {"key": "f3", "name": "F3", "kind": "computed", "expr": "obd2.maf * 2 - obd2.load"},
 {"key": "f4", "name": "F4", "kind": "computed", "expr": "obd2.speed + obd2.throttle"},
 {"key": "f5", "name": "F5", "kind": "computed", "expr": "obd2.coolant - obd2.iat"},
 {"key": "f6", "name": "F6", "kind": "computed", "expr": "obd2.module_voltage * obd2.fuel_level"},
 {"key": "f7", "name": "F7", "kind": "computed", "expr": "obd2.rpm / 100 + obd2.speed"},
 {"key": "f8", "name": "F8", "kind": "computed", "expr": "obd2.load * 2 + 1"}]})";

const char* const kChainFile = R"({"havne_signals": 2, "name": "Bench chain",
 "source": {"name": "bench_bridge"}, "extends": "obd2", "signals": [
 {"key": "c8", "name": "C8", "kind": "computed", "expr": "c7 + obd2.fuel_level"},
 {"key": "c7", "name": "C7", "kind": "computed", "expr": "c6 * obd2.module_voltage"},
 {"key": "c6", "name": "C6", "kind": "computed", "expr": "c5 + obd2.iat"},
 {"key": "c5", "name": "C5", "kind": "computed", "expr": "c4 - obd2.coolant"},
 {"key": "c4", "name": "C4", "kind": "computed", "expr": "c3 + obd2.speed"},
 {"key": "c3", "name": "C3", "kind": "computed", "expr": "c2 * 2 - obd2.maf"},
 {"key": "c2", "name": "C2", "kind": "computed", "expr": "c1 + obd2.load"},
 {"key": "c1", "name": "C1", "kind": "computed", "expr": "obd2.rpm / 100"}]})";

constexpr int kDecodeRounds = 9;
constexpr int kDecodePasses = 40; /* the fixture's frames, each pass later */

// The fixture's frames, read from its frames.csv
std::vector<ecuio_frame_t> fixtureFrames()
{
    std::vector<ecuio_frame_t> out;
    QFile f(QStringLiteral(ECUIO_REPLAY_FIXTURES) +
            QStringLiteral("/2026-10-25-engine-start/frames.csv"));
    if (!f.open(QIODevice::ReadOnly))
        return out;
    f.readLine(); /* the header */
    while (!f.atEnd()) {
        const QList<QByteArray> c = f.readLine().trimmed().split(',');
        if (c.size() != 5)
            continue;
        ecuio_frame_t fr{};
        fr.timestamp_us = c[1].toULongLong();
        fr.arbitration_id = c[2].toUInt(nullptr, 16);
        fr.flags = c[3].toUInt(nullptr, 16);
        for (const QByteArray& b : c[4].split(' ')) {
            if (!b.isEmpty() && fr.data_length < sizeof fr.data)
                fr.data[fr.data_length++] =
                    static_cast<uint8_t>(b.toUInt(nullptr, 16));
        }
        out.push_back(fr);
    }
    return out;
}

void countValues(const ecuio_value_batch_t* b, void* user)
{
    *static_cast<uint64_t*>(user) += b->count;
}

struct DecodeSet {
    const char* label = "";
    const char* profile = nullptr;
    const char* file = nullptr; /* nullptr: the built-in profile alone */
    std::string refused;
    ecuio_signal_file_t* loaded = nullptr;
    std::vector<double> ns;
    double values = 0.0;
    std::string conditions; /* a condition file's JSON; "" for none */
    bool consumer = true;   /* a values callback is registered */
};

// ns a frame for one pass of frames through a session under profile, and
// the values a frame (the condition events a frame for a set with no
// values consumer)
double decodeNs(const DecodeSet& set, std::vector<ecuio_frame_t>& frames,
                double* values)
{
    const char* profile = set.profile;
    const std::string uri = std::string("mock://bench?profile=") + profile;
    ecuio_session_t* session = nullptr;
    if (ecuio_session_create(uri.c_str(), &session) != ECUIO_OK) {
        std::fprintf(stderr, "create %s failed: %s\n", uri.c_str(),
                     ecuio_last_error_detail());
        std::exit(1);
    }
    uint64_t count = 0;
    if (set.consumer) {
        ecuio_register_values_callback(session, countValues, &count);
    } else {
        ecuio_register_conditions_callback(session, countEvents, &count);
    }
    if (!set.conditions.empty()) {
        attachConditions(session, set.conditions);
    }
    const uint64_t span = frames.back().timestamp_us + 1000000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int pass = 0; pass < kDecodePasses; ++pass) {
        for (ecuio_frame_t& f : frames) {
            ecuio_session_dispatch_callbacks(session, &f);
            f.timestamp_us += span;
        }
    }
    const double ns = std::chrono::duration<double, std::nano>(
                          std::chrono::steady_clock::now() - t0)
                          .count() /
                      (static_cast<double>(frames.size()) * kDecodePasses);
    for (ecuio_frame_t& f : frames)
        f.timestamp_us -= span * kDecodePasses;
    *values = static_cast<double>(count) /
              (static_cast<double>(frames.size()) * kDecodePasses);
    ecuio_session_destroy(session);
    return ns;
}

bool decodeTest()
{
    std::vector<ecuio_frame_t> frames = fixtureFrames();
    if (frames.empty()) {
        std::fprintf(stderr, "the fixture's frames.csv didn't read\n");
        return false;
    }
    DecodeSet sets[7];
    sets[0].label = "obd2 alone";
    sets[1].label = "8 computed, format 1";
    sets[1].profile = "user.bench-flat";
    sets[1].file = kFlatFile;
    sets[2].label = "a chain 8 deep";
    sets[2].profile = "user.bench-chain";
    sets[2].file = kChainFile;
    sets[3].label = "obd2, a typical condition set";
    sets[3].conditions = typicalConditions();
    sets[4].label = "obd2, the largest condition set";
    sets[4].conditions = largestConditions();
    sets[5].label = "obd2, no values consumer, no set";
    sets[5].consumer = false;
    sets[6].label = "obd2, no values consumer, a typical set";
    sets[6].conditions = typicalConditions();
    sets[6].consumer = false;
    const int kSets = 7;
    for (DecodeSet& s : sets) {
        if (s.profile == nullptr) {
            s.profile = "obd2";
        }
    }
    for (DecodeSet& s : sets) {
        if (s.file == nullptr)
            continue;
        if (ecuio_signal_file_load(s.file, std::strlen(s.file), &s.loaded) !=
            ECUIO_OK) {
            s.refused = ecuio_last_error_detail();
            continue;
        }
        if (ecuio_profile_register(s.profile, ecuio_signal_file_set(s.loaded),
                                   nullptr) != ECUIO_OK) {
            s.refused = "the profile didn't register";
        }
    }
    for (int r = 0; r < kDecodeRounds; ++r) {
        for (int k = 0; k < kSets; ++k) {
            DecodeSet& s = sets[(r + k) % kSets];
            if (s.refused.empty())
                s.ns.push_back(decodeNs(s, frames, &s.values));
        }
    }
    std::printf("Core's decode a frame: the fixture's %zu frames x %d passes, "
                "median of %d rounds\n\n",
                frames.size(), kDecodePasses, kDecodeRounds);
    std::printf("| Set | ns a frame | fastest to slowest | values a frame |\n");
    std::printf("|---|---|---|---|\n");
    for (DecodeSet& s : sets) {
        if (!s.refused.empty()) {
            std::printf("| %s | refused | %s | - |\n", s.label,
                        s.refused.c_str());
            continue;
        }
        std::sort(s.ns.begin(), s.ns.end());
        std::printf("| %s | %.1f | %.1f to %.1f | %.2f |\n", s.label,
                    s.ns[s.ns.size() / 2], s.ns.front(), s.ns.back(), s.values);
    }
    for (DecodeSet& s : sets) {
        if (s.loaded != nullptr) {
            ecuio_profile_unregister(s.profile);
            ecuio_signal_file_free(s.loaded);
        }
    }

    /* Core's own worker, saturated: the fixture replayed in a loop at
     * full speed under obd2 with a values consumer, frames a second with
     * no set, the typical one and the largest, alternated */
    const std::string none;
    const std::string typical = typicalConditions();
    const std::string largest = largestConditions();
    const std::string* which[] = {&none, &typical, &largest};
    const char* const names[] = {"no set", "a typical set", "the largest set"};
    std::vector<double> rates[3];
    const QByteArray replay =
        (QStringLiteral("replay://") + QStringLiteral(ECUIO_REPLAY_FIXTURES) +
         QStringLiteral(
             "/2026-10-25-engine-start?profile=obd2&speed=max&loop=1"))
            .toUtf8();
    for (int r = 0; r < 5; ++r) {
        for (int k = 0; k < 3; ++k) {
            const int at = (r + k) % 3;
            ecuio_session_t* session = nullptr;
            if (ecuio_session_create(replay.constData(), &session) !=
                ECUIO_OK) {
                std::fprintf(stderr, "the replay didn't open: %s\n",
                             ecuio_last_error_detail());
                return false;
            }
            std::atomic<uint64_t> seen{0};
            uint64_t values = 0;
            ecuio_register_frame_callback(
                session,
                [](const ecuio_frame_t*, void* user) {
                    static_cast<std::atomic<uint64_t>*>(user)->fetch_add(
                        1, std::memory_order_relaxed);
                },
                &seen);
            ecuio_register_values_callback(session, countValues, &values);
            if (!which[at]->empty()) {
                attachConditions(session, *which[at]);
            }
            ecuio_session_start(session);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const uint64_t from = seen.load();
            const auto t0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const uint64_t to = seen.load();
            const double s = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
            ecuio_session_close(session);
            ecuio_session_destroy(session);
            rates[at].push_back(static_cast<double>(to - from) / s);
        }
    }
    std::printf("\nCore's replay worker looping at full speed under obd2, "
                "with a values consumer: median of 5 runs of 1.5 s\n\n");
    std::printf("| Conditions | frames a second | lowest to highest | of no "
                "set |\n|---|---|---|---|\n");
    double base = 0.0;
    for (int k = 0; k < 3; ++k) {
        std::sort(rates[k].begin(), rates[k].end());
        const double median = rates[k][rates[k].size() / 2];
        if (k == 0) {
            base = median;
        }
        std::printf("| %s | %.0f | %.0f to %.0f | %.3f |\n", names[k], median,
                    rates[k].front(), rates[k].back(), median / base);
    }
    return true;
}

} // namespace
