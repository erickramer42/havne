// File/layer: tests/test_bridge_conditions_firing.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_conditions_support.h"

// A save writes the window's firings before its attach, and a handler of
// those lines runs between the two: a firing the worker gives then, after
// the flush and before core has the new set, is still a line
TEST(BridgeTest, AFiringAfterAnAttachsFlushIsStillALine)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    const ecuio_condition_event_t late[] = {
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 260, 3000000),
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 12, 2000000),
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 13, 2500000),
    };
    QObject::connect(
        &bridge, &SessionBridge::logMessage, [&](const QString& line) {
            log << line;
            if (line == QStringLiteral("Alert: Engine stopped, 777 RPM")) {
                bridge.dispatchConditionsForTesting(late, 3);
            }
        });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://flushed")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 1u);

    const ecuio_condition_event_t staged =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 777);
    bridge.dispatchConditionsForTesting(&staged, 1);
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 2u);
    ASSERT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 777 RPM")))
        << "the save's flush, whose handler gave the late firings";
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Condition: Moving, 260 km/h"));
        },
        5000))
        << log.join(u'\n').toStdString();
    const qsizetype twice =
        log.indexOf(QStringLiteral("Alert: Engine stopped fired 2 times"));
    EXPECT_GE(twice, 0) << "counted as the set before counts: "
                        << log.join(u'\n').toStdString();
    EXPECT_LT(twice, log.indexOf(QStringLiteral("Condition: Moving, 260 km/h")))
        << "in the order they fired";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

// The session's worker parked in a frame of the mock's telemetry: it has
// taken the set it judges the frame by and has not judged it yet
// (tests/worker_hold.h's hold, on one ID and from one frame to the next).
// Declared before the bridge, with a Release after it
struct TelemetryHold {
    std::mutex m;
    std::condition_variable cv;
    bool armed = false;
    int parks = 0;  // the frames parked so far
    int passed = 0; // the parked frames let go

    static void onFrame(const ecuio_frame_t* frame, void* user)
    {
        auto* h = static_cast<TelemetryHold*>(user);
        if (frame->arbitration_id != ECUIO_MOCK_TELEMETRY_ARB_ID) {
            return;
        }
        std::unique_lock<std::mutex> lock(h->m);
        if (!h->armed) {
            return;
        }
        const int mine = ++h->parks;
        h->cv.notify_all();
        h->cv.wait(lock, [h, mine] { return !h->armed || h->passed >= mine; });
    }
    // True once the worker is parked in a telemetry frame, within ms: its
    // next, or with one parked already the one after it
    bool hold(int ms)
    {
        std::unique_lock<std::mutex> lock(m);
        armed = true;
        passed = parks;
        const int want = parks + 1;
        cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::milliseconds(ms),
                         [this, want] { return parks >= want; })) {
            armed = false;
            cv.notify_all();
            return false;
        }
        return true;
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(m);
        armed = false;
        cv.notify_all();
    }
    struct Release {
        TelemetryHold& h;
        ~Release() { h.release(); }
    };
};

// One alert, true through the mock's KeyOn: it fires at the first
// telemetry frame judged by it
QByteArray stoppedConditions()
{
    return QByteArray(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"stopped","name":"Engine stopped",)"
        R"("when":{"signal":["id:1001"],"unit":"RPM","below":300},)"
        R"("alert":{"level":"danger","sound":true}}]})");
}

// A session whose worker is parked in the frame that fires
// stoppedConditions (generation 1), which it has taken and not judged
struct HeldFiring {
    SettingsDirScope dir;
    TelemetryHold hold;
    QStringList log;
    SessionBridge bridge;
    TelemetryHold::Release unhold{hold};

    bool start()
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
        if (!dir.valid() ||
            !bridge.connectSession(QStringLiteral("mock://held")).isEmpty() ||
            ecuio_register_frame_callback(bridge.sessionForTesting(),
                                          &TelemetryHold::onFrame,
                                          &hold) != ECUIO_OK ||
            // Parked with no set: the attach is whole before the next frame
            !hold.hold(5000) ||
            !bridge.saveConditions(QString::fromUtf8(stoppedConditions()))
                 .value(QStringLiteral("ok"))
                 .toBool() ||
            bridge.conditionsGenerationForTesting() != 1u) {
            return false;
        }
        // The next telemetry frame: the first the set judges, taken with it
        return hold.hold(5000);
    }
    bool fired() const
    {
        return log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM"));
    }
    uint64_t danger() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_DANGER);
    }
};

// ecuio_session_test_after_conditions_handover for a scope
struct AfterHandover {
    AfterHandover(void (*hook)(void*), void* user)
    {
        ecuio_session_test_after_conditions_handover(hook, user);
    }
    ~AfterHandover()
    {
        ecuio_session_test_after_conditions_handover(nullptr, nullptr);
    }
};

} // namespace

// The worker's dispatch under way at a re-attach took the set before: it
// judges its frame by that set and gives the firing once core's call has
// returned. The firing is that set's line and tone
TEST(BridgeTest, AFiringTheWorkerGivesAfterAReattachIsStillALineAndATone)
{
    HeldFiring run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), 2u);
    spinLoop(300);
    ASSERT_FALSE(run.fired()) << "the worker is held: nothing judged yet";
    EXPECT_EQ(run.danger(), 0u);

    run.hold.release();
    EXPECT_TRUE(spinUntil([&run]() { return run.fired(); }, 5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "the set before's alert sounds";
    EXPECT_TRUE(spinUntil(
        [&run]() {
            return !linesStarting(run.log, QStringLiteral("Alert: Second set"))
                        .isEmpty();
        },
        5000))
        << "the set attached is judged from the next frame: "
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(
        linesStarting(run.log, QStringLiteral("Alert: Engine stopped")).size(),
        1);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// The same firing given while the attach is inside core's call, after the
// window's flush and before the call returns
TEST(BridgeTest, AFiringTheWorkerGivesDuringCoresAttachIsStillALine)
{
    // Before the session: the worker calls with it until the close
    struct Given {
        std::mutex m;
        std::condition_variable cv;
        bool given = false; // the bridge has the set before's firing
        bool inTime = false;
        TelemetryHold* hold = nullptr;
    } given;
    HeldFiring run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    given.hold = &run.hold;
    // Registered after the bridge's, so called once the bridge has the event
    const auto onConditions = [](const ecuio_condition_event_t* events,
                                 uint32_t count, void* user) {
        auto* g = static_cast<Given*>(user);
        for (uint32_t i = 0; i < count; ++i) {
            if (events[i].generation == 1 &&
                (events[i].flags & ECUIO_CONDITION_FIRED) != 0) {
                std::lock_guard<std::mutex> lock(g->m);
                g->given = true;
                g->cv.notify_all();
            }
        }
    };
    ASSERT_EQ(ecuio_register_conditions_callback(run.bridge.sessionForTesting(),
                                                 +onConditions, &given),
              ECUIO_OK);
    {
        // Inside core's call: the worker goes on and gives the firing
        AfterHandover inside(
            [](void* user) {
                auto* g = static_cast<Given*>(user);
                g->hold->release();
                std::unique_lock<std::mutex> lock(g->m);
                g->inTime = g->cv.wait_for(lock, std::chrono::seconds(5),
                                           [g] { return g->given; });
            },
            &given);
        ASSERT_TRUE(
            run.bridge.saveConditions(QString::fromUtf8(otherConditions()))
                .value(QStringLiteral("ok"))
                .toBool());
    }
    ASSERT_TRUE(given.inTime) << "the firing came before core's call returned";
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), 2u);
    ASSERT_FALSE(run.fired()) << "no tick since";
    EXPECT_TRUE(spinUntil([&run]() { return run.fired(); }, 5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// Each firing is a line of the activity log, and a note in the
// recording; several of one condition's in one window are one line, and
// a change that isn't a firing is none
TEST(BridgeTest, EachFiringIsAnActivityLine)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://firings")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(rowReads(bridge, 1, "on"));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM"));
        },
        5000));
    EXPECT_EQ(
        linesStarting(log, QStringLiteral("Alert: Engine stopped")).size(), 1)
        << "one firing, one line";
    EXPECT_EQ(
        linesStarting(log, QStringLiteral("Alert: Coolant cold, ")).size(), 1);

    const uint32_t g = bridge.conditionsGenerationForTesting();
    std::vector<ecuio_condition_event_t> window;
    for (int i = 0; i < 1000; ++i) {
        window.push_back(conditionEvent(0, g, ECUIO_CONDITION_FIRED, i));
        window.push_back(conditionEvent(0, g, 0, i)); // back OFF
    }
    window.push_back(conditionEvent(2, g, ECUIO_CONDITION_FIRED, 260));
    window.push_back(
        conditionEvent(1, g, ECUIO_CONDITION_STALE, 20)); // no firing
    bridge.dispatchConditionsForTesting(window.data(),
                                        static_cast<uint32_t>(window.size()));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(
                QStringLiteral("Alert: Engine stopped fired 1000 times"));
        },
        5000))
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Condition: Moving, 260 km/h")))
        << "a condition with no alert";
    EXPECT_EQ(linesStarting(log, QStringLiteral("Alert: Coolant cold")).size(),
              1)
        << "a STALE change isn't a firing";

    // In the order they fired, whatever their order in the set; a
    // condition's place is its window's first firing
    const ecuio_condition_event_t first[] = {
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 1, 2000000),
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 2, 3000000),
    };
    bridge.dispatchConditionsForTesting(first, 2);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Engine stopped, 2 RPM"));
        },
        5000));
    EXPECT_LT(log.indexOf(QStringLiteral("Condition: Moving, 1 km/h")),
              log.indexOf(QStringLiteral("Alert: Engine stopped, 2 RPM")));
    const ecuio_condition_event_t second[] = {
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 11, 4000000),
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 12, 5000000),
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 13, 6000000),
    };
    bridge.dispatchConditionsForTesting(second, 3);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Condition: Moving, 12 km/h"));
        },
        5000));
    EXPECT_LT(
        log.indexOf(QStringLiteral("Alert: Engine stopped fired 2 times")),
        log.indexOf(QStringLiteral("Condition: Moving, 12 km/h")));

    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QLatin1Char('/') + folders.first();
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains("Alert: Engine stopped, 0 RPM"))
        << "noted in the recording";
    EXPECT_TRUE(events.contains("Alert: Engine stopped fired 1000 times"));
    EXPECT_EQ(readFile(folder + QStringLiteral("/conditions.json")),
              mockConditions())
        << "the set the session judged";
}

// A recording holds its set (ADR-0063 section 6): an edit saved while one
// runs waits for its end, then is judged; a disconnect ends the wait too
TEST(BridgeTest, AnEditDuringARecordingWaitsForItsEnd)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://edit")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(bridge.startLog());

    QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(otherConditions()));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(),
              QStringLiteral("applies when this recording ends"));
    EXPECT_EQ(bridge.conditionsPending(),
              QStringLiteral("applies when this recording ends"));
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u)
        << "core would refuse it";
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3) << "the set judged";
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(otherConditions()))
        << "saved";
    EXPECT_TRUE(
        log.contains(QStringLiteral("Alerts saved: 1 condition; the change "
                                    "applies when this recording ends")));

    bridge.stopLog();
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 2u);
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    EXPECT_EQ(readFile(root.path() + QLatin1Char('/') + folders.first() +
                       QStringLiteral("/conditions.json")),
              mockConditions())
        << "the recording kept its set";

    // A disconnect ends a recording's wait: the saved set, offline
    ASSERT_TRUE(bridge.startLog());
    saved = bridge.saveConditions(QString::fromUtf8(mockConditions()));
    EXPECT_FALSE(saved.value(QStringLiteral("pending")).toString().isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
    EXPECT_FALSE(conditionRow(bridge.conditionModel(), 0)
                     .value(QStringLiteral("live"))
                     .toBool());
}

// A replay is judged by its recording's own conditions.json while it
// matches the header's hash (ADR-0063 decision 12); an edit then waits for
// the replay's end, and a copy changed since is passed by for the saved set
TEST(BridgeTest, AReplayJudgesByItsRecordingsCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://recorded")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 10; }, 5000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);

    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantList rows = bridge.recentRecordings(1);
    ASSERT_EQ(rows.size(), 1);
    const QString uri =
        rows.first().toMap().value(QStringLiteral("uri")).toString();
    const QString folder =
        rows.first().toMap().value(QStringLiteral("folder")).toString();

    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_TRUE(bridge.conditionsFromRecording());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Alerts: judging the recording's own 3 conditions")))
        << log.join(u'\n').toStdString();
    ASSERT_EQ(bridge.conditionModel()->rowCount(), 3);
    EXPECT_EQ(conditionRow(bridge.conditionModel(), 1)
                  .value(QStringLiteral("key"))
                  .toString(),
              QStringLiteral("cold"));
    ASSERT_TRUE(rowReads(bridge, 0, "on")) << "the replay fires what fired";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM")));

    const QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(otherConditions()));
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(),
              QStringLiteral("applies when this replay is disconnected"));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.conditionsFromRecording());
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1) << "the saved set's rows";

    // A copy changed since the recording: the saved set, and a line
    {
        QFile copy(folder + QStringLiteral("/conditions.json"));
        ASSERT_TRUE(copy.open(QIODevice::Append));
        copy.write(" ");
    }
    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_FALSE(bridge.conditionsFromRecording());
    const qsizetype unmatched = log.indexOf(QStringLiteral(
        "Replay: the recording's conditions.json doesn't match its hash; "
        "judging the saved conditions"));
    EXPECT_GE(unmatched, 0) << log.join(u'\n').toStdString();
    const QStringList connected =
        linesStarting(log, QStringLiteral("Connected: "));
    ASSERT_EQ(connected.size(), 1);
    EXPECT_GT(unmatched, log.indexOf(connected.first()))
        << "with the connect's lines";
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));

    // With no saved set, the line says none is judged
    {
        SettingsDirScope none;
        ASSERT_TRUE(none.valid());
        SessionBridge fresh;
        fresh.setRecordingsRootForTesting(root.path());
        QStringList lines;
        QObject::connect(&fresh, &SessionBridge::logMessage,
                         [&lines](const QString& line) { lines << line; });
        ASSERT_TRUE(fresh.connectSession(uri).isEmpty());
        EXPECT_TRUE(lines.contains(QStringLiteral(
            "Replay: the recording's conditions.json doesn't match its "
            "hash; judging none")))
            << lines.join(u'\n').toStdString();
        EXPECT_TRUE(linesStarting(lines, QStringLiteral("Alerts")).isEmpty());
        EXPECT_EQ(fresh.conditionModel()->rowCount(), 0);
        ASSERT_TRUE(disconnectAndWait(fresh));
    }

    // A copy that matches but isn't judged: the saved set in its place
    {
        QFile copy(folder + QStringLiteral("/conditions.json"));
        ASSERT_TRUE(copy.open(QIODevice::ReadWrite));
        copy.resize(copy.size() - 1);
    }
    bridge.failConditionsAttachesForTesting(1);
    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_FALSE(bridge.conditionsFromRecording());
    EXPECT_FALSE(
        linesStarting(log, QStringLiteral("Replay: the recording's "
                                          "conditions.json isn't judged ("))
            .isEmpty())
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);
    ASSERT_TRUE(disconnectAndWait(bridge));
}
