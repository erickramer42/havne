#include "test_bridge_fake_support.h"

#ifdef _WIN32

namespace {

// Six modules at 7E9 to 7EE; s1 to s3 ranked, the rest asked when held
const char kRoomFile[] =
    R"({"havne_signals":1,"name":"Room","source":{"name":"t"},"ecus":[)"
    R"({"id":"m1","rx":"7E9","name":"M1"},{"id":"m2","rx":"7EA","name":"M2"},)"
    R"({"id":"m3","rx":"7EB","name":"M3"},{"id":"door","rx":"7EC"},)"
    R"({"id":"m5","rx":"7ED","name":"M5"},{"id":"m6","rx":"7EE","name":"M6"}],)"
    R"("signals":[)"
    R"({"key":"s1","name":"N s1","ecu":"m1","read":{"service":"22","number":"1000"},"field":{"byte":0},"plot":1},)"
    R"({"key":"s1b","name":"N s1b","ecu":"m1","read":{"service":"22","number":"1009"},"field":{"byte":0}},)"
    R"({"key":"s2","name":"N s2","ecu":"m2","read":{"service":"22","number":"1001"},"field":{"byte":0},"plot":2},)"
    R"({"key":"s2b","name":"N s2b","ecu":"m2","read":{"service":"22","number":"1007"},"field":{"byte":0}},)"
    R"({"key":"s3","name":"N s3","ecu":"m3","read":{"service":"22","number":"1002"},"field":{"byte":0},"plot":3},)"
    R"({"key":"s3b","name":"N s3b","ecu":"m3","read":{"service":"22","number":"1006"},"field":{"byte":0}},)"
    R"({"key":"s4","name":"N s4","ecu":"door","read":{"service":"22","number":"1003"},"field":{"byte":0}},)"
    R"({"key":"s5","name":"N s5","ecu":"m5","read":{"service":"22","number":"1004"},"field":{"byte":0}},)"
    R"({"key":"s5b","name":"N s5b","ecu":"m5","read":{"service":"22","number":"1008"},"field":{"byte":0}}]})";
const char kRoomDids[] =
    "7E9:1000=11;7E9:1009=19;7EA:1001=12;7EA:1007=17;7EB:1002=13;"
    "7EB:1006=16;7EC:1003=14;7ED:1004=15;7ED:1008=18";
const QStringList kKeys{"s1",  "s1b", "s2", "s2b", "s3",
                        "s3b", "s4",  "s5", "s5b"};

class EnvScope
{
public:
    EnvScope(const char* name, const char* value)
        : m_name(name), m_before(qgetenv(name)),
          m_had(qEnvironmentVariableIsSet(name))
    {
        qputenv(name, value);
    }
    ~EnvScope()
    {
        if (m_had) {
            qputenv(m_name, m_before);
        } else {
            qunsetenv(m_name);
        }
    }
    EnvScope(const EnvScope&) = delete;
    EnvScope& operator=(const EnvScope&) = delete;

private:
    const char* m_name;
    QByteArray m_before;
    bool m_had;
};

struct RoomCase {
    SettingsDirScope dir;
    FakeHooks hooks;
    EnvScope dids{"ECUIO_FAKE_MODULE_DIDS", kRoomDids};
    FakeEnvArm gating{"ECUIO_FAKE_FILTER_GATING", "1"};
    FakeEnvArm max;
    SessionBridge bridge;
    QStringList log;

    explicit RoomCase(const char* filterMax)
        : max("ECUIO_FAKE_FILTER_MAX", filterMax)
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& l) { log << l; });
    }
    ~RoomCase()
    {
        disconnectAndWait(bridge);
        removeEveryPlugin(bridge);
    }

    bool connect()
    {
        const QVariantMap kept =
            bridge.keepSignalText(QString::fromUtf8(kRoomFile), {}, {}, {});
        const QString profile =
            kept.value(QStringLiteral("profiles")).toStringList().value(0);
        const QString err = bridge.connectSession(
            QStringLiteral("j2534://%1?profile=%2&identify=0&diagnose=0")
                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH), profile));
        EXPECT_TRUE(err.isEmpty()) << err.toStdString();
        return err.isEmpty();
    }
    int row(const QString& key) const
    {
        return static_cast<int>(kKeys.indexOf(key));
    }
    QString why(const QString& key) const
    {
        SignalListModel* m = bridge.signalModel();
        return m->data(m->index(row(key)), SignalListModel::UnaskedWhyRole)
            .toString();
    }
    void tick(const QString& key)
    {
        bridge.signalModel()->toggleSubscribed(row(key));
    }
    QStringList noRoom(const QVariantMap& r) const
    {
        QStringList names;
        for (const QVariant& m : r.value(QStringLiteral("noRoom")).toList()) {
            names << m.toMap().value(QStringLiteral("name")).toString();
        }
        return names;
    }
    QStringList freeable(const QVariantMap& r) const
    {
        QStringList names;
        for (const QVariant& m : r.value(QStringLiteral("freeable")).toList()) {
            names << m.toMap().value(QStringLiteral("name")).toString();
        }
        return names;
    }
    // Waits for the room the adapter gives to settle on `count` modules
    QVariantMap waitNoRoom(int count, const QVariantList& pages = {})
    {
        QVariantMap r;
        spinUntil(
            [&] {
                r = bridge.roomHolders(pages);
                return noRoom(r).size() == count;
            },
            8000);
        return r;
    }
};

const QVariantList kPages{
    QVariantMap{{QStringLiteral("name"), QStringLiteral("Track")},
                {QStringLiteral("cells"),
                 QVariantList{QStringList{QStringLiteral("key:s5")}}}},
    QVariantMap{{QStringLiteral("name"), QStringLiteral("Home")},
                {QStringLiteral("cells"),
                 QVariantList{QStringList{QStringLiteral("key:s5")},
                              QStringList{QStringLiteral("key:s5b")},
                              QStringList{QStringLiteral("key:s2b")},
                              QStringList{QStringLiteral("key:s1b")}}}}};

// The room case at four filters: s1 to s3 ranked and s5 ticked hold all
// four, and s4's tick finds none
void fourHeldAndDoorWaiting(RoomCase& t)
{
    ASSERT_TRUE(t.connect());
    t.tick(QStringLiteral("s5"));
    t.tick(QStringLiteral("s1b"));
    ASSERT_TRUE(spinUntil(
        [&] { return t.bridge.roomHolders({}).value("budget").toInt() == 4; },
        8000));
    t.tick(QStringLiteral("s4"));
    ASSERT_EQ(t.noRoom(t.waitNoRoom(1)), QStringList{QStringLiteral("DOOR")});
}

} // namespace

// The adapter holds two: a ranked row whose module waits reads "room",
// counted apart from "Not asked", and an unheld row there reads "pick"
TEST(BridgeTest, NoRoomIsAHeldRowOnANoRoomModuleCountedApart)
{
    RoomCase t("2");
    ASSERT_TRUE(t.connect());
    const QStringList waiting = t.noRoom(t.waitNoRoom(1));
    ASSERT_EQ(waiting.size(), 1);
    const QString ranked = waiting.first() == QStringLiteral("M2")
                               ? QStringLiteral("s2")
                               : QStringLiteral("s3");
    const QString unheld = ranked + QLatin1Char('b');
    ASSERT_TRUE(spinUntil([&] { return t.why(ranked) == "room"; }, 5000))
        << waiting.first().toStdString() << ": " << t.why(ranked).toStdString();
    EXPECT_EQ(t.why(unheld).toStdString(), "pick");
    EXPECT_EQ(t.why(QStringLiteral("s1")).toStdString(), "");
    EXPECT_EQ(t.why(QStringLiteral("s4")).toStdString(), "pick");
    SignalListModel* m = t.bridge.signalModel();
    EXPECT_EQ(m->noRoomCount(), 1);
    EXPECT_EQ(m->unaskedCount(), 6)
        << "the six unranked rows, never the held one";
    EXPECT_EQ(m->noRoomNames(),
              QStringList{waiting.first() + QStringLiteral("'s N ") + ranked});
}

TEST(BridgeTest, RoomHoldersNeverOfferARangeConversationOrVisitModule)
{
    RoomCase t("4");
    ASSERT_NO_FATAL_FAILURE(fourHeldAndDoorWaiting(t));
    const QVariantMap r = t.bridge.roomHolders(kPages);
    EXPECT_FALSE(t.freeable(r).contains(QStringLiteral("M1")))
        << "M1 answers the emissions range, ticked or not";
}

TEST(BridgeTest, RoomHoldersNeverOfferAModuleHoldingARankedDef)
{
    RoomCase t("4");
    ASSERT_NO_FATAL_FAILURE(fourHeldAndDoorWaiting(t));
    const QVariantMap r = t.bridge.roomHolders(kPages);
    EXPECT_EQ(t.freeable(r), QStringList{QStringLiteral("M5")})
        << "M2's gauge can go, its ranked signal can't";
}

TEST(BridgeTest, RoomHoldersCountTicksGaugesByPageAndAlerts)
{
    RoomCase t("4");
    ASSERT_NO_FATAL_FAILURE(fourHeldAndDoorWaiting(t));
    const QVariantMap saved = t.bridge.saveConditions(QStringLiteral(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["key:s5"],"unit":"","above":200},)"
        R"("alert":{"level":"warning","banner":true}},)"
        R"({"key":"off","name":"Off","enabled":false,"when":{"signal":["key:s5b"],"unit":"","above":200},)"
        R"("alert":{"level":"warning","banner":true}},)"
        R"({"key":"mark","name":"Mark","when":{"signal":["key:s5"],"unit":"","above":200},"mark":true}]})"));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    const QVariantMap r = t.bridge.roomHolders(kPages);
    const QVariantList free = r.value(QStringLiteral("freeable")).toList();
    ASSERT_EQ(free.size(), 1);
    const QVariantMap m5 = free.first().toMap();
    EXPECT_EQ(m5.value(QStringLiteral("ticks")).toInt(), 1);
    EXPECT_EQ(m5.value(QStringLiteral("alerts")).toInt(), 1)
        << "a disabled alert and a mark hold nothing the user frees here";
    const QVariantList gauges = m5.value(QStringLiteral("gauges")).toList();
    ASSERT_EQ(gauges.size(), 2);
    EXPECT_EQ(gauges.at(0).toMap().value(QStringLiteral("count")).toInt(), 1);
    EXPECT_EQ(gauges.at(1).toMap().value(QStringLiteral("count")).toInt(), 2);
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "DOOR isn't read: the adapter listens to 4 modules at a time "
              "and all 4 are in use. To read it, free one: M5 (1 tick, "
              "1 gauge on Track, 2 gauges on Home, 1 alert).");
}

TEST(BridgeTest, ANoRoomModuleIsNamedByNameElseReplyId)
{
    RoomCase t("4");
    ASSERT_NO_FATAL_FAILURE(fourHeldAndDoorWaiting(t));
    const QString words =
        t.bridge.roomHolders(kPages).value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.startsWith(QStringLiteral("DOOR isn't read")))
        << words.toStdString();
    EXPECT_FALSE(words.contains(QStringLiteral("7E4")))
        << "never the request ID";
    EXPECT_EQ(t.why(QStringLiteral("s4")).toStdString(), "room");
    EXPECT_EQ(t.bridge.signalModel()->noRoomNames(),
              QStringList{QStringLiteral("DOOR's N s4")});
}

TEST(BridgeTest, RoomHoldersOfferNoneWhileALogRuns)
{
    RoomCase t("4");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    t.bridge.setRecordingsRootForTesting(root.path());
    ASSERT_NO_FATAL_FAILURE(fourHeldAndDoorWaiting(t));
    ASSERT_TRUE(t.bridge.startLog());
    const QVariantMap r = t.bridge.roomHolders(kPages);
    EXPECT_TRUE(r.value(QStringLiteral("freeable")).toList().isEmpty());
    EXPECT_TRUE(r.value(QStringLiteral("words"))
                    .toString()
                    .endsWith(QStringLiteral(
                        " The log keeps reading what it started with: stop "
                        "it to change what is read.")))
        << r.value(QStringLiteral("words")).toString().toStdString();
    t.bridge.stopLog();
}

TEST(BridgeTest, StartLogNamesTheSignalsWithNoRoom)
{
    RoomCase t("4");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    t.bridge.setRecordingsRootForTesting(root.path());
    ASSERT_NO_FATAL_FAILURE(fourHeldAndDoorWaiting(t));
    ASSERT_TRUE(spinUntil(
        [&] { return t.bridge.signalModel()->noRoomCount() == 1; }, 5000));
    ASSERT_TRUE(t.bridge.startLog());
    EXPECT_TRUE(t.log.contains(QStringLiteral(
        "Logging 1 signal the adapter has no room for: DOOR's N s4: their "
        "columns stay empty")))
        << t.log.join(QLatin1Char('\n')).toStdString();
    t.bridge.stopLog();
}

TEST(BridgeTest, ChannelSupportTextSaysClosedAndNoRoom)
{
    ecuio_channel_support_t s{};
    s.rx_id = 0x7EC;
    s.flags = ECUIO_SUPPORT_CLOSED;
    EXPECT_TRUE(SessionBridge::channelSupportText(s).contains(
        QStringLiteral("ECU 0x7EC: closed: nothing read on it; ")));
    s.flags = ECUIO_SUPPORT_NO_ROOM;
    EXPECT_TRUE(SessionBridge::channelSupportText(s).contains(QStringLiteral(
        "ECU 0x7EC: no room: every module the adapter listens to is in use; ")));
}

#endif // _WIN32
