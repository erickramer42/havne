// File/layer: tests/sim_gateway_support.h | C++17, Qt-free, every platform
// Testing: UNIT: SimGatewayTest's helpers (test_obd2)
//
// What the simulated gateway's cases share: the URI read alone, an
// unbound sim asked at the case's own time, and a profile of modules on
// IDs the case names.

#ifndef ECUIO_TESTS_SIM_GATEWAY_SUPPORT_H
#define ECUIO_TESTS_SIM_GATEWAY_SUPPORT_H

#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "sim_ecu.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sim_gateway_test {

using Bytes = std::vector<uint8_t>;

inline ecuio_sim_config parsed(const std::string& uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri(uri.c_str(), &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << uri << ": " << why;
    return c;
}

/* The URI's refusal, its words; "" when it was taken */
inline std::string refusal(const std::string& uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    if (ecuio_sim_parse_uri(uri.c_str(), &c, why, sizeof why) ==
        ecuio_sim_uri_result::ok) {
        return "";
    }
    return why;
}

/* The unbound create's refusal: its status, and its words in *why */
inline ecuio_status_t create_status(const std::string& uri, std::string* why)
{
    ecuio_sim_t* sim = nullptr;
    const ecuio_status_t st = ecuio_sim_create_unbound(uri.c_str(), 0, &sim);
    *why = ecuio_last_error_detail();
    ecuio_sim_destroy(sim);
    return st;
}

struct Reply {
    uint32_t id;
    uint32_t flags;
    int64_t delay_us;
    Bytes data;
};

/* An unbound sim asked at the case's time, destroyed at the end */
struct Sim {
    ecuio_sim_t* sim = nullptr;
    int64_t now = 0;

    explicit Sim(const std::string& uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri.c_str(), 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Sim() { ecuio_sim_destroy(sim); }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    /* Each reply to q on id at now */
    std::vector<Reply> ask(uint32_t id, const Bytes& q)
    {
        std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        EXPECT_EQ(ecuio_sim_ask(
                      sim, id, q.data(), static_cast<uint32_t>(q.size()), now,
                      out.data(), static_cast<uint32_t>(out.size()), &n),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        std::vector<Reply> r;
        for (uint32_t i = 0; i < n && i < out.size(); ++i) {
            r.push_back({out[i].reply_id, out[i].flags, out[i].delay_us,
                         Bytes(out[i].data, out[i].data + out[i].length)});
        }
        return r;
    }

    /* The one reply's bytes, or {} with none (a second is a failure) */
    Bytes one(uint32_t id, const Bytes& q)
    {
        const std::vector<Reply> r = ask(id, q);
        EXPECT_LE(r.size(), 1u);
        return r.empty() ? Bytes{} : r[0].data;
    }

    ecuio_sim_stats_t stats() const
    {
        ecuio_sim_stats_t s{};
        EXPECT_EQ(ecuio_sim_get_stats(sim, &s), ECUIO_OK);
        return s;
    }
};

/* A def keyed 62 <did> at offset 0 on rx, one byte at byte 3 */
inline ecuio_signal_def_t did_def(uint16_t id, uint32_t rx, uint16_t did)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.kind = ECUIO_VALUE_UINT;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = 3;
    d.bit_length = 8;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "Module %X", rx);
    return d;
}

/* A profile of modules on the (reply, request) pairs given, each with
 * DID 0x1000 + its place, registered under name until the scope ends */
struct Profile {
    std::string name;
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    ecuio_signal_set_t set{};

    Profile(const char* n,
            const std::vector<std::pair<uint32_t, uint32_t>>& ids)
        : name(n)
    {
        for (size_t i = 0; i < ids.size(); ++i) {
            ecuio_channel_decl_t c{};
            c.rx_id = ids[i].first;
            c.tx_id = ids[i].second;
            channels.push_back(c);
            defs.push_back(did_def(static_cast<uint16_t>(100 + i), c.rx_id,
                                   static_cast<uint16_t>(0x1000 + i)));
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = static_cast<uint32_t>(defs.size());
        set.defs = defs.data();
        set.channel_count = static_cast<uint32_t>(channels.size());
        set.channels = channels.data();
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(name.c_str(), &set, &why), ECUIO_OK)
            << why.code << " at " << why.index;
    }
    ~Profile() { EXPECT_EQ(ecuio_profile_unregister(name.c_str()), ECUIO_OK); }
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
};

/* n modules, reply 0x740 + i answering request 0x700 + i */
inline std::vector<std::pair<uint32_t, uint32_t>> modules(uint32_t n)
{
    std::vector<std::pair<uint32_t, uint32_t>> ids;
    for (uint32_t i = 0; i < n; ++i) {
        ids.emplace_back(0x740u + i, 0x700u + i);
    }
    return ids;
}

} // namespace sim_gateway_test

#endif /* ECUIO_TESTS_SIM_GATEWAY_SUPPORT_H */
