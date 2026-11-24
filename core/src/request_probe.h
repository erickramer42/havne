// File/layer: core/src/request_probe.h | internal, not ABI-exported
// The start's emissions probe (ADR-0065 step 5's RANGE): at attach, before
// the module table is fixed, one functional 01 00, its replies heard for
// the one-shot's wait. A module that answers 41 00 is open for RANGE,
// whatever the picks; a refusal or silence leaves it to the picks. On
// 11-bit (7DF) the eight replies 7E8-7EF are heard, and an answerer no set
// declares joins the table. On 29-bit (18DB33F1) only the declared
// 18DAF1xx modules are heard, in groups no larger than the live budget,
// one request a group. No answer at all learns nothing: the engine
// module (PID 0C's channel), else the first, stands in alone. Each
// transport does its own wire and writes the probe's line in its session
// log; the rules are here.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: ProbeRangeTest (test_obd2); the transports' own cases

#ifndef ECUIO_REQUEST_PROBE_H
#define ECUIO_REQUEST_PROBE_H

#include "ecuio/signals.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

constexpr uint32_t ECUIO_PROBE_TX_11 = 0x7DF;
constexpr uint32_t ECUIO_PROBE_TX_29 = 0x18DB33F1u;
constexpr uint32_t ECUIO_PROBE_FIRST_11 = 0x7E8; /* J1979's emissions replies */
constexpr uint32_t ECUIO_PROBE_LAST_11 = 0x7EF;
constexpr uint8_t ECUIO_PROBE_REQUEST[2] = {0x01, 0x00};

/* One probe: the modules that answered 41 00 (reply IDs, in the order
 * heard), the requests sent, and how long it took */
struct ecuio_probe {
    uint32_t answered[ECUIO_MAX_CHANNELS] = {};
    uint32_t answered_count = 0;
    uint32_t requests = 0;
    int64_t took_us = 0;
};

/* Whether an attach probes (internal.h's ecuio_set_start_probe, a test's
 * switch); off, a session is attached as one the probe heard no answer on */
inline std::atomic<bool>& ecuio_start_probe_on()
{
    static std::atomic<bool> on{true};
    return on;
}

/* Whether rx is a 29-bit physical reply to the tester (18DAF1xx) */
inline bool ecuio_probe_candidate_29(uint32_t rx)
{
    return (rx & 0xFFFFFF00u) == 0x18DAF100u;
}

/* The reply IDs group g listens for, into out; how many (0: no group g).
 * 11-bit: one group, 7E8-7EF. 29-bit: the table's 18DAF1xx modules in
 * table order, at most budget a group (budget 0: no bound). */
inline uint32_t ecuio_probe_group(const uint32_t* rx_ids, uint32_t count,
                                  bool ext29, uint32_t budget, uint32_t g,
                                  uint32_t* out)
{
    if (!ext29) {
        if (g != 0) {
            return 0;
        }
        for (uint32_t k = 0; k <= ECUIO_PROBE_LAST_11 - ECUIO_PROBE_FIRST_11;
             ++k) {
            out[k] = ECUIO_PROBE_FIRST_11 + k;
        }
        return ECUIO_PROBE_LAST_11 - ECUIO_PROBE_FIRST_11 + 1;
    }
    const uint32_t size = budget == 0 ? ECUIO_MAX_CHANNELS : budget;
    uint32_t seen = 0;
    uint32_t n = 0;
    for (uint32_t c = 0; c < count; ++c) {
        if (!ecuio_probe_candidate_29(rx_ids[c])) {
            continue;
        }
        if (seen / size == g && n < size) {
            out[n++] = rx_ids[c];
        }
        ++seen;
    }
    return n;
}

/* A reply heard while probing, from rx, its PDU service byte first: a
 * positive answer to 01 00 counts once; a refusal is no answer */
inline void ecuio_probe_take(ecuio_probe& p, uint32_t rx, const uint8_t* pdu,
                             size_t len)
{
    if (len < 2 || pdu[0] != 0x41 || pdu[1] != 0x00) {
        return;
    }
    for (uint32_t k = 0; k < p.answered_count; ++k) {
        if (p.answered[k] == rx) {
            return;
        }
    }
    if (p.answered_count < ECUIO_MAX_CHANNELS) {
        p.answered[p.answered_count++] = rx;
    }
}

/* Whether rx answered the probe */
inline bool ecuio_probe_answered(const ecuio_probe& p, uint32_t rx)
{
    for (uint32_t k = 0; k < p.answered_count; ++k) {
        if (p.answered[k] == rx) {
            return true;
        }
    }
    return false;
}

/* Each 11-bit answerer the table doesn't declare joins it, its requests
 * to rx - 8, while the table has room; how many joined */
inline uint32_t ecuio_probe_join(const ecuio_probe& p, uint32_t* rx_ids,
                                 uint32_t* tx_ids, uint32_t* count)
{
    uint32_t joined = 0;
    for (uint32_t k = 0; k < p.answered_count; ++k) {
        const uint32_t rx = p.answered[k];
        if (rx < ECUIO_PROBE_FIRST_11 || rx > ECUIO_PROBE_LAST_11) {
            continue;
        }
        bool declared = false;
        for (uint32_t c = 0; c < *count; ++c) {
            declared = declared || rx_ids[c] == rx;
        }
        if (declared || *count >= ECUIO_MAX_CHANNELS) {
            continue;
        }
        rx_ids[*count] = rx;
        tx_ids[*count] = rx - 8;
        ++*count;
        ++joined;
    }
    return joined;
}

/* The table's RANGE: a bit for each module that answered. With no answer
 * at all (the ignition off at the start, the bus silent) nothing is
 * learned, and one module stands in alone, so the session still reads and
 * judges its link: the engine module (the channel asking PID 0C, engine),
 * else the first; the rest open by the picks. */
inline uint32_t ecuio_probe_range(const ecuio_probe& p, const uint32_t* rx_ids,
                                  uint32_t count, uint32_t engine)
{
    if (p.answered_count == 0) {
        const uint32_t in = engine < count ? engine : 0u;
        return count > 0 && in < ECUIO_MAX_CHANNELS ? 1u << in : 0u;
    }
    uint32_t range = 0;
    for (uint32_t c = 0; c < count && c < ECUIO_MAX_CHANNELS; ++c) {
        if (ecuio_probe_answered(p, rx_ids[c])) {
            range |= 1u << c;
        }
    }
    return range;
}

/* The session log's line: the requests, the time, the answerers */
inline void ecuio_probe_describe(const ecuio_probe& p, char* out, size_t cap)
{
    int n = std::snprintf(out, cap,
                          "emissions probe: %u functional 01 00 in %lld ms, ",
                          static_cast<unsigned>(p.requests),
                          static_cast<long long>(p.took_us / 1000));
    if (p.answered_count == 0) {
        (void)std::snprintf(out + n, cap - static_cast<size_t>(n), "no answer");
        return;
    }
    n += std::snprintf(out + n, cap - static_cast<size_t>(n), "answered by");
    for (uint32_t k = 0;
         k < p.answered_count && n > 0 && static_cast<size_t>(n) < cap; ++k) {
        n += std::snprintf(out + n, cap - static_cast<size_t>(n), " %X",
                           static_cast<unsigned>(p.answered[k]));
    }
}

#endif
