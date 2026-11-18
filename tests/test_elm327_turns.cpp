// File/layer: tests/test_elm327_turns.cpp | GTest, HARNESS tier, headless
//             CI job (every platform)
// Testing: HARNESS: Elm327TurnsTest (test_elm327)
//
// Which ECU an ELM327 session asks next (request_turns.h), against the
// fake ELM327 (fake_elm327.h): the adapter asks one channel at a time, so
// a channel keeps the turn until it has asked each of its rows, and a
// walking channel until its walk ends. Judged on the fake's command
// record (the first 96 commands), not on counts in a wall-clock window.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "fake_elm327.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

#ifdef _WIN32
constexpr char kPort[] = "COM7";
#else
constexpr char kPort[] = "/dev/fake-elm327";
#endif

/* What the adapter was sent, in order (the fake's first records) */
std::vector<std::string> typed()
{
    std::vector<std::string> out;
    fake_elm327::with([&](fake_elm327::adapter& a) {
        for (uint32_t i = 0; i < a.commands && i < a.kRecordDepth; ++i) {
            out.emplace_back(a.recorded[i]);
        }
    });
    return out;
}

template <typename Done> bool wait_for(Done done, int ms)
{
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

/* The fake's knob and module table for a scope */
struct Fake {
    explicit Fake(uint32_t delay_ms, const char* modules, uint32_t absent = 0)
    {
        fake_elm327::with([delay_ms](fake_elm327::adapter& a) {
            a.k.reply_delay_ms = delay_ms;
        });
        EXPECT_TRUE(fake_ecu::set_module_dids(modules));
        fake_ecu::absent_ecu() = absent;
    }
    ~Fake()
    {
        fake_ecu::set_module_dids(nullptr);
        fake_ecu::absent_ecu() = 0;
    }
    Fake(const Fake&) = delete;
    Fake& operator=(const Fake&) = delete;
    fake_elm327::installed installed;
};

struct Session {
    ecuio_session_t* s = nullptr;
    ~Session() { ecuio_session_destroy(s); }
    ecuio_status_t create(const std::string& query)
    {
        const std::string uri = std::string("elm327://") + kPort + query;
        return ecuio_session_create(uri.c_str(), &s);
    }
};

/* A def of one byte read from a reply keyed key (two or three bytes) on
 * rx, its value after the key */
ecuio_signal_def_t def_of(uint16_t id, uint32_t rx, uint32_t key, uint8_t len)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = len;
    d.mux_key = key;
    d.kind = ECUIO_VALUE_UINT;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = len;
    d.bit_length = 8;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "turns %u", unsigned{id});
    return d;
}

} // namespace

/* Six DIDs on the engine ECU and one on a module: between two of the
 * module's asks the engine ECU's six are each asked once, under two
 * header switches a lap, not one switch before and after each of the
 * module's asks */
TEST(Elm327TurnsTest, ABusyChannelGetsATurnForEachOfItsRequests)
{
    Fake fake(25, "7E8:D801=01;7E8:D802=02;7E8:D803=03;7E8:D804=04;"
                  "7E8:D805=05;7E8:D806=06;7E9:D901=09");
    Session s;
    ASSERT_EQ(s.create("?profile=uds-generic&did=7E8:D801,7E8:D802,7E8:D803,"
                       "7E8:D804,7E8:D805,7E8:D806,7E9:D901"
                       "&identify=0&diagnose=0"),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [] { return typed().size() >= fake_elm327::adapter::kRecordDepth; },
        10000));
    /* each 7E9 ask's place among the requests, with the header switches
     * before each request */
    std::vector<std::string> asks;
    std::vector<int> switches;
    int since = 0;
    for (const std::string& c : typed()) {
        if (c.rfind("ATSH", 0) == 0) {
            ++since;
        } else if (c.rfind("22D", 0) == 0) {
            asks.push_back(c.substr(0, 6));
            switches.push_back(since);
            since = 0;
        }
    }
    std::vector<size_t> module;
    for (size_t i = 0; i < asks.size(); ++i) {
        if (asks[i] == "22D901") {
            module.push_back(i);
        }
    }
    ASSERT_GE(module.size(), 4u) << "too few laps in the record";
    for (size_t m = 0; m + 1 < module.size(); ++m) {
        std::set<std::string> engine;
        int header = 0;
        for (size_t i = module[m] + 1; i < module[m + 1]; ++i) {
            engine.insert(asks[i]);
            header += switches[i];
        }
        header += switches[module[m + 1]];
        EXPECT_EQ(module[m + 1] - module[m], 7u) << "lap " << m;
        EXPECT_EQ(engine.size(), 6u) << "lap " << m;
        EXPECT_EQ(header, 2) << "lap " << m;
    }
}

/* The test car's pick, with more modules: the engine ECU's Service 01
 * PIDs after five modules (four answering a DID, one absent), each with
 * identification, each request 40 ms. The engine ECU's walk reaches the
 * fake's last bitmap (0xA0), the walk keeping the turn and its second its
 * own, and its PIDs then pack. Its six bitmaps take about 240 ms of its
 * second; taken channel by channel, each bitmap waits five other
 * exchanges, so no more than five fit in the second. */
TEST(Elm327TurnsTest, TheWalkEndsWithSixChannelsOneSilent)
{
    Fake fake(40, "7E9:D801=01;7EB:D803=03;7EC:D804=04;7ED:D805=05", 0x7EA);
    static const ecuio_channel_decl_t kChannels[] = {
        {0x7E9u, 0x7E1u, 0, 0}, {0x7EAu, 0x7E2u, 0, 0}, {0x7EBu, 0x7E3u, 0, 0},
        {0x7ECu, 0x7E4u, 0, 0}, {0x7EDu, 0x7E5u, 0, 0}, {0x7E8u, 0x7E0u, 0, 0}};
    static ecuio_signal_def_t defs[9];
    defs[0] = def_of(1, 0x7E9u, 0x62D801u, 3);
    defs[1] = def_of(2, 0x7EAu, 0x62D802u, 3);
    defs[2] = def_of(3, 0x7EBu, 0x62D803u, 3);
    defs[3] = def_of(4, 0x7ECu, 0x62D804u, 3);
    defs[4] = def_of(5, 0x7EDu, 0x62D805u, 3);
    defs[5] = def_of(6, 0x7E8u, 0x410Du, 2);
    defs[6] = def_of(7, 0x7E8u, 0x4133u, 2);
    defs[7] = def_of(8, 0x7E8u, 0x4146u, 2);
    defs[8] = def_of(9, 0x7E8u, 0x415Cu, 2);
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 9;
    set.defs = defs;
    set.channel_count = 6;
    set.channels = kChannels;
    ASSERT_EQ(ecuio_profile_register("elm327-turns", &set, nullptr), ECUIO_OK)
        << ecuio_last_error_detail();
    Session s;
    ASSERT_EQ(s.create("?profile=elm327-turns&diagnose=0"), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    ecuio_channel_support_t sup{};
    const bool packed = wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.s, 5, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_PACKED) != 0;
        },
        15000);
    EXPECT_EQ(sup.rx_id, 0x7E8u);
    EXPECT_EQ(sup.flags & ECUIO_SUPPORT_COMPLETE, ECUIO_SUPPORT_COMPLETE)
        << "the walk stopped short";
    EXPECT_EQ(sup.ranges, 0x3Fu) << "the bitmaps at 0x00 to 0xA0";
    EXPECT_TRUE(packed);
    ecuio_session_destroy(s.s);
    s.s = nullptr;
    EXPECT_EQ(ecuio_profile_unregister("elm327-turns"), ECUIO_OK);
}
