// File/layer: tests/test_bridge_diagnostics.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest.Channel*Text, Diagnostics*, InspectionEntry* (test_bridge)
//
// Cases for Diagnostics: identity, codes, the clear on the mock.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <cstring>
#include <initializer_list>

#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

/* The identity line (ADR-0021): each answered item by name, the refused
 * requests with their codes, the unanswered count; a request outside the
 * list is named by its bytes, an empty answer says so. */
TEST(BridgeTest, ChannelIdentityTextNamesWhatTheEcuSaid)
{
    const auto item = [](std::initializer_list<uint8_t> request, uint8_t outcome,
                         uint8_t nrc, const char* text) {
        ecuio_identity_item_t it{};
        uint8_t k = 0;
        for (uint8_t b : request) {
            it.request[k++] = b;
        }
        it.length = k;
        it.outcome = outcome;
        it.nrc = nrc;
        it.text_length = static_cast<uint8_t>(strlen(text));
        memcpy(it.text, text, it.text_length);
        return it;
    };

    ecuio_channel_identity_t car{};
    car.flags = ECUIO_IDENTITY_COMPLETE;
    car.rx_id = 0x7E8;
    car.items = 5;
    car.list[0] = item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED, 0, "FAKEJ2534VIN00001");
    car.list[1] = item({0x09, 0x04}, ECUIO_IDENTITY_ANSWERED, 0, "CAL-A, CAL-B");
    car.list[2] = item({0x22, 0xF1, 0x94}, ECUIO_IDENTITY_REFUSED, 0x31, "");
    car.list[3] = item({0x09, 0x0A}, ECUIO_IDENTITY_UNANSWERED, 0, "");
    car.list[4] = item({0x22, 0xF1, 0x8C}, ECUIO_IDENTITY_REFUSED, 0x33, "");
    EXPECT_EQ(SessionBridge::channelIdentityText(car).toStdString(),
              "ECU 0x7E8 identity: VIN FAKEJ2534VIN00001; CALID CAL-A, CAL-B; "
              "2 refused: 22 F1 94 (7F 22 31), 22 F1 8C (7F 22 33); 1 unanswered");

    ecuio_channel_identity_t silent{};
    silent.flags = ECUIO_IDENTITY_COMPLETE;
    silent.rx_id = 0x18DAF110;
    silent.items = 2;
    silent.list[0] = item({0x09, 0x02}, ECUIO_IDENTITY_UNANSWERED, 0, "");
    silent.list[1] = item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_UNANSWERED, 0, "");
    EXPECT_EQ(SessionBridge::channelIdentityText(silent).toStdString(),
              "ECU 0x18DAF110 identity: 2 unanswered");

    ecuio_channel_identity_t odd{};
    odd.rx_id = 0x7E9;
    odd.items = 2;
    odd.list[0] = item({0x22, 0x12, 0x34}, ECUIO_IDENTITY_ANSWERED, 0, "");
    odd.list[1] = item({0x22, 0xF1, 0x91}, ECUIO_IDENTITY_UNASKED, 0, "");
    EXPECT_EQ(SessionBridge::channelIdentityText(odd).toStdString(),
              "ECU 0x7E9 identity: 22 12 34 (empty)");

    ecuio_channel_identity_t none{};
    none.rx_id = 0x7E8;
    EXPECT_EQ(SessionBridge::channelIdentityText(none).toStdString(),
              "ECU 0x7E8 identity: nothing asked");
}

/* The mock's one ECU reports the fake ECU's diagnostics, so the line
 * the activity log and a recording's events.log keep is the same on
 * every platform: logged once, after the Session line */
TEST(BridgeTest, ConnectLogsTheMocksDiagnostics)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    const auto lines = [&log]() {
        return log.filter(QStringLiteral(" diagnostics: "));
    };
    ASSERT_TRUE(spinUntil([&]() { return !lines().isEmpty(); }, 2000))
        << "no diagnostics line in the log";
    spinLoop(300);
    ASSERT_EQ(lines().size(), 1) << lines().join('\n').toStdString();
    EXPECT_EQ(lines().first().toStdString(), kFakeEcuDiagnosticsLine);
    EXPECT_GT(
        log.indexOf(lines().first()),
        log.indexOf(QStringLiteral("Session: mock transport; profile mock")));
    ASSERT_TRUE(disconnectAndWait(bridge));

    /* ?diagnose=0: no line */
    log.clear();
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo?diagnose=0"))
                    .isEmpty());
    spinLoop(300);
    EXPECT_TRUE(lines().isEmpty()) << lines().join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The diagnostics line's wording: each list as answered, cut, refused
 * or unanswered; readiness with the incomplete monitors by their short
 * names, a compression engine's by its own; the freeze frame's code or
 * its absence; nothing asked */
TEST(BridgeTest, ChannelDiagnosticsTextNamesWhatTheEcuReported)
{
    const auto monitor = [](uint8_t id, bool complete) {
        ecuio_monitor_t m{};
        m.id = id;
        m.flags = complete ? ECUIO_MONITOR_COMPLETE : 0;
        return m;
    };

    ecuio_channel_diagnostics_t older{};
    older.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS;
    older.rx_id = 0x7E8;
    older.readiness = ECUIO_DIAG_ANSWERED;
    older.monitor_count = 4;
    older.monitors[0] = monitor(0, true);
    older.monitors[1] = monitor(8, false);
    older.monitors[2] = monitor(10, false);
    older.monitors[3] = monitor(14, true);
    older.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    older.lists[ECUIO_DIAG_PENDING].outcome = ECUIO_DIAG_ANSWERED;
    older.lists[ECUIO_DIAG_PENDING].reported = 5;
    older.lists[ECUIO_DIAG_PENDING].held = 2;
    older.lists[ECUIO_DIAG_PENDING].codes[0] = 0x0171;
    older.lists[ECUIO_DIAG_PENDING].codes[1] = 0x4035;
    older.lists[ECUIO_DIAG_PERMANENT].outcome = ECUIO_DIAG_REFUSED;
    older.lists[ECUIO_DIAG_PERMANENT].nrc = 0x11;
    older.freeze = ECUIO_DIAG_ANSWERED;
    EXPECT_EQ(
        SessionBridge::channelDiagnosticsText(older).toStdString(),
        "ECU 0x7E8 diagnostics: MIL off; stored none; pending P0171 C0035 "
        "and 3 more; permanent refused (7F 0A 11); readiness 2 of 4 "
        "complete (catalyst, EVAP incomplete); no freeze frame");

    ecuio_channel_diagnostics_t diesel{};
    diesel.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE |
                   ECUIO_DIAG_READINESS | ECUIO_DIAG_MIL_ON |
                   ECUIO_DIAG_COMPRESSION | ECUIO_DIAG_FREEZE_FRAME;
    diesel.rx_id = 0x7E8;
    diesel.readiness = ECUIO_DIAG_ANSWERED;
    diesel.monitor_count = 2;
    diesel.monitors[0] = monitor(8, false);
    diesel.monitors[1] = monitor(14, false);
    diesel.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    diesel.lists[ECUIO_DIAG_STORED].reported = 1;
    diesel.lists[ECUIO_DIAG_STORED].held = 1;
    diesel.lists[ECUIO_DIAG_STORED].codes[0] = 0x2002;
    diesel.freeze = ECUIO_DIAG_ANSWERED;
    diesel.freeze_dtc = 0x2002;
    EXPECT_EQ(
        SessionBridge::channelDiagnosticsText(diesel).toStdString(),
        "ECU 0x7E8 diagnostics: MIL on; stored P2002; readiness 0 of 2 "
        "complete (NMHC catalyst, PM filter incomplete); freeze frame for "
        "P2002");

    ecuio_channel_diagnostics_t silent{};
    silent.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE;
    silent.rx_id = 0x18DAF110;
    silent.readiness = ECUIO_DIAG_UNANSWERED;
    for (ecuio_dtc_list_t& l : silent.lists) {
        l.outcome = ECUIO_DIAG_UNANSWERED;
    }
    silent.freeze = ECUIO_DIAG_REFUSED;
    silent.freeze_nrc = 0x12;
    EXPECT_EQ(SessionBridge::channelDiagnosticsText(silent).toStdString(),
              "ECU 0x18DAF110 diagnostics: stored unanswered; pending "
              "unanswered; permanent unanswered; readiness unanswered; freeze "
              "frame refused (7F 02 12)");

    ecuio_channel_diagnostics_t none{};
    none.rx_id = 0x7E8;
    EXPECT_EQ(SessionBridge::channelDiagnosticsText(none).toStdString(),
              "ECU 0x7E8 diagnostics: nothing asked");
}

/* The page's entry for an ECU (ADR-0037): the mock's report, named and
 * converted as the Diagnostics page shows it, in either unit system */
TEST(BridgeTest, DiagnosticsEntryIsWhatThePageShows)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    ecuio_session_destroy(s);

    const QVariantMap e =
        SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("ecu").toString(), QStringLiteral("0x7E8"));
    EXPECT_TRUE(e.value("asked").toBool());
    EXPECT_TRUE(e.value("complete").toBool());
    EXPECT_EQ(e.value("readiness").toString(), QStringLiteral("answered"));
    EXPECT_TRUE(e.value("mil").toBool());
    EXPECT_EQ(e.value("dtcCount").toInt(), 3);
    EXPECT_FALSE(e.value("compression").toBool());
    EXPECT_EQ(e.value("cycle").toString(), QStringLiteral("answered"));

    const QVariantList lists = e.value("lists").toList();
    ASSERT_EQ(lists.size(), 3);
    const QStringList keys = {"stored", "pending", "permanent"};
    const QList<QStringList> codes = {
        {"P0301", "P0420", "U0100"}, {"P0171", "C0035", "B1001"}, {"P0420"}};
    for (int l = 0; l < 3; ++l) {
        const QVariantMap list = lists.at(l).toMap();
        EXPECT_EQ(list.value("key").toString(), keys.at(l));
        EXPECT_EQ(list.value("outcome").toString(), QStringLiteral("answered"));
        EXPECT_EQ(list.value("codes").toStringList(), codes.at(l));
        // Each code's title beside it, "" with none (ADR-0044)
        const QStringList listed = list.value("titles").toStringList();
        ASSERT_EQ(listed.size(), codes.at(l).size());
        for (int i = 0; i < listed.size(); ++i) {
            uint16_t raw = 0;
            ASSERT_EQ(
                ecuio_dtc_parse(codes.at(l).at(i).toLatin1().constData(), &raw),
                ECUIO_OK);
            EXPECT_EQ(listed.at(i), QString::fromLatin1(ecuio_dtc_title(raw)));
        }
        EXPECT_EQ(list.value("more").toInt(), 0);
    }

    const QVariantList monitors = e.value("monitors").toList();
    QStringList names;
    for (const QVariant& m : monitors) {
        names << m.toMap().value("name").toString();
        EXPECT_TRUE(m.toMap().value("complete").toBool());
        EXPECT_TRUE(m.toMap().value("cycleEnabled").toBool());
        EXPECT_TRUE(m.toMap().value("cycleComplete").toBool());
    }
    EXPECT_EQ(names,
              (QStringList{"Misfire", "Fuel system", "Comprehensive components",
                           "Catalyst", "Evaporative system", "Oxygen sensor",
                           "Oxygen sensor heater", "EGR and/or VVT system"}));

    const QVariantMap freeze = e.value("freeze").toMap();
    EXPECT_EQ(freeze.value("outcome").toString(), QStringLiteral("answered"));
    EXPECT_TRUE(freeze.value("stored").toBool());
    EXPECT_EQ(freeze.value("code").toString(), QStringLiteral("P0301"));
    const auto value = [](const QVariantMap& f, int i) {
        return f.value("values").toList().at(i).toMap();
    };
    ASSERT_EQ(freeze.value("values").toList().size(), 4);
    EXPECT_EQ(value(freeze, 0).value("name").toString(),
              QStringLiteral("Engine Load"));
    EXPECT_NEAR(value(freeze, 0).value("value").toDouble(), 0xB3 * 100.0 / 255,
                1e-9);
    EXPECT_EQ(value(freeze, 0).value("decimals").toInt(), 1);
    EXPECT_EQ(value(freeze, 1).value("name").toString(),
              QStringLiteral("Coolant Temp"));
    EXPECT_EQ(value(freeze, 1).value("value").toDouble(), 90.0);
    EXPECT_EQ(value(freeze, 1).value("unit").toString(),
              QString::fromUtf8("\xC2\xB0"
                                "C"));
    EXPECT_EQ(value(freeze, 2).value("name").toString(), QStringLiteral("RPM"));
    EXPECT_EQ(value(freeze, 2).value("value").toDouble(), 3000.0);
    EXPECT_EQ(value(freeze, 3).value("name").toString(),
              QStringLiteral("Vehicle Speed"));
    EXPECT_EQ(value(freeze, 3).value("unit").toString(),
              QStringLiteral("km/h"));

    /* The same report in imperial units: the values convert, as Live's do */
    const QVariantMap us =
        SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_IMPERIAL)
            .value("freeze")
            .toMap();
    EXPECT_EQ(value(us, 1).value("unit").toString(),
              QString::fromUtf8("\xC2\xB0"
                                "F"));
    EXPECT_NEAR(value(us, 1).value("value").toDouble(), 194.0, 1e-9);
    EXPECT_EQ(value(us, 3).value("unit").toString(), QStringLiteral("mph"));
    EXPECT_NEAR(value(us, 3).value("value").toDouble(), 80.0 / 1.609344, 1e-9);

    /* Refusals carry their reply; an unasked report says so */
    ecuio_channel_diagnostics_t old{};
    old.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE;
    old.rx_id = 0x7E9;
    old.readiness = ECUIO_DIAG_REFUSED;
    old.readiness_nrc = 0x12;
    old.lists[ECUIO_DIAG_PERMANENT].outcome = ECUIO_DIAG_REFUSED;
    old.lists[ECUIO_DIAG_PERMANENT].nrc = 0x11;
    old.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    old.lists[ECUIO_DIAG_STORED].reported = 40;
    old.lists[ECUIO_DIAG_STORED].held = 1;
    old.lists[ECUIO_DIAG_STORED].codes[0] = 0xC100;
    old.freeze = ECUIO_DIAG_UNANSWERED;
    const QVariantMap o =
        SessionBridge::diagnosticsEntry(old, ECUIO_UNITS_METRIC);
    EXPECT_EQ(o.value("ecu").toString(), QStringLiteral("0x7E9"));
    EXPECT_EQ(o.value("readiness").toString(), QStringLiteral("refused"));
    EXPECT_EQ(o.value("readinessNrc").toString(), QStringLiteral("7F 01 12"));
    EXPECT_EQ(o.value("lists").toList().at(0).toMap().value("more").toInt(),
              39);
    EXPECT_EQ(
        o.value("lists").toList().at(1).toMap().value("outcome").toString(),
        QStringLiteral("unasked"));
    EXPECT_EQ(o.value("lists").toList().at(2).toMap().value("nrc").toString(),
              QStringLiteral("7F 0A 11"));
    EXPECT_EQ(o.value("freeze").toMap().value("outcome").toString(),
              QStringLiteral("unanswered"));
    EXPECT_FALSE(SessionBridge::diagnosticsEntry(ecuio_channel_diagnostics_t{},
                                                 ECUIO_UNITS_METRIC)
                     .value("asked")
                     .toBool());
}

/* Bridge.diagnostics follows the session: the mock's report after the
 * first tick, converted again when the unit system changes, kept after
 * disconnect, emptied at the next connect, and Read again moves it on */
TEST(BridgeTest, DiagnosticsFollowTheSession)
{
    SessionBridge bridge;
    bridge.setUnitSystem(ECUIO_UNITS_METRIC);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    EXPECT_TRUE(bridge.diagnostics().isEmpty());
    bridge.readDiagnostics(); // no session: nothing happens
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    const auto speedUnit = [&bridge]() {
        return bridge.diagnostics()
            .at(0)
            .toMap()
            .value("freeze")
            .toMap()
            .value("values")
            .toList()
            .at(3)
            .toMap()
            .value("unit")
            .toString();
    };
    EXPECT_EQ(speedUnit(), QStringLiteral("km/h"));
    // the mock's light is on: the car's verdict (ADR-0045)
    EXPECT_EQ(bridge.inspection().value("verdict").toString(),
              QStringLiteral("mil_on"));
    bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(speedUnit(), QStringLiteral("mph"));
    bridge.setUnitSystem(ECUIO_UNITS_METRIC);

    const auto lines = [&log]() {
        return log.filter(QStringLiteral(" diagnostics: "));
    };
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 1; }, 2000));
    bridge.readDiagnostics();
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 2; }, 2000))
        << "Read again logs a second read";

    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.diagnostics().size(), 1) << "the last read, kept";
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo?diagnose=0"))
                    .isEmpty());
    EXPECT_TRUE(bridge.diagnostics().isEmpty()) << "emptied at connect";
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    EXPECT_FALSE(bridge.diagnostics().at(0).toMap().value("asked").toBool());
    EXPECT_TRUE(bridge.inspection().value("verdict").toString().isEmpty())
        << "nothing read, nothing judged";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A clear's line for each outcome (ADR-0038), as the activity log
 * and a recording's events.log keep it, a speed in the unit system */
TEST(BridgeTest, ChannelClearTextNamesEachOutcome)
{
    ecuio_channel_diagnostics_t d{};
    d.rx_id = 0x7E8;
    const auto text = [&d](uint8_t clear, uint8_t nrc, uint32_t rpm) {
        d.clear = clear;
        d.clear_nrc = nrc;
        d.clear_rpm = rpm;
        return SessionBridge::channelClearText(d, ECUIO_UNITS_METRIC)
            .toStdString();
    };
    const auto imperial = [&d](uint8_t clear, uint32_t rpm) {
        d.clear = clear;
        d.clear_nrc = 0;
        d.clear_rpm = rpm;
        return SessionBridge::channelClearText(d, ECUIO_UNITS_IMPERIAL)
            .toStdString();
    };
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 0), "ECU 0x7E8 clear: cleared");
    EXPECT_EQ(text(ECUIO_CLEAR_REFUSED, 0x22, 0),
              "ECU 0x7E8 clear: refused (7F 04 22)");
    EXPECT_EQ(text(ECUIO_CLEAR_UNANSWERED, 0, 0), "ECU 0x7E8 clear: no answer");
    EXPECT_EQ(text(ECUIO_CLEAR_ENGINE_RUNNING, 0, 812),
              "ECU 0x7E8 clear: not sent: engine running (812 rpm)");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_ENGINE_SPEED, 0, 0),
              "ECU 0x7E8 clear: not sent: no engine speed");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_ENGINE_SPEED, 0x12, 0),
              "ECU 0x7E8 clear: not sent: no engine speed (7F 01 12)");
    EXPECT_EQ(text(ECUIO_CLEAR_REPORT_CHANGED, 0, 0),
              "ECU 0x7E8 clear: not sent: the report changed");
    EXPECT_EQ(text(ECUIO_CLEAR_NONE, 0, 0), "ECU 0x7E8 clear: not asked");

    /* Under the user's word (ADR-0038's appendix of 2026-11-01): what was
     * read, the vehicle's speed, and why an ECU refused a running engine */
    d.clear_kmh = 48;
    EXPECT_EQ(text(ECUIO_CLEAR_MOVING, 0, 2000),
              "ECU 0x7E8 clear: not sent: moving (48 km/h)");
    EXPECT_EQ(imperial(ECUIO_CLEAR_MOVING, 2000),
              "ECU 0x7E8 clear: not sent: moving (30 mph)");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_VEHICLE_SPEED, 0, 0),
              "ECU 0x7E8 clear: not sent: no vehicle speed");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_VEHICLE_SPEED, 0x12, 0),
              "ECU 0x7E8 clear: not sent: no vehicle speed (7F 01 12)");
    d.flags = ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH;
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 812),
              "ECU 0x7E8 clear: cleared (812 rpm, 48 km/h)");
    EXPECT_EQ(imperial(ECUIO_CLEAR_CLEARED, 812),
              "ECU 0x7E8 clear: cleared (812 rpm, 30 mph)");
    EXPECT_EQ(text(ECUIO_CLEAR_REFUSED, 0x22, 812),
              "ECU 0x7E8 clear: refused (7F 04 22): this ECU won't clear with "
              "the engine running: stop and turn it off, then try again");
    EXPECT_EQ(text(ECUIO_CLEAR_REFUSED, 0x22, 0),
              "ECU 0x7E8 clear: refused (7F 04 22)")
        << "at 0 rpm, not the engine";
    d.flags = ECUIO_DIAG_CLEAR_KMH;
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 0),
              "ECU 0x7E8 clear: cleared (engine speed unknown, 48 km/h)");
    d.flags = ECUIO_DIAG_CLEAR_RPM;
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 0), "ECU 0x7E8 clear: cleared")
        << "no vehicle speed asked: the stopped engine's clear";
    d.flags = 0;
    d.clear_kmh = 0;

    /* The page's entry carries the same */
    d.clear = ECUIO_CLEAR_REFUSED;
    d.clear_nrc = 0x22;
    QVariantMap e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("refused"));
    EXPECT_EQ(e.value("clearNrc").toString(), QStringLiteral("7F 04 22"));
    EXPECT_FALSE(e.value("clearing").toBool());
    d.clear = ECUIO_CLEAR_ENGINE_RUNNING;
    d.clear_nrc = 0;
    d.clear_rpm = 812;
    d.flags = ECUIO_DIAG_CLEARING;
    e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("engineRunning"));
    EXPECT_EQ(e.value("clearRpm").toInt(), 812);
    EXPECT_TRUE(e.value("clearing").toBool());
    EXPECT_FALSE(e.value("clearRunning").toBool()) << "no reading flagged";
    EXPECT_EQ(e.value("clearSpeed").toString(), QString());
    d.clear = ECUIO_CLEAR_MOVING;
    d.clear_kmh = 48;
    d.flags = ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH;
    e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("moving"));
    EXPECT_TRUE(e.value("clearRunning").toBool());
    EXPECT_EQ(e.value("clearSpeed").toString(), QStringLiteral("30 mph"));
    d.clear = ECUIO_CLEAR_NO_VEHICLE_SPEED;
    d.clear_nrc = 0x12;
    e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("noVehicleSpeed"));
    EXPECT_EQ(e.value("clearNrc").toString(), QStringLiteral("7F 01 12"));
}

/* The saved file is havne-cli's: a start line, then each report as its
 * diagnostics line, here the mock's before any clear */
TEST(BridgeTest, DiagnosticsFileTextIsTheCliLines)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://file", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    ecuio_session_destroy(s);

    const QList<QByteArray> lines =
        SessionBridge::diagnosticsFileText(&d, 1).split('\n');
    ASSERT_EQ(lines.size(), 3) << "two lines, each ended";
    EXPECT_TRUE(lines.at(2).isEmpty());
    const QJsonObject start = QJsonDocument::fromJson(lines.at(0)).object();
    EXPECT_EQ(start.value("type").toString(), QStringLiteral("start"));
    EXPECT_EQ(start.value("schema").toInt(), 1);
    EXPECT_EQ(start.value("core").toString(),
              QString::fromUtf8(ecuio_version_string()));
    const QJsonObject line = QJsonDocument::fromJson(lines.at(1)).object();
    EXPECT_EQ(line.value("type").toString(), QStringLiteral("diagnostics"));
    EXPECT_EQ(line.value("rx").toString(), QStringLiteral("0x7E8"));
    EXPECT_EQ(line.value("generation").toInt(), static_cast<int>(d.generation));
    EXPECT_TRUE(line.value("readiness").toObject().value("mil").toBool());
    const QJsonArray stored =
        line.value("stored").toObject().value("codes").toArray();
    ASSERT_EQ(stored.size(), 3);
    EXPECT_EQ(stored.at(0).toString(), QStringLiteral("P0301"));
    EXPECT_EQ(line.value("freeze").toObject().value("code").toString(),
              QStringLiteral("P0301"));
}

/* ADR-0038 on the mock, which starts each loop with its engine stopped:
 * the report goes to a file of its own first (no vehicle in a mock's
 * name), then the clear, its line, and the read after it, the only
 * other diagnostics line */
/* ADR-0038: the one rule for a clear's ECUs, which the confirmation
 * names (clearTargets) and clearDiagnostics sends to: a completed read,
 * no clear under way */
TEST(BridgeTest, AClearGoesToCompletedReportsNotClearing)
{
    ecuio_channel_diagnostics_t r{};
    EXPECT_FALSE(SessionBridge::clearTarget(r)) << "a read under way";
    r.flags = ECUIO_DIAG_COMPLETE;
    EXPECT_TRUE(SessionBridge::clearTarget(r));
    r.flags = ECUIO_DIAG_COMPLETE | ECUIO_DIAG_CLEARING;
    EXPECT_FALSE(SessionBridge::clearTarget(r)) << "a clear under way";
    r.flags = ECUIO_DIAG_CLEARING;
    EXPECT_FALSE(SessionBridge::clearTarget(r));

    SessionBridge bridge;
    EXPECT_TRUE(bridge.clearTargets().isEmpty()) << "no session, no report";
}

/* A clear with nothing to erase still goes out (a reset after a repair
 * is a use), and the confirmation says so: only what the ECU answered
 * counts as empty */
TEST(BridgeTest, NothingStoredIsWhatTheEcuAnsweredEmpty)
{
    ecuio_channel_diagnostics_t r{};
    r.flags = ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS;
    r.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    r.lists[ECUIO_DIAG_PENDING].outcome = ECUIO_DIAG_ANSWERED;
    r.lists[ECUIO_DIAG_PERMANENT].outcome = ECUIO_DIAG_ANSWERED;
    r.lists[ECUIO_DIAG_PERMANENT].reported = 1;
    r.lists[ECUIO_DIAG_PERMANENT].held = 1;
    EXPECT_TRUE(SessionBridge::nothingStored(r))
        << "a permanent code isn't a clear's to erase";

    const auto with = [&r](auto change) {
        ecuio_channel_diagnostics_t c = r;
        change(c);
        return SessionBridge::nothingStored(c);
    };
    EXPECT_FALSE(with([](auto& c) { c.flags |= ECUIO_DIAG_MIL_ON; }));
    EXPECT_FALSE(with([](auto& c) { c.dtc_count = 1; }));
    EXPECT_FALSE(with([](auto& c) { c.flags |= ECUIO_DIAG_FREEZE_FRAME; }));
    EXPECT_FALSE(
        with([](auto& c) { c.lists[ECUIO_DIAG_STORED].reported = 2; }));
    EXPECT_FALSE(with([](auto& c) { c.lists[ECUIO_DIAG_PENDING].held = 1; }));
    EXPECT_FALSE(with([](auto& c) { c.flags &= ~ECUIO_DIAG_READINESS; }))
        << "PID 01 unanswered: the light unknown";
    EXPECT_FALSE(with([](auto& c) {
        c.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_REFUSED;
    })) << "a refused list isn't known to be empty";
    EXPECT_FALSE(with([](auto& c) {
        c.lists[ECUIO_DIAG_PENDING].outcome = ECUIO_DIAG_UNANSWERED;
    }));

    SessionBridge bridge;
    EXPECT_FALSE(bridge.clearFindsNothing()) << "no report: nothing known";
}

TEST(BridgeTest, AClearSavesTheReportFirst)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setVehicleLabel(QStringLiteral("Daily driver"));
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "));
    };
    const auto clear_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 clear: "));
    };
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 2000));

    const QString shown = bridge.clearDiagnostics();
    const QStringList files =
        QDir(root.path()).entryList({QStringLiteral("*.jsonl")}, QDir::Files);
    ASSERT_EQ(files.size(), 1) << files.join(',').toStdString();
    EXPECT_TRUE(QRegularExpression(
                    QStringLiteral("^diagnostics-[0-9]{8}-[0-9]{6}\\.jsonl$"))
                    .match(files.first())
                    .hasMatch())
        << files.first().toStdString();
    EXPECT_TRUE(shown.endsWith(files.first())) << shown.toStdString();
    QFile saved(root.path() + QLatin1Char('/') + files.first());
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = saved.readAll().split('\n');
    ASSERT_GE(lines.size(), 2);
    EXPECT_TRUE(
        lines.at(1).contains("\"codes\":[\"P0301\",\"P0420\",\"U0100\"]"))
        << "the codes before the clear: " << lines.at(1).toStdString();

    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 2000))
        << log.join('\n').toStdString();
    spinLoop(300);
    ASSERT_EQ(clear_lines().size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(clear_lines().first().toStdString(),
              "ECU 0x7E8 clear: cleared (0 rpm, 0 km/h)")
        << "the mock's key-on: engine stopped, car standing";
    EXPECT_EQ(diagnostics_lines().size(), 2);
    const QString after = diagnostics_lines().at(1);
    EXPECT_TRUE(after.contains(
        QStringLiteral("MIL off; stored none; pending none; permanent P0420")))
        << after.toStdString();
    const QStringList saved_lines =
        log.filter(QStringLiteral("Diagnostics saved before the clear: "));
    ASSERT_EQ(saved_lines.size(), 1) << log.join('\n').toStdString();
    EXPECT_TRUE(saved_lines.first().endsWith(shown));
    EXPECT_LT(log.indexOf(saved_lines.first()),
              log.indexOf(clear_lines().first()));
    EXPECT_LT(log.indexOf(clear_lines().first()), log.indexOf(after));
    EXPECT_EQ(bridge.diagnostics().at(0).toMap().value("clear").toString(),
              QStringLiteral("cleared"));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.clearDiagnostics().isEmpty()) << "no session";
}

/* A report that can't be saved sends no clear: the root is under a
 * file, so no folder can be made */
TEST(BridgeTest, AFailedSaveSendsNoClear)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString file = root.path() + QStringLiteral("/not-a-folder");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(file + QStringLiteral("/recordings"));
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    EXPECT_TRUE(bridge.clearDiagnostics().isEmpty());
    EXPECT_TRUE(log.isEmpty()) << "no session: nothing to say";
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    EXPECT_TRUE(bridge.clearDiagnostics().isEmpty());
    spinLoop(300);
    EXPECT_EQ(log.filter(QStringLiteral(
                             "Clear not sent: the report couldn't be saved "
                             "(can't create "))
                  .size(),
              1)
        << log.join('\n').toStdString();
    EXPECT_TRUE(log.filter(QStringLiteral(" clear: ")).isEmpty());
    EXPECT_EQ(log.filter(QStringLiteral(" diagnostics: ")).size(), 1);
    EXPECT_EQ(bridge.diagnostics().at(0).toMap().value("clear").toString(),
              QStringLiteral("none"));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* ADR-0044: Diagnostics' lookup of a typed code: spaces and lower case
 * taken, a title, a code with none, not a code */
namespace {

// A read report whose PID 01 answered, the first `incomplete` of the
// monitors incomplete
ecuio_channel_diagnostics_t readinessReport(std::initializer_list<uint8_t> ids,
                                            uint32_t incomplete,
                                            bool mil = false)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
              (mil ? ECUIO_DIAG_MIL_ON : 0u);
    d.readiness = ECUIO_DIAG_ANSWERED;
    for (uint8_t id : ids) {
        d.monitors[d.monitor_count].id = id;
        d.monitors[d.monitor_count].flags =
            d.monitor_count < incomplete ? 0 : ECUIO_MONITOR_COMPLETE;
        ++d.monitor_count;
    }
    return d;
}

} // namespace

/* The inspection line as Diagnostics shows it (ADR-0045): each verdict's
 * word, as havne-cli writes it, and its sentence */
TEST(BridgeTest, InspectionEntryIsWhatThePageShows)
{
    const auto entry = [](const ecuio_channel_diagnostics_t& d, uint32_t year) {
        return SessionBridge::inspectionEntry(&d, 1, year);
    };
    QVariantMap m = entry(readinessReport({8, 10, 13}, 0), 2015);
    EXPECT_EQ(m.value("verdict").toString(), QStringLiteral("ready"));
    EXPECT_EQ(m.value("text").toString(),
              QStringLiteral("Ready for an emissions inspection: every monitor "
                             "complete, the check-engine light off."));
    EXPECT_TRUE(
        m.value("rule").toString().contains(QStringLiteral("40 CFR 85.2222")));
    m = entry(readinessReport({10, 8, 13}, 1), 2015);
    EXPECT_EQ(m.value("text").toString(),
              QStringLiteral("Ready for an emissions inspection: 1 monitor "
                             "incomplete (EVAP), which a model year 2015 car "
                             "may have."));
    m = entry(readinessReport({8, 10, 13}, 2), 2015);
    EXPECT_EQ(m.value("verdict").toString(), QStringLiteral("not_ready"));
    EXPECT_EQ(
        m.value("text").toString(),
        QStringLiteral("Not ready for an emissions inspection: 2 monitors "
                       "incomplete (catalyst, EVAP), and a model year "
                       "2015 car may have 1 at most. A station would "
                       "send it away to be driven until they complete."));
    EXPECT_EQ(entry(readinessReport({8, 10, 13}, 2), 1999)
                  .value("verdict")
                  .toString(),
              QStringLiteral("ready"));
    m = entry(readinessReport({8, 10, 13}, 2), 0);
    EXPECT_EQ(m.value("verdict").toString(), QStringLiteral("year_needed"));
    EXPECT_EQ(
        m.value("text").toString(),
        QStringLiteral("2 monitors incomplete (catalyst, EVAP): ready for "
                       "an emissions inspection if the car is model year "
                       "1996-2000, not if it's 2001 or newer, and no VIN "
                       "gave its year."));
    EXPECT_EQ(
        entry(readinessReport({8}, 0, true), 2015).value("verdict").toString(),
        QStringLiteral("mil_on"));
    EXPECT_EQ(entry(readinessReport({8}, 0), 1995).value("verdict").toString(),
              QStringLiteral("not_covered"));
    ecuio_channel_diagnostics_t reading = readinessReport({8}, 0);
    reading.flags &= ~ECUIO_DIAG_COMPLETE;
    EXPECT_TRUE(entry(reading, 2015).value("verdict").toString().isEmpty());
    EXPECT_TRUE(SessionBridge::inspectionEntry(nullptr, 0, 2015)
                    .value("text")
                    .toString()
                    .isEmpty());
}

TEST(BridgeTest, ATypedCodeIsDescribed)
{
    SessionBridge bridge;
    EXPECT_EQ(bridge.describeDtc(QStringLiteral(" p0 301 ")),
              QStringLiteral("P0301: Cylinder 1 Misfire Detected"));
    EXPECT_TRUE(bridge.describeDtc(QStringLiteral("C0035"))
                    .startsWith(QStringLiteral("C0035: no title here")));
    EXPECT_TRUE(bridge.describeDtc(QStringLiteral("P0301X"))
                    .startsWith(QStringLiteral("not a trouble code")));
    EXPECT_TRUE(bridge.describeDtc(QStringLiteral("   ")).isEmpty());
}
