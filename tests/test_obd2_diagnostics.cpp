// File/layer: tests/test_obd2_diagnostics.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

namespace {

/* A row of tests/fixtures/obd2/monitors.csv */
struct MonitorRow {
    int line = 0;
    std::string engine;    /* any, spark, compression */
    std::string available; /* B0..B2, C0..C7 */
    std::string complete;  /* B4..B6, D0..D7: set while not complete */
    unsigned id = 0;
    std::string name;
    std::string short_name;
};

constexpr const char* kMonitorHeader =
    "engine,available,complete,id,name,short,note";

std::vector<MonitorRow> load_monitors(std::string* why)
{
    std::vector<MonitorRow> rows;
    std::ifstream in(ECUIO_OBD2_MONITORS, std::ios::binary);
    if (!in) {
        *why = "can't open " ECUIO_OBD2_MONITORS;
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
            if (text != kMonitorHeader) {
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
        if (f.size() != 7) {
            *why = "line " + std::to_string(n) + " has " +
                   std::to_string(f.size()) + " fields, not 7";
            return {};
        }
        MonitorRow r;
        r.line = n;
        r.engine = f[0];
        r.available = f[1];
        r.complete = f[2];
        r.id = static_cast<unsigned>(std::strtoul(f[3].c_str(), nullptr, 10));
        r.name = f[4];
        r.short_name = f[5];
        rows.push_back(r);
    }
    return rows;
}

/* PID 01's four bytes with one monitor's availability bit set, and its
 * completeness bit too when incomplete; B3 set for a compression row */
std::vector<uint8_t> readiness_bytes(const MonitorRow& r, bool incomplete)
{
    std::vector<uint8_t> abcd(4, 0);
    const auto bit = [](const std::string& s) {
        return static_cast<unsigned>(s[1] - '0');
    };
    abcd[r.available[0] == 'B' ? 1 : 2] |=
        static_cast<uint8_t>(1u << bit(r.available));
    if (incomplete) {
        abcd[r.complete[0] == 'B' ? 1 : 3] |=
            static_cast<uint8_t>(1u << bit(r.complete));
    }
    if (r.engine == "compression") {
        abcd[1] |= 0x08;
    }
    return abcd;
}

uint32_t flags_for(const MonitorRow& r)
{
    return r.engine == "compression" ? ECUIO_DIAG_COMPRESSION : 0u;
}

} // namespace

/* Every code, against the test's own reading of J1979's layout */
TEST_F(DiagnosticsTest, ACodeIsItsLetterAndFourDigits)
{
    char out[6];
    const std::pair<uint16_t, const char*> kKnown[] = {
        {0x0301, "P0301"}, {0x4035, "C0035"}, {0x9001, "B1001"},
        {0xC100, "U0100"}, {0x0000, "P0000"}, {0xFFFF, "U3FFF"},
        {0x3ABC, "P3ABC"},
    };
    for (const auto& k : kKnown) {
        ASSERT_EQ(ecuio_dtc_format(k.first, out, sizeof out), ECUIO_OK);
        EXPECT_STREQ(out, k.second);
    }
    for (uint32_t code = 0; code <= 0xFFFF; ++code) {
        ASSERT_EQ(
            ecuio_dtc_format(static_cast<uint16_t>(code), out, sizeof out),
            ECUIO_OK);
        ASSERT_EQ(std::string(out), dtc(static_cast<uint8_t>(code >> 8),
                                        static_cast<uint8_t>(code)))
            << std::hex << code;
    }
    EXPECT_EQ(ecuio_dtc_format(0x0301, nullptr, 6), ECUIO_ERR_NULL_ARGUMENT);
    std::memset(out, 'x', sizeof out);
    EXPECT_EQ(ecuio_dtc_format(0x0301, out, 5), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out[0], 'x') << "out untouched";
}

/* core's table is the fixture's, row for row, and nothing else has a
 * name: ids 3-7 (byte B's other bits) and compression's C2 and C4 */
TEST_F(DiagnosticsTest, EveryMonitorIsNamedAsTheTableSays)
{
    std::string why;
    const std::vector<MonitorRow> rows = load_monitors(&why);
    ASSERT_FALSE(rows.empty()) << why;
    EXPECT_EQ(rows.size(), sizeof(ECUIO_MONITORS) / sizeof(ECUIO_MONITORS[0]));
    for (const MonitorRow& r : rows) {
        SCOPED_TRACE("monitors.csv line " + std::to_string(r.line));
        ASSERT_LT(r.id, 16u);
        const bool in_b = r.id < 8;
        const unsigned bit = in_b ? r.id : r.id - 8;
        EXPECT_EQ(r.available, (in_b ? "B" : "C") + std::to_string(bit))
            << "id is the availability bit's place";
        EXPECT_EQ(r.complete, in_b ? "B" + std::to_string(bit + 4)
                                   : "D" + std::to_string(bit));
        const std::vector<uint32_t> engines =
            (r.engine == "any")
                ? std::vector<uint32_t>{0u, ECUIO_DIAG_COMPRESSION}
                : std::vector<uint32_t>{flags_for(r)};
        for (uint32_t f : engines) {
            EXPECT_STREQ(ecuio_monitor_name(static_cast<uint8_t>(r.id), f),
                         r.name.c_str());
            EXPECT_STREQ(
                ecuio_monitor_short_name(static_cast<uint8_t>(r.id), f),
                r.short_name.c_str());
        }
    }
    for (uint32_t f : {0u, ECUIO_DIAG_COMPRESSION}) {
        for (unsigned id = 0; id < 256; ++id) {
            bool listed = false;
            for (const MonitorRow& r : rows) {
                listed = listed || (r.id == id &&
                                    (r.engine == "any" || flags_for(r) == f));
            }
            if (!listed) {
                EXPECT_STREQ(ecuio_monitor_name(static_cast<uint8_t>(id), f),
                             "")
                    << id << " flags " << f;
                EXPECT_STREQ(
                    ecuio_monitor_short_name(static_cast<uint8_t>(id), f), "");
            }
        }
    }
}

/* Each monitor's two bits, alone, through the parser: the fixture's
 * columns and PID 01's layout agree */
TEST_F(DiagnosticsTest, EachMonitorReadsFromItsOwnBits)
{
    std::string why;
    const std::vector<MonitorRow> rows = load_monitors(&why);
    ASSERT_FALSE(rows.empty()) << why;
    for (const MonitorRow& r : rows) {
        SCOPED_TRACE("monitors.csv line " + std::to_string(r.line));
        for (bool incomplete : {false, true}) {
            ecuio_channel_diagnostics_t d{};
            ecuio_diag_take_readiness(readiness_bytes(r, incomplete).data(),
                                      &d);
            ASSERT_EQ(d.monitor_count, 1u);
            EXPECT_EQ(d.monitors[0].id, r.id);
            EXPECT_EQ(d.monitors[0].flags,
                      incomplete ? 0u : ECUIO_MONITOR_COMPLETE);
            EXPECT_EQ(d.flags & ECUIO_DIAG_COMPRESSION, flags_for(r));
        }
    }
}

/* Every availability bit set: the reserved ones and the engine-type bit
 * name no monitor */
TEST_F(DiagnosticsTest, ReservedBitsNameNoMonitor)
{
    const uint8_t spark[4] = {0x00, 0x07 | 0x80, 0xFF, 0x00};
    ecuio_channel_diagnostics_t d{};
    ecuio_diag_take_readiness(spark, &d);
    EXPECT_EQ(d.monitor_count, 11u) << "3 common and 8 spark";

    const uint8_t compression[4] = {0x00, 0x0F, 0xFF, 0xFF};
    d = ecuio_channel_diagnostics_t{};
    ecuio_diag_take_readiness(compression, &d);
    ASSERT_EQ(d.monitor_count, 9u) << "3 common and 6: C2 and C4 are reserved";
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_NE(d.monitors[i].id, 10u);
        EXPECT_NE(d.monitors[i].id, 12u);
        EXPECT_EQ(d.monitors[i].flags, i < 3 ? ECUIO_MONITOR_COMPLETE : 0u)
            << "D set: C's monitors incomplete";
    }
}

/* The fake ECU's PID 01 before and after a clear (J1979: the clear
 * leaves each monitor incomplete) */
TEST_F(DiagnosticsTest, ReadinessFromTheFakeEcu)
{
    const Bytes before = ecu({0x01, 0x01});
    ASSERT_EQ(before, (Bytes{0x41, 0x01, 0x83, 0x07, 0xE5, 0x00}));
    ecuio_channel_diagnostics_t d{};
    ecuio_diag_take_readiness(before.data() + 2, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_READINESS | ECUIO_DIAG_MIL_ON);
    EXPECT_EQ(d.dtc_count, 3u);
    const uint8_t kIds[] = {0, 1, 2, 8, 10, 13, 14, 15};
    ASSERT_EQ(d.monitor_count, sizeof kIds);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_EQ(d.monitors[i].id, kIds[i]);
        EXPECT_EQ(d.monitors[i].flags, ECUIO_MONITOR_COMPLETE) << int{kIds[i]};
    }

    ASSERT_EQ(ecu({0x04}), (Bytes{0x44}));
    const Bytes after = ecu({0x01, 0x01});
    ecuio_diag_take_readiness(after.data() + 2, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_READINESS) << "the MIL off";
    EXPECT_EQ(d.dtc_count, 0u);
    ASSERT_EQ(d.monitor_count, sizeof kIds);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_EQ(d.monitors[i].flags,
                  kIds[i] < 8 ? ECUIO_MONITOR_COMPLETE : 0u)
            << int{kIds[i]};
    }
}

/* PID 41 onto each monitor PID 01 listed, from that monitor's own two
 * bits: enabled this drive cycle or not, complete this cycle or not;
 * PID 01's completeness stays as it was */
TEST_F(DiagnosticsTest, EachMonitorReadsThisDriveCycleFromItsOwnBits)
{
    std::string why;
    const std::vector<MonitorRow> rows = load_monitors(&why);
    ASSERT_FALSE(rows.empty()) << why;
    for (const MonitorRow& r : rows) {
        SCOPED_TRACE("monitors.csv line " + std::to_string(r.line));
        for (bool enabled : {false, true}) {
            for (bool incomplete : {false, true}) {
                ecuio_channel_diagnostics_t d{};
                ecuio_diag_take_readiness(readiness_bytes(r, true).data(), &d);
                ASSERT_EQ(d.monitor_count, 1u);
                std::vector<uint8_t> cycle = readiness_bytes(r, incomplete);
                if (!enabled) {
                    /* the availability bit off; B3, the engine type, stays */
                    const std::vector<uint8_t> lone = readiness_bytes(r, false);
                    cycle[1] =
                        static_cast<uint8_t>(cycle[1] & ~(lone[1] & ~0x08u));
                    cycle[2] = static_cast<uint8_t>(cycle[2] & ~lone[2]);
                }
                ecuio_diag_take_cycle(cycle.data(), &d);
                EXPECT_TRUE(d.flags & ECUIO_DIAG_CYCLE);
                EXPECT_EQ(d.monitors[0].flags,
                          (enabled ? ECUIO_MONITOR_CYCLE_ENABLED : 0u) |
                              (incomplete ? 0u : ECUIO_MONITOR_CYCLE_COMPLETE))
                    << "enabled " << enabled << " incomplete " << incomplete;
            }
        }
    }
}

/* A monitor PID 41 enables but PID 01 doesn't list stays out: the
 * report's monitors are the ones the ECU has */
TEST_F(DiagnosticsTest, ThisDriveCycleAddsNoMonitor)
{
    const uint8_t misfire[4] = {0x00, 0x01, 0x00, 0x00};
    ecuio_channel_diagnostics_t d{};
    ecuio_diag_take_readiness(misfire, &d);
    const uint8_t every[4] = {0x00, 0x07, 0xFF, 0x00};
    ecuio_diag_take_cycle(every, &d);
    ASSERT_EQ(d.monitor_count, 1u);
    EXPECT_EQ(d.monitors[0].flags, ECUIO_MONITOR_COMPLETE |
                                       ECUIO_MONITOR_CYCLE_ENABLED |
                                       ECUIO_MONITOR_CYCLE_COMPLETE);
}

/* The count as the ECU said it, and the codes the list and the reply
 * both hold */
TEST_F(DiagnosticsTest, ACodeListHoldsWhatTheReplyCarries)
{
    ecuio_dtc_list_t l{};
    const Bytes stored = ecu({0x03});
    ecuio_diag_take_codes(stored.data(), static_cast<uint32_t>(stored.size()),
                          &l);
    EXPECT_EQ(l.reported, 3u);
    ASSERT_EQ(l.held, 3u);
    EXPECT_EQ(l.codes[0], 0x0301u);
    EXPECT_EQ(l.codes[1], 0x0420u);
    EXPECT_EQ(l.codes[2], 0xC100u);

    const uint8_t none[] = {0x43, 0x00};
    ecuio_diag_take_codes(none, sizeof none, &l);
    EXPECT_EQ(l.reported, 0u);
    EXPECT_EQ(l.held, 0u);

    const uint8_t cut[] = {0x47, 0x03, 0x01, 0x71, 0x40, 0x35, 0x90};
    ecuio_diag_take_codes(cut, sizeof cut, &l);
    EXPECT_EQ(l.reported, 3u);
    EXPECT_EQ(l.held, 2u) << "the reply ends inside the third";

    Bytes many{0x4A, 40};
    for (uint8_t i = 0; i < 40; ++i) {
        many.push_back(0x01);
        many.push_back(i);
    }
    ecuio_diag_take_codes(many.data(), static_cast<uint32_t>(many.size()), &l);
    EXPECT_EQ(l.reported, 40u);
    ASSERT_EQ(l.held, ECUIO_DIAG_MAX_DTCS);
    EXPECT_EQ(l.codes[ECUIO_DIAG_MAX_DTCS - 1], 0x011Fu);

    const uint8_t bare[] = {0x43};
    ecuio_diag_take_codes(bare, sizeof bare, &l);
    EXPECT_EQ(l.reported, 0u);
    EXPECT_EQ(l.held, 0u);
}

namespace {

/* An engine with one channel, one rotation request (01 0C) and one
 * scanned DID, asked to probe, identify and diagnose */
void full_engine(ecuio_request_engine* e)
{
    ecuio_engine_init(e, 1);
    const uint8_t rpm[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(e, 0, rpm, 2), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(e, 0, 0x1E20));
    ecuio_engine_probe(e);
    ecuio_engine_identify(e);
    ecuio_engine_diagnose(e, ecuio_obd2_signal_set());
}

} // namespace

/* Against the fakes' ECU: the walk first, then identification, then
 * diagnostics, then the scan, each one-shot taking turns with the
 * rotation; the freeze frame's code and values follow its bitmap; the
 * report holds what the ECU said */
TEST_F(DiagnosticsTest, TheListTakesItsTurnAfterIdentificationAndFillsTheReport)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(full_engine(&e));
    int64_t now = 0;
    const std::vector<SentRequest> sent = drive(&e, &now, fake_reply);

    std::vector<Bytes> once;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (!sent[i].once) {
            continue;
        }
        once.push_back(sent[i].bytes);
        const bool walking =
            sent[i].bytes[0] == 0x01 && sent[i].bytes[1] % 0x20 == 0;
        if (!walking && i + 1 < sent.size()) {
            EXPECT_FALSE(sent[i + 1].once)
                << "the rotation's turn follows each one-shot";
        }
    }
    const std::vector<Bytes> kDiag = {
        {0x01, 0x01},
        {0x01, 0x41},
        {0x03},
        {0x07},
        {0x0A},
        {0x02, 0x00, 0x00},
        {0x02, 0x02, 0x00},
        {0x02, 0x04, 0x00},
        {0x02, 0x05, 0x00},
        {0x02, 0x0C, 0x00},
        {0x02, 0x0D, 0x00},
    };
    const size_t walk = 6; /* 01 00 to 01 A0 */
    ASSERT_EQ(once.size(), walk + ECUIO_IDENT_COUNT + kDiag.size() + 1);
    for (size_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
        EXPECT_EQ(once[walk + i][0], ECUIO_IDENT_REQUESTS[i].bytes[0]);
    }
    for (size_t i = 0; i < kDiag.size(); ++i) {
        EXPECT_EQ(once[walk + ECUIO_IDENT_COUNT + i], kDiag[i]) << i;
    }
    EXPECT_EQ(once.back(), (Bytes{0x22, 0x1E, 0x20})) << "the scan last";
    EXPECT_EQ(e.count, 1u) << "nothing joins the rotation";

    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(kDiag.size(), fake_diagnostics::kItems);
    EXPECT_EQ(d.generation, kDiag.size()) << "one per item settled";
    fake_diagnostics::expect_report(d);
}

/* An ECU whose bitmaps leave PID 41 out isn't asked it: the item
 * settles unsupported, unsent, and the rest of the report is the
 * fake's */
TEST_F(DiagnosticsTest, APid41TheBitmapsLeaveOutIsNotAsked)
{
    struct Withhold41 {
        Withhold41()
        {
            fake_ecu::withheld_pids()[0x41 / 32] |= 1u << (0x41 % 32);
        }
        ~Withhold41() { fake_ecu::withheld_pids()[0x41 / 32] = 0; }
    } withhold;
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(full_engine(&e));
    int64_t now = 0;
    for (const SentRequest& r : drive(&e, &now, fake_reply)) {
        EXPECT_NE(r.bytes, (Bytes{0x01, 0x41})) << "asked though unsupported";
    }
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.cycle, ECUIO_DIAG_UNSUPPORTED);
    EXPECT_FALSE(d.flags & ECUIO_DIAG_CYCLE);
    EXPECT_TRUE(d.flags & ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.generation, fake_diagnostics::kItems) << "settled once";
    ASSERT_EQ(d.monitor_count, 8u);
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        EXPECT_EQ(d.monitors[i].flags, ECUIO_MONITOR_COMPLETE);
    }
}

/* After a clear: no codes stored, the freeze frame's code 00 00, so no
 * values are asked; the permanent code stays */
TEST_F(DiagnosticsTest, NoFreezeFrameStoredAsksNoValues)
{
    ASSERT_EQ(ecu({0x04}), (Bytes{0x44}));
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    int64_t now = 0;
    const std::vector<SentRequest> sent = drive(&e, &now, fake_reply);
    ASSERT_EQ(sent.size(), 7u);
    EXPECT_EQ(sent.back().bytes, (Bytes{0x02, 0x02, 0x00}));

    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE |
                           ECUIO_DIAG_READINESS | ECUIO_DIAG_CYCLE);
    EXPECT_EQ(d.dtc_count, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].outcome, ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].held, 0u);
    ASSERT_EQ(d.lists[ECUIO_DIAG_PERMANENT].held, 1u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].codes[0], 0x0420u);
    EXPECT_EQ(d.freeze_dtc, 0u);
    EXPECT_EQ(d.freeze_count, 0u);
}

/* Every outcome: refused items keep their code, a silent one is
 * unanswered until a rescan asks it again, and the report is complete
 * only once nothing is unasked */
TEST_F(DiagnosticsTest, RefusalsAndSilenceSettleEachItem)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set()); /* idempotent */
    EXPECT_EQ(e.chans[0].diag_count, ECUIO_DIAG_FIRST_COUNT);

    const Responder refuse = [](const Bytes& req) -> Bytes {
        switch (req[0]) {
        case 0x01:
            return {0x7F, 0x01, 0x12};
        case 0x03:
            return {}; /* silent */
        case 0x07:
        case 0x0A:
            return {0x7F, req[0], 0x11};
        default:
            return {0x7F, 0x02, 0x12};
        }
    };
    int64_t now = 0;
    drive(&e, &now, refuse);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE);
    EXPECT_EQ(d.readiness, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(d.readiness_nrc, 0x12u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].outcome, ECUIO_DIAG_UNANSWERED);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].outcome, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PENDING].nrc, 0x11u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_PERMANENT].outcome, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(d.freeze, ECUIO_DIAG_REFUSED);
    EXPECT_EQ(e.chans[0].diag_count, ECUIO_DIAG_FIRST_COUNT)
        << "a refused freeze frame asks nothing more";

    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.chans[0].diag_pending, 1u) << "only the silent one";
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags & ECUIO_DIAG_COMPLETE, 0u);
    const std::vector<SentRequest> again = drive(&e, &now, fake_reply);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].bytes, (Bytes{0x03}));
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_NE(d.flags & ECUIO_DIAG_COMPLETE, 0u);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held, 3u);
}

/* 43, 47 and 4A carry no key: a reply of the request's service answers
 * it; another service's, or a negative one naming another, doesn't */
TEST_F(DiagnosticsTest, AOneByteRequestIsAnsweredByItsServiceAlone)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    int64_t now = 0;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const Bytes readiness = fake_reply({0x01, 0x01});
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, readiness.data(), 6, now, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const Bytes cycle = fake_reply({0x01, 0x41});
    ASSERT_EQ(ecuio_engine_on_frame(&e, 0, cycle.data(), 6, now, nullptr),
              ecuio_reply::positive);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ASSERT_EQ(e.chans[0].once.len, 1u);
    ASSERT_EQ(e.chans[0].once.bytes[0], 0x03u);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const uint8_t other[] = {0x47, 0x00};
    const uint8_t other_negative[] = {0x7F, 0x07, 0x11};
    const uint8_t pending[] = {0x7F, 0x03, 0x78};
    const uint8_t mine[] = {0x43, 0x01, 0x03, 0x01}; /* one code: P0301 */
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other, sizeof other, now, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, other_negative,
                                    sizeof other_negative, now, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, mine, 0, now, nullptr),
              ecuio_reply::none);
    EXPECT_EQ(
        ecuio_engine_on_frame(&e, 0, pending, sizeof pending, now, nullptr),
        ecuio_reply::pending);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, mine, sizeof mine, now, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].diag.lists[ECUIO_DIAG_STORED].outcome,
              ECUIO_DIAG_ANSWERED);
    EXPECT_EQ(e.chans[0].diag.lists[ECUIO_DIAG_STORED].reported, 1u);
    EXPECT_EQ(e.chans[0].diag.lists[ECUIO_DIAG_STORED].codes[0], 0x0301u);
}

/* Read again starts the list over once a read has settled, with a new
 * generation; mid-read, or on a channel never asked, it does nothing */
TEST_F(DiagnosticsTest, ReadAgainStartsOverOnlyOnceSettled)
{
    static ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    ecuio_engine_diagnose_again(&e);
    EXPECT_EQ(e.chans[0].diag_count, 0u) << "never asked: nothing to repeat";

    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    ecuio_engine_diagnose_again(&e);
    EXPECT_EQ(e.chans[0].diag_pending, ECUIO_DIAG_FIRST_COUNT)
        << "mid-read: left as it is";
    EXPECT_EQ(e.chans[0].diag.generation, 0u);

    int64_t now = 0;
    drive(&e, &now, fake_reply);
    ecuio_channel_diagnostics_t first;
    ecuio_engine_diagnostics(&e, 0, &first);
    ASSERT_NE(first.flags & ECUIO_DIAG_COMPLETE, 0u);

    ecuio_engine_diagnose_again(&e);
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, ECUIO_DIAG_ASKED) << "emptied, and asked again";
    EXPECT_EQ(d.generation, first.generation + 1);
    EXPECT_EQ(d.freeze_count, 0u);
    EXPECT_EQ(e.chans[0].diag_count, ECUIO_DIAG_FIRST_COUNT);
    EXPECT_EQ(e.chans[0].diag_pending, ECUIO_DIAG_FIRST_COUNT);

    drive(&e, &now, fake_reply);
    ecuio_engine_diagnostics(&e, 0, &d);
    EXPECT_EQ(d.flags, first.flags);
    EXPECT_EQ(d.freeze_count, first.freeze_count);
    EXPECT_EQ(d.lists[ECUIO_DIAG_STORED].held,
              first.lists[ECUIO_DIAG_STORED].held);
    EXPECT_GT(d.generation, first.generation);
}

// ---- Trouble-code titles (ADR-0044): Wal33D's generic list, SAE J2012's
// powertrain and network ranges only

namespace {

// J2012's ranges SAE controls, of the letters the table keeps
bool sae_titled_range(uint16_t code)
{
    const unsigned letter = code >> 14;
    const unsigned d1 = (code >> 12) & 3;
    const unsigned d2 = (code >> 8) & 0xF;
    if (letter == 0) { // P
        return d1 == 0 || d1 == 2 || (d1 == 3 && d2 >= 4 && d2 <= 9);
    }
    if (letter == 3) { // U
        return d1 == 0 || d1 == 3;
    }
    return false; // C and B are left out
}

} // namespace

TEST(DtcTitleTest, CommonCodesHaveTheirTitles)
{
    EXPECT_STREQ(ecuio_dtc_title(0x0301), "Cylinder 1 Misfire Detected");
    EXPECT_STREQ(ecuio_dtc_title(0x0420),
                 "Catalyst System Efficiency Below Threshold Bank 1");
    EXPECT_STREQ(ecuio_dtc_title(0x0171), "System Too Lean Bank 1");
    EXPECT_STREQ(ecuio_dtc_title(0xC100), "Lost Communication With ECM/PCM A");
    // An en dash in the source is a hyphen here
    EXPECT_STREQ(ecuio_dtc_title(0x2110),
                 "Throttle Actuator A Control System - Forced Limited RPM");
    EXPECT_EQ(
        std::strncmp(ecuio_dtc_title_source(), "Wal33D/dtc-database ", 20), 0);
}

TEST(DtcTitleTest, OnlySaesPowertrainAndNetworkRangesHaveTitles)
{
    // Every code: a title only in the ranges kept, each plain ASCII text;
    // most of the ranges' codes are listed (J2012 leaves some reserved)
    int titled = 0;
    for (uint32_t c = 0; c <= 0xFFFF; ++c) {
        const char* t = ecuio_dtc_title(static_cast<uint16_t>(c));
        ASSERT_NE(t, nullptr);
        if (*t == '\0') {
            continue;
        }
        ++titled;
        EXPECT_TRUE(sae_titled_range(static_cast<uint16_t>(c)))
            << std::hex << c;
        for (const char* p = t; *p != '\0'; ++p) {
            EXPECT_TRUE(*p >= 32 && *p <= 126) << std::hex << c;
        }
    }
    EXPECT_GT(titled, 8000);
    // The fake ECU's: a manufacturer's code and a chassis code have none
    EXPECT_STREQ(ecuio_dtc_title(0x1234), ""); // P1234
    EXPECT_STREQ(ecuio_dtc_title(0x4035), ""); // C0035
    EXPECT_STREQ(ecuio_dtc_title(0x9001), ""); // B1001
    EXPECT_STREQ(ecuio_dtc_title(0xD000), ""); // U1000
}

TEST(DtcTitleTest, ParseIsFormatsInverse)
{
    for (uint32_t c = 0; c <= 0xFFFF; ++c) {
        char text[8] = {};
        ASSERT_EQ(ecuio_dtc_format(static_cast<uint16_t>(c), text, sizeof text),
                  ECUIO_OK);
        uint16_t back = 0;
        ASSERT_EQ(ecuio_dtc_parse(text, &back), ECUIO_OK) << text;
        EXPECT_EQ(back, c) << text;
    }
}

TEST(DtcTitleTest, ParseRefusesWhatIsntACode)
{
    for (const char* bad : {"p0301", "P4301", "P030", "P03011", "X0301",
                            "P03G1", "P03a1", "", " P0301", "P0301 "}) {
        uint16_t out = 0xABCD;
        EXPECT_EQ(ecuio_dtc_parse(bad, &out), ECUIO_ERR_INVALID_ARGUMENT)
            << bad;
        EXPECT_EQ(out, 0xABCD) << bad;
    }
    uint16_t out = 0;
    EXPECT_EQ(ecuio_dtc_parse(nullptr, &out), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dtc_parse("P0301", nullptr), ECUIO_ERR_NULL_ARGUMENT);
}

namespace {

// A read report whose PID 01 answered: these monitors, the first
// `incomplete` of them incomplete
ecuio_channel_diagnostics_t inspected_report(std::initializer_list<uint8_t> ids,
                                             uint32_t incomplete,
                                             bool mil = false)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
              (mil ? ECUIO_DIAG_MIL_ON : 0u);
    d.readiness = ECUIO_DIAG_ANSWERED;
    for (uint8_t id : ids) {
        ecuio_monitor_t& m = d.monitors[d.monitor_count];
        m.id = id;
        m.flags = d.monitor_count < incomplete ? 0 : ECUIO_MONITOR_COMPLETE;
        ++d.monitor_count;
    }
    return d;
}

ecuio_inspection_t inspect(const std::vector<ecuio_channel_diagnostics_t>& r,
                           uint32_t model_year)
{
    ecuio_inspection_t out{};
    EXPECT_EQ(ecuio_diag_inspect(r.data(), static_cast<uint32_t>(r.size()),
                                 model_year, &out),
              ECUIO_OK);
    return out;
}

} // namespace

// 40 CFR 85.2222(c)(2): two incomplete for 1996-2000, one from 2001
TEST(InspectionTest, TheModelYearSetsHowManyMayBeIncomplete)
{
    const std::vector<ecuio_channel_diagnostics_t> two = {
        inspected_report({8, 10, 13, 0}, 2)};
    const std::vector<ecuio_channel_diagnostics_t> one = {
        inspected_report({8, 10, 13, 0}, 1)};
    EXPECT_EQ(inspect(two, 1996).verdict, ECUIO_INSPECTION_READY);
    EXPECT_EQ(inspect(two, 2000).verdict, ECUIO_INSPECTION_READY);
    EXPECT_EQ(inspect(two, 2000).allowed, 2);
    EXPECT_EQ(inspect(two, 2001).verdict, ECUIO_INSPECTION_NOT_READY);
    EXPECT_EQ(inspect(two, 2001).allowed, 1);
    EXPECT_EQ(inspect(one, 2001).verdict, ECUIO_INSPECTION_READY);
    EXPECT_EQ(inspect(one, 2026).verdict, ECUIO_INSPECTION_READY);
    const std::vector<ecuio_channel_diagnostics_t> three = {
        inspected_report({8, 10, 13, 0}, 3)};
    EXPECT_EQ(inspect(three, 1998).verdict, ECUIO_INSPECTION_NOT_READY);
    const ecuio_inspection_t i = inspect(two, 2001);
    EXPECT_EQ(i.incomplete, 2);
    EXPECT_EQ(i.incomplete_ids, (1u << 8) | (1u << 10));
    EXPECT_EQ(i.reports, 1);
    EXPECT_EQ(i.diag_flags, 0u);
}

// With no model year only two incomplete needs one: one passes both
// rules, three fail both
TEST(InspectionTest, WithoutAModelYearOnlyTwoIncompleteIsUndecided)
{
    for (uint32_t n = 0; n <= 4; ++n) {
        const ecuio_inspection_t i =
            inspect({inspected_report({8, 10, 13, 15}, n)}, 0);
        EXPECT_EQ(i.verdict, n <= 1   ? ECUIO_INSPECTION_READY
                             : n == 2 ? ECUIO_INSPECTION_YEAR_NEEDED
                                      : ECUIO_INSPECTION_NOT_READY)
            << n;
        EXPECT_EQ(i.incomplete, n);
        EXPECT_EQ(i.allowed, 0);
    }
}

// (d)(1): a commanded MIL fails, whatever the monitors say
TEST(InspectionTest, ACommandedMilFails)
{
    EXPECT_EQ(inspect({inspected_report({8, 10}, 0, true)}, 2015).verdict,
              ECUIO_INSPECTION_MIL_ON);
    EXPECT_EQ(inspect({inspected_report({8, 10}, 0, true)}, 0).verdict,
              ECUIO_INSPECTION_MIL_ON);
    // on a second ECU only, it still fails the car
    EXPECT_EQ(
        inspect({inspected_report({8, 10}, 0), inspected_report({2}, 0, true)},
                2015)
            .verdict,
        ECUIO_INSPECTION_MIL_ON);
}

// The car's monitors are every ECU's: one incomplete anywhere counts,
// once however many ECUs have it
TEST(InspectionTest, ReportsCountTogetherAsOneCar)
{
    const ecuio_inspection_t i = inspect(
        {inspected_report({8, 13}, 1), inspected_report({2, 8}, 2)}, 2015);
    EXPECT_EQ(i.incomplete, 2); // catalyst on both, components on the second
    EXPECT_EQ(i.incomplete_ids, (1u << 8) | (1u << 2));
    EXPECT_EQ(i.reports, 2);
    EXPECT_EQ(i.verdict, ECUIO_INSPECTION_NOT_READY);
    // a compression-ignition report names its monitors so
    ecuio_channel_diagnostics_t diesel = inspected_report({9}, 1);
    diesel.flags |= ECUIO_DIAG_COMPRESSION;
    EXPECT_EQ(inspect({diesel}, 2015).diag_flags, ECUIO_DIAG_COMPRESSION);
}

TEST(InspectionTest, ReadingNoAnswerAndOldCarsHaveNoCount)
{
    // a report still being read holds the verdict back
    ecuio_channel_diagnostics_t reading = inspected_report({8}, 0);
    reading.flags &= ~ECUIO_DIAG_COMPLETE;
    EXPECT_EQ(inspect({inspected_report({8}, 0), reading}, 2015).verdict,
              ECUIO_INSPECTION_READING);
    // one never asked (?diagnose=0) doesn't
    ecuio_channel_diagnostics_t unasked{};
    EXPECT_EQ(inspect({inspected_report({8}, 0), unasked}, 2015).verdict,
              ECUIO_INSPECTION_READY);
    // no PID 01 answer from any ECU
    ecuio_channel_diagnostics_t refused{};
    refused.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE;
    refused.readiness = ECUIO_DIAG_REFUSED;
    EXPECT_EQ(inspect({refused}, 2015).verdict, ECUIO_INSPECTION_UNKNOWN);
    EXPECT_EQ(inspect({}, 2015).verdict, ECUIO_INSPECTION_UNKNOWN);
    // before 1996 the rule says nothing, even with the MIL on
    EXPECT_EQ(inspect({inspected_report({8}, 0, true)}, 1995).verdict,
              ECUIO_INSPECTION_NOT_COVERED);
    ecuio_inspection_t out{};
    EXPECT_EQ(ecuio_diag_inspect(nullptr, 1, 2015, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_diag_inspect(nullptr, 0, 2015, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_diag_inspect(nullptr, 0, 2015, &out), ECUIO_OK);
    EXPECT_EQ(out.verdict, ECUIO_INSPECTION_UNKNOWN);
}
