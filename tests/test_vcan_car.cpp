// File/layer: tests/test_vcan_car.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SimCarBusTest (test_vcan)
//
// The simulated car on the bus (ADR-0065 section 10): as many modules as
// a set holds answering a functional request each on its own reply ID,
// two segmented replies under way at once, a fault on one module alone,
// one session reading the made-up whole car (plugins/example-car), and
// a module's reply timing (delay=, fc=, cf=).

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef ECUIO_PLUGINS_DIR
#error                                                                         \
    "tests/CMakeLists.txt defines ECUIO_PLUGINS_DIR, the repository's plugins/"
#endif

namespace {

using namespace std::chrono_literals;
using vcan_bus::Fd;

class Sim
{
public:
    explicit Sim(const std::string& uri)
    {
        status_ =
            ecuio_sim_create(uri.c_str(), vcan_bus::interface_name(), &sim_);
        detail_ = ecuio_last_error_detail();
    }
    ~Sim() { ecuio_sim_destroy(sim_); }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_sim_t* get() const { return sim_; }
    ecuio_sim_stats_t stats() const
    {
        ecuio_sim_stats_t s{};
        EXPECT_EQ(ecuio_sim_get_stats(sim_, &s), ECUIO_OK);
        return s;
    }

private:
    ecuio_sim_t* sim_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

/* A started socketcan:// session, closed and destroyed when it goes */
class Session
{
public:
    explicit Session(const std::string& query)
    {
        const std::string uri =
            std::string("socketcan://") + vcan_bus::interface_name() + query;
        status_ = ecuio_session_create(uri.c_str(), &s_);
        detail_ = ecuio_last_error_detail();
    }
    ~Session()
    {
        if (s_ != nullptr) {
            ecuio_session_close(s_);
            ecuio_session_destroy(s_);
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_session_t* get() const { return s_; }

private:
    ecuio_session_t* s_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

/* What a session decoded, by signal ID */
struct Values {
    std::mutex mutex;
    std::map<uint16_t, double> number;
    std::map<uint16_t, std::string> text;
    static void on_values(const ecuio_value_batch_t* b, void* user)
    {
        auto* self = static_cast<Values*>(user);
        std::lock_guard<std::mutex> lock(self->mutex);
        for (uint32_t i = 0; i < b->count; ++i) {
            const ecuio_value_t& v = b->values[i];
            if (v.text != nullptr) {
                self->text[v.signal_id] = std::string(v.text, v.text_len);
            } else {
                self->number[v.signal_id] = v.value;
            }
        }
    }
    bool has_all(const std::vector<uint16_t>& ids)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (uint16_t id : ids) {
            if (number.count(id) == 0 && text.count(id) == 0) {
                return false;
            }
        }
        return true;
    }
};

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

bool put(int fd, uint32_t id, std::initializer_list<uint8_t> data)
{
    can_frame f{};
    f.can_id = id;
    f.len = 8;
    std::memset(f.data, vcan_bus::kPad, sizeof f.data);
    std::copy_n(data.begin(), std::min<size_t>(data.size(), sizeof f.data),
                f.data);
    return ::write(fd, &f, sizeof f) == static_cast<ssize_t>(sizeof f);
}

/* A def keyed 62 <did> on rx: one byte at byte 3, or a text from byte 3 */
ecuio_signal_def_t did_def(uint16_t id, uint32_t rx, uint16_t did, bool text,
                           const char* name)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.kind = text ? ECUIO_VALUE_TEXT : ECUIO_VALUE_UINT;
    d.byte_order = text ? ECUIO_BYTE_ORDER_LE : ECUIO_BYTE_ORDER_BE;
    d.start_byte = 3;
    d.bit_length = text ? 0 : 8;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "%s", name);
    return d;
}

/* n modules (request 0x700 + i, reply 0x740 + i), each with DID 2000, a
 * byte, and DID 4A00, a text longer than a frame; registered under name
 * for the scope */
struct Modules {
    std::string name;
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    ecuio_signal_set_t set{};

    Modules(const char* n, uint32_t count) : name(n)
    {
        for (uint32_t i = 0; i < count; ++i) {
            ecuio_channel_decl_t c{};
            c.rx_id = 0x740u + i;
            c.tx_id = 0x700u + i;
            channels.push_back(c);
            char label[32];
            std::snprintf(label, sizeof label, "Module %02u Byte", i);
            defs.push_back(did_def(static_cast<uint16_t>(100 + i), c.rx_id,
                                   0x2000, false, label));
            std::snprintf(label, sizeof label, "Module %02u Name Text", i);
            defs.push_back(did_def(static_cast<uint16_t>(200 + i), c.rx_id,
                                   0x4A00, true, label));
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
    ~Modules() { EXPECT_EQ(ecuio_profile_unregister(name.c_str()), ECUIO_OK); }
    Modules(const Modules&) = delete;
    Modules& operator=(const Modules&) = delete;
};

/* The Single Frame positive replies heard within ms, by reply ID */
std::set<uint32_t> single_replies(int fd, int ms)
{
    std::set<uint32_t> ids;
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              end - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            return ids;
        }
        can_frame f{};
        if (vcan_bus::read_for(fd, &f, sizeof f, static_cast<int>(left)) <= 0) {
            continue;
        }
        if ((f.data[0] & 0xF0) == 0x00 && f.data[1] == 0x62) {
            ids.insert(f.can_id & CAN_SFF_MASK);
        }
    }
}

class SimCarBusTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string why;
        if (!vcan_bus::available(&why)) {
            const char* required = std::getenv("ECUIO_VCAN_REQUIRED");
            if (required != nullptr && std::string(required) == "1") {
                FAIL() << "ECUIO_VCAN_REQUIRED=1, but " << why;
            }
            GTEST_SKIP() << why;
        }
    }
};

} // namespace

// A set's 32 modules on the bus, each its own node: one functional
// request gets 32 replies, each on its own reply ID
TEST_F(SimCarBusTest, ThirtyTwoModulesEachAnswerAFunctionalRequest)
{
    Modules m("sim-car-bus-32", ECUIO_MAX_CHANNELS);
    Sim sim("mock://sim?profile=sim-car-bus-32");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd raw = vcan_bus::open_raw();
    ASSERT_TRUE(raw.ok());
    ASSERT_TRUE(put(raw.get(), 0x7DF, {0x03, 0x22, 0x20, 0x00}));
    const std::set<uint32_t> ids = single_replies(raw.get(), 1000);
    EXPECT_EQ(ids.size(), size_t{ECUIO_MAX_CHANNELS});
    if (!ids.empty()) {
        EXPECT_EQ(*ids.begin(), 0x740u);
        EXPECT_EQ(*ids.rbegin(), 0x740u + ECUIO_MAX_CHANNELS - 1);
    }
    EXPECT_EQ(sim.stats().replies, uint64_t{ECUIO_MAX_CHANNELS});
}

// Two modules' segmented replies under way at once: both First Frames
// come before any Flow Control, each module's Flow Control goes to its
// own request ID, and each message arrives whole
TEST_F(SimCarBusTest, TwoModulesSegmentedRepliesAreUnderWayAtOnce)
{
    Modules m("sim-car-bus-two", 2);
    Sim sim("mock://sim?profile=sim-car-bus-two");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd raw = vcan_bus::open_raw();
    ASSERT_TRUE(raw.ok());
    ASSERT_TRUE(put(raw.get(), 0x7DF, {0x03, 0x22, 0x4A, 0x00}));
    std::map<uint32_t, std::vector<uint8_t>> got;
    std::map<uint32_t, size_t> want;
    bool flowed = false;
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < end) {
        can_frame f{};
        if (vcan_bus::read_for(raw.get(), &f, sizeof f, 50) <= 0) {
            continue;
        }
        const uint32_t id = f.can_id & CAN_SFF_MASK;
        if (id != 0x740 && id != 0x741) {
            continue;
        }
        const uint8_t pci = f.data[0] & 0xF0;
        if (pci == 0x10) {
            EXPECT_FALSE(flowed) << "a First Frame after the Flow Control";
            want[id] =
                static_cast<size_t>(((f.data[0] & 0x0F) << 8) | f.data[1]);
            got[id].assign(f.data + 2, f.data + 8);
        } else if (pci == 0x20 && got.count(id) != 0) {
            got[id].insert(got[id].end(), f.data + 1, f.data + 8);
        }
        if (!flowed && want.size() == 2) {
            /* Only now, so both are under way at once */
            ASSERT_TRUE(put(raw.get(), 0x700, {0x30, 0x00, 0x00}));
            ASSERT_TRUE(put(raw.get(), 0x701, {0x30, 0x00, 0x00}));
            flowed = true;
        }
        if (flowed && got[0x740].size() >= want[0x740] &&
            got[0x741].size() >= want[0x741]) {
            break;
        }
    }
    ASSERT_EQ(want.size(), 2u) << "both modules' First Frames";
    for (uint32_t id : {0x740u, 0x741u}) {
        SCOPED_TRACE(id);
        ASSERT_GE(got[id].size(), want[id]);
        const std::string text(got[id].begin() + 3,
                               got[id].begin() + static_cast<long>(want[id]));
        EXPECT_EQ(got[id][0], 0x62);
        char name[32];
        std::snprintf(name, sizeof name, "Module %02u Name Text", id - 0x740);
        EXPECT_EQ(text.substr(0, std::strlen(name)), name);
    }
    EXPECT_EQ(sim.stats().unflowed, 0u);
}

// silent:1@741 on the bus: the module answering on 0x741 says nothing,
// asked directly or functionally, and every other module answers
TEST_F(SimCarBusTest, AFaultAimedAtOneModuleSilencesItAlone)
{
    Modules m("sim-car-bus-aimed", 3);
    Sim sim("mock://sim?profile=sim-car-bus-aimed&faults=silent:1@741");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd raw = vcan_bus::open_raw();
    ASSERT_TRUE(raw.ok());
    ASSERT_TRUE(put(raw.get(), 0x7DF, {0x03, 0x22, 0x20, 0x00}));
    EXPECT_EQ(single_replies(raw.get(), 500),
              (std::set<uint32_t>{0x740, 0x742}));
    ASSERT_TRUE(put(raw.get(), 0x701, {0x03, 0x22, 0x20, 0x00}));
    EXPECT_TRUE(single_replies(raw.get(), 300).empty());
    EXPECT_EQ(sim.stats().silent, 2u);
}

namespace {

/* plugins/example-car's files, read from disk for the loader */
struct CarPackage {
    std::map<std::string, std::string> files;

    static std::string slurp(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }
    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<CarPackage*>(user);
        auto it = self->files.find(path);
        if (it == self->files.end()) {
            const std::string root =
                std::string(ECUIO_PLUGINS_DIR) + "/example-car/";
            it = self->files.emplace(path, slurp(root + path)).first;
        }
        *bytes = it->second.data();
        *length = it->second.size();
        return it->second.empty() ? 1 : 0;
    }
};

} // namespace

// One session reads the made-up whole car: every module of its 18
// answers its own value and its name, the engine ECU among them
TEST_F(SimCarBusTest, ASessionReadsTheMadeUpCarWhole)
{
    CarPackage pkg;
    const std::string manifest = CarPackage::slurp(
        std::string(ECUIO_PLUGINS_DIR) + "/example-car/manifest.json");
    ecuio_plugin_t* plugin = nullptr;
    ASSERT_EQ(ecuio_plugin_load(manifest.data(), manifest.size(),
                                &CarPackage::read, &pkg, &plugin),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_plugin_register(plugin, 0, nullptr, 0), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_plugin_variant_set(plugin, 0);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->channel_count, 18u);
    {
        Sim sim("mock://sim?profile=plugin.example-car.whole");
        ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
        /* Each module's first number, given a value of its own; each
         * module's name, which the sim answers as its def's name */
        std::vector<uint16_t> ids;
        std::map<uint16_t, double> given;
        std::map<uint16_t, std::string> names;
        std::set<uint32_t> numbered;
        for (uint32_t d = 0; d < set->count; ++d) {
            const ecuio_signal_def_t& def = set->defs[d];
            if (def.mux_length != 3 || (def.mux_key >> 16) != 0x62) {
                continue; /* obd2's PIDs and the computed */
            }
            if (def.kind == ECUIO_VALUE_TEXT) {
                ids.push_back(def.id);
                names[def.id] = def.name;
            } else if (def.kind != ECUIO_VALUE_ENUM &&
                       numbered.insert(def.message_id).second) {
                const double v = def.offset + 7 * def.scale;
                ASSERT_EQ(ecuio_sim_set_value(sim.get(), def.id, v), ECUIO_OK)
                    << def.name;
                ids.push_back(def.id);
                given[def.id] = v;
            }
        }
        EXPECT_EQ(names.size(), 18u);
        EXPECT_EQ(numbered.size(), 15u) << "the modules with a number";
        Values values;
        Session s("?profile=plugin.example-car.whole");
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_EQ(ecuio_register_values_callback(s.get(), &Values::on_values,
                                                 &values),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(s.get()), ECUIO_OK);
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
        EXPECT_EQ(info.module_count, 18u);
        EXPECT_TRUE(wait_for([&] { return values.has_all(ids); }, 15000ms))
            << "every module's value and name";
        std::lock_guard<std::mutex> lock(values.mutex);
        for (const auto& kv : given) {
            EXPECT_NEAR(values.number[kv.first], kv.second, 1e-9) << kv.first;
        }
        for (const auto& kv : names) {
            EXPECT_EQ(values.text[kv.first].substr(0, kv.second.size()),
                      kv.second)
                << kv.first;
        }
    }
    EXPECT_EQ(ecuio_profile_unregister("plugin.example-car.whole"), ECUIO_OK);
    EXPECT_EQ(ecuio_plugin_free(plugin), ECUIO_OK);
}

namespace {

double ms_since(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - from)
        .count();
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

/* One segmented read of DID 4A00 by the case as the tester: the request
 * to its First Frame, its Flow Control (STmin stmin) to the first
 * Consecutive Frame, and the gaps between the rest, in ms */
struct Paced {
    double first = -1;
    double after_flow = -1;
    std::vector<double> gaps;
};

Paced paced_read(int fd, uint32_t tx, uint32_t rx, uint8_t stmin)
{
    Paced out;
    const auto asked = std::chrono::steady_clock::now();
    if (!put(fd, tx, {0x03, 0x22, 0x4A, 0x00})) {
        return out;
    }
    std::chrono::steady_clock::time_point flowed;
    std::chrono::steady_clock::time_point last;
    size_t left = 0;
    while (ms_since(asked) < 2000) {
        can_frame f{};
        if (vcan_bus::read_for(fd, &f, sizeof f, 100) <= 0 ||
            (f.can_id & CAN_SFF_MASK) != rx) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if ((f.data[0] & 0xF0) == 0x10) {
            out.first = ms_since(asked);
            left = ((f.data[0] & 0x0Fu) << 8 | f.data[1]) - 6u;
            put(fd, tx, {0x30, 0x00, stmin});
            flowed = std::chrono::steady_clock::now();
        } else if ((f.data[0] & 0xF0) == 0x20 && left > 0) {
            if (out.after_flow < 0) {
                out.after_flow = ms_since(flowed);
            } else {
                out.gaps.push_back(
                    std::chrono::duration<double, std::milli>(now - last)
                        .count());
            }
            last = now;
            left = left > 7 ? left - 7 : 0;
            if (left == 0) {
                return out;
            }
        }
    }
    return out;
}

} // namespace

// delay=, fc= and cf= on the bus: a module's First Frame waits its
// delay, its first Consecutive Frame fc after the tester's Flow Control,
// and the rest cf apart, or the tester's STmin when that is longer; an
// aimed cf= is that module's alone. Floors judged by medians of the
// case's own read times, the ceilings loose (a loaded machine).
TEST_F(SimCarBusTest, AModulesRepliesKeepItsTiming)
{
    Modules m("sim-car-bus-timing", 2);
    Sim sim("mock://sim?profile=sim-car-bus-timing&delay=40000:40000"
            "&fc=20000&cf=15000,30000@741");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd raw = vcan_bus::open_raw();
    ASSERT_TRUE(raw.ok());
    const struct {
        uint32_t tx, rx;
        uint8_t stmin;
        double gap; /* the gap's floor: cf or the tester's STmin */
    } cases[] = {
        {0x700, 0x740, 0x00, 15.0}, /* cf over STmin 0 */
        {0x700, 0x740, 0x32, 50.0}, /* the tester's 50 ms over cf */
        {0x701, 0x741, 0x05, 30.0}, /* the aimed cf over STmin 5 */
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.rx);
        SCOPED_TRACE(c.stmin);
        std::vector<double> first, after_flow, gaps;
        for (int round = 0; round < 5; ++round) {
            const Paced p = paced_read(raw.get(), c.tx, c.rx, c.stmin);
            ASSERT_GE(p.first, 0) << "no First Frame, round " << round;
            ASSERT_GE(p.after_flow, 0) << "no Consecutive Frame";
            ASSERT_EQ(p.gaps.size(), 2u) << "19 bytes of name: three CFs";
            first.push_back(p.first);
            after_flow.push_back(p.after_flow);
            gaps.insert(gaps.end(), p.gaps.begin(), p.gaps.end());
        }
        EXPECT_GE(median(first), 40.0);
        EXPECT_LT(median(first), 160.0);
        EXPECT_GE(median(after_flow), 20.0);
        EXPECT_LT(median(after_flow), 80.0);
        EXPECT_GE(median(gaps), c.gap);
        EXPECT_LT(median(gaps), 4 * c.gap);
    }
    EXPECT_EQ(sim.stats().unflowed, 0u);
}
