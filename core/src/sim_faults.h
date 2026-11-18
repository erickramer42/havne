// File/layer: core/src/sim_faults.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: SimEcuTest (test_obd2); fuzz_sim
//
// The simulated ECU's faults (ADR-0046, ADR-0041 section 4): which
// replies the URI's faults= breaks, how each is broken, and the
// grammar that reads them.

#ifndef ECUIO_SIM_FAULTS_H
#define ECUIO_SIM_FAULTS_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/* Every Nth reply of its kind, 0 = never (ADR-0041 section 4) */
struct ecuio_sim_faults {
    uint32_t silent = 0;
    uint32_t late = 0;
    uint32_t bad_sequence = 0; /* segmented replies only */
    uint32_t stalled = 0;      /* segmented replies only */
    uint32_t pending = 0;      /* 7F <service> 78 first, the reply later */
};

constexpr uint32_t ECUIO_SIM_FAULT_MAX = 10000;

/* A reply's fate */
enum class ecuio_sim_fate : uint8_t {
    intact,
    silent,
    late,
    bad_sequence,
    stalled,
    pending
};

/* Past the request engine's wait (100 ms), before the next request's */
constexpr int64_t ECUIO_SIM_LATE_US = 130000;
/* Past N_Cr (150 ms) after the frame before */
constexpr int64_t ECUIO_SIM_STALLED_US = 160000;
/* "Response pending": the reply this long after its 7F <service> 78,
 * past the request engine's 100 ms wait and an ELM327's AT ST timeout
 * (ATST19, 100 ms), well inside P2* (5 s), so only a tester that keeps
 * waiting after the 78 gets it */
constexpr int64_t ECUIO_SIM_PENDING_US = 300000;

/* faults=<name>:<n>[,...] into *out; false with why (a line) */
inline bool ecuio_sim_parse_faults(const char* text, size_t len,
                                   ecuio_sim_faults* out, char* why,
                                   size_t why_cap)
{
    *out = ecuio_sim_faults{};
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        const char* item = text + at;
        const size_t item_len = end - at;
        size_t colon = 0;
        while (colon < item_len && item[colon] != ':') {
            ++colon;
        }
        uint32_t* slot = nullptr;
        const auto named = [&](const char* name) {
            return colon == std::strlen(name) &&
                   std::strncmp(item, name, colon) == 0;
        };
        if (named("silent")) {
            slot = &out->silent;
        } else if (named("late")) {
            slot = &out->late;
        } else if (named("bad_sequence")) {
            slot = &out->bad_sequence;
        } else if (named("stalled")) {
            slot = &out->stalled;
        } else if (named("pending")) {
            slot = &out->pending;
        }
        uint32_t n = 0;
        bool digits = colon + 1 < item_len;
        for (size_t i = colon + 1; digits && i < item_len; ++i) {
            const char c = item[i];
            if (c < '0' || c > '9' || n > ECUIO_SIM_FAULT_MAX) {
                digits = false;
            } else {
                n = n * 10 + static_cast<uint32_t>(c - '0');
            }
        }
        if (slot == nullptr || colon == item_len || !digits || n == 0 ||
            n > ECUIO_SIM_FAULT_MAX) {
            std::snprintf(why, why_cap,
                          "faults: '%.*s' isn't <fault>:<n>, the fault one of "
                          "silent, late, pending, bad_sequence or stalled and "
                          "n from 1 to %u",
                          static_cast<int>(item_len < 40 ? item_len : 40), item,
                          ECUIO_SIM_FAULT_MAX);
            return false;
        }
        *slot = n;
        at = end + 1;
    }
    if (len == 0 || text[len - 1] == ',') {
        std::snprintf(why, why_cap, "faults: names none");
        return false;
    }
    return true;
}

#endif /* ECUIO_SIM_FAULTS_H */
