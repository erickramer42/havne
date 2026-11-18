// File/layer: tests/test_plugin_car.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest (test_signals)
//
// The made-up whole car, plugins/example-car (ADR-0065 section 10): 18
// modules on made-up addresses, read whole with no car. Its package as it
// is on disk, its set, its gauges and its written copy, and the simulated
// ECU answering every module of it at once.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/sim.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef ECUIO_PLUGINS_DIR
#error                                                                         \
    "tests/CMakeLists.txt defines ECUIO_PLUGINS_DIR, the repository's plugins/"
#endif

namespace {

constexpr uint32_t kModules = 18;
const char* const kIds[kModules] = {
    "pcm",  "tcm",  "abs", "eps", "bcm",  "ipc",  "hvac", "srs",   "gw",
    "tpms", "sccm", "ddm", "pdm", "seat", "park", "cam",  "radar", "audio"};

/* The package's files as they are on disk, LF or CRLF */
struct Car {
    std::map<std::string, std::string> files;

    static std::string slurp(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        EXPECT_TRUE(in.good()) << path;
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    Car()
    {
        const std::string root =
            std::string(ECUIO_PLUGINS_DIR) + "/example-car/";
        files["manifest.json"] = slurp(root + "manifest.json");
        files["fixtures/values.json"] = slurp(root + "fixtures/values.json");
        for (const char* id : kIds) {
            const std::string p = std::string("data/") + id + ".json";
            files[p] = slurp(root + p);
        }
    }

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<Car*>(user);
        auto it = self->files.find(path);
        if (it == self->files.end()) {
            return 1;
        }
        *bytes = it->second.data();
        *length = it->second.size();
        return 0;
    }
};

struct PluginFree {
    void operator()(ecuio_plugin_t* p) const
    {
        EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);
    }
};
using Plugin = std::unique_ptr<ecuio_plugin_t, PluginFree>;

Plugin load(Car& car)
{
    ecuio_plugin_t* p = nullptr;
    const std::string& m = car.files["manifest.json"];
    EXPECT_EQ(ecuio_plugin_load(m.data(), m.size(), &Car::read, &car, &p),
              ECUIO_OK)
        << ecuio_last_error_detail();
    return Plugin(p);
}

/* The module a key of the car's own names, by its "<id>_" start; -1 for
 * the base set's */
int module_of(const std::string& key)
{
    for (uint32_t i = 0; i < kModules; ++i) {
        const std::string prefix = std::string(kIds[i]) + "_";
        if (key.compare(0, prefix.size(), prefix) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

// The package loads as one variant over obd2, its 18 modules on the
// engine ECU's OBD pair and a made-up block of 11-bit pairs (no
// manufacturer's layout), each module with a name DID of its own
TEST(PluginTest, TheMadeUpCarHoldsEighteenModulesOnMadeUpAddresses)
{
    Car car;
    Plugin p = load(car);
    ASSERT_NE(p, nullptr);
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_plugin_info(p.get(), &info), ECUIO_OK);
    EXPECT_STREQ(info.id, "example-car");
    EXPECT_EQ(info.variant_count, 1u);
    EXPECT_EQ(info.fixture_count, 1u);
    const ecuio_signal_set_t* set = ecuio_plugin_variant_set(p.get(), 0);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->channel_count, kModules);
    EXPECT_EQ(set->channels[0].rx_id, 0x7E8u);
    EXPECT_EQ(set->channels[0].tx_id, 0x7E0u);
    for (uint32_t i = 1; i < kModules; ++i) {
        SCOPED_TRACE(kIds[i]);
        EXPECT_EQ(set->channels[i].rx_id, 0x520u + i - 1);
        EXPECT_EQ(set->channels[i].tx_id, 0x500u + i - 1);
    }
    const ecuio_signal_file_t* file = ecuio_plugin_variant_file(p.get(), 0);
    ASSERT_NE(file, nullptr);
    std::set<uint32_t> named;
    for (uint32_t d = 0; d < set->count; ++d) {
        const ecuio_signal_def_t& def = set->defs[d];
        const std::string key = ecuio_signal_file_key(file, &def);
        if (def.kind == ECUIO_VALUE_TEXT && module_of(key) >= 0 &&
            def.mux_key == 0x624A00u) {
            named.insert(def.message_id);
        }
    }
    EXPECT_EQ(named.size(), size_t{kModules}) << "each module's own name";
}

// Every number and computed signal of the car's own gives a gauge (a dial
// starts on its range, not a guess) and no text or enumeration does; a few
// are ranked, so a session with no pick asks something
TEST(PluginTest, TheMadeUpCarsDialsStartOnTheirOwnRanges)
{
    Car car;
    Plugin p = load(car);
    ASSERT_NE(p, nullptr);
    const ecuio_signal_set_t* set = ecuio_plugin_variant_set(p.get(), 0);
    const ecuio_signal_file_t* file = ecuio_plugin_variant_file(p.get(), 0);
    ASSERT_NE(set, nullptr);
    uint32_t own = 0, gauged = 0, ranked = 0, computed = 0;
    for (uint32_t d = 0; d < set->count; ++d) {
        const ecuio_signal_def_t& def = set->defs[d];
        const std::string key = ecuio_signal_file_key(file, &def);
        if (module_of(key) < 0) {
            continue;
        }
        ++own;
        const bool dial = def.kind != ECUIO_VALUE_TEXT &&
                          def.kind != ECUIO_VALUE_HEX &&
                          def.kind != ECUIO_VALUE_ENUM;
        const ecuio_signal_gauge_t* g = ecuio_signal_file_gauge(file, &def);
        EXPECT_EQ(g != nullptr, dial) << key;
        if (g != nullptr) {
            ++gauged;
            EXPECT_LT(g->min, g->max) << key;
        }
        computed += (def.flags & ECUIO_DEF_FLAG_COMPUTED) != 0 ? 1u : 0u;
        ranked += def.plot_rank > 0 ? 1u : 0u;
    }
    EXPECT_GE(own, 2 * kModules);
    EXPECT_GE(gauged, kModules);
    EXPECT_GE(computed, 2u) << "a computed signal's gauge too";
    EXPECT_GE(ranked, 3u);
    EXPECT_LE(ranked, 8u) << "a few ranked: a driver's first signals";
}

// The variant's written copy (what a recording keeps) reads back to a set
// of the same signals over the same 18 modules
TEST(PluginTest, TheMadeUpCarsWrittenCopyReadsBack)
{
    Car car;
    Plugin p = load(car);
    ASSERT_NE(p, nullptr);
    const ecuio_signal_set_t* set = ecuio_plugin_variant_set(p.get(), 0);
    size_t length = 0;
    const char* text =
        ecuio_signal_file_text(ecuio_plugin_variant_file(p.get(), 0), &length);
    ASSERT_GT(length, 0u);
    ecuio_signal_file_t* back = nullptr;
    ASSERT_EQ(ecuio_signal_file_load_with(text, length, 0, &back), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t* again = ecuio_signal_file_set(back);
    EXPECT_EQ(again->count, set->count);
    EXPECT_EQ(again->channel_count, kModules);
    ecuio_signal_file_free(back);
}

// The simulated ECU on the car's profile answers a functional request for
// the name DID from every module at once, each on its own reply ID, each
// reply longer than a frame
TEST(PluginTest, TheMadeUpCarAnswersAsEighteenModulesAtOnce)
{
    Car car;
    Plugin p = load(car);
    ASSERT_NE(p, nullptr);
    char name[64] = {};
    ASSERT_EQ(ecuio_plugin_register(p.get(), 0, name, sizeof name), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_STREQ(name, "plugin.example-car.whole");
    {
        ecuio_sim_t* sim = nullptr;
        ASSERT_EQ(ecuio_sim_create_unbound(
                      "mock://car?profile=plugin.example-car.whole", 0, &sim),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
        const uint8_t q[3] = {0x22, 0x4A, 0x00};
        uint32_t n = 0;
        ASSERT_EQ(ecuio_sim_ask(sim, 0x7DF, q, 3, ECUIO_SIM_NOW, out.data(),
                                ECUIO_SIM_MAX_REPLIES, &n),
                  ECUIO_OK);
        std::set<uint32_t> ids;
        for (uint32_t i = 0; i < n; ++i) {
            ids.insert(out[i].reply_id);
            EXPECT_GT(out[i].length, 7u) << std::hex << out[i].reply_id;
            EXPECT_EQ(out[i].data[0], 0x62);
        }
        EXPECT_EQ(n, kModules);
        EXPECT_EQ(ids.size(), size_t{kModules});
        ecuio_sim_destroy(sim);
    }
    EXPECT_EQ(ecuio_profile_unregister("plugin.example-car.whole"), ECUIO_OK);
}
