// File/layer: tests/car_sized_set.h | test helper, Qt-free, C++17
// Testing: none of its own: PickOnlyTest and Elm327SessionTest build
//          their sessions on it
//
// A set that derives twice the requests a rotation holds (ADR-0065
// section 3): two modules, each asked its most.

#ifndef ECUIO_TESTS_CAR_SIZED_SET_H
#define ECUIO_TESTS_CAR_SIZED_SET_H

#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include <cstdint>
#include <cstdio>

namespace car_sized {

constexpr uint32_t kMost = ECUIO_MODULE_MAX_REQUESTS;
constexpr uint32_t kDefs = 2 * kMost + 1;
constexpr uint16_t kFirstId = 1000;
constexpr uint16_t kFirstDid = 0x1000;
constexpr uint32_t kRxA = 0x7E8;
constexpr uint32_t kRxB = 0x7E9;

/* Def i's signal id; defs 0 to kMost - 1 are module A's, the next kMost
 * module B's */
constexpr uint16_t id_of(uint32_t def)
{
    return static_cast<uint16_t>(kFirstId + def);
}

/* The DID def i reads: kFirstDid on in each module, and the last def
 * reads def 0's reply */
constexpr uint16_t did_of(uint32_t def)
{
    return static_cast<uint16_t>(kFirstDid +
                                 (def == 2 * kMost ? 0 : def % kMost));
}

/* kDefs one-byte numbers, each a DID of its own on its module but the
 * last, which reads the byte after def 0's: 2 * kMost requests */
inline const ecuio_signal_set_t* set()
{
    static ecuio_signal_def_t defs[kDefs];
    static ecuio_channel_decl_t channels[2] = {{kRxA, 0x7E0, 0, 0, {}},
                                               {kRxB, 0x7E1, 0, 0, {}}};
    static ecuio_signal_set_t made = {};
    if (made.count != 0) {
        return &made;
    }
    for (uint32_t i = 0; i < kDefs; ++i) {
        ecuio_signal_def_t& d = defs[i];
        d = ecuio_signal_def_t{};
        d.id = id_of(i);
        const uint32_t of = i == 2 * kMost ? 0 : i;
        d.message_id = channels[of < kMost ? 0 : 1].rx_id;
        d.mux_length = 3;
        d.mux_key = 0x620000u | did_of(i);
        d.kind = ECUIO_VALUE_UINT;
        d.byte_order = ECUIO_BYTE_ORDER_BE;
        d.start_byte = static_cast<uint16_t>(i == 2 * kMost ? 4 : 3);
        d.bit_length = 8;
        d.scale = 1.0;
        std::snprintf(d.name, sizeof d.name, "Signal %u",
                      static_cast<unsigned>(i));
    }
    made.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    made.count = kDefs;
    made.defs = defs;
    made.channel_count = 2;
    made.channels = channels;
    return &made;
}

} // namespace car_sized

#endif // ECUIO_TESTS_CAR_SIZED_SET_H
