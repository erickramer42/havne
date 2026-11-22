// File/layer: tests/test_obd2.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

/* ---------- set integrity ---------- */

TEST(Obd2SetTest, SetIsValidAndComplete) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(ecuio_validate_signal_set(set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(set->count, 131u)
        << "every linear field of J1979's table and the computed boost";
}

/* The ten keep 7001-7010, first and in their order; every other def's
 * ID is ECUIO_OBD2_PID_SIG_ID of its PID and its place among the PID's
 * fields (ADR-0039 section 4), clear of the mock's band and uds-generic's. */
TEST(Obd2SetTest, IdsFollowTheRule)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const uint16_t ten[] = {
        ECUIO_OBD2_SIG_ENGINE_LOAD_ID, ECUIO_OBD2_SIG_COOLANT_TEMP_ID,
        ECUIO_OBD2_SIG_RPM_ID,         ECUIO_OBD2_SIG_SPEED_ID,
        ECUIO_OBD2_SIG_IAT_ID,         ECUIO_OBD2_SIG_THROTTLE_ID,
        ECUIO_OBD2_SIG_FUEL_LEVEL_ID,  ECUIO_OBD2_SIG_BATTERY_V_ID,
        ECUIO_OBD2_SIG_MAF_ID,         ECUIO_OBD2_SIG_OIL_TEMP_ID};
    ASSERT_GE(obd2->count, 10u);
    for (uint32_t i = 0; i < 10; ++i) {
        EXPECT_EQ(obd2->defs[i].id, ten[i]) << i;
    }
    unsigned last_pid = 0, field = 0;
    for (uint32_t i = 10; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        SCOPED_TRACE(d.name);
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            EXPECT_EQ(i + 1, obd2->count) << "a computed def comes last";
            continue;
        }
        const unsigned pid = d.mux_key & 0xFFu;
        ASSERT_GE(pid, last_pid) << "the rest in PID order";
        field = (pid == last_pid) ? field + 1 : 0;
        last_pid = pid;
        EXPECT_EQ(d.id, ECUIO_OBD2_PID_SIG_ID(pid, field));
        EXPECT_GE(d.id, 11000u);
        EXPECT_LT(d.id, ECUIO_UDS_GENERIC_SIG_ID_BASE);
    }
}

/* ADR-0039 section 8: the signals a UI plots when a session starts,
 * ranked 1 up with no gaps or repeats, the five engine-state ones
 * first; every other def unranked */
TEST(Obd2SetTest, RanksTheSignalsAUiPlotsFirst)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const uint16_t order[] = {
        ECUIO_OBD2_SIG_RPM_ID,          ECUIO_OBD2_SIG_COOLANT_TEMP_ID,
        ECUIO_OBD2_SIG_IAT_ID,          ECUIO_OBD2_SIG_MAF_ID,
        ECUIO_OBD2_SIG_OIL_TEMP_ID,     ECUIO_OBD2_SIG_SPEED_ID,
        ECUIO_OBD2_SIG_ENGINE_LOAD_ID,  ECUIO_OBD2_PID_SIG_ID(0x0E, 0),
        ECUIO_OBD2_PID_SIG_ID(0x06, 0), ECUIO_OBD2_PID_SIG_ID(0x07, 0),
        ECUIO_OBD2_PID_SIG_ID(0x0B, 0), ECUIO_OBD2_SIG_BATTERY_V_ID,
        ECUIO_OBD2_PID_SIG_ID(0x49, 0), ECUIO_OBD2_PID_SIG_ID(0x44, 0)};
    for (size_t r = 0; r < sizeof order / sizeof order[0]; ++r) {
        const ecuio_signal_def_t* d = find_def(obd2, order[r]);
        ASSERT_NE(d, nullptr) << order[r];
        EXPECT_EQ(d->plot_rank, r + 1) << d->name;
    }
    uint32_t ranked = 0;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        ranked += obd2->defs[i].plot_rank != 0 ? 1u : 0u;
    }
    EXPECT_EQ(ranked, sizeof order / sizeof order[0]) << "the rest unranked";

    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    for (uint32_t i = 0; i < mock->count; ++i) {
        EXPECT_EQ(mock->defs[i].plot_rank, 0u) << "the mock ranks none: a UI plots all";
    }
}

/* ADR-0050: every def placed, a PID's fields together but for 4F's
 * intake pressure, a def of each group where it belongs; the mock's
 * defs where obd2's of the same name are */
TEST(Obd2SetTest, GroupsEachSignalByThePartOfTheCarItReads)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        EXPECT_NE(d.group, ECUIO_SIGNAL_GROUP_OTHER) << d.name;
        EXPECT_STRNE(ecuio_signal_group_name(d.group), "") << d.name;
        const ecuio_signal_def_t* first = find_def(
            obd2,
            static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(d.mux_key & 0xFFu, 0)));
        if (first != nullptr && d.id != ECUIO_OBD2_PID_SIG_ID(0x4F, 3)) {
            EXPECT_EQ(d.group, first->group)
                << d.name << " and " << first->name;
        }
    }
    const struct {
        uint16_t id;
        uint8_t group;
    } placed[] = {
        {ECUIO_OBD2_SIG_RPM_ID, ECUIO_SIGNAL_GROUP_ENGINE},
        {ECUIO_OBD2_SIG_OIL_TEMP_ID, ECUIO_SIGNAL_GROUP_ENGINE},
        {ECUIO_OBD2_PID_SIG_ID(0x64, 2), ECUIO_SIGNAL_GROUP_ENGINE},
        {ECUIO_OBD2_SIG_IAT_ID, ECUIO_SIGNAL_GROUP_AIR},
        {ECUIO_OBD2_PID_SIG_ID(0x49, 0), ECUIO_SIGNAL_GROUP_AIR},
        {ECUIO_OBD2_PID_SIG_ID(0x4F, 3), ECUIO_SIGNAL_GROUP_AIR},
        {ECUIO_OBD2_PID_SIG_ID(0x06, 0), ECUIO_SIGNAL_GROUP_FUEL},
        {ECUIO_OBD2_SIG_FUEL_LEVEL_ID, ECUIO_SIGNAL_GROUP_FUEL},
        {ECUIO_OBD2_PID_SIG_ID(0x14, 1), ECUIO_SIGNAL_GROUP_OXYGEN},
        {ECUIO_OBD2_PID_SIG_ID(0x4F, 0), ECUIO_SIGNAL_GROUP_OXYGEN},
        {ECUIO_OBD2_PID_SIG_ID(0x2C, 0), ECUIO_SIGNAL_GROUP_EMISSIONS},
        {ECUIO_OBD2_PID_SIG_ID(0x3C, 0), ECUIO_SIGNAL_GROUP_EMISSIONS},
        {ECUIO_OBD2_SIG_SPEED_ID, ECUIO_SIGNAL_GROUP_VEHICLE},
        {ECUIO_OBD2_PID_SIG_ID(0xA6, 0), ECUIO_SIGNAL_GROUP_VEHICLE},
        {ECUIO_OBD2_SIG_BATTERY_V_ID, ECUIO_SIGNAL_GROUP_ELECTRICAL},
        {ECUIO_OBD2_PID_SIG_ID(0x21, 0), ECUIO_SIGNAL_GROUP_DIAGNOSTICS},
        {ECUIO_OBD2_PID_SIG_ID(0x4E, 0), ECUIO_SIGNAL_GROUP_DIAGNOSTICS},
    };
    for (const auto& p : placed) {
        const ecuio_signal_def_t* d = find_def(obd2, p.id);
        ASSERT_NE(d, nullptr) << p.id;
        EXPECT_EQ(d->group, p.group) << d->name;
    }

    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    for (uint32_t i = 0; i < mock->count; ++i) {
        const ecuio_signal_def_t& m = mock->defs[i];
        /* obd2 has neither: Lambda reads a wideband, boost is computed */
        uint8_t expected = std::strcmp(m.name, "Boost") == 0
                               ? ECUIO_SIGNAL_GROUP_AIR
                               : ECUIO_SIGNAL_GROUP_OXYGEN;
        for (uint32_t k = 0; k < obd2->count; ++k) {
            if (std::strcmp(obd2->defs[k].name, m.name) == 0) {
                expected = obd2->defs[k].group;
            }
        }
        EXPECT_EQ(m.group, expected) << m.name;
    }
}

/* ADR-0051 section 5: boost is MAP (PID 0B) minus baro (PID 33), the mock's
 * computed def over obd2's inputs */
TEST(Obd2SetTest, BoostIsComputedFromMapAndBaro)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(obd2, ECUIO_OBD2_SIG_BOOST_ID);
    ASSERT_NE(d, nullptr);
    EXPECT_STREQ(d->name, "Boost");
    EXPECT_STREQ(d->unit, "kPa");
    EXPECT_NE(d->flags & ECUIO_DEF_FLAG_COMPUTED, 0u);
    EXPECT_EQ(d->plot_rank, 0u);
    ASSERT_LT(d->table_first, obd2->computed_count);
    const ecuio_computed_t& c = obd2->computed[d->table_first];
    ASSERT_EQ(c.input_count, 2u);
    EXPECT_EQ(c.input_ids[0], ECUIO_OBD2_PID_SIG_ID(0x0B, 0));
    EXPECT_EQ(c.input_ids[1], ECUIO_OBD2_PID_SIG_ID(0x33, 0));
    EXPECT_EQ(ecuio_validate_signal_set(obd2, nullptr), 0);
}

/* A set's names are one each (a recording keys values by name, and a signal
 * file's signal named as a def replaces it): the built-in sets already are */
TEST(Obd2SetTest, EachBuiltInSetNamesEachSignalOnce)
{
    for (const ecuio_signal_set_t* set :
         {ecuio_obd2_signal_set(), ecuio_mock_signal_set()}) {
        for (uint32_t i = 0; i < set->count; ++i) {
            for (uint32_t j = i + 1; j < set->count; ++j) {
                EXPECT_STRNE(set->defs[i].name, set->defs[j].name);
            }
        }
    }
}

TEST(Obd2SetTest, DeclaresSingleCanChannel) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->channel_count, 1u);
    ASSERT_NE(set->channels, nullptr);
    EXPECT_EQ(set->channels[0].rx_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(set->channels[0].tx_id, ECUIO_OBD2_TESTER_ARB_ID);
}

TEST(Obd2SetTest, RejectsDuplicateAndIllegalChannelDecls) {
    ecuio_signal_def_t defs[1] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].start_byte = 2;
    defs[0].bit_length = 8;
    defs[0].mux_offset = 0;
    defs[0].mux_length = 2;
    defs[0].mux_key = 0x4104u;
    defs[0].message_id = 0x7E8u;
    auto set_with = [&defs](const ecuio_channel_decl_t* channels, uint32_t n) {
        ecuio_signal_set_t s = {};
        s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        s.count = 1u;
        s.defs = defs;
        s.channel_count = n;
        s.channels = channels;
        return s;
    };

    const ecuio_channel_decl_t dup[] = { {0x7E8u, 0x7E0u, 0, 0},
                                         {0x7E8u, 0x7E2u, 0, 0} };
    const ecuio_signal_set_t s_dup = set_with(dup, 2u);
    EXPECT_EQ(ecuio_validate_signal_set(&s_dup, nullptr), ECUIO_VALIDATE_ERR_DUPLICATE_RX);

    const ecuio_channel_decl_t zero_rx[] = { {0u, 0x7E0u, 0, 0} };
    const ecuio_signal_set_t s_zero = set_with(zero_rx, 1u);
    EXPECT_EQ(ecuio_validate_signal_set(&s_zero, nullptr), ECUIO_VALIDATE_ERR_CHANNEL_ID);

    const ecuio_channel_decl_t legal[] = { {0x7E8u, 0x7E0u, 0, 0} };
    const ecuio_signal_set_t s_ok = set_with(legal, 1u);
    EXPECT_EQ(ecuio_validate_signal_set(&s_ok, nullptr), ECUIO_VALIDATE_OK);
}

/* ---------- decode round-trips (canned wire bytes) ---------- */

TEST(Obd2DecodeTest, RpmBigEndianScaled) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x0C, 0x1F, 0x40};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_TRUE(ecuio_def_applies_to_frame(d, &f));
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 2000.0);   /* 0x1F40 * 0.25 */
}

TEST(Obd2DecodeTest, CoolantOffsetApplied) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_COOLANT_TEMP_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x05, 0x3C};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 20.0);    /* 60 - 40 */
}

TEST(Obd2DecodeTest, BatteryVoltageMillivoltScale) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_BATTERY_V_ID);
    ASSERT_NE(d, nullptr);
    /* 0x3630 = 13872 raw -> 13.872 V at 0.001 scale. Realistic
     * running-system voltage, and the byte pair is chosen so the
     * expected value is DERIVED from it, not asserted from memory. */
    const uint8_t wire[] = {0x41, 0x42, 0x36, 0x30};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_NEAR(v, 13.872, 0.001);   /* 0x3630 * 0.001 */
}

TEST(Obd2DecodeTest, EngineLoadPercentScale) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t wire[] = {0x41, 0x04, 25};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_NEAR(v, 25.0 * 100.0 / 255.0, 0.01);
}

TEST(Obd2DecodeTest, TemperatureIsUnsignedMinusFortyOffset)
{
    /* J1979 temps are UNSIGNED A - 40. Decoded as INT8, 0xF0 would read
     * -56 C; correct is 240 - 40 = +200 C, and 0x00 is the -40 C
     * sentinel. Both ends pinned. */
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_IAT_ID);
    ASSERT_NE(d, nullptr);
    const uint8_t lo[] = {0x41, 0x0F, 0x00};
    ecuio_frame_t f = make_obd2_frame(lo, sizeof lo);
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, -40.0);
    const uint8_t hi[] = {0x41, 0x0F, 0xF0};
    f = make_obd2_frame(hi, sizeof hi);
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 200.0);
}

TEST(Obd2DecodeTest, MafCentigramScale) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_MAF_ID);
    ASSERT_NE(d, nullptr);
    /* 0x0064 = 100 raw * 0.01 = 1.00 g/s: pins the /100 (centi-grams/s)
     * scale through the generic scale+offset path, with no decoder
     * special case. */
    const uint8_t wire[] = {0x41, 0x10, 0x00, 0x64};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_TRUE(ecuio_def_applies_to_frame(d, &f));
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_NEAR(v, 1.00, 0.0001);
}

TEST(Obd2DecodeTest, OilTempMatchesCoolantShape) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* d = find_def(set, ECUIO_OBD2_SIG_OIL_TEMP_ID);
    ASSERT_NE(d, nullptr);
    /* 0x78 = 120 raw - 40 = 80 degC, the fake's canned default. */
    const uint8_t wire[] = {0x41, 0x5C, 0x78};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    ASSERT_TRUE(ecuio_def_applies_to_frame(d, &f));
    double v = 0.0;
    ASSERT_EQ(decode(d, f, &v), ECUIO_DECODE_OK);
    EXPECT_DOUBLE_EQ(v, 80.0);
}

/* ---------- mux gate ---------- */

TEST(Obd2MuxTest, WrongPidRejectedByMatchAndDecode) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* rpm = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    /* A PID 0x0D response whose data bytes would vacuously decode as
     * RPM if the mux gate were absent: the Blocker-1 scenario. */
    const uint8_t wire[] = {0x41, 0x0D, 0x1F, 0x40};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    EXPECT_FALSE(ecuio_def_applies_to_frame(rpm, &f));
    double v = 0.0;
    EXPECT_EQ(decode(rpm, f, &v),
              ECUIO_DECODE_ERR_MUX_MISMATCH);
}

TEST(Obd2MuxTest, WrongArbIdRejected) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* rpm = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    const uint8_t wire[] = {0x41, 0x0C, 0x1F, 0x40};
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    f.arbitration_id = 0x0C0;   /* mock telemetry channel */
    EXPECT_FALSE(ecuio_def_applies_to_frame(rpm, &f));
}

TEST(Obd2MuxTest, ShortFrameRejectedWithoutOverread) {
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    const ecuio_signal_def_t* rpm = find_def(set, ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_NE(rpm, nullptr);
    const uint8_t wire[] = {0x41};   /* truncated before PID */
    ecuio_frame_t f = make_obd2_frame(wire, sizeof wire);
    EXPECT_FALSE(ecuio_def_applies_to_frame(rpm, &f));
    double v = 0.0;
    EXPECT_EQ(decode(rpm, f, &v),
              ECUIO_DECODE_ERR_PAST_PAYLOAD);
}

/* ---------- the Service 01 table (fixtures/obd2/service01.csv) ---------- */

namespace {

struct PidRow {
    int line = 0;
    unsigned pid = 0;
    unsigned bytes = 0;
    std::string kind;
    unsigned start = 0;
    unsigned bits = 0;
    bool is_signed = false;
    double scale = 0;
    double offset = 0;
    std::string unit;
    double min = 0;
    double max = 0;
    std::string name;
};

constexpr const char* kTableHeader =
    "pid,bytes,kind,start,bits,signed,scale,offset,unit,min,max,name,note";

/* A number, or a fraction like 100/255 */
double number(const std::string& s)
{
    const size_t slash = s.find('/');
    if (slash == std::string::npos) {
        return std::strtod(s.c_str(), nullptr);
    }
    return std::strtod(s.substr(0, slash).c_str(), nullptr) /
           std::strtod(s.substr(slash + 1).c_str(), nullptr);
}

/* The table's rows; empty with why set when it won't read */
std::vector<PidRow> load_table(std::string* why)
{
    std::vector<PidRow> rows;
    std::ifstream in(ECUIO_OBD2_TABLE, std::ios::binary);
    if (!in) {
        *why = "can't open " ECUIO_OBD2_TABLE;
        return {};
    }
    std::string text;
    int n = 0;
    bool header = false;
    while (std::getline(in, text)) {
        ++n;
        if (!text.empty() && text.back() == '\r') {
            text.pop_back();
        }
        if (text.empty() || text[0] == '#') {
            continue;
        }
        if (!header) {
            if (text != kTableHeader) {
                *why = "line " + std::to_string(n) + " isn't the header";
                return {};
            }
            header = true;
            continue;
        }
        std::vector<std::string> f;
        size_t from = 0;
        for (size_t comma; (comma = text.find(',', from)) != std::string::npos;
             from = comma + 1) {
            f.push_back(text.substr(from, comma - from));
        }
        f.push_back(text.substr(from));
        if (f.size() != 13) {
            *why = "line " + std::to_string(n) + " has " +
                   std::to_string(f.size()) + " fields, not 13";
            return {};
        }
        PidRow r;
        r.line = n;
        r.pid = static_cast<unsigned>(std::strtoul(f[0].c_str(), nullptr, 16));
        r.bytes =
            static_cast<unsigned>(std::strtoul(f[1].c_str(), nullptr, 10));
        r.kind = f[2];
        if (r.kind == "linear") {
            r.start =
                static_cast<unsigned>(std::strtoul(f[3].c_str(), nullptr, 10));
            r.bits =
                static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
            r.is_signed = f[5] == "1";
            r.scale = number(f[6]);
            r.offset = number(f[7]);
            r.unit = f[8];
            r.min = number(f[9]);
            r.max = number(f[10]);
        }
        r.name = f[11];
        rows.push_back(r);
    }
    if (rows.empty()) {
        *why = "no rows";
    }
    return rows;
}

class Obd2TableTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        fake_ecu::reset_diagnostics();
        std::string why;
        rows_ = load_table(&why);
        ASSERT_FALSE(rows_.empty()) << why;
    }

    /* The linear row for a PID's field at a data byte, or nullptr */
    const PidRow* linear(unsigned pid, unsigned start) const
    {
        for (const PidRow& r : rows_) {
            if (r.pid == pid && r.kind == "linear" && r.start == start) {
                return &r;
            }
        }
        return nullptr;
    }

    /* The PID's byte count, or 0 when the table lacks it */
    unsigned bytes_of(unsigned pid) const
    {
        for (const PidRow& r : rows_) {
            if (r.pid == pid) {
                return r.bytes;
            }
        }
        return 0;
    }

    std::vector<PidRow> rows_;
};

} // namespace

TEST_F(Obd2TableTest, RowsAreWellFormed)
{
    const std::string kinds = " linear bitmap bits enum dtc complex ";
    unsigned last = 0;
    for (const PidRow& r : rows_) {
        SCOPED_TRACE("line " + std::to_string(r.line) + ", " + r.name);
        EXPECT_NE(kinds.find(" " + r.kind + " "), std::string::npos) << r.kind;
        EXPECT_LE(r.pid, 0xFFu);
        EXPECT_GE(r.pid, last) << "rows run in PID order";
        last = r.pid;
        EXPECT_EQ(r.bytes, bytes_of(r.pid)) << "one byte count per PID";
        EXPECT_GE(r.bytes, 1u);
        EXPECT_EQ(r.kind == "bitmap", r.pid % 0x20 == 0)
            << "bitmaps at 00, 20, 40 and on";
        if (r.kind == "bitmap") {
            EXPECT_EQ(r.bytes, 4u);
        }
        if (r.kind == "linear") {
            EXPECT_TRUE(r.bits == 8 || r.bits == 16 || r.bits == 32) << r.bits;
            EXPECT_LE(r.start + r.bits / 8, r.bytes)
                << "the field fits the reply";
            EXPECT_NE(r.scale, 0.0);
            EXPECT_EQ(linear(r.pid, r.start), &r) << "one field per start byte";
        }
    }
}

/* The range the scaling gives a field's raw extremes against the
 * source's: a wrong scale, offset, width or sign shows as a range that
 * doesn't match. Within 0.2% of the span, which the source's rounded
 * maxima (99.2 for 99.21875) need. */
TEST_F(Obd2TableTest, EachRangeFollowsFromItsScaling)
{
    for (const PidRow& r : rows_) {
        if (r.kind != "linear") {
            continue;
        }
        SCOPED_TRACE("line " + std::to_string(r.line) + ", " + r.name);
        const double span = std::ldexp(1.0, static_cast<int>(r.bits));
        const double raw_lo = r.is_signed ? -span / 2 : 0.0;
        const double raw_hi = r.is_signed ? span / 2 - 1 : span - 1;
        const double lo = raw_lo * r.scale + r.offset;
        const double hi = raw_hi * r.scale + r.offset;
        const double tolerance = 0.002 * std::fabs(r.max - r.min) + 1e-9;
        EXPECT_NEAR(lo, r.min, tolerance);
        EXPECT_NEAR(hi, r.max, tolerance);
    }
}

/* Each obd2 def against its PID's row: where the field sits, its width,
 * sign, byte order, scaling and unit */
TEST_F(Obd2TableTest, EveryObd2DefMatchesItsRow)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    ASSERT_NE(set, nullptr);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        SCOPED_TRACE(d.name);
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue; /* no PID, so no row */
        }
        ASSERT_EQ(d.mux_offset, 0u);
        ASSERT_EQ(d.mux_length, 2u);
        ASSERT_EQ(d.mux_key >> 8, 0x41u) << "a Service 01 reply";
        const unsigned pid = d.mux_key & 0xFFu;
        ASSERT_GE(d.start_byte, 2u);
        const PidRow* r = linear(pid, d.start_byte - 2u);
        ASSERT_NE(r, nullptr) << "no linear row for PID " << pid;
        EXPECT_EQ(d.bit_offset, 0u);
        EXPECT_EQ(d.bit_length, r->bits);
        EXPECT_EQ(d.kind,
                  static_cast<uint8_t>(r->is_signed ? ECUIO_VALUE_INT
                                                    : ECUIO_VALUE_UINT));
        if (r->bits > 8) {
            EXPECT_EQ(d.byte_order, static_cast<uint8_t>(ECUIO_BYTE_ORDER_BE))
                << "J1979's multi-byte values";
        }
        EXPECT_NEAR(d.scale, r->scale, 1e-12 * std::fabs(r->scale));
        EXPECT_DOUBLE_EQ(d.offset, r->offset);
        EXPECT_STREQ(d.unit, r->unit.c_str());
    }
}

/* The set covers the table: exactly one def for every linear row (its
 * PID and start byte), and none for a row of another kind (ADR-0039
 * section 1, section 2). */
TEST_F(Obd2TableTest, EveryLinearRowHasExactlyOneDef)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (const PidRow& r : rows_) {
        SCOPED_TRACE("line " + std::to_string(r.line) + ", " + r.name);
        unsigned defs = 0;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if ((d.mux_key & 0xFFu) == r.pid && d.start_byte == 2 + r.start) {
                ++defs;
            }
        }
        EXPECT_EQ(defs, r.kind == "linear" ? 1u : 0u);
    }
}

/* Every fast PID (obd2_rates.h) is one the set asks */
TEST_F(Obd2TableTest, EveryFastPidIsInTheSet)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint8_t pid : ECUIO_OBD2_FAST_PIDS) {
        bool found = false;
        for (uint32_t i = 0; i < set->count; ++i) {
            found = found || (set->defs[i].mux_key & 0xFFu) == pid;
        }
        EXPECT_TRUE(found) << "PID " << int(pid);
        EXPECT_NE(linear(pid, 0), nullptr) << "PID " << int(pid);
    }
}

/* A recording keys each value by its name (signals.csv), and so do
 * Live and the recordings' diff, so every built-in set's names are
 * unique, and each fits the def's buffer with room for its end */
TEST(Obd2SetTest, BuiltInSetNamesAreUniqueAndFit)
{
    for (const ecuio_signal_set_t* set :
         {ecuio_obd2_signal_set(), ecuio_mock_signal_set()}) {
        std::vector<std::string> names;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const size_t n = strnlen(d.name, sizeof(d.name));
            ASSERT_LT(n, sizeof(d.name)) << "def " << d.id;
            EXPECT_GT(n, 0u) << "def " << d.id;
            names.emplace_back(d.name, n);
        }
        for (size_t i = 0; i < names.size(); ++i) {
            for (size_t j = i + 1; j < names.size(); ++j) {
                EXPECT_NE(names[i], names[j]) << "defs " << i << " and " << j;
            }
        }
    }
}

/* Every PID the fakes' ECU supports answers with the table's byte
 * count, and every PID the obd2 set asks is one it supports (else the
 * probe drops it in every test) */
TEST_F(Obd2TableTest, TheFakeEcuAnswersEachSupportedPidWithItsLength)
{
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        if (!fake_ecu::supports_pid(pid)) {
            continue;
        }
        SCOPED_TRACE("PID " + std::to_string(pid));
        const unsigned bytes = bytes_of(pid);
        ASSERT_NE(bytes, 0u) << "the table lacks it";
        const unsigned char req[2] = {0x01, static_cast<unsigned char>(pid)};
        unsigned char out[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        ASSERT_TRUE(fake_ecu::answer(req, sizeof(req), -1, out, &n));
        EXPECT_EQ(out[0], 0x41);
        EXPECT_EQ(out[1], pid);
        EXPECT_EQ(n, 2u + bytes);
    }
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        if ((set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue;
        }
        EXPECT_TRUE(fake_ecu::supports_pid(set->defs[i].mux_key & 0xFFu))
            << set->defs[i].name;
    }
}

/* Core's byte count per PID, which splits a packed reply, is the
 * table's for every PID it lists and 0 for every other */
TEST_F(Obd2TableTest, CoresByteCountPerPidIsTheTables)
{
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        EXPECT_EQ(ECUIO_OBD2_PID_BYTES[pid], bytes_of(pid)) << "PID " << pid;
    }
}

// Every value the fakes answer for a Service 01 PID decodes inside its
// def's range (ecuio_def_range): what a gauge's default scale starts from
TEST(Obd2SetTest, EveryValueTheFakesAnswerIsInsideItsDefsRange)
{
    fake_ecu::reset_diagnostics();
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    auto frame = std::make_unique<ecuio_frame_t>();
    uint32_t checked = 0;
    for (unsigned pid = 0; pid <= 0xFF; ++pid) {
        const unsigned char req[2] = {0x01, static_cast<unsigned char>(pid)};
        unsigned char out[fake_ecu::kReplyMax];
        size_t n = 0;
        if (!fake_ecu::answer(req, 2, -1, out, &n) || out[0] != 0x41) {
            continue;
        }
        *frame = {};
        frame->arbitration_id = 0x7E8u;
        frame->data_length = static_cast<uint16_t>(n);
        std::memcpy(frame->data, out, n);
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (!ecuio_def_applies_to_frame(&d, frame.get())) {
                continue;
            }
            double lo = 0.0;
            double hi = 0.0;
            if (ecuio_def_range(&d, &lo, &hi) != ECUIO_DECODE_OK) {
                continue;
            }
            double v = 0.0;
            ASSERT_EQ(
                ecuio_decode_signal(&d, frame->data, frame->data_length, &v),
                ECUIO_DECODE_OK)
                << d.name;
            EXPECT_GE(v, lo) << d.name;
            EXPECT_LE(v, hi) << d.name;
            ++checked;
        }
    }
    EXPECT_GT(checked, 100u) << "most of the set's numbers were answered";
    fake_ecu::reset_diagnostics();
}
