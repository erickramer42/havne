// File/layer: tests/test_bridge_consent.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: a plugin's consent at connect and its unlock (ADR-0066's
//          appendix of 2026-11-23, app/consentflow.*), over the fake J2534
//          DLL with the format-3 plugin test_core's conversation cases build
//          (conversation_session_support.h), in a settings folder per case

// Core's internal headers before Qt's: they name members slots and signal
#ifdef _WIN32
#include "conversation_session_support.h"
#endif

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include "sessionbridge.h"
#include "settings_home.h" // cli/: the CLI's store paths
#include "bridge_support.h"

#ifdef _WIN32
#include "fake_control.h"
#endif

namespace {

QString cleaned(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

} // namespace

// D7: havne-cli's default stores are the app's, with and without
// HAVNE_SETTINGS_DIR
TEST(BridgeTest, TheCliFindsTheAppsStores)
{
    SettingsDirScope scope;
    ASSERT_TRUE(scope.valid());
    for (const char* name : {"consent.json", "security.json"}) {
        EXPECT_EQ(cleaned(QString::fromStdString(cli::settings_file(name))),
                  cleaned(ConsentFlow::storePath(name)))
            << name;
    }
    const QByteArray before = qgetenv("HAVNE_SETTINGS_DIR");
    const QString org = QCoreApplication::organizationName();
    const QString app = QCoreApplication::applicationName();
    qunsetenv("HAVNE_SETTINGS_DIR");
    QCoreApplication::setOrganizationName(QStringLiteral("Havne"));
    QCoreApplication::setApplicationName(QStringLiteral("Havne"));
    const QString cli = QString::fromStdString(cli::settings_file("consent.json"));
    const QString ours = ConsentFlow::storePath("consent.json");
    QCoreApplication::setOrganizationName(org);
    QCoreApplication::setApplicationName(app);
    qputenv("HAVNE_SETTINGS_DIR", before);
    EXPECT_FALSE(ours.isEmpty());
    EXPECT_EQ(cleaned(cli), cleaned(ours));
}

// A handler that disconnects inside the apply's emits leaves the consent
// flow no session: an unlock after it never reaches the freed one
TEST(BridgeTest, AnUnlockAfterAHandlersDisconnectHasNoSession)
{
    SessionBridge bridge;
    bool once = false;
    QObject::connect(&bridge, &SessionBridge::connectedChanged,
                     [&](bool connected) {
                         if (connected && !once) {
                             once = true;
                             bridge.disconnectSession();
                         }
                     });
    EXPECT_TRUE(bridge.connectSession(QStringLiteral("mock://")).isEmpty());
    ASSERT_TRUE(once);
    ASSERT_TRUE(spinUntil([&]() { return !bridge.disconnecting(); }, 5000, 10));
    const QVariantMap r = bridge.consent()->unlock(0, 1);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              QStringLiteral("No such module"));
}

#ifdef _WIN32

namespace {

// The bridge, its log, and the plugin imported into the case's store
struct ConsentCase {
    SettingsDirScope scope;
    FakeHooks hooks;
    QTemporaryDir folder;
    QString profile;
    QString print; // 64 hex digits
    QStringList log;

    // made3() into the store bridge reads; false on a failure, said
    bool setUp(SessionBridge& bridge)
    {
        if (!scope.valid() || !folder.isValid() || !hooks.load()) {
            ADD_FAILURE() << "no scratch folder or fake page";
            return false;
        }
        watch(bridge);
        const QString path = folder.filePath(QStringLiteral("talker.havne-plugin.json"));
        QFile f(path);
        const std::string text = conversation_session::made3().json();
        if (!f.open(QIODevice::WriteOnly) ||
            f.write(text.data(), static_cast<qint64>(text.size())) !=
                static_cast<qint64>(text.size())) {
            ADD_FAILURE() << "the package didn't write";
            return false;
        }
        f.close();
        const QVariantMap entry = bridge.importPlugin(QUrl::fromLocalFile(path));
        const QStringList profiles = entry.value(QStringLiteral("profiles")).toStringList();
        if (!entry.value(QStringLiteral("ok")).toBool() || profiles.isEmpty()) {
            ADD_FAILURE() << entry.value(QStringLiteral("words")).toString().toStdString();
            return false;
        }
        profile = profiles.first();
        print = entry.value(QStringLiteral("fingerprint")).toString();
        EXPECT_TRUE(entry.value(QStringLiteral("conversation")).toBool());
        EXPECT_EQ(entry.value(QStringLiteral("publisher")).toString(),
                  QStringLiteral("Made-up Tools"));
        return true;
    }

    void watch(SessionBridge& bridge)
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
    }

    QString uri() const
    {
        return QStringLiteral("j2534://%1?profile=%2&identify=0&diagnose=0&gap=150")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH), profile);
    }

    bool logged(const QString& start) const
    {
        return std::any_of(log.begin(), log.end(), [&](const QString& l) {
            return l.startsWith(start);
        });
    }
};

bool waitsForConsent(SessionBridge& bridge)
{
    return spinUntil([&]() { return bridge.consent()->waiting(); }, 10000, 10);
}

bool connects(SessionBridge& bridge)
{
    return spinUntil([&]() { return bridge.connected() && !bridge.connecting(); },
                     10000, 10);
}

// Whether channel 0's module is talked to within ms
bool talks(SessionBridge& bridge, int ms)
{
    return spinUntil(
        [&]() {
            const QVariantList c = bridge.consent()->conversations();
            return !c.isEmpty() &&
                   c.first().toMap().value(QStringLiteral("state")).toString() ==
                       QLatin1String("talking");
        },
        ms, 20);
}

QVariantList recordsOf(SessionBridge& bridge)
{
    bridge.consent()->refresh();
    EXPECT_TRUE(spinUntil([&]() { return !bridge.consent()->busy(); }, 5000, 10));
    return bridge.consent()->records();
}

} // namespace

// Section 1 and C6: the connect waits unstarted for the answer; Decline
// and the limit both start it reading signals only, with nothing given
TEST(BridgeTest, AConnectWaitsForConsentThenStartsAsData)
{
    ConsentCase k;
    {
        SessionBridge bridge;
        ASSERT_TRUE(k.setUp(bridge));
        bridge.connectSessionAsync(k.uri());
        ASSERT_TRUE(waitsForConsent(bridge));
        EXPECT_TRUE(bridge.connecting());
        EXPECT_FALSE(bridge.connected());
        const QVariantMap ask = bridge.consent()->ask();
        EXPECT_EQ(ask.value(QStringLiteral("fingerprint")).toString(), k.print);
        EXPECT_EQ(ask.value(QStringLiteral("fingerprintShort")).toString().remove(' '),
                  k.print.left(16));
        EXPECT_EQ(ask.value(QStringLiteral("terms")).toString(),
                  QStringLiteral("Use at your own risk."));
        EXPECT_FALSE(ask.value(QStringLiteral("lines")).toList().isEmpty());
        EXPECT_TRUE(ask.value(QStringLiteral("changed")).toMap().isEmpty());
        bridge.consent()->answer(false);
        ASSERT_TRUE(connects(bridge));
        EXPECT_FALSE(talks(bridge, 1500));
        EXPECT_TRUE(k.logged(QStringLiteral("Consent declined:")));
        EXPECT_TRUE(recordsOf(bridge).isEmpty());
        EXPECT_TRUE(disconnectAndWait(bridge));
    }
    SessionBridge bridge;
    k.watch(bridge);
    bridge.consent()->setLimitForTesting(300);
    bridge.connectSessionAsync(k.uri());
    ASSERT_TRUE(waitsForConsent(bridge));
    ASSERT_TRUE(connects(bridge)); // no answer
    EXPECT_FALSE(bridge.consent()->waiting());
    EXPECT_TRUE(k.logged(QStringLiteral("No answer in")));
    EXPECT_FALSE(talks(bridge, 1000));
    EXPECT_TRUE(recordsOf(bridge).isEmpty());
    EXPECT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, AllowAppliesAndBinds)
{
    ConsentCase k;
    SessionBridge bridge;
    ASSERT_TRUE(k.setUp(bridge));
    bridge.connectSessionAsync(k.uri());
    ASSERT_TRUE(waitsForConsent(bridge));
    bridge.consent()->answer(true);
    ASSERT_TRUE(connects(bridge));
    EXPECT_TRUE(k.logged(QStringLiteral("Consent given:")));
    EXPECT_TRUE(talks(bridge, 10000));
    const QVariantList records = recordsOf(bridge);
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records.first().toMap().value(QStringLiteral("fingerprint")).toString(),
              k.print);
    EXPECT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, ADeclineIsRememberedForTheRun)
{
    ConsentCase k;
    {
        SessionBridge bridge;
        ASSERT_TRUE(k.setUp(bridge));
        bridge.connectSessionAsync(k.uri());
        ASSERT_TRUE(waitsForConsent(bridge));
        bridge.consent()->answer(false);
        ASSERT_TRUE(connects(bridge));
        ASSERT_TRUE(disconnectAndWait(bridge));
        k.log.clear();
        bridge.connectSessionAsync(k.uri());
        ASSERT_TRUE(connects(bridge));
        EXPECT_FALSE(k.logged(QStringLiteral("Consent asked:")));
        EXPECT_TRUE(k.logged(QStringLiteral("Consent declined earlier:")));
        EXPECT_TRUE(disconnectAndWait(bridge));
    }
    // The next run asks again
    SessionBridge bridge;
    bridge.connectSessionAsync(k.uri());
    EXPECT_TRUE(waitsForConsent(bridge));
    bridge.consent()->answer(false);
    ASSERT_TRUE(connects(bridge));
    EXPECT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, AWithdrawAsksAgain)
{
    ConsentCase k;
    SessionBridge bridge;
    ASSERT_TRUE(k.setUp(bridge));
    bridge.connectSessionAsync(k.uri());
    ASSERT_TRUE(waitsForConsent(bridge));
    bridge.consent()->answer(true);
    ASSERT_TRUE(connects(bridge));
    ASSERT_TRUE(disconnectAndWait(bridge));
    // A record found is applied with no question
    bridge.connectSessionAsync(k.uri());
    ASSERT_TRUE(connects(bridge));
    EXPECT_TRUE(k.logged(QStringLiteral("Consent applied:")));
    EXPECT_TRUE(talks(bridge, 10000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    bool withdrawn = false;
    QObject::connect(bridge.consent(), &ConsentFlow::withdrawn,
                     [&](bool ok, const QString&) { withdrawn = ok; });
    bridge.consent()->withdraw(QStringLiteral("talker"), k.print);
    ASSERT_TRUE(spinUntil([&]() { return withdrawn; }, 5000, 10));
    EXPECT_TRUE(recordsOf(bridge).isEmpty());
    bridge.connectSessionAsync(k.uri());
    EXPECT_TRUE(waitsForConsent(bridge));
    bridge.consent()->answer(false);
    ASSERT_TRUE(connects(bridge));
    EXPECT_TRUE(disconnectAndWait(bridge));
}

// D8: the word is the user's alone, given once, on the consent worker
TEST(BridgeTest, AnUnlockIsAskedOnceOffTheGuiThread)
{
    ConsentCase k;
    SessionBridge bridge;
    ASSERT_TRUE(k.setUp(bridge));
    const FakeEnvArm strict("ECUIO_FAKE_SECURITY_STRICT", "1");
    bridge.setVehicleLabel(QStringLiteral("Bench car"));
    bridge.connectSessionAsync(k.uri());
    ASSERT_TRUE(waitsForConsent(bridge));
    bridge.consent()->answer(true);
    ASSERT_TRUE(connects(bridge));
    ASSERT_TRUE(talks(bridge, 10000));
    int answers = 0;
    bool asked = false;
    QObject::connect(bridge.consent(), &ConsentFlow::unlockAnswered,
                     [&](int, bool ok, const QString&) {
                         ++answers;
                         asked = ok;
                     });
    // No word before the user's
    EXPECT_EQ(bridge.consent()->conversations().first().toMap()
                  .value(QStringLiteral("unlock")).toString(),
              QStringLiteral("none"));
    const QVariantMap first = bridge.consent()->unlock(0, 1);
    EXPECT_TRUE(first.value(QStringLiteral("ok")).toBool());
    const QVariantMap second = bridge.consent()->unlock(0, 1);
    EXPECT_FALSE(second.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(second.value(QStringLiteral("words")).toString(),
              QStringLiteral("An unlock is under way"));
    ASSERT_TRUE(spinUntil([&]() { return answers > 0; }, 5000, 10));
    EXPECT_TRUE(asked);
    const Qt::HANDLE worker = bridge.consent()->unlockThreadForTesting();
    EXPECT_NE(worker, nullptr);
    EXPECT_NE(worker, QThread::currentThreadId());
    EXPECT_TRUE(spinUntil(
        [&]() {
            const QVariantList c = bridge.consent()->conversations();
            return !c.isEmpty() &&
                   c.first().toMap().value(QStringLiteral("unlock")).toString() ==
                       QLatin1String("unlocked");
        },
        10000, 20));
    spinLoop(300);
    EXPECT_EQ(answers, 1);
    const QVariantMap row = bridge.consent()->conversations().first().toMap();
    EXPECT_EQ(row.value(QStringLiteral("failed")).toLongLong(), 0);
    EXPECT_TRUE(row.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("ecu")).toString(), QStringLiteral("pcm"));
    EXPECT_TRUE(disconnectAndWait(bridge));
}

#endif // _WIN32
