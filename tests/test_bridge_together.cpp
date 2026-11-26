#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <string>
#include <vector>

#include "sessionbridge.h"
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>
#include "bridge_support.h"

namespace {

QByteArray part(const QString& name, const QString& key, const QString& did,
                bool obd2 = false)
{
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},%4)"
               R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
               R"({"key":"%2","name":"N %2","read":{"service":"22","number":"%3"},)"
               R"("field":{"byte":0,"bits":8}}]})")
        .arg(name, key, did,
             obd2 ? QStringLiteral(R"("extends":"obd2",)") : QString())
        .toUtf8();
}

QString str(const QVariantMap& m, const char* key)
{
    return m.value(QLatin1String(key)).toString();
}

struct Together {
    SettingsDirScope dir;
    SessionBridge bridge;
    QStringList log;
    Together()
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
    }
    ~Together() { removeEveryPlugin(bridge); }
    // The profile of a plugin kept from text, "" refused
    QString keep(const QByteArray& text)
    {
        const QVariantMap r =
            bridge.keepSignalText(QString::fromUtf8(text), {}, {}, {});
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << str(r, "words").toStdString();
        const QStringList p =
            r.value(QStringLiteral("profiles")).toStringList();
        return p.isEmpty() ? QString() : p.first();
    }
    QString shaOf(const QString& profile)
    {
        for (const QVariant& row : bridge.pickRow({profile}, false)) {
            return row.toMap().value(QStringLiteral("sha256")).toString();
        }
        return {};
    }
    int lines(const QString& start) const
    {
        int n = 0;
        for (const QString& l : log) {
            n += l.startsWith(start) ? 1 : 0;
        }
        return n;
    }
};

const ecuio_signal_set_t* setOf(const QString& profile)
{
    const ecuio_signal_set_t* set = nullptr;
    const QByteArray name = profile.toUtf8();
    return ecuio_profile_lookup(name.constData(), &set) == ECUIO_OK ? set
                                                                    : nullptr;
}

QStringList partProfiles(const QVariantMap& answer)
{
    QStringList out;
    for (const QVariant& p : answer.value(QStringLiteral("parts")).toList()) {
        out << p.toMap().value(QStringLiteral("profile")).toString();
    }
    return out;
}

} // namespace

TEST(BridgeTest, StorePicksObd2WithNoPluginAndPastSixteenVariants)
{
    Together t;
    const QString real = t.keep(part(QStringLiteral("Real"), "a", "D100"));
    QStringList picks{QStringLiteral("obd2")};
    for (int i = 0; i < PluginStore::MaxPicks - 1; ++i) {
        picks << QStringLiteral("plugin.p%1.base").arg(i);
    }
    QVariantMap r = t.bridge.pickVariant(picks, real);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << "obd2 isn't one of the 16: " << str(r, "words").toStdString();
    picks << QStringLiteral("plugin.p15.base");
    r = t.bridge.pickVariant(picks, real);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(str(r, "words").toStdString(),
              "A vehicle picks at most 16 plugin variants: unpick one first");
    picks.removeFirst();
    r = t.bridge.pickVariant(picks, QStringLiteral("obd2"));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << "obd2 needs no plugin and is taken past 16";
    EXPECT_EQ(r.value(QStringLiteral("picks")).toStringList().last(),
              QStringLiteral("obd2"));
}

TEST(BridgeTest, PickRowGivesObd2ABuiltInRow)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QVariantList row =
        t.bridge.pickRow({QStringLiteral("obd2"), a}, true);
    ASSERT_EQ(row.size(), 2);
    const QVariantMap obd2 = row.at(0).toMap();
    EXPECT_TRUE(obd2.value(QStringLiteral("builtIn")).toBool());
    EXPECT_FALSE(obd2.value(QStringLiteral("missing")).toBool());
    EXPECT_FALSE(obd2.value(QStringLiteral("standIn")).toBool());
    EXPECT_EQ(str(obd2, "name").toStdString(), "OBD-II standard signals");
    EXPECT_FALSE(row.at(1).toMap().value(QStringLiteral("builtIn")).toBool());
}

TEST(BridgeTest, TogetherObd2AloneReadsTheBuiltInSet)
{
    Together t;
    const QVariantMap r = t.bridge.vehicleSet({QStringLiteral("obd2")}, {});
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(str(r, "reads").toStdString(), "obd2");
    EXPECT_EQ(str(r, "profile").toStdString(), "obd2");
    EXPECT_EQ(t.bridge.togetherMergesForTesting(), 0) << "no merge of no part";
}

// Nothing picked reads obd2's modules alone, so the emissions codes are
// still read; Live has no signal
TEST(BridgeTest, TogetherNothingPickedReadsObd2sModulesAlone)
{
    Together t;
    const QVariantMap r = t.bridge.vehicleSet({}, {});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << str(r, "words").toStdString();
    EXPECT_EQ(str(r, "reads").toStdString(), "merged");
    EXPECT_EQ(str(r, "base").toStdString(), "modules");
    const ecuio_signal_set_t* set = setOf(str(r, "profile"));
    ASSERT_NE(set, nullptr) << "registered under its name";
    EXPECT_EQ(set->count, 0u);
    ASSERT_GE(set->channel_count, 1u);
    EXPECT_EQ(set->channels[0].rx_id, 0x7E8u);
    EXPECT_TRUE(r.value(QStringLiteral("parts")).toList().isEmpty());
}

TEST(BridgeTest, TogetherAMissingPickRefusesNamingIt)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QVariantMap r = t.bridge.vehicleSet(
        {QStringLiteral("obd2"), a, QStringLiteral("plugin.gone.track")}, {},
        QStringLiteral("Test car"));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(str(r, "reads").toStdString(), "refused");
    EXPECT_EQ(str(r, "words").toStdString(),
              "Test car picks gone track, which isn't installed here: install "
              "it or unpick it on Settings, Signals");
    EXPECT_EQ(r.value(QStringLiteral("missing")).toStringList(),
              QStringList{QStringLiteral("plugin.gone.track")});
    EXPECT_TRUE(str(r, "profile").isEmpty());
    EXPECT_EQ(t.bridge.togetherMergesForTesting(), 0)
        << "the rest isn't read in its place";
}

// Unpicked obd2 keeps its module (Diagnostics reads 7E8) and none of its
// signals (Live shows the part's alone)
TEST(BridgeTest, TogetherObd2UnpickedKeepsItsModuleAndNoSignal)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QVariantMap r = t.bridge.vehicleSet({a}, {});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << str(r, "words").toStdString();
    EXPECT_EQ(str(r, "base").toStdString(), "modules");
    const ecuio_signal_set_t* set = setOf(str(r, "profile"));
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->count, 1u) << "no obd2 signal";
    ASSERT_GE(set->channel_count, 1u);
    EXPECT_EQ(set->channels[0].rx_id, 0x7E8u);
    ASSERT_TRUE(t.bridge
                    .connectSession(QStringLiteral("mock://t?profile=") +
                                    str(r, "profile"))
                    .isEmpty());
    EXPECT_EQ(t.bridge.signalModel()->rowCount(), 1);
    EXPECT_TRUE(
        spinUntil([&t] { return !t.bridge.diagnostics().isEmpty(); }, 5000))
        << "the emissions codes are read";
    EXPECT_TRUE(disconnectAndWait(t.bridge));
}

TEST(BridgeTest, TogetherObd2PickedSetsTheBaseWithNoPartExtendingIt)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QVariantMap r = t.bridge.vehicleSet({QStringLiteral("obd2"), a}, {});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << str(r, "words").toStdString();
    EXPECT_EQ(str(r, "base").toStdString(), "obd2");
    const ecuio_signal_set_t* set = setOf(str(r, "profile"));
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->count, ecuio_obd2_signal_set()->count + 1);
}

TEST(BridgeTest, TogetherKeysTheCacheOnTheBaseFlag)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QVariantMap with =
        t.bridge.vehicleSet({QStringLiteral("obd2"), a}, {});
    const QVariantMap without = t.bridge.vehicleSet({a}, {});
    EXPECT_EQ(t.bridge.togetherMergesForTesting(), 2);
    EXPECT_NE(str(with, "profile"), str(without, "profile"));
    EXPECT_EQ(str(without, "base").toStdString(), "modules");
}

TEST(BridgeTest, TogetherMergesThePicksWithTheOwnVariantLast)
{
    Together t;
    const QString own = t.keep(part(QStringLiteral("Mine"), "m", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QVariantMap r = t.bridge.vehicleSet({own, b}, own);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << str(r, "words").toStdString();
    EXPECT_EQ(partProfiles(r), (QStringList{b, own}));
    const QVariantList parts = r.value(QStringLiteral("parts")).toList();
    EXPECT_TRUE(parts.at(1).toMap().value(QStringLiteral("own")).toBool());
    EXPECT_FALSE(parts.at(0).toMap().value(QStringLiteral("own")).toBool());
}

TEST(BridgeTest,
     TogetherRegistersTheMergedProfileAndAMockSessionDecodesEachPart)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QVariantMap r = t.bridge.vehicleSet({a, b}, {});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << str(r, "words").toStdString();
    const QString profile = str(r, "profile");
    EXPECT_TRUE(profile.startsWith(QStringLiteral("merged.")));
    EXPECT_TRUE(registered(profile.toUtf8().constData()));
    const QVariantMap totals = r.value(QStringLiteral("totals")).toMap();
    EXPECT_EQ(totals.value(QStringLiteral("plugins")).toInt(), 2);
    EXPECT_EQ(totals.value(QStringLiteral("signals")).toInt(), 2);
    EXPECT_EQ(t.lines(QStringLiteral("Read together: 2 plugins, 1 module, "
                                     "2 signals, 0 notes (") +
                      profile + QLatin1Char(')')),
              1);
    ASSERT_TRUE(
        t.bridge.connectSession(QStringLiteral("mock://t?profile=") + profile)
            .isEmpty());
    EXPECT_TRUE(spinUntil(
        [&t] {
            return !std::isnan(liveValue(t.bridge, QStringLiteral("N a"))) &&
                   !std::isnan(liveValue(t.bridge, QStringLiteral("N b")));
        },
        5000));
    EXPECT_TRUE(disconnectAndWait(t.bridge));
}

TEST(BridgeTest, TogetherAnswersTheSameKeyWithoutAMerge)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QVariantMap first = t.bridge.vehicleSet({a}, {});
    const QVariantMap again = t.bridge.vehicleSet({a}, {});
    EXPECT_EQ(t.bridge.togetherMergesForTesting(), 1);
    EXPECT_EQ(first, again);
    EXPECT_EQ(t.lines(QStringLiteral("Read together: ")), 1);
}

TEST(BridgeTest, TogetherMergesAgainWhenAPartsPluginIsUpdated)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QVariantMap first = t.bridge.vehicleSet({a}, {});
    const QVariantMap put = t.bridge.keepSignalText(
        QString::fromUtf8(part(QStringLiteral("Alpha"), "a2", "D101")),
        t.shaOf(a), QStringLiteral("base"), {});
    ASSERT_TRUE(put.value(QStringLiteral("ok")).toBool())
        << str(put, "words").toStdString();
    const QVariantMap again = t.bridge.vehicleSet({a}, {});
    EXPECT_EQ(t.bridge.togetherMergesForTesting(), 2);
    const ecuio_signal_set_t* set = setOf(str(again, "profile"));
    ASSERT_NE(set, nullptr);
    EXPECT_STREQ(set->defs[0].name, "N a2") << "the new version's signal";
}

// A session's set outlives a later merge until the session is torn down
TEST(BridgeTest, TogetherKeepsTheSessionsSetUntilTheTeardown)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QString first = str(t.bridge.vehicleSet({a, b}, {}), "profile");
    ASSERT_TRUE(
        t.bridge.connectSession(QStringLiteral("mock://t?profile=") + first)
            .isEmpty());
    const QString second = str(t.bridge.vehicleSet({a}, {}), "profile");
    EXPECT_NE(first, second);
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 1);
    EXPECT_TRUE(registered(first.toUtf8().constData()));
    EXPECT_TRUE(spinUntil(
        [&t] {
            return !std::isnan(liveValue(t.bridge, QStringLiteral("N b")));
        },
        5000))
        << "the session still decodes with its set";
    EXPECT_TRUE(disconnectAndWait(t.bridge));
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 0);
    EXPECT_FALSE(registered(first.toUtf8().constData()));
    EXPECT_TRUE(registered(second.toUtf8().constData()));
}

TEST(BridgeTest, TogetherRefusesAPartsUpdateWhileASessionReadsIt)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QString profile = str(t.bridge.vehicleSet({a, b}, {}), "profile");
    ASSERT_TRUE(
        t.bridge.connectSession(QStringLiteral("mock://t?profile=") + profile)
            .isEmpty());
    const QVariantMap put = t.bridge.keepSignalText(
        QString::fromUtf8(part(QStringLiteral("Alpha"), "a2", "D101")),
        t.shaOf(a), QStringLiteral("base"), {});
    EXPECT_FALSE(put.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(put.value(QStringLiteral("inUse")).toBool());
    const QVariantMap removed = t.bridge.removePlugin(t.shaOf(b));
    EXPECT_FALSE(removed.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(str(removed, "words").toStdString(),
              "A session decodes with it: disconnect, then remove it");
    EXPECT_TRUE(disconnectAndWait(t.bridge));
    EXPECT_TRUE(
        t.bridge.removePlugin(t.shaOf(b)).value(QStringLiteral("ok")).toBool())
        << "free once the session is gone";
}

namespace {

bool removed(Together& t, const QString& profile)
{
    return t.bridge.removePlugin(t.shaOf(profile))
        .value(QStringLiteral("ok"))
        .toBool();
}

} // namespace

TEST(BridgeTest, TogetherKeepsTheSetThroughTheTeardownUntilItsEnd)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QString first = str(t.bridge.vehicleSet({a, b}, {}), "profile");
    ASSERT_TRUE(
        t.bridge.connectSession(QStringLiteral("mock://t?profile=") + first)
            .isEmpty());
    t.bridge.disconnectSession();
    ASSERT_TRUE(t.bridge.disconnecting());
    const QString second = str(t.bridge.vehicleSet({a}, {}), "profile");
    EXPECT_TRUE(registered(first.toUtf8().constData()))
        << "the teardown's thread still decodes with it";
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 1);
    EXPECT_FALSE(removed(t, b)) << "its parts stay busy until the end";
    ASSERT_TRUE(
        spinUntil([&t] { return !t.bridge.disconnecting(); }, 5000, 10));
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 0);
    EXPECT_FALSE(registered(first.toUtf8().constData()));
    EXPECT_TRUE(registered(second.toUtf8().constData()));
    EXPECT_TRUE(removed(t, b)) << "free once the teardown ends";
}

TEST(BridgeTest, TogetherKeepsWhatAnAsyncConnectMayReadUntilItsCompletion)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QString first = str(t.bridge.vehicleSet({a, b}, {}), "profile");
    bool done = false;
    QString err = QStringLiteral("unset");
    QObject::connect(&t.bridge, &SessionBridge::connectResult,
                     [&](const QString&, const QString& e) {
                         err = e;
                         done = true;
                     });
    t.bridge.connectSessionAsync(QStringLiteral("mock://t?profile=") + first);
    ASSERT_TRUE(t.bridge.connecting());
    EXPECT_FALSE(removed(t, b)) << "busy from the connect's start";
    (void)t.bridge.vehicleSet({a}, {});
    (void)t.bridge.vehicleSet({b}, {});
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 2)
        << "nothing freed while the connect thread may look a name up";
    EXPECT_FALSE(removed(t, a)) << "the connected set's parts, not the last";
    ASSERT_TRUE(spinUntil([&done] { return done; }, 5000, 10));
    EXPECT_EQ(err.toStdString(), "");
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 1)
        << "the merge no session reads is freed at the completion";
    EXPECT_TRUE(registered(first.toUtf8().constData()));
    EXPECT_TRUE(disconnectAndWait(t.bridge));
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 0);
    EXPECT_FALSE(registered(first.toUtf8().constData()));
}

TEST(BridgeTest, TogetherKeepsTheSetASyncConnectsHandlerRetires)
{
    Together t;
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QString first = str(t.bridge.vehicleSet({a, b}, {}), "profile");
    int seen = -1;
    QObject::connect(&t.bridge, &SessionBridge::currentUriChanged, [&] {
        if (seen < 0) {
            (void)t.bridge.vehicleSet({a}, {});
            seen = registered(first.toUtf8().constData()) ? 1 : 0;
        }
    });
    ASSERT_TRUE(
        t.bridge.connectSession(QStringLiteral("mock://t?profile=") + first)
            .isEmpty());
    EXPECT_EQ(seen, 1) << "kept while the apply still runs";
    EXPECT_TRUE(registered(first.toUtf8().constData()));
    EXPECT_TRUE(disconnectAndWait(t.bridge));
}

TEST(BridgeTest, TogetherFileOfSetIsTheMergedFile)
{
    Together t;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString a = t.keep(part(QStringLiteral("Alpha"), "a", "D100"));
    const QString b = t.keep(part(QStringLiteral("Beta"), "b", "D200"));
    const QString profile = str(t.bridge.vehicleSet({a, b}, {}), "profile");
    const ecuio_signal_set_t* set = setOf(profile);
    ASSERT_NE(set, nullptr);
    const QByteArray text = t.bridge.signalFileTextOfSet(set);
    ASSERT_FALSE(text.isEmpty());
    EXPECT_TRUE(text.contains("\"parts\""));
    t.bridge.setRecordingsRootForTesting(root.path());
    t.bridge.setRecording(true);
    ASSERT_TRUE(
        t.bridge.connectSession(QStringLiteral("mock://t?profile=") + profile)
            .isEmpty());
    EXPECT_TRUE(disconnectAndWait(t.bridge));
    t.bridge.setRecording(false);
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    QFile copy(root.path() + QLatin1Char('/') + folders.first() +
               QStringLiteral("/signals.json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), text) << "the recording copies the merged file";
}

// A merge core refuses registers nothing, and the set before it, which no
// session reads, goes
TEST(BridgeTest, TogetherSaysCoresWordsForARefusalAndRegistersNothing)
{
    Together t;
    const QString small = t.keep(part(QStringLiteral("Small"), "s", "D100"));
    const QString before = str(t.bridge.vehicleSet({small}, {}), "profile");
    ASSERT_TRUE(registered(before.toUtf8().constData()));
    const auto many = [](const char* name, int first) {
        QString list;
        for (int i = 0; i < 520; ++i) {
            list += QStringLiteral(
                        R"(%1{"key":"k%2","name":"N %2","read":{"service":)"
                        R"("22","number":"%3"},"field":{"byte":0}})")
                        .arg(i == 0 ? QString() : QStringLiteral(","))
                        .arg(first + i)
                        .arg(QString::number(first + i, 16).toUpper());
        }
        return QStringLiteral(
                   R"({"havne_signals":1,"name":"%1","source":{"name":"t"},)"
                   R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[%2]})")
            .arg(QLatin1String(name), list)
            .toUtf8();
    };
    const QString one = t.keep(many("One", 0x1000));
    const QString two = t.keep(many("Two", 0x2000));
    const QVariantMap r = t.bridge.vehicleSet({one, two}, {});
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(str(r, "reads").toStdString(), "refused");
    EXPECT_TRUE(str(r, "words").contains(QStringLiteral("1024")))
        << str(r, "words").toStdString();
    EXPECT_TRUE(str(r, "profile").isEmpty());
    EXPECT_EQ(
        t.lines(QStringLiteral("Read together refused: ") + str(r, "words")),
        1);
    EXPECT_FALSE(registered(before.toUtf8().constData()))
        << "no stale set left registered";
    EXPECT_EQ(t.bridge.togetherRetiredForTesting(), 0);
}

TEST(BridgeTest, TogetherBesideGivesEachNotesBaseAndStandingKey)
{
    Together t;
    const QString old = t.keep(QByteArrayLiteral(
        R"({"havne_signals":2,"name":"Old","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"my_rpm","name":"RPM","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8}}]})"));
    const QVariantMap r =
        t.bridge.vehicleSet({QStringLiteral("obd2"), old}, {});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << str(r, "words").toStdString();
    const QVariantList beside = r.value(QStringLiteral("beside")).toList();
    ASSERT_EQ(beside.size(), 1);
    const QVariantMap n = beside.first().toMap();
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    std::vector<ecuio_signal_key_t> keys(obd2->count);
    uint32_t count = 0;
    ASSERT_EQ(
        ecuio_signal_set_keys(obd2, nullptr, keys.data(), obd2->count, &count),
        ECUIO_OK);
    const int base = n.value(QStringLiteral("baseDef")).toInt();
    ASSERT_GE(base, 0);
    ASSERT_LT(base, static_cast<int>(obd2->count));
    EXPECT_EQ(obd2->defs[base].id,
              static_cast<uint16_t>(ECUIO_OBD2_SIG_RPM_ID));
    EXPECT_EQ(str(n, "baseRef"),
              QStringLiteral("id:%1").arg(ECUIO_OBD2_SIG_RPM_ID));
    EXPECT_EQ(str(n, "baseTick").toStdString(), std::string(keys[base].text));
    EXPECT_EQ(str(n, "standingKey").toStdString(), "my_rpm");
    EXPECT_EQ(str(n, "name").toStdString(), "RPM");
    EXPECT_EQ(str(n, "part").toStdString(), "Old");
    const ecuio_signal_set_t* set = setOf(str(r, "profile"));
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(n.value(QStringLiteral("standing")).toInt(),
              static_cast<int>(obd2->count))
        << "the part's signal, after obd2's";
    const QVariantList folded = r.value(QStringLiteral("folded")).toList();
    ASSERT_EQ(folded.size(), 1);
    EXPECT_EQ(str(folded.first().toMap(), "kind").toStdString(), "besideBase");
}

#ifdef _WIN32

namespace {

// Old's RPM, at another address than obd2's, read beside obd2's ranked one
const QByteArray kOldRpm = QByteArrayLiteral(
    R"({"havne_signals":2,"name":"Old","source":{"name":"t"},)"
    R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
    R"({"key":"my_rpm","name":"RPM","read":{"service":"22","number":"D100"},)"
    R"("field":{"byte":0,"bits":8}}]})");

bool obd2RpmUnasked(SessionBridge& bridge)
{
    SignalListModel* m = bridge.signalModel();
    for (int r = 0; r < m->rowCount(); ++r) {
        const QModelIndex i = m->index(r);
        if (m->data(i, SignalListModel::IdRole).toInt() ==
            static_cast<int>(ECUIO_OBD2_SIG_RPM_ID)) {
            return m->data(i, SignalListModel::UnaskedRole).toBool();
        }
    }
    return false;
}

QString pickLine(const QStringList& log)
{
    for (const QString& l : log) {
        if (l.startsWith(QStringLiteral("Asking "))) {
            return l;
        }
    }
    return {};
}

QString fakeTogetherUri(const QString& profile)
{
    return QStringLiteral("j2534://%1?profile=%2&probe=0&identify=0&diagnose=0")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH), profile);
}

} // namespace

TEST(BridgeTest, DefaultPickLeavesOutAReplacedObd2DefNothingReads)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    Together t;
    const QString old = t.keep(kOldRpm);
    const QString profile =
        str(t.bridge.vehicleSet({QStringLiteral("obd2"), old}, {}), "profile");
    ASSERT_TRUE(t.bridge.connectSession(fakeTogetherUri(profile)).isEmpty());
    EXPECT_TRUE(pickLine(t.log).startsWith(QStringLiteral(
        "Asking 13 signals: Engine Load, Coolant Temp, Vehicle Speed")))
        << pickLine(t.log).toStdString();
    EXPECT_TRUE(spinUntil([&t] { return obd2RpmUnasked(t.bridge); }, 5000))
        << "obd2's RPM stayed asked beside Old's";
    EXPECT_TRUE(disconnectAndWait(t.bridge));
}

TEST(BridgeTest, DefaultPickKeepsAReplacedObd2DefATickOrGaugeAdds)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    Together t;
    const QString old = t.keep(kOldRpm);
    const QString profile =
        str(t.bridge.vehicleSet({QStringLiteral("obd2"), old}, {}), "profile");
    t.bridge.setPickGauges({QVariant(
        QStringList{QStringLiteral("id:%1").arg(ECUIO_OBD2_SIG_RPM_ID)})});
    ASSERT_TRUE(t.bridge.connectSession(fakeTogetherUri(profile)).isEmpty());
    EXPECT_TRUE(pickLine(t.log).startsWith(QStringLiteral(
        "Asking 14 signals: Engine Load, Coolant Temp, RPM, Vehicle Speed")))
        << pickLine(t.log).toStdString();
    spinLoop(500);
    EXPECT_FALSE(obd2RpmUnasked(t.bridge)) << "the gauge asks it";
    EXPECT_TRUE(disconnectAndWait(t.bridge));
}

#endif // _WIN32
