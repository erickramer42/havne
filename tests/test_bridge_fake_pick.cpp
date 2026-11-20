// File/layer: tests/test_bridge_fake_pick.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_fake_support.h"

#ifdef _WIN32

namespace {

/* The obd2 set's id for Service 01 PID pid */
uint16_t obd2IdOf(uint8_t pid)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            d.mux_key == (0x4100u | pid)) {
            return d.id;
        }
    }
    ADD_FAILURE() << "no def reads PID " << int(pid);
    return 0;
}

/* The next 16 writes after a reset of the fake's page, as the Service 01
 * PIDs they ask; empty when they don't come or one isn't a Service 01
 * request */
std::vector<uint8_t> nextPids()
{
    fake_control::reset(fakePage());
    if (!spinUntil([]() { return fake_control::writes(fakePage()) >= 16; },
                   5000, 10)) {
        return {};
    }
    std::vector<uint8_t> pids;
    for (uint32_t i = 0; i < 16; ++i) {
        fake_control::Write w{};
        if (!fake_control::recorded_write(fakePage(), i, &w) || w.size < 6 ||
            w.data[4] != 0x01) {
            return {};
        }
        pids.push_back(w.data[5]);
    }
    return pids;
}

bool onlyThese(const std::vector<uint8_t>& pids,
               std::initializer_list<uint8_t> picks)
{
    return !pids.empty() &&
           std::all_of(pids.begin(), pids.end(), [&picks](uint8_t pid) {
               return std::find(picks.begin(), picks.end(), pid) != picks.end();
           });
}

/* obd2's ranked PIDs (kPlotOrder): its default pick with nothing added */
const std::vector<uint8_t> kRankedPids = {0x0C, 0x05, 0x0F, 0x10, 0x5C,
                                          0x0D, 0x04, 0x0E, 0x06, 0x07,
                                          0x0B, 0x42, 0x49, 0x44};

std::vector<uint8_t> rankedAnd(std::initializer_list<uint8_t> more)
{
    std::vector<uint8_t> pids = kRankedPids;
    pids.insert(pids.end(), more.begin(), more.end());
    return pids;
}

bool onlyOf(const std::vector<uint8_t>& pids, const std::vector<uint8_t>& picks)
{
    return !pids.empty() &&
           std::all_of(pids.begin(), pids.end(), [&picks](uint8_t pid) {
               return std::find(picks.begin(), picks.end(), pid) != picks.end();
           });
}

int modelRowOf(SignalListModel* model, uint16_t id)
{
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->data(model->index(r), SignalListModel::IdRole).toUInt() ==
            id) {
            return r;
        }
    }
    return -1;
}

bool unaskedRow(SignalListModel* model, uint16_t id)
{
    const int row = modelRowOf(model, id);
    return row >= 0 &&
           model->data(model->index(row), SignalListModel::UnaskedRole)
               .toBool();
}

/* The first line starting with prefix, or -1 */
int lineStarting(const QStringList& log, const QString& prefix)
{
    for (int i = 0; i < log.size(); ++i) {
        if (log[i].startsWith(prefix)) {
            return i;
        }
    }
    return -1;
}

QString fakeObd2PickUri()
{
    return QStringLiteral(
               "j2534://%1?profile=obd2&probe=0&identify=0&diagnose=0")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
}

/* A set past a rotation (tests/car_sized_set.h), asked only its picks
 * (ADR-0065): unranked, and a copy that ranks every def */
const char* const kCarSet = "bridge-car";
const char* const kCarRanked = "bridge-car-ranked";

const ecuio_signal_set_t* rankedCarSet()
{
    static std::vector<ecuio_signal_def_t> defs;
    static ecuio_signal_set_t made = {};
    if (made.count == 0) {
        const ecuio_signal_set_t* base = car_sized::set();
        defs.assign(base->defs, base->defs + base->count);
        for (ecuio_signal_def_t& d : defs) {
            d.plot_rank = 1;
        }
        made = *base;
        made.defs = defs.data();
    }
    return &made;
}

QString carUri(const char* profile)
{
    return QStringLiteral("j2534://%1?profile=%2&identify=0&diagnose=0")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH),
             QString::fromUtf8(profile));
}

const QString kNoneToPick = QStringLiteral(
    "Asking no signal: this profile is asked only what is picked. Tick "
    "signals on Live or place gauges");

bool anyAskedEvery(const QStringList& log)
{
    return std::any_of(log.begin(), log.end(), [](const QString& l) {
        return l.startsWith(QStringLiteral("Asking every"));
    });
}

} // namespace

/* ADR-0065: on a session read by its picks nothing is asked until a row
 * is ticked, and the pick line says so rather than "every signal"; a
 * ticked row is asked even though the set ranks none, and unticked, the
 * asking stops; Start log and Stop log leave it asking nothing */
TEST(BridgeTest, APickOnlySessionAsksNothingUntilARowIsTicked)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm module("ECUIO_FAKE_MODULE_DIDS", "7E8:1007=2A;7E9:1003=11");
    ASSERT_EQ(ecuio_profile_register(kCarSet, car_sized::set(), nullptr),
              ECUIO_OK);
    {
        QTemporaryDir root;
        ASSERT_TRUE(root.isValid());
        SessionBridge bridge;
        bridge.setRecordingsRootForTesting(root.path());
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [&log](const QString& line) { log << line; });
        ASSERT_TRUE(bridge.connectSession(carUri(kCarSet)).isEmpty());
        EXPECT_TRUE(log.contains(kNoneToPick)) << log.join('\n').toStdString();
        EXPECT_FALSE(anyAskedEvery(log)) << log.join('\n').toStdString();
        const auto writes = [] { return fake_control::writes(fakePage()); };
        QElapsedTimer quiet;
        quiet.start();
        ASSERT_TRUE(spinUntil([&]() { return quiet.elapsed() >= 400; }, 5000));
        EXPECT_EQ(writes(), 0u) << "nothing is asked before a pick";

        SignalListModel* model = bridge.signalModel();
        const uint16_t picked = car_sized::id_of(7);
        model->toggleSubscribed(modelRowOf(model, picked));
        ASSERT_TRUE(spinUntil(
            [&]() {
                return lineStarting(log, QStringLiteral("Asking 1 signal: "
                                                        "Signal 7")) >= 0;
            },
            5000))
            << log.join('\n').toStdString();
        ASSERT_TRUE(spinUntil([&]() { return writes() >= 4; }, 5000))
            << "the ticked row is asked";
        fake_control::Write w{};
        ASSERT_TRUE(fake_control::recorded_write(fakePage(), 0, &w));
        EXPECT_EQ(w.size, 7u);
        EXPECT_EQ(w.data[5], 0x10);
        EXPECT_EQ(w.data[6], 0x07);

        model->toggleSubscribed(modelRowOf(model, picked));
        ASSERT_TRUE(
            spinUntil([&]() { return log.count(kNoneToPick) == 2; }, 5000))
            << log.join('\n').toStdString();
        QElapsedTimer settle;
        settle.start();
        ASSERT_TRUE(spinUntil([&]() { return settle.elapsed() >= 300; }, 5000));
        const auto after = writes();
        settle.restart();
        ASSERT_TRUE(spinUntil([&]() { return settle.elapsed() >= 400; }, 5000));
        EXPECT_EQ(writes(), after) << "unticked: the asking stops";

        // A log of nothing isn't started; a log of a ticked row, its row
        // unticked meanwhile, ends asking nothing and says so
        EXPECT_FALSE(bridge.startLog());
        model->toggleSubscribed(modelRowOf(model, picked));
        ASSERT_TRUE(
            spinUntil([&]() { return model->tickedCount() == 1; }, 5000));
        ASSERT_TRUE(bridge.startLog());
        model->toggleSubscribed(modelRowOf(model, picked));
        log.clear();
        bridge.stopLog();
        EXPECT_TRUE(log.contains(kNoneToPick)) << log.join('\n').toStdString();
        EXPECT_FALSE(anyAskedEvery(log)) << log.join('\n').toStdString();
        const auto afterLog = writes();
        settle.restart();
        ASSERT_TRUE(spinUntil([&]() { return settle.elapsed() >= 400; }, 5000));
        EXPECT_EQ(writes(), afterLog);
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
    EXPECT_EQ(ecuio_profile_unregister(kCarSet), ECUIO_OK);
}

/* A default pick core refuses (every def ranked: 2048 requests) leaves a
 * pick-only session asking nothing, with core's words; never "every
 * signal" */
TEST(BridgeTest, APickOnlyPickCoreRefusesAsksNothing)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm module("ECUIO_FAKE_MODULE_DIDS", "7E8:1007=2A;7E9:1003=11");
    ASSERT_EQ(ecuio_profile_register(kCarRanked, rankedCarSet(), nullptr),
              ECUIO_OK);
    {
        SessionBridge bridge;
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [&log](const QString& line) { log << line; });
        ASSERT_TRUE(bridge.connectSession(carUri(kCarRanked)).isEmpty());
        const int at = lineStarting(log, QStringLiteral("Asking no signal: "));
        ASSERT_GE(at, 0) << log.join('\n').toStdString();
        EXPECT_TRUE(log[at].contains(
            QStringLiteral("ask 2048 requests: a session asks 1024 at a time")))
            << log[at].toStdString();
        EXPECT_FALSE(anyAskedEvery(log)) << log.join('\n').toStdString();
        QElapsedTimer quiet;
        quiet.start();
        ASSERT_TRUE(spinUntil([&]() { return quiet.elapsed() >= 400; }, 5000));
        EXPECT_EQ(fake_control::writes(fakePage()), 0u);
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
    EXPECT_EQ(ecuio_profile_unregister(kCarRanked), ECUIO_OK);
}

/* Start log over the fake ECU (ADR-0056 sections 2 and 3): the session
 * asks the plotted PIDs alone, ticks on Live meanwhile leave that so,
 * the recording's time starts at Start (not at the connect), and Stop
 * asks the default pick again, saying so. */
TEST(BridgeTest, ALogAsksThePlottedPidsAlone)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList stopped;
    QObject::connect(
        &bridge, &SessionBridge::logStopped,
        [&stopped](const QString& folder, bool) { stopped << folder; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=obd2&filter=0x7E8&probe=0&"
                       "identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    QElapsedTimer age;
    age.start();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    const std::vector<uint16_t> picks = {obd2IdOf(0x0C), obd2IdOf(0x0D)};
    plotOnly(model, picks);
    ASSERT_EQ(model->subscribedCount(), 2);
    const QStringList pickKeys = keysOf(bridge, model, picks);
    // A session old enough that a recording timed from its start shows it
    ASSERT_TRUE(spinUntil([&]() { return age.elapsed() >= 2000; }, 10000));
    const std::vector<uint8_t> before = nextPids();
    ASSERT_FALSE(before.empty());
    ASSERT_FALSE(onlyThese(before, {0x0C, 0x0D}))
        << "the whole set is asked before the log";

    const qint64 ageAtStartMs = age.elapsed();
    ASSERT_TRUE(bridge.startLog());
    const QString folder = bridge.recordingFolderUrl().toLocalFile();
    ASSERT_TRUE(spinUntil([]() { return onlyThese(nextPids(), {0x0C, 0x0D}); },
                          15000, 10))
        << "the rotation never narrowed to 0C and 0D";

    // A tick meanwhile plots the row and leaves the focus as it was
    plotOnly(model, {picks[0], picks[1], obd2IdOf(0x05)});
    ASSERT_EQ(model->subscribedCount(), 3);
    for (int round = 0; round < 2; ++round) {
        const std::vector<uint8_t> pids = nextPids();
        EXPECT_TRUE(onlyThese(pids, {0x0C, 0x0D})) << "round " << round;
    }

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.stopLog();
    ASSERT_EQ(stopped, QStringList{folder});
    // The ticked rows are ranked ones: the pick is obd2's ranked PIDs
    EXPECT_GT(lineStarting(log, QStringLiteral("Asking 14 signals: ")),
              lineStarting(log, QStringLiteral("Log stopped: ")))
        << log.join('\n').toStdString();
    ASSERT_TRUE(spinUntil(
        []() {
            const std::vector<uint8_t> pids = nextPids();
            return onlyOf(pids, kRankedPids) &&
                   !onlyThese(pids, {0x0C, 0x0D, 0x05});
        },
        15000, 10))
        << "the default pick never came back";
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QList<QByteArray> rows =
        fileLines(folder + QStringLiteral("/frames.csv"));
    ASSERT_GE(rows.size(), 2) << "no frame in the log";
    const qint64 firstHostUs = rows[1].split(',')[0].toLongLong();
    EXPECT_LT(firstHostUs, ageAtStartMs * 1000 / 2)
        << "timed from the connect, " << ageAtStartMs << " ms before Start";
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains(
        (QStringLiteral("focus  ") + pickKeys.join(',')).toUtf8()))
        << events.toStdString();
}

/* A focus core refuses (under ?poll=0x0C, 0D alone leaves nothing to
 * ask) refuses Start log with core's words and opens no folder; the
 * polled PID's is taken. */
TEST(BridgeTest, AFocusCoreRefusesStartsNoLog)
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
    const QString uri =
        QStringLiteral(
            "j2534://%1?profile=obd2&poll=0x0C&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());

    plotOnly(bridge.signalModel(), {obd2IdOf(0x0D)});
    ASSERT_EQ(bridge.signalModel()->subscribedCount(), 1);
    EXPECT_FALSE(bridge.startLog());
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Log not started: the focus leaves nothing to request: none of the "
        "picked signals is asked on this session")))
        << log.join('\n').toStdString();
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());

    plotOnly(bridge.signalModel(), {obd2IdOf(0x0C)});
    EXPECT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    bridge.stopLog();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(recordingFolders(root.path()).size(), 1);
}

/* The default pick (ADR-0056 section 2's second use): from the start the
 * session asks obd2's ranked PIDs and the gauges' signals, its line after
 * the session's; the rest read "not asked"; a tick asks its row within a
 * tick, and a gauge taken away stops asking its signal */
TEST(BridgeTest, TheDefaultPickAsksTheRankedRowsTheGaugesAndTheTicks)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    // Set while disconnected: Throttle Position, which obd2 doesn't rank
    bridge.setPickGauges({QVariant(
        QStringList{QStringLiteral("id:9"), QStringLiteral("id:7006")})});
    ASSERT_TRUE(bridge.connectSession(fakeObd2PickUri()).isEmpty());
    const int session = lineStarting(log, QStringLiteral("Session: "));
    const int asking = lineStarting(log, QStringLiteral("Asking "));
    ASSERT_GE(asking, 0) << log.join('\n').toStdString();
    EXPECT_GT(asking, session);
    EXPECT_TRUE(log[asking].startsWith(QStringLiteral(
        "Asking 15 signals: Engine Load, Coolant Temp, RPM, Vehicle Speed, "
        "Intake Air Temp, Throttle Position, Battery Voltage")))
        << log[asking].toStdString();
    EXPECT_TRUE(log[asking].endsWith(QStringLiteral(" and 3 more")))
        << log[asking].toStdString();
    ASSERT_TRUE(spinUntil(
        []() { return onlyOf(nextPids(), rankedAnd({0x11})); }, 15000, 10))
        << "the rotation never kept to the pick";

    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil(
        [&]() { return unaskedRow(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID); },
        5000))
        << "Fuel Level never read not asked";
    EXPECT_FALSE(unaskedRow(model, ECUIO_OBD2_SIG_THROTTLE_ID));
    EXPECT_FALSE(unaskedRow(model, ECUIO_OBD2_SIG_RPM_ID));
    EXPECT_GT(model->unaskedCount(), 0);

    // A tick asks the row
    model->toggleSubscribed(modelRowOf(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(spinUntil(
        [&]() { return !unaskedRow(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID); },
        5000))
        << "the tick never asked Fuel Level";
    EXPECT_GE(lineStarting(log, QStringLiteral("Asking 16 signals: ")), 0)
        << log.join('\n').toStdString();
    ASSERT_TRUE(spinUntil(
        []() {
            const std::vector<uint8_t> pids = nextPids();
            return onlyOf(pids, rankedAnd({0x11, 0x2F})) &&
                   std::find(pids.begin(), pids.end(), 0x2F) != pids.end();
        },
        15000, 10))
        << "Fuel Level's PID never went out";

    // A gauge taken away stops asking its signal
    bridge.setPickGauges({});
    ASSERT_TRUE(spinUntil(
        [&]() { return unaskedRow(model, ECUIO_OBD2_SIG_THROTTLE_ID); }, 5000))
        << "Throttle Position stayed asked";
    EXPECT_GE(lineStarting(log, QStringLiteral("Asking 15 signals: ")), 0);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A Record sessions recording of a session that asks names its default
 * pick on a pick line, and the Recordings list calls it a session, not
 * a log; a pick changed by a tick is core's pick line in the activity,
 * and the Log page's "Asking" line is no note beside it */
TEST(BridgeTest, ASessionRecordingNamesItsPickAndIsNoLog)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeObd2PickUri()).isEmpty());
    spinLoop(500);
    SignalListModel* model = bridge.signalModel();
    model->toggleSubscribed(modelRowOf(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return lineStarting(log, QStringLiteral("Asking 15 signals: ")) >=
                   0;
        },
        5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QLatin1Char('/') + folders[0];
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    // Picks go by key: Engine Load, Coolant Temp, RPM, then Fuel Level
    EXPECT_TRUE(events.contains(
        "\npick  7E8:4104:2.0:8,7E8:4105:2.0:8,7E8:410C:2.0:16,"))
        << events.toStdString();
    EXPECT_FALSE(events.contains("\nfocus  ")) << events.toStdString();
    const int change = events.indexOf("  pick  15 signals: 7E8:4104:2.0:8,");
    ASSERT_GE(change, 0) << events.toStdString();
    EXPECT_GT(events.indexOf("7E8:412F:2.0:8", change), change);
    EXPECT_FALSE(events.contains("note  Asking ")) << events.toStdString();
    EXPECT_FALSE(
        bridge.recordingRow(folder).value(QStringLiteral("log")).toBool());
}

/* A new minimum time between sweeps reaches the running session's pick
 * (decided choice 4), though the picked signals stay the same, and Off
 * takes it away again */
TEST(BridgeTest, ANewMinimumTimeReachesTheRunningPick)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeObd2PickUri()).isEmpty());
    ASSERT_GE(lineStarting(log, QStringLiteral("Asking 14 signals: ")), 0)
        << log.join('\n').toStdString();
    bridge.setMinSweepMs(250);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return lineStarting(
                       log, QStringLiteral("Asking 14 signals (at least 250 ms "
                                           "between sweeps): ")) >= 0;
        },
        5000))
        << log.join('\n').toStdString();
    bridge.setMinSweepMs(0);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.filter(QRegularExpression(
                                  QStringLiteral("^Asking 14 signals: ")))
                       .size() == 2;
        },
        5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Core paces only a focus, so a set that ranks none, asked whole, says a
 * minimum doesn't apply to it, and says nothing of one while it's Off;
 * the line says why, which core's own focus line doesn't, so a recording
 * keeps it as a note */
TEST(BridgeTest, TheWholeSetSaysAMinimumTimeDoesntApply)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://"
                       "%1?profile=uds-generic&did=1E1C:u16&identify=0&"
                       "diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString ranksNone =
        QStringLiteral("Asking every signal: the profile ranks none");
    EXPECT_TRUE(log.contains(ranksNone)) << log.join('\n').toStdString();
    bridge.setMinSweepMs(250);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(
                ranksNone +
                QStringLiteral(
                    "; no minimum time between sweeps applies to it"));
        },
        5000))
        << log.join('\n').toStdString();
    bridge.setMinSweepMs(0);
    ASSERT_TRUE(spinUntil([&]() { return log.count(ranksNone) == 2; }, 5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray events =
        readFile(root.path() + QLatin1Char('/') + folders[0] +
                 QStringLiteral("/events.log"));
    const QByteArray note = ("  note  " + ranksNone).toUtf8();
    EXPECT_TRUE(events.contains(note + "\n") || events.contains(note + "\r\n"))
        << events.toStdString();
}

/* A pick core refuses (under ?poll=0x2F none of the ranked PIDs is asked)
 * leaves the session asking its whole set, and says why; a tick that
 * gives the pick something to ask is taken. The lines say a polled
 * session asks its polled signals, never the set */
TEST(BridgeTest, APickCoreRefusesAsksTheWholeSet)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral(
            "j2534://%1?profile=obd2&poll=0x2F&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Asking every polled signal: the focus leaves nothing to request: "
        "none of the picked signals is asked on this session")))
        << log.join('\n').toStdString();
    ASSERT_TRUE(
        spinUntil([]() { return onlyThese(nextPids(), {0x2F}); }, 15000, 10));

    SignalListModel* model = bridge.signalModel();
    model->toggleSubscribed(modelRowOf(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return lineStarting(log, QStringLiteral(
                                         "Picked 15 signals; ?poll= asks only "
                                         "the polled ones among them: ")) >= 0;
        },
        5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(
        spinUntil([]() { return onlyThese(nextPids(), {0x2F}); }, 15000, 10));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A pick the walk leaves with nothing to ask (the ECU lacks every ranked
 * PID) gives way to the whole set, with core's words, and leaves no row
 * unasked */
TEST(BridgeTest, APickLeftWithNothingToAskAsksTheWholeSet)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    // Past the page's 31 characters: the environment, which the surrogate
    // child takes at its spawn
    EnvRestore restore("ECUIO_FAKE_WITHHELD_PIDS");
    qputenv("ECUIO_FAKE_WITHHELD_PIDS",
            "0C,05,0F,10,5C,0D,04,0E,06,07,0B,42,49,44");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=obd2&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_GE(lineStarting(log, QStringLiteral("Asking 14 signals: ")), 0)
        << log.join('\n').toStdString();
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral(
                "Asking every signal: the focus leaves nothing to request: "
                "none of the picked signals is asked on this session"));
        },
        15000))
        << log.join('\n').toStdString();
    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil([&]() { return model->unaskedCount() == 0; }, 5000));
    EXPECT_FALSE(unaskedRow(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* ADR-0051 section 3: a DID the fake ECU holds no reply for (it answers
 * 7F 22 31) shows on Live as refused, with the code by name, and the one
 * it answers takes its value; neither is hidden */
TEST(BridgeTest, LiveSaysWhichDidsTheFakeEcuRefuses)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://"
                                       "%1?profile=uds-generic&did=1E1C:u16,"
                                       "F1FE:u16&identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    const auto rowNamed = [model](const QString& name) {
        for (int r = 0; r < model->rowCount(); ++r) {
            if (model->data(model->index(r), SignalListModel::NameRole)
                    .toString() == name) {
                return r;
            }
        }
        return -1;
    };
    const int answered = rowNamed(QStringLiteral("DID 1E1C"));
    const int refused = rowNamed(QStringLiteral("DID F1FE"));
    ASSERT_GE(answered, 0);
    ASSERT_GE(refused, 0);
    const auto words = [model](int row) {
        return model->data(model->index(row), SignalListModel::RefusedRole)
            .toString();
    };
    ASSERT_TRUE(spinUntil([&]() { return !words(refused).isEmpty(); }, 5000))
        << "DID F1FE never refused";
    ASSERT_TRUE(spinUntil(
        [&]() {
            return model
                ->data(model->index(answered), SignalListModel::HasValueRole)
                .toBool();
        },
        5000))
        << "DID 1E1C never answered";
    EXPECT_EQ(words(refused),
              QStringLiteral("Refused: requestOutOfRange (7F 22 31)"));
    EXPECT_EQ(words(answered), QString());
    EXPECT_EQ(model->hiddenCount(), 0) << "a refused row stays on Live";
    EXPECT_EQ(bridge.refusedUnnamed(), QString()) << "every refusal named";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Ten DIDs the fake refuses: the report lists eight, whose rows say so,
 * and Live's status says the two more it can't name; a new connect
 * starts with none */
TEST(BridgeTest, LiveCountsTheRefusalsItCantName)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList dids;
    for (int n = 0xF0; n < 0xFA; ++n) {
        dids << QStringLiteral("F1%1")
                        .arg(n, 2, 16, QLatin1Char('0'))
                        .toUpper() +
                    QStringLiteral(":u16");
    }
    const QString uri =
        QStringLiteral(
            "j2534://%1?profile=uds-generic&did=%2&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH),
                 dids.join(QLatin1Char(',')));
    int changed = 0;
    QObject::connect(&bridge, &SessionBridge::refusedUnnamedChanged,
                     [&changed]() { ++changed; });
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    // The two refusals past the list can land on either side of a 100 ms
    // tick, so the text may read "1 more request" first: wait for both
    const QString both = QStringLiteral("ECU 0x7E8 refused 2 more requests");
    ASSERT_TRUE(
        spinUntil([&]() { return bridge.refusedUnnamed() == both; }, 5000))
        << "refusals counted past the list: "
        << bridge.refusedUnnamed().toStdString();
    EXPECT_GE(changed, 1);
    SignalListModel* model = bridge.signalModel();
    int named = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        named += model->data(model->index(r), SignalListModel::RefusedRole)
                         .toString()
                         .isEmpty()
                     ? 0
                     : 1;
    }
    EXPECT_EQ(named, ECUIO_SUPPORT_MAX_DROPPED);
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    EXPECT_EQ(bridge.refusedUnnamed(), QString())
        << "a new session starts with none";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A refusal is the session's: a module that refused a DID in the last
 * session refuses nothing in the next until it answers. Channel 0 (a
 * one-DID scan, found at once) reports first in both; in the second the
 * fake answers refusals with nothing, so the module on 0x7E9 never
 * reports, and its row must never read refused */
TEST(BridgeTest, ALastSessionsRefusalIsNotTheNextOnes)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://"
                                       "%1?profile=uds-generic&did=1E1C-1E1D,"
                                       "7E9:F1FE&identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    SignalListModel* model = bridge.signalModel();
    const auto rowNamed = [model](const QString& name) {
        for (int r = 0; r < model->rowCount(); ++r) {
            if (model->data(model->index(r), SignalListModel::NameRole)
                    .toString() == name) {
                return r;
            }
        }
        return -1;
    };
    const auto words = [model](int row) {
        return row < 0
                   ? QStringLiteral("no row")
                   : model
                         ->data(model->index(row), SignalListModel::RefusedRole)
                         .toString();
    };
    const auto answered = [model](int row) {
        return row >= 0 &&
               model->data(model->index(row), SignalListModel::HasValueRole)
                   .toBool();
    };

    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    int found = rowNamed(QStringLiteral("DID 1E1C"));
    int other = rowNamed(QStringLiteral("0x7E9 DID F1FE"));
    ASSERT_GE(other, 0);
    ASSERT_TRUE(spinUntil([&]() { return !words(other).isEmpty(); }, 5000))
        << "the module on 0x7E9 never refused its DID F1FE";
    ASSERT_TRUE(disconnectAndWait(bridge));

    FakeEnvArm silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    found = rowNamed(QStringLiteral("DID 1E1C"));
    other = rowNamed(QStringLiteral("0x7E9 DID F1FE"));
    ASSERT_GE(other, 0);
    bool everRefused = false;
    const auto watch = [&]() {
        everRefused = everRefused || !words(other).isEmpty();
        return false;
    };
    EXPECT_TRUE(spinUntil(
        [&]() {
            (void)watch();
            return answered(found);
        },
        5000))
        << "channel 0's scan never found DID 1E1C";
    (void)spinUntil(watch, 1000);
    EXPECT_FALSE(everRefused)
        << "the last session's refusal: " << words(other).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

#endif  // _WIN32
