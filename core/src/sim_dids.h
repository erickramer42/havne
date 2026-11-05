// File/layer: core/src/sim_dids.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimDidsTest (test_obd2): each field kind and byte order
//          through ecuio_decode_signal and ecuio_decode_text, the edges of
//          a field's range and the values past them, two signals in one
//          DID's reply, a DID the set lacks, another module's DID, another
//          service, a functional request's refusals; HARNESS: SimBusTest
//          (test_vcan, Linux): a requesting socketcan:// session on the
//          same profile reads the values the simulated ECU was given, on
//          the engine ECU and on a module of the set's own
//
// The simulated ECU's Service 22 answers from a signal set (ADR-0046's
// appendix "DIDs from a signal set"): each def keyed 62 <DID> at offset 0
// is a field of that DID's reply on the module whose reply ID is its
// message_id, written from a physical value by inverting the def's
// decode: raw = (value - offset) / scale, rounded, in the def's width,
// signedness, byte order, start byte and bit offset. A text def's field
// holds its name. Every def answered from one DID's request shares one
// reply, as an ECU's record of several fields does. Pure: the caller
// keeps the values and the bus.

#ifndef ECUIO_SIM_DIDS_H
#define ECUIO_SIM_DIDS_H

#include "ecuio/signals.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/* Whether d is a field of a Service 22 reply the simulated ECU answers:
 * keyed 62 <DID> at offset 0, its field past the key */
inline bool ecuio_sim_did_def(const ecuio_signal_def_t& d)
{
    return d.mux_offset == 0 && d.mux_length == 3 &&
           (d.mux_key >> 16) == 0x62u && d.start_byte >= 3;
}

/* A def's name's length, within its buffer */
inline uint32_t ecuio_sim_name_len(const ecuio_signal_def_t& d)
{
    uint32_t n = 0;
    while (n < sizeof d.name && d.name[n] != '\0') {
        ++n;
    }
    return n;
}

inline bool ecuio_sim_text_def(const ecuio_signal_def_t& d)
{
    return d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX;
}

/* Where d's field ends in the reply: the byte after its last */
inline uint32_t ecuio_sim_field_end(const ecuio_signal_def_t& d)
{
    if (ecuio_sim_text_def(d)) {
        const uint32_t n =
            d.byte_length != 0 ? d.byte_length : ecuio_sim_name_len(d);
        return d.start_byte + n;
    }
    return d.start_byte + (d.bit_offset + d.bit_length + 7u) / 8u;
}

/* The raw field that decodes to value through d: the nearest raw of the
 * def's width and signedness, or for a FLOAT the binary32 pattern.
 * False for a text def, a value that isn't finite, a zero scale off its
 * offset, or a raw outside the field */
inline bool ecuio_sim_raw_for(const ecuio_signal_def_t& d, double value,
                              uint32_t* raw)
{
    if (ecuio_sim_text_def(d) || !std::isfinite(value) || d.bit_length == 0 ||
        d.bit_length > 32) {
        return false;
    }
    if (d.scale == 0.0) {
        if (value != d.offset) {
            return false;
        }
        *raw = 0;
        return true;
    }
    const double x = (value - d.offset) / d.scale;
    if (!std::isfinite(x)) {
        return false;
    }
    if (d.kind == ECUIO_VALUE_FLOAT) {
        if (std::fabs(x) > 3.4028234663852886e38) {
            return false;
        }
        const float f = static_cast<float>(x);
        std::memcpy(raw, &f, sizeof f);
        return true;
    }
    const double r = std::nearbyint(x);
    const unsigned bits = d.bit_length;
    if (d.kind == ECUIO_VALUE_INT) {
        const double lo = -std::ldexp(1.0, static_cast<int>(bits) - 1);
        const double hi = std::ldexp(1.0, static_cast<int>(bits) - 1) - 1.0;
        if (r < lo || r > hi) {
            return false;
        }
        const int64_t s = static_cast<int64_t>(r);
        const uint64_t mask = bits == 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1);
        *raw = static_cast<uint32_t>(static_cast<uint64_t>(s) & mask);
        return true;
    }
    /* An enum is answered by its raw number (ADR-0051) */
    if (d.kind != ECUIO_VALUE_UINT && d.kind != ECUIO_VALUE_ENUM) {
        return false; /* a kind with no inverse here: unanswered */
    }
    const double hi = std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
    if (r < 0.0 || r > hi) {
        return false;
    }
    *raw = static_cast<uint32_t>(r);
    return true;
}

/* Writes raw into d's field of reply (the whole reply, 62 at 0), which
 * holds at least ecuio_sim_field_end(d) bytes */
inline void ecuio_sim_put_field(const ecuio_signal_def_t& d, uint32_t raw,
                                uint8_t* reply)
{
    if (d.byte_order == ECUIO_BYTE_ORDER_BE) {
        const uint32_t n = d.bit_length / 8u;
        for (uint32_t i = 0; i < n; ++i) {
            reply[d.start_byte + i] =
                static_cast<uint8_t>(raw >> (8u * (n - 1u - i)));
        }
        return;
    }
    for (uint32_t k = 0; k < d.bit_length; ++k) {
        const uint32_t at = d.bit_offset + k;
        uint8_t& b = reply[d.start_byte + at / 8u];
        const uint8_t bit = static_cast<uint8_t>(1u << (at % 8u));
        if ((raw >> k) & 1u) {
            b = static_cast<uint8_t>(b | bit);
        } else {
            b = static_cast<uint8_t>(b & ~bit);
        }
    }
}

/* A text def's field: its name, padded with spaces to its byte_length */
inline void ecuio_sim_put_text(const ecuio_signal_def_t& d, uint8_t* reply)
{
    const uint32_t end = ecuio_sim_field_end(d);
    const uint32_t name = ecuio_sim_name_len(d);
    for (uint32_t i = d.start_byte; i < end; ++i) {
        const uint32_t k = i - d.start_byte;
        reply[i] = k < name ? static_cast<uint8_t>(d.name[k]) : ' ';
    }
}

/* The reply to req (len bytes, the service byte first) from a module
 * whose reply ID is rx, answering set's DIDs, into out (cap bytes); its
 * length, 0 for none. value(i) is def i's physical value. 22 <DID>: 62
 * <DID> and every field the set's defs on rx put there, 7F 22 31 for a
 * DID none of them reads; 7F 22 31 for any other 22 request (a DID list
 * is never asked); 7F <service> 11 for another service. A functional
 * request's refusals are unsaid. A field past cap is left out */
template <class Value>
uint32_t ecuio_sim_did_answer(const ecuio_signal_set_t& set, uint32_t rx,
                              const uint8_t* req, uint32_t len, bool functional,
                              Value value, uint8_t* out, uint32_t cap)
{
    if (len == 0 || cap < 3) {
        return 0;
    }
    uint32_t n = 0;
    if (req[0] == 0x22 && len == 3) {
        const uint32_t key =
            0x620000u | (static_cast<uint32_t>(req[1]) << 8) | req[2];
        for (uint32_t i = 0; i < set.count; ++i) {
            const ecuio_signal_def_t& d = set.defs[i];
            if (d.message_id != rx || !ecuio_sim_did_def(d) ||
                d.mux_key != key) {
                continue;
            }
            const uint32_t end = ecuio_sim_field_end(d);
            if (end > cap) {
                continue;
            }
            if (n == 0) {
                out[0] = 0x62;
                out[1] = req[1];
                out[2] = req[2];
                n = 3;
            }
            if (end > n) {
                std::memset(out + n, 0, end - n);
                n = end;
            }
            if (ecuio_sim_text_def(d)) {
                ecuio_sim_put_text(d, out);
                continue;
            }
            uint32_t raw = 0;
            if (!ecuio_sim_raw_for(d, value(i), &raw)) {
                raw = 0;
            }
            ecuio_sim_put_field(d, raw, out);
        }
    }
    if (n == 0) {
        out[0] = 0x7F;
        out[1] = req[0];
        out[2] = req[0] == 0x22 ? 0x31 : 0x11;
        n = 3;
    }
    if (functional && out[0] == 0x7F) {
        return 0;
    }
    return n;
}

/* The modules a set puts on the bus: the engine ECU's pair (0x7E0 to
 * 0x7E8) answers the set's DIDs on it when a channel names that pair or
 * the set declares none (a session's default channel); every other
 * channel is a module of its own */
struct ecuio_sim_module_plan {
    bool engine = false;
    uint32_t count = 0;
    uint32_t tx[ECUIO_MAX_CHANNELS] = {};
    uint32_t rx[ECUIO_MAX_CHANNELS] = {};
};

/* set's modules into *plan; false with why (a line) for a set the
 * simulated ECU can't stand in for: a 29-bit ID (it hears 11-bit
 * frames only), a channel on the functional ID, or one sharing an ID
 * with the engine ECU's pair but not the pair itself */
inline bool ecuio_sim_plan_modules(const ecuio_signal_set_t& set,
                                   ecuio_sim_module_plan* plan, char* why,
                                   size_t why_cap)
{
    *plan = ecuio_sim_module_plan{};
    if (set.channel_count == 0 || set.channels == nullptr) {
        plan->engine = true;
        return true;
    }
    for (uint32_t i = 0; i < set.channel_count; ++i) {
        const ecuio_channel_decl_t& c = set.channels[i];
        if (c.tx_id > 0x7FFu || c.rx_id > 0x7FFu) {
            std::snprintf(why, why_cap,
                          "the simulated ECU answers 11-bit modules only; "
                          "channel %u is 0x%X to 0x%X",
                          i, c.tx_id, c.rx_id);
            return false;
        }
        if (c.tx_id == 0x7DFu || c.rx_id == 0x7DFu) {
            std::snprintf(why, why_cap,
                          "channel %u uses the functional request ID, 0x7DF",
                          i);
            return false;
        }
        const bool tx_engine = c.tx_id == 0x7E0u, rx_engine = c.rx_id == 0x7E8u;
        if (tx_engine && rx_engine) {
            plan->engine = true;
            continue;
        }
        if (tx_engine || rx_engine || c.tx_id == 0x7E8u || c.rx_id == 0x7E0u) {
            std::snprintf(why, why_cap,
                          "channel %u (0x%X to 0x%X) shares an ID with the "
                          "engine ECU's 0x7E0 to 0x7E8",
                          i, c.tx_id, c.rx_id);
            return false;
        }
        plan->tx[plan->count] = c.tx_id;
        plan->rx[plan->count] = c.rx_id;
        ++plan->count;
    }
    return true;
}

#endif /* ECUIO_SIM_DIDS_H */
