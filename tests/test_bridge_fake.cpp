// File/layer: tests/test_bridge_fake.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Cases for sessions against the fake J2534 DLL: async open, teardown, VIN, the clear (one request over the ECUs too), withheld PIDs, the default pick.

#include "test_bridge_fake_support.h"
#include "worker_hold.h" // the worker parked in a frame

#include <functional>

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Connect and disconnect threads, against the fake J2534 DLL: its delay
// knobs stall the vendor call, so a call made on the GUI thread would
// block it, and its control page counts the teardown's calls.
// ---------------------------------------------------------------------------

/* An async connect makes the slow vendor open on its own thread: with
 * PassThruOpen stalled, connectSessionAsync returns at once, so the
 * open didn't run on this (GUI) thread. */
TEST(BridgeTest, AsyncConnectOpensOffGuiThread)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm delay("ECUIO_FAKE_OPEN_DELAY_MS", "600");

    SessionBridge bridge;
    std::atomic<bool> done{false};
    QString gotErr = "unset";
    QObject::connect(&bridge, &SessionBridge::connectResult,
                     [&](const QString&, const QString& error) {
                         gotErr = error;
                         done.store(true);
                     });

    QElapsedTimer clock;
    clock.start();
    bridge.connectSessionAsync(fakeUri());
    EXPECT_LT(clock.elapsed(), 300) << "connectSessionAsync blocked on the open";
    EXPECT_TRUE(bridge.connecting());

    ASSERT_TRUE(spinUntil([&done]() { return done.load(); }, 10000, 10));
    EXPECT_EQ(gotErr.toStdString(), "");
    ASSERT_TRUE(bridge.connected());

    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A slow vendor teardown runs off the GUI thread. With PassThruDisconnect
 * stalled, disconnectSession returns at once with the session already
 * gone as far as the UI can tell; `disconnecting` holds, and refuses a
 * connect, until the teardown ends; the log brackets it. */
TEST(BridgeTest, DisconnectTearsDownOffGuiThread)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    const unsigned long disconnects_before = hooks.disconnect_count();

    FakeEnvArm delay("ECUIO_FAKE_DISCONNECT_DELAY_MS", "800");
    QElapsedTimer clock;
    clock.start();
    bridge.disconnectSession();
    EXPECT_LT(clock.elapsed(), 300) << "disconnectSession blocked on the teardown";
    EXPECT_FALSE(bridge.connected());
    EXPECT_TRUE(bridge.disconnecting());
    EXPECT_EQ(bridge.linkState(), QStringLiteral("disconnected"));
    EXPECT_TRUE(bridge.currentUri().isEmpty());

    // No connect while the adapter is still closing
    EXPECT_FALSE(bridge.connectSession(fakeUri()).isEmpty());
    bridge.connectSessionAsync(fakeUri());
    EXPECT_FALSE(bridge.connecting());

    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.disconnecting(); }, 10000, 10));
    EXPECT_GE(clock.elapsed(), 700) << "the stall never reached the teardown";
    EXPECT_EQ(hooks.disconnect_count(), disconnects_before + 1);

    ASSERT_GE(log.size(), 3);
    EXPECT_TRUE(log.at(log.size() - 3).startsWith(QStringLiteral("Disconnecting: ")));
    EXPECT_TRUE(log.at(log.size() - 2).startsWith(QStringLiteral("Delivery: ")));
    EXPECT_TRUE(log.last().startsWith(QStringLiteral("Disconnected: ")));

    // The adapter is free again once the teardown has ended
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A bridge destroyed mid-teardown waits for it: the destructor joins the
 * teardown thread before shutting core down, and the dropped completion
 * never runs against the freed bridge. */
TEST(BridgeTest, DestroyedMidDisconnectWaitsForTeardown)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());

    auto bridge = std::make_unique<SessionBridge>();
    ASSERT_TRUE(bridge->connectSession(fakeUri()).isEmpty());
    const unsigned long disconnects_before = hooks.disconnect_count();

    FakeEnvArm delay("ECUIO_FAKE_DISCONNECT_DELAY_MS", "500");
    bridge->disconnectSession();
    ASSERT_TRUE(bridge->disconnecting());

    QElapsedTimer clock;
    clock.start();
    bridge.reset();
    EXPECT_GE(clock.elapsed(), 300) << "the destructor didn't wait for the teardown";
    EXPECT_EQ(hooks.disconnect_count(), disconnects_before + 1);
    spinLoop(50);   // the dropped completion must not run
}

/* The adapter's session in the activity log, right after "Connected:",
 * from the fake's own readings. */
TEST(BridgeTest, ConnectLogsTheAdaptersSession)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    ASSERT_GE(log.size(), 2);
    EXPECT_TRUE(log.at(log.size() - 2).startsWith(QStringLiteral("Connected: ")));
    EXPECT_EQ(log.last().toStdString(),
              "Session: x64 DLL, through the surrogate; ISO 15765 at 500000 baud; "
              "profile raw, channel 0x7E8/0x7E0 (rx/tx); battery 13.8 V; "
              "firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04");
    /* What Settings keeps for the device, and none past the session */
    EXPECT_EQ(bridge.adapterVersions().toStdString(),
              "firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04");
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.adapterVersions().isEmpty());
}

/* The probe's answer reaches the activity log once: the fake's ECU lists
 * every linear PID J1979 defines, with 01, 41 and 7F, across its six
 * bitmaps, so nothing the set asks is dropped. The fake answers packed
 * requests, so a second line says so, and nothing more follows. */
TEST(BridgeTest, ConnectLogsWhatTheEcuSupports)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri = QStringLiteral("j2534://%1")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const auto ecu_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8:"));
    };
    ASSERT_TRUE(spinUntil([&]() { return ecu_lines().size() >= 2; }, 5000))
        << ecu_lines().join('\n').toStdString();
    spinLoop(300);
    ASSERT_EQ(ecu_lines().size(), 2) << ecu_lines().join('\n').toStdString();
    EXPECT_EQ(ecu_lines().at(0).toStdString(),
              "ECU 0x7E8: supports 95 PIDs; 92 requests, none dropped");
    EXPECT_EQ(ecu_lines().at(1).toStdString(),
              "ECU 0x7E8: supports 95 PIDs; 92 requests, none dropped; "
              "several PIDs per request");

    /* Identification (ADR-0021) logs one line once every item has an
     * outcome: the fake's FAKE values, and the two DIDs it refuses. */
    const auto identity_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 identity:"));
    };
    ASSERT_TRUE(spinUntil([&]() { return !identity_lines().isEmpty(); }, 5000))
        << "no identity line in the log";
    spinLoop(300);
    ASSERT_EQ(identity_lines().size(), 1) << identity_lines().join('\n').toStdString();
    EXPECT_EQ(identity_lines().first().toStdString(),
              "ECU 0x7E8 identity: VIN FAKEJ2534VIN00001; CALID FAKECAL-01; "
              "CVN DE AD BE EF; ECU name FAKE-ECM; part number FAKE-PART-0001; "
              "software number FAKE-SW-0001; software version FAKE 1.0; "
              "supplier FAKE SUPPLIER; ECU serial FAKE-SERIAL-000001; "
              "VIN (F190) FAKEJ2534VIN00001; hardware number FAKE-HW-01; "
              "2 refused: 22 F1 94 (7F 22 31), 22 F1 95 (7F 22 31)");

    /* Diagnostics (ADR-0037) log one line once the list has settled:
     * the fake ECU's codes, readiness and freeze frame */
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics:"));
    };
    ASSERT_TRUE(
        spinUntil([&]() { return !diagnostics_lines().isEmpty(); }, 5000))
        << "no diagnostics line in the log";
    spinLoop(300);
    ASSERT_EQ(diagnostics_lines().size(), 1)
        << diagnostics_lines().join('\n').toStdString();
    EXPECT_EQ(diagnostics_lines().first().toStdString(),
              kFakeEcuDiagnosticsLine);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* ADR-0060 in the app: with ?session=03 each module's session has a line
 * of its own, once; the module with none is named with its refusal and
 * read in the default session; the support lines end with the session in
 * force; Disconnect names the module returning to the default session; a
 * recording keeps every one of them as a note */
TEST(BridgeTest, AnExtendedSessionNamesEachModulesSessionOnce)
{
    FakeEnvArm module("ECUIO_FAKE_MODULE_DIDS", "7E9:D801=00 12");
    /* Paced by its replies, the range's scan spans two ticks or more
     * only when each reply takes a while (well under the 100 ms wait) */
    FakeEnvArm slow("ECUIO_FAKE_REPLY_DELAY_MS", "40");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=uds-generic&did=1E1C,D800-D803,"
                       "7E9:D801&session=03&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString entered =
        QStringLiteral("ECU 0x7E8: reading in the extended session (50 03)");
    const QString refused = QStringLiteral(
        "ECU 0x7E9: refused the extended session (subFunctionNotSupported, "
        "7F 10 12): reading in the default session");
    ASSERT_TRUE(spinUntil(
        [&]() { return log.contains(entered) && log.contains(refused); }, 5000))
        << log.join('\n').toStdString();
    spinLoop(300);
    EXPECT_EQ(log.count(entered), 1) << log.join('\n').toStdString();
    EXPECT_EQ(log.count(refused), 1) << log.join('\n').toStdString();
    const auto support = [&log](const QString& ecu) {
        QStringList out;
        for (const QString& l : log.filter(ecu)) {
            if (l.contains(QStringLiteral(" requests")) ||
                l.contains(QStringLiteral(" request,"))) {
                out << l;
            }
        }
        return out;
    };
    ASSERT_FALSE(support(QStringLiteral("ECU 0x7E8: ")).isEmpty());
    ASSERT_FALSE(support(QStringLiteral("ECU 0x7E9: ")).isEmpty());
    /* the range's steps report after the entry, so a repeat would show */
    EXPECT_GE(log.mid(log.indexOf(entered))
                  .filter(QStringLiteral("ECU 0x7E8: "))
                  .size(),
              3)
        << log.join('\n').toStdString();
    EXPECT_TRUE(support(QStringLiteral("ECU 0x7E8: "))
                    .last()
                    .endsWith(QStringLiteral("; extended session")))
        << support(QStringLiteral("ECU 0x7E8: ")).last().toStdString();
    for (const QString& l : support(QStringLiteral("ECU 0x7E9: "))) {
        EXPECT_TRUE(l.endsWith(QStringLiteral("; default session")))
            << l.toStdString();
    }
    const QString folder = bridge.recordingFolder();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QString returned =
        QStringLiteral("ECU 0x7E8: returning to the default session (10 01)");
    EXPECT_EQ(log.count(returned), 1) << log.join('\n').toStdString();
    EXPECT_FALSE(
        log.join('\n').contains(QStringLiteral("ECU 0x7E9: returning")));
    const QByteArray events =
        readFile(folder + QStringLiteral("/events.log")).replace("\r\n", "\n");
    for (const QString& line : {entered, refused, returned}) {
        EXPECT_TRUE(events.contains(("  note  " + line + "\n").toUtf8()))
            << line.toStdString() << "\n"
            << events.toStdString();
    }
}

/* Without ?session=03 the same modules give no session line and no
 * session part in their support lines (the module scans two DIDs, so it
 * reports something) */
TEST(BridgeTest, WithoutASessionNoSessionIsNamed)
{
    FakeEnvArm module("ECUIO_FAKE_MODULE_DIDS", "7E9:D801=00 12");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=uds-generic&did=1E1C,7E9:D800-D801"
                       "&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil(
        [&]() { return !log.filter(QStringLiteral("ECU 0x7E9: ")).isEmpty(); },
        5000))
        << log.join('\n').toStdString();
    spinLoop(300);
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QString all = log.join('\n');
    EXPECT_FALSE(all.contains(QStringLiteral("session (")));
    EXPECT_FALSE(all.contains(QStringLiteral("; extended session")));
    EXPECT_FALSE(all.contains(QStringLiteral("; default session")));
    EXPECT_FALSE(all.contains(QStringLiteral("returning to the default")));
}

/* A session of six modules: the session line lists the info's four and
 * counts the rest, and the modules past them get their ECU lines too */
TEST(BridgeTest, ASessionOfSixModulesNamesTheRestAndReportsEachOne)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=uds-generic&did=1E1C,7E9:D800-D801,"
                       "7EA:D800-D801,7EB:D800-D801,7EC:D800-D801,"
                       "7ED:D800-D801&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QStringList session = log.filter(QStringLiteral("Session: "));
    ASSERT_EQ(session.size(), 1) << log.join('\n').toStdString();
    EXPECT_TRUE(session.first().contains(QStringLiteral(
        "channels 0x7E8/0x7E0, 0x7E9/0x7E1, 0x7EA/0x7E2, 0x7EB/0x7E3 "
        "and 2 more (rx/tx)")))
        << session.first().toStdString();
    ASSERT_TRUE(spinUntil(
        [&]() {
            return !log.filter(QStringLiteral("ECU 0x7EC: ")).isEmpty() &&
                   !log.filter(QStringLiteral("ECU 0x7ED: ")).isEmpty();
        },
        10000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A second connect from the same bridge names the session again: what was
 * written is per connect */
TEST(BridgeTest, AReconnectNamesTheSessionAgain)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri = QStringLiteral("j2534://%1?profile=uds-generic&did=1E1C"
                                       "&session=03&identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    const QString entered =
        QStringLiteral("ECU 0x7E8: reading in the extended session (50 03)");
    for (int n = 1; n <= 2; ++n) {
        ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
        ASSERT_TRUE(spinUntil([&]() { return log.count(entered) == n; }, 5000))
            << "connect " << n << "\n"
            << log.join('\n').toStdString();
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
}

/* A session that opens but won't start (the adapter refuses the obd2
 * channel's filter) fails the connect with the cause, and the bridge
 * keeps no session: a second connect works once the refusal is gone. */
TEST(BridgeTest, RefusedStartNamesItsCauseAndLeavesNoSession)
{
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://%1")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    {
        FakeEnvArm refuse("ECUIO_FAKE_FILTER_STATUS", "7");
        EXPECT_EQ(bridge.connectSession(uri).toStdString(),
                  "the adapter's driver refused: PassThruStartMsgFilter "
                  "returned status 7 (0x7, ERR_FAILED): fake_j2534: "
                  "PassThruStartMsgFilter refused (ECUIO_FAKE_FILTER_STATUS)");
        EXPECT_FALSE(bridge.connected());
    }
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_TRUE(bridge.connected());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* ADR-0027 section 3: the fake's VIN reaches QML only as a salted SHA-256, once
 * a session, and nothing is left of it after the disconnect. A bare URI
 * runs obd2, whose rotation asks the identity. */
TEST(BridgeTest, AConnectsVinReachesQmlOnlyAsASaltedHash)
{
    SessionBridge bridge;
    int identified = 0;
    QObject::connect(&bridge, &SessionBridge::vehicleIdentified,
                     [&identified]() { ++identified; });
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    EXPECT_TRUE(bridge.vinKey(QStringLiteral("salt")).isEmpty())
        << "no session, no key";
    const QString uri = QStringLiteral("j2534://%1")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil([&identified]() { return identified > 0; }, 5000))
        << "vehicleIdentified never came";
    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayLiteral("saltFAKEJ2534VIN00001"),
                                 QCryptographicHash::Sha256)
            .toHex());
    EXPECT_EQ(bridge.vinKey(QStringLiteral("salt")), expected);
    EXPECT_NE(bridge.vinKey(QStringLiteral("pepper")), expected)
        << "the salt is in the key";
    spinLoop(300);
    EXPECT_EQ(identified, 1) << "once a session";
    // ADR-0043: its decode is logged once too; the fake's has an I
    EXPECT_EQ(log.filter(QRegularExpression(QStringLiteral("^VIN: "))),
              QStringList{QStringLiteral(
                  "VIN: not a VIN: 49 CFR 565 takes 17 digits and capital "
                  "letters, never I, O or Q")});
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.vinKey(QStringLiteral("salt")).isEmpty());
}

/* ADR-0027 section 3: the vehicle is known once 09 02 has answered, while
 * the rest of the identification is still out. The fake holds every
 * Service 0x22 request (7F 22 78, no answer), so the list's DIDs stay out
 * about 5 s each and no identity line comes within the wait. */
TEST(BridgeTest, TheVinIsTakenWhileTheRestOfTheIdentificationIsOut)
{
    FakeEnvArm hold("ECUIO_FAKE_HELD_SERVICE", "22");
    SessionBridge bridge;
    int identified = 0;
    QObject::connect(&bridge, &SessionBridge::vehicleIdentified,
                     [&identified]() { ++identified; });
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    // A bare URI runs obd2, which identifies
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(spinUntil([&identified]() { return identified > 0; }, 4000))
        << "vehicleIdentified waited for the whole list:\n"
        << log.join('\n').toStdString();
    EXPECT_EQ(
        bridge.vinKey(QStringLiteral("salt")),
        QString::fromLatin1(
            QCryptographicHash::hash(QByteArrayLiteral("saltFAKEJ2534VIN00001"),
                                     QCryptographicHash::Sha256)
                .toHex()));
    EXPECT_EQ(log.filter(QRegularExpression(QStringLiteral("^VIN: "))).size(),
              1);
    EXPECT_TRUE(
        log.filter(QRegularExpression(QStringLiteral("^ECU 0x7E8 identity: ")))
            .isEmpty())
        << "the list isn't complete, so no identity line yet";
    spinLoop(300);
    EXPECT_EQ(identified, 1) << "once a session";
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.vinKey(QStringLiteral("salt")).isEmpty());
}

/* ADR-0043: the "VIN:" line and Settings' lookup, one summary. The VINs
 * are made up around FAKE, their check digits by 49 CFR 565.15 */
TEST(BridgeTest, AVinSummarySaysItsYearMakeAndCheckDigit)
{
    EXPECT_EQ(
        SessionBridge::vinSummary(QStringLiteral("1FAFAKE10M0000001"), 2027),
        QStringLiteral("model year 2021; Ford (manufacturer code 1FA); "
                       "check digit matches"));
    EXPECT_EQ(
        SessionBridge::vinSummary(QStringLiteral("1FAFAKE13Y0000001"), 2027),
        QStringLiteral("model year 2000 (not 2030, which is still ahead); "
                       "Ford (manufacturer code 1FA); check digit matches"));
    EXPECT_EQ(
        SessionBridge::vinSummary(QStringLiteral("ZZZFAKE10U0000001"), 2027),
        QStringLiteral("no model year (position 10 is U); manufacturer "
                       "code ZZZ isn't in NHTSA's list; check digit 0 "
                       "isn't the 1 it works out to (North America "
                       "requires it; other markets may not)"));
    EXPECT_EQ(SessionBridge::vinSummary(
                  QStringLiteral("1FAFAKE10M000000") + QChar(0xC9), 2027)
                  .left(10),
              QStringLiteral("not a VIN:"));
    SessionBridge bridge;
    EXPECT_EQ(
        bridge.describeVin(QStringLiteral(" 1fa fake10m 0000001 ")).left(14),
        QStringLiteral("model year 202"))
        << "spaces dropped, upper-cased";
    EXPECT_TRUE(bridge.describeVin(QStringLiteral("  ")).isEmpty());
    EXPECT_TRUE(
        bridge.vinSource().startsWith(QStringLiteral("vPICList_lite_")));
}

/* ADR-0027 section 2: an adapter session's recording folder names the picked
 * vehicle, as a folder name may take it; a mock session's doesn't. */
TEST(BridgeTest, RecordingFolderNamesThePickedVehicleForAnAdapterOnly)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    bridge.setVehicleLabel(QStringLiteral("Daily driver/2"));

    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://rec")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 2) << folders.join(',').toStdString();
    const QRegularExpression dateOnly(
        QStringLiteral("^[0-9]{8}-[0-9]{6}(-[0-9]+)?$"));
    const QRegularExpression named(
        QStringLiteral("^[0-9]{8}-[0-9]{6}(-[0-9]+)? Daily driver2$"));
    EXPECT_EQ(folders.filter(dateOnly).size(), 1)
        << folders.join(',').toStdString();
    EXPECT_EQ(folders.filter(named).size(), 1)
        << folders.join(',').toStdString();
}

/* ADR-0038 through the fake J2534 DLL, whose engine turns (2000 rpm)
 * and refuses to clear running (ECUIO_FAKE_CLEAR_NRC 0x22, J1979's
 * conditionsNotCorrect): the refusal is one line that says why, and the
 * generations the clear moved log no second diagnostics line; a Read
 * again after it logs its read */
TEST(BridgeTest, AClearTheEngineRefusesLogsOneLineAndNoRead)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm refuses("ECUIO_FAKE_CLEAR_NRC", "0x22");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "));
    };
    const auto clear_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 clear: "));
    };
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 5000));
    EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
    ASSERT_TRUE(spinUntil([&]() { return !clear_lines().isEmpty(); }, 5000));
    spinLoop(500);
    ASSERT_EQ(clear_lines().size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(clear_lines().first().toStdString(),
              "ECU 0x7E8 clear: refused (7F 04 22): this ECU won't clear with "
              "the engine running: stop and turn it off, then try again");
    EXPECT_EQ(diagnostics_lines().size(), 1) << log.join('\n').toStdString();

    bridge.readDiagnostics();
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 5000))
        << "a read after the clear logs";
    EXPECT_EQ(diagnostics_lines().at(1).toStdString(), kFakeEcuDiagnosticsLine);
    spinLoop(300);
    EXPECT_EQ(clear_lines().size(), 1) << "one line per clear asked";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The user's word on the engine (ADR-0038's appendix of 2026-11-01): the
 * fake's engine turns (2000 rpm) and its car stands, so the clear goes
 * through, its line naming both speeds */
TEST(BridgeTest, AClearAtTheFakesRunningEngineGoesThrough)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "));
    };
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 5000));
    EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 5000))
        << log.join('\n').toStdString();
    EXPECT_EQ(log.filter(QStringLiteral("ECU 0x7E8 clear: ")),
              QStringList{QStringLiteral(
                  "ECU 0x7E8 clear: cleared (2000 rpm, 0 km/h)")});
    EXPECT_TRUE(bridge.clearMoving().isEmpty()) << "nothing waits";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Moving (ECUIO_FAKE_SPEED_KMH 48), the word on the engine isn't enough:
 * the clear ends unsent and clearMoving names the ECU and the speed, in
 * the unit system, the log's lines too; the word on a moving vehicle
 * clears it, with no second file. Canceled instead, nothing more is sent */
TEST(BridgeTest, AClearWhileMovingAsksOnceMore)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm moving("ECUIO_FAKE_SPEED_KMH", "48");
    for (const bool word : {true, false}) {
        SCOPED_TRACE(word ? "cleared moving" : "canceled");
        QTemporaryDir root;
        ASSERT_TRUE(root.isValid());
        SessionBridge bridge;
        bridge.setRecordingsRootForTesting(root.path());
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [&log](const QString& line) { log << line; });
        const auto clear_lines = [&log]() {
            return log.filter(QStringLiteral("ECU 0x7E8 clear: "));
        };
        int moving_signals = 0;
        QObject::connect(&bridge, &SessionBridge::clearMovingChanged,
                         [&moving_signals]() { ++moving_signals; });
        ASSERT_TRUE(bridge
                        .connectSession(
                            QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
                        .isEmpty());
        ASSERT_TRUE(spinUntil(
            [&]() {
                return !log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "))
                            .isEmpty();
            },
            5000));
        EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
        ASSERT_TRUE(
            spinUntil([&]() { return !bridge.clearMoving().isEmpty(); }, 5000))
            << log.join('\n').toStdString();
        EXPECT_EQ(clear_lines(),
                  QStringList{QStringLiteral(
                      "ECU 0x7E8 clear: not sent: moving (48 km/h)")});
        EXPECT_EQ(bridge.clearMoving(), QStringList{QStringLiteral("0x7E8")});
        EXPECT_EQ(bridge.clearMovingSpeed(), QStringLiteral("48 km/h"));
        bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
        EXPECT_EQ(bridge.clearMovingSpeed(), QStringLiteral("30 mph"));
        EXPECT_GT(moving_signals, 0);

        if (word) {
            bridge.clearDiagnosticsMoving();
            EXPECT_TRUE(bridge.clearMoving().isEmpty());
            ASSERT_TRUE(
                spinUntil([&]() { return clear_lines().size() == 2; }, 5000))
                << log.join('\n').toStdString();
            EXPECT_EQ(clear_lines().at(1).toStdString(),
                      "ECU 0x7E8 clear: cleared (2000 rpm, 30 mph)")
                << "the log in the unit system the clear ended in";
            EXPECT_EQ(
                log.filter(QStringLiteral("Clearing while moving")).size(), 1);
        } else {
            bridge.cancelClearMoving();
            EXPECT_TRUE(bridge.clearMoving().isEmpty());
            EXPECT_EQ(log.filter(QStringLiteral("Clear while moving not sent: "
                                                "0x7E8"))
                          .size(),
                      1);
            spinLoop(500);
            EXPECT_EQ(clear_lines().size(), 1) << "nothing more sent";
            bridge.clearDiagnosticsMoving();
            spinLoop(300);
            EXPECT_EQ(clear_lines().size(), 1) << "nothing waits to send";
        }
        EXPECT_EQ(QDir(root.path())
                      .entryList({QStringLiteral("*.jsonl")}, QDir::Files)
                      .size(),
                  1)
            << "one saved file";
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
}

/* A clear asked again over a report holding the last clear's outcome:
 * until the worker takes the request, the poll reads that old MOVING
 * at the generation asked, which is no end of the new clear. The worker
 * is parked in a frame while the ticks run, so the poll comes first */
TEST(BridgeTest, AClearAskedAgainEndsOnlyWithItsOwnOutcome)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm moving("ECUIO_FAKE_SPEED_KMH", "48");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    WorkerHold hold;
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto clear_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 clear: "));
    };
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_EQ(hold.attach(bridge.sessionForTesting()), ECUIO_OK);
    WorkerHold::Release release{hold};
    ASSERT_TRUE(spinUntil(
        [&]() {
            return !log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "))
                        .isEmpty();
        },
        5000));
    EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.clearMoving().isEmpty(); }, 5000))
        << log.join('\n').toStdString();

    /* Asked again, then the moving word, each while the worker is parked */
    const auto parked = [&](const std::function<void()>& ask) {
        const qsizetype before = clear_lines().size();
        ASSERT_TRUE(hold.hold(5000)) << "the worker parks in a frame";
        ask();
        spinLoop(400);
        EXPECT_EQ(clear_lines().size(), before) << log.join('\n').toStdString();
        EXPECT_TRUE(bridge.clearMoving().isEmpty())
            << "no outcome before the worker takes the clear";
        hold.release();
    };
    parked([&]() { EXPECT_FALSE(bridge.clearDiagnostics().isEmpty()); });
    ASSERT_TRUE(spinUntil([&]() { return clear_lines().size() == 2; }, 5000))
        << log.join('\n').toStdString();
    EXPECT_EQ(clear_lines().at(1).toStdString(),
              "ECU 0x7E8 clear: not sent: moving (48 km/h)");
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.clearMoving().isEmpty(); }, 5000));
    parked([&]() { bridge.clearDiagnosticsMoving(); });
    ASSERT_TRUE(spinUntil([&]() { return clear_lines().size() == 3; }, 5000))
        << log.join('\n').toStdString();
    EXPECT_EQ(clear_lines().at(2).toStdString(),
              "ECU 0x7E8 clear: cleared (2000 rpm, 48 km/h)")
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* With the fake's engine stopped the clear goes through: its line, then
 * the read after it, once, though the report passes through an
 * incomplete read on the way; an adapter's file names the vehicle */
TEST(BridgeTest, AClearAtTheFakesStoppedEngineLogsTheReadAfter)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm off("ECUIO_FAKE_ENGINE_OFF", "1");
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
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 5000));
    const QString shown = bridge.clearDiagnostics();
    EXPECT_TRUE(QRegularExpression(
                    QStringLiteral(
                        "diagnostics-[0-9]{8}-[0-9]{6} Daily driver\\.jsonl$"))
                    .match(shown)
                    .hasMatch())
        << shown.toStdString();
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 5000))
        << log.join('\n').toStdString();
    spinLoop(500);
    EXPECT_EQ(log.filter(QStringLiteral("ECU 0x7E8 clear: ")),
              QStringList{
                  QStringLiteral("ECU 0x7E8 clear: cleared (0 rpm, 0 km/h)")});
    ASSERT_EQ(diagnostics_lines().size(), 2) << log.join('\n').toStdString();
    EXPECT_TRUE(diagnostics_lines().at(1).contains(
        QStringLiteral("MIL off; stored none; pending none; permanent P0420")))
        << diagnostics_lines().at(1).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Live leaves out what the fake ECU withholds from its bitmaps, and
 * only that: the bridge reads the session's support report (ADR-0039
 * section 6) */
TEST(BridgeTest, LiveHidesThePidsTheFakeEcuWithholds)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm withheld("ECUIO_FAKE_WITHHELD_PIDS", "0A,5C");
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://%1?identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil([&]() { return model->hiddenCount() > 0; }, 5000))
        << "nothing hidden";
    spinLoop(300);
    QStringList hidden;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r);
        if (model->data(i, SignalListModel::HiddenRole).toBool()) {
            hidden << model->data(i, SignalListModel::NameRole).toString();
        }
    }
    /* In set order: the ten first, then the table's */
    EXPECT_EQ(hidden, (QStringList{QStringLiteral("Engine Oil Temp"),
                                   QStringLiteral("Fuel Pressure")}));
    EXPECT_EQ(bridge.liveSignalModel()->rowCount(), model->rowCount() - 2);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Clear codes over two ECUs is one clear request (ADR-0038): the fake's
 * engine ECU (7E8) clears, and 7E9, the fake's module table, which
 * answers no OBD service, ends as no emissions module; neither is
 * refused because the other's request was running */
TEST(BridgeTest, ClearCodesAsksEveryEcuInOneRequest)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm modules("ECUIO_FAKE_MODULE_DIDS", "7E9:1000=11");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?profile=uds-generic&"
                                           "identify=0&did=7E8:1E1C,7E9:1000")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return bridge.clearTargets().size() == 2; }, 8000))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
    const auto clear_of = [&log](const char* rx) {
        return log.filter(QStringLiteral("ECU %1 clear: ").arg(rx));
    };
    ASSERT_TRUE(spinUntil(
        [&]() {
            return !clear_of("0x7E8").isEmpty() && !clear_of("0x7E9").isEmpty();
        },
        8000))
        << log.join('\n').toStdString();
    EXPECT_TRUE(clear_of("0x7E8").first().startsWith(
        QStringLiteral("ECU 0x7E8 clear: cleared")))
        << log.join('\n').toStdString();
    EXPECT_EQ(clear_of("0x7E9"),
              QStringList{QStringLiteral(
                  "ECU 0x7E9 clear: not sent: not an emissions module")})
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

#endif  // _WIN32
