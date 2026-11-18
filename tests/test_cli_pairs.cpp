// File/layer: tests/test_cli_pairs.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliPairsTest (test_cli)
//
// cli/signal_pairs.h: which signal of another recording a signal compares
// with, the rule view's delta and the app's Compare share.

#include "signal_pairs.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Sig {
    const char* key;
    const char* name;
};

std::vector<ecuio_recording_signal_t> recording(const std::vector<Sig>& sigs)
{
    std::vector<ecuio_recording_signal_t> out(sigs.size());
    for (size_t i = 0; i < sigs.size(); ++i) {
        std::strcpy(out[i].key, sigs[i].key);
        std::strcpy(out[i].name, sigs[i].name);
        std::strcpy(out[i].unit, "rpm");
    }
    return out;
}

std::vector<uint32_t> pairs(const std::vector<Sig>& a,
                            const std::vector<Sig>& b)
{
    const auto x = recording(a);
    const auto y = recording(b);
    return cli::pair_signals(
        static_cast<uint32_t>(x.size()),
        [&](uint32_t k) -> const auto& { return x[k]; },
        static_cast<uint32_t>(y.size()),
        [&](uint32_t k) -> const auto& { return y[k]; });
}

const char* const kRpm1 = "7E8:410C:2.0:16";
const char* const kRpm2 = "7E9:410C:2.0:16";
const char* const kRpm3 = "7E0:22F40C:4.0:16";
const uint32_t kNone = cli::kNoPair;

} // namespace

TEST(CliPairsTest, TwoSignalsOfOneNamePairByKey)
{
    EXPECT_EQ(pairs({{kRpm1, "RPM"}, {kRpm2, "RPM"}},
                    {{kRpm2, "RPM"}, {kRpm1, "RPM"}}),
              (std::vector<uint32_t>{1, 0}));
    EXPECT_EQ(pairs({{kRpm1, "Engine speed"}},
                    {{kRpm3, "Engine speed"}, {kRpm1, "RPM"}}),
              (std::vector<uint32_t>{1}))
        << "a key both have beats a name both have";
}

TEST(CliPairsTest, ANameEachHasOncePairsWhereNoKeyDoes)
{
    EXPECT_EQ(pairs({{"", "RPM"}, {"", "Boost"}},
                    {{kRpm1, "RPM"}, {"boost", "Boost"}}),
              (std::vector<uint32_t>{0, 1}))
        << "an older recording, no keys, against a format 4 one";
    EXPECT_EQ(pairs({{kRpm1, "RPM"}, {"boost", "Boost"}},
                    {{"", "Boost"}, {"", "RPM"}}),
              (std::vector<uint32_t>{1, 0}));
    EXPECT_EQ(pairs({{kRpm3, "RPM"}}, {{kRpm1, "RPM"}}),
              (std::vector<uint32_t>{0}))
        << "keys on both sides and none in common: obd2's against a plugin's";
    EXPECT_EQ(pairs({{"", "RPM"}}, {{"", "rpm"}}),
              (std::vector<uint32_t>{kNone}))
        << "a name's case is another name";
}

TEST(CliPairsTest, ANameTwoSignalsHavePairsNothingByName)
{
    EXPECT_EQ(pairs({{"", "RPM"}}, {{kRpm1, "RPM"}, {kRpm2, "RPM"}}),
              (std::vector<uint32_t>{kNone}))
        << "the other recording has the name twice";
    EXPECT_EQ(pairs({{kRpm1, "RPM"}, {kRpm2, "RPM"}}, {{"", "RPM"}}),
              (std::vector<uint32_t>{kNone, kNone}))
        << "this recording has the name twice";
    EXPECT_EQ(pairs({{kRpm1, "RPM"}, {kRpm2, "RPM"}}, {{kRpm2, "RPM"}}),
              (std::vector<uint32_t>{kNone, 0}))
        << "a key still pairs where the name can't";
}

TEST(CliPairsTest, ASignalAKeyPairsIsNoNamesPair)
{
    EXPECT_EQ(
        pairs({{"oil_temp", "Oil temperature"}, {"oil_temp_2", "Oil temp"}},
              {{"oil_temp", "Oil temp"}}),
        (std::vector<uint32_t>{0, kNone}))
        << "a renamed signal keeps its pair; the new one of its old name "
           "takes none";
    EXPECT_EQ(
        pairs({{"oil_temp_2", "Oil temp"}, {"oil_temp", "Oil temperature"}},
              {{"oil_temp", "Oil temp"}}),
        (std::vector<uint32_t>{kNone, 0}))
        << "whichever comes first";
    EXPECT_EQ(pairs({{"", "Oil temp"}}, {{"oil_temp", "Oil temp"}}),
              (std::vector<uint32_t>{0}))
        << "a key this recording lacks claims nothing";
}

TEST(CliPairsTest, AnIdKeyPairsByNameNeverByItsNumber)
{
    EXPECT_EQ(pairs({{"id:5", "Load"}}, {{"id:5", "Fuel"}, {"id:6", "Load"}}),
              (std::vector<uint32_t>{1}));
    EXPECT_EQ(pairs({{"id:5", "Load"}}, {{"id:5", "Fuel"}}),
              (std::vector<uint32_t>{kNone}));
    EXPECT_TRUE(cli::key_pairs("boost"));
    EXPECT_TRUE(cli::key_pairs(kRpm1));
    EXPECT_FALSE(cli::key_pairs(""));
    EXPECT_FALSE(cli::key_pairs("id:12"));
}

TEST(CliPairsTest, AFieldWithNoNulIsReadToItsEnd)
{
    ecuio_recording_signal_t full{};
    std::memset(full.name, 'n', sizeof full.name);
    EXPECT_EQ(cli::field_text(full.name), std::string(sizeof full.name, 'n'));
    std::vector<ecuio_recording_signal_t> a{full};
    std::vector<ecuio_recording_signal_t> b{full};
    const auto got = cli::pair_signals(
        1, [&](uint32_t k) -> const auto& { return a[k]; }, 1,
        [&](uint32_t k) -> const auto& { return b[k]; });
    EXPECT_EQ(got, (std::vector<uint32_t>{0}));
}

TEST(CliPairsTest, OneUnitIsItsAsciiCaseAside)
{
    ecuio_recording_signal_t speed{};
    std::strcpy(speed.name, "Vehicle Speed");
    std::strcpy(speed.unit, "km/h");
    ecuio_recording_signal_t other = speed;
    EXPECT_TRUE(cli::same_unit(speed, other));
    std::strcpy(other.unit, "mph");
    EXPECT_FALSE(cli::same_unit(speed, other))
        << "km/h less mph is no difference";
    std::strcpy(other.unit, "KM/H");
    EXPECT_TRUE(cli::same_unit(speed, other))
        << "a unit's case is no other unit: RPM and rpm";
    std::strcpy(other.unit, "km/h ");
    EXPECT_FALSE(cli::same_unit(speed, other));
    std::strcpy(other.unit, "km");
    EXPECT_FALSE(cli::same_unit(speed, other));
    other.unit[0] = '\0';
    speed.unit[0] = '\0';
    EXPECT_TRUE(cli::same_unit(speed, other)) << "no unit on either side";
}
