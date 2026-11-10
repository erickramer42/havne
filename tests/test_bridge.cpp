// File/layer: tests/test_bridge.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest, ReadOnceTest, ObdbImportTest (test_bridge)

#include <cmath>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTcpSocket>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <thread>
#include <map>
#include <vector>

#include "adapterrelay.h"
#include "blepicker.h"
#include "btconnect.h"
#include "btretire.h"
#include "carryover.h"
#include "healthreport.h"
#include "fake_bt_socket.h"
#include "fake_theme.h"
#include "obdbimport.h"
#include "packed_plugin.h"
#include "pluginstore.h"
#include "readonce.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include "windowtheme.h"
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "pid_support.h" // core/src: core's judgment, fed to the model

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "fake_control.h"   // the fake J2534 DLL's control page
#endif

namespace {

/* The obd2 set's signals core judges unsupported against reports: what
 * ecuio_session_get_unsupported_signals hands the bridge */
std::vector<uint16_t> unsupportedIds(const ecuio_channel_support_t* reports,
                                     uint32_t n)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> ids(set->count);
    ids.resize(ecuio_unsupported_signals(*set, reports, n, ids.data(),
                                         static_cast<uint32_t>(ids.size())));
    return ids;
}

/* Spin the GUI event loop for `ms`, letting queued invocations drain.
 * Without this, worker-posted lambdas never execute. */
void spinLoop(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/* Poll-until-predicate with deadline, event-loop flavored. The mock's
 * 50ms cadence is a LOCAL expectation, not a CI-runner guarantee:
 * a starved macOS VM can deliver 2 frames in 200+ ms of wall clock.
 * Polling keeps the asserted CONTRACT (the
 * condition eventually holds on a healthy stream) and drops the
 * coincidence (any particular wall-clock duration). The event loop
 * must keep turning while polling: worker lambdas drain through it. */
template <typename Predicate>
bool spinUntil(Predicate pred, int timeout_ms, int step_ms = 50)
{
    QElapsedTimer clock;
    clock.start();
    while (!pred()) {
        if (clock.elapsed() >= timeout_ms) {
            return false;
        }
        spinLoop(step_ms);
    }
    return true;
}

/* Disconnect is async: connected drops at once and the teardown ends on
 * a worker thread. A test that connects again waits for it, as the UI
 * does (Connect stays disabled while disconnecting). */
bool disconnectAndWait(SessionBridge& bridge, int timeout_ms = 5000)
{
    bridge.disconnectSession();
    return spinUntil([&bridge]() { return !bridge.disconnecting(); }, timeout_ms, 10);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/* Lines without their line ends (the recorder writes text mode, CRLF
 * on Windows). */
QList<QByteArray> fileLines(const QString& path)
{
    QList<QByteArray> lines;
    for (QByteArray line : readFile(path).split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty()) lines << line;
    }
    return lines;
}

QStringList recordingFolders(const QString& root)
{
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

/* Restores an environment variable as it was, set or not. */
struct EnvRestore {
    const char* name;
    bool had;
    QByteArray value;
    explicit EnvRestore(const char* n)
        : name(n), had(qEnvironmentVariableIsSet(n)), value(qgetenv(n)) {}
    ~EnvRestore() { had ? qputenv(name, value) : qunsetenv(name); }
    EnvRestore(const EnvRestore&) = delete;
    EnvRestore& operator=(const EnvRestore&) = delete;
};

/* What the surrogate child would inherit: the OS environment block, not
 * just the C runtime's copy. */
QByteArray osEnv(const char* name)
{
#ifdef _WIN32
    char buf[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? QByteArray(buf, static_cast<qsizetype>(n)) : QByteArray();
#else
    return qgetenv(name);
#endif
}

/* The fake ECU's diagnostics as the activity log words them (ADR-0037):
 * the fake J2534 DLL's and the mock's alike (core/src/mock_ecu.h) */
const char* const kFakeEcuDiagnosticsLine =
    "ECU 0x7E8 diagnostics: MIL on; stored P0301 P0420 U0100; pending P0171 "
    "C0035 B1001; permanent P0420; readiness 8 of 8 complete; freeze frame "
    "for P0301";

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);   // ONE instance, lives for the process
    // Every bridge's signal store (SignalStore::root) in a scratch folder,
    // never the user's app data; a case that reads one sets its own
    QTemporaryDir settings;
    if (!settings.isValid()) {
        return 1;
    }
    qputenv("HAVNE_SETTINGS_DIR", settings.path().toUtf8());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(BridgeTest, LifecycleIsIdempotent)
{
    SessionBridge bridge;

    EXPECT_FALSE(bridge.connected());

    EXPECT_TRUE(bridge.connectSession("mock://test").isEmpty());
    EXPECT_TRUE(bridge.connected());

    // Double connect = idempotent success, no new session
    EXPECT_TRUE(bridge.connectSession("mock://test").isEmpty());
    EXPECT_TRUE(bridge.connected());

    bridge.disconnectSession();
    EXPECT_FALSE(bridge.connected());     // drops at once; the teardown
    EXPECT_TRUE(bridge.disconnecting());  // ends on its own thread

    bridge.disconnectSession();   // double + triple disconnect: no-op
    bridge.disconnectSession();
    EXPECT_FALSE(bridge.connected());

    // No connect until the teardown ends
    EXPECT_FALSE(bridge.connectSession("mock://test").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.disconnecting(); }, 5000, 10));

    // Reconnect after disconnect must work (fresh session path)
    EXPECT_TRUE(bridge.connectSession("mock://test").isEmpty());
    EXPECT_TRUE(bridge.connected());
    bridge.disconnectSession();
}

TEST(BridgeTest, InvalidUriReturnsError)
{
    SessionBridge bridge;
    EXPECT_FALSE(bridge.connectSession("j2534://dev0").isEmpty());   // no transport
    EXPECT_FALSE(bridge.connected());

    EXPECT_FALSE(bridge.connectSession("").isEmpty());               // bad URI
    EXPECT_FALSE(bridge.connected());
}

// The error a failed connect returns, and logs, names the cause: the
// status, then core's detail.
TEST(BridgeTest, FailedConnectNamesItsCause)
{
    SessionBridge bridge;
    EXPECT_EQ(bridge.connectSession("mock://demo?interval=10"),
              QStringLiteral("malformed transport URI: mock:// takes no query "
                             "key 'interval', only profile, did and diagnose"));
    EXPECT_EQ(bridge.connectSession("mock://demo?profile=bogus"),
              QStringLiteral("unknown profile: no profile is named 'bogus'"));
    EXPECT_FALSE(bridge.connected());
    EXPECT_EQ(bridge.statusString(ECUIO_ERR_BAD_URI),
              QStringLiteral("malformed transport URI"));
}

// An app-fed stream's link is readied before core is asked (ADR-0028):
// the preparer sees the trimmed URI before each connect, sync or async,
// and its refusal fails the connect with its words, logged as any other
TEST(BridgeTest, AStreamPreparersRefusalFailsTheConnect)
{
    SessionBridge bridge;
    QStringList asked;
    bridge.setStreamPreparer([&asked](const QString& uri) {
        asked << uri;
        return uri.startsWith(QStringLiteral("elm327://bt:"))
                   ? QStringLiteral("bt:AA: not reachable")
                   : QString();
    });
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    EXPECT_EQ(bridge.connectSession("  elm327://bt:AA?poll=0x0C "),
              QStringLiteral("bt:AA: not reachable"));
    EXPECT_FALSE(bridge.connected());
    ASSERT_EQ(asked.size(), 1);
    EXPECT_EQ(asked.front(), QStringLiteral("elm327://bt:AA?poll=0x0C"));
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Connect failed: elm327://bt:AA?poll=0x0C (bt:AA: not reachable)")))
        << log.join('\n').toStdString();

    QString result = QStringLiteral("none");
    QObject::connect(
        &bridge, &SessionBridge::connectResult,
        [&result](const QString&, const QString& error) { result = error; });
    bridge.connectSessionAsync("elm327://bt:AA");
    EXPECT_EQ(result, QStringLiteral("bt:AA: not reachable"))
        << "refused at once";
    EXPECT_FALSE(bridge.connecting()) << "no connect started";

    EXPECT_EQ(bridge.connectSession("mock://demo"), QString());
    EXPECT_TRUE(bridge.connected()) << "a URI it passes connects";
    EXPECT_EQ(asked.size(), 3);
    bridge.disconnectSession();
}

namespace {

BleCharacteristic bleChar(const QString& uuid, bool write, bool noResponse,
                          bool notify, bool indicate = false)
{
    BleCharacteristic c;
    c.uuid = uuid;
    c.write = write;
    c.writeNoResponse = noResponse;
    c.notify = notify;
    c.indicate = indicate;
    return c;
}

BleService bleService(const QString& uuid, std::vector<BleCharacteristic> chars)
{
    BleService s;
    s.uuid = uuid;
    s.characteristics = std::move(chars);
    return s;
}

// What the generic access service and a device-information service look
// like: never the adapter's text
std::vector<BleService> bleStandardServices()
{
    return {bleService("1800", {bleChar("2a00", false, false, false)}),
            bleService("180a", {bleChar("2a29", false, false, false)})};
}

} // namespace

TEST(BridgeTest, BlePickerExpandsShortUuids)
{
    EXPECT_EQ(bleUuid("FFF0"),
              QStringLiteral("0000fff0-0000-1000-8000-00805f9b34fb"));
    EXPECT_EQ(bleUuid("{6E400001-B5A3-F393-E0A9-E50E24DCCA9E}"),
              QStringLiteral("6e400001-b5a3-f393-e0a9-e50e24dcca9e"));
}

// ADR-0028 section 2's order: the Nordic UART service first, then FFF0, then
// FFE0, whatever order the adapter lists them in
TEST(BridgeTest, BlePickerTakesTheListedServicesInOrder)
{
    std::vector<BleService> services = bleStandardServices();
    services.push_back(
        bleService("ffe0", {bleChar("ffe1", false, true, true)}));
    services.push_back(
        bleService("fff0", {bleChar("fff1", false, false, true),
                            bleChar("fff2", true, false, false)}));
    services.push_back(bleService(
        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
        {bleChar("6e400003-b5a3-f393-e0a9-e50e24dcca9e", false, false, true),
         bleChar("6e400002-b5a3-f393-e0a9-e50e24dcca9e", true, true, false)}));

    BlePick pick = pickBleService(services);
    EXPECT_EQ(pick.service, 4) << "the Nordic UART service";
    EXPECT_EQ(pick.write, 1);
    EXPECT_EQ(pick.notify, 0);

    services.pop_back();
    pick = pickBleService(services);
    EXPECT_EQ(pick.service, 3) << "FFF0";
    EXPECT_EQ(pick.write, 1);
    EXPECT_EQ(pick.notify, 0);

    services.pop_back();
    pick = pickBleService(services);
    EXPECT_EQ(pick.service, 2) << "FFE0, one characteristic both ways";
    EXPECT_EQ(pick.write, 0);
    EXPECT_EQ(pick.notify, 0);
}

// A listed service that can't carry the text is passed over for the next
// rule; the last rule takes the first service that can write and notify,
// indications counting as notifications
TEST(BridgeTest, BlePickerPassesOverAServiceMissingItsParts)
{
    std::vector<BleService> services = bleStandardServices();
    services.push_back(
        bleService("fff0", {bleChar("fff1", false, false, true)})); // no FFF2
    services.push_back(bleService(
        "ffe0", {bleChar("ffe1", false, false, true)})); // can't write
    services.push_back(bleService(
        "0000abcd-0000-1000-8000-00805f9b34fb",
        {bleChar("0000abce-0000-1000-8000-00805f9b34fb", false, false, false,
                 true),
         bleChar("0000abcf-0000-1000-8000-00805f9b34fb", false, true, false)}));
    const BlePick pick = pickBleService(services);
    EXPECT_EQ(pick.service, 4) << "the first that writes and notifies";
    EXPECT_EQ(pick.write, 1);
    EXPECT_EQ(pick.notify, 0);
}

TEST(BridgeTest, BlePickerFindsNothingInAnAdapterWithoutAWay)
{
    EXPECT_FALSE(pickBleService({}).found());
    EXPECT_FALSE(pickBleService(bleStandardServices()).found());
}

using BtAction = BtConnectRules::Action;

// The plain path: open, connect, the link up; close takes it down
TEST(BridgeTest, BtConnectOpensAndClosesALink)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_TRUE(r.connecting());
    EXPECT_EQ(r.connected(), BtAction::ReportOpened);
    EXPECT_TRUE(r.up());
    EXPECT_EQ(r.close(), BtAction::CloseSocket);
    EXPECT_FALSE(r.up());

    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.connectFailed(), BtAction::ReportFailed) << "core hears why";
    EXPECT_FALSE(r.connecting());
    EXPECT_FALSE(r.wanted());

    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.connected(), BtAction::ReportOpened);
    EXPECT_EQ(r.lost(), BtAction::ReportFailed) << "a dropped link";
    EXPECT_EQ(r.lost(), BtAction::None) << "told once";
    EXPECT_EQ(r.close(), BtAction::None) << "core's close after the failure";
}

// The car run (docs/bench-sessions/2026-10-28-android-8.md): core gave up
// on an open while its connect still ran, and the next open started a
// second connect beside it. Now the next open adopts the running one
TEST(BridgeTest, BtConnectAnOpenAdoptsTheRunningConnect)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None) << "the connect is left running";
    EXPECT_TRUE(r.connecting());
    EXPECT_EQ(r.open(), BtAction::None) << "no second connect";
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.open(), BtAction::None) << "still one connect";
    EXPECT_EQ(r.connected(), BtAction::ReportOpened) << "the adopted one opens";
    EXPECT_TRUE(r.up());
}

// An abandoned connect that succeeds with no open waiting is closed, and
// one that fails is dropped quietly: core gave up on it already
TEST(BridgeTest, BtConnectAnAbandonedConnectEndsQuietly)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.connected(), BtAction::CloseSocket);
    EXPECT_FALSE(r.up());
    EXPECT_FALSE(r.connecting());

    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.connectFailed(), BtAction::None);
    EXPECT_EQ(r.open(), BtAction::Connect) << "nothing running: a fresh one";
}

// An adopted connect that fails began for an earlier open: the waiting
// open gets a connect of its own, and only its failure reaches core
TEST(BridgeTest, BtConnectAFailedAdoptedConnectStartsAFreshOne)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.open(), BtAction::None);
    EXPECT_EQ(r.connectFailed(), BtAction::Connect);
    EXPECT_TRUE(r.connecting());
    EXPECT_EQ(r.connectFailed(), BtAction::ReportFailed);
    EXPECT_FALSE(r.wanted());
}

// Results with no connect running are stale: nothing happens
TEST(BridgeTest, BtConnectIgnoresResultsWithNoConnectRunning)
{
    BtConnectRules r;
    EXPECT_EQ(r.connected(), BtAction::None);
    EXPECT_EQ(r.connectFailed(), BtAction::None);
    EXPECT_EQ(r.lost(), BtAction::None);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_FALSE(r.up());
}

namespace {

using BtState = FakeBtSocket::SocketState;

/* Runs the deletions deleteLater posted, and for ms the timers too */
void runDeletions(int ms = 0)
{
    QElapsedTimer t;
    t.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    } while (t.elapsed() < ms);
}

} // namespace

// An abort that leaves the socket unconnected (mid-connect, or on a
// platform whose abort is immediate): it goes at once
TEST(BridgeTest, BtRetireDeletesAnUnconnectedSocketAtOnce)
{
    QPointer<FakeBtSocket> s = new FakeBtSocket(BtState::UnconnectedState);
    retireBtSocket(s.data());
    runDeletions();
    EXPECT_TRUE(s.isNull());
}

// Android's abort on an open link: the socket is still being written
// into by Qt's reader thread until it says unconnected. It outlives the
// link that owned it and every other state, then goes.
TEST(BridgeTest, BtRetireKeepsASocketUntilItSaysUnconnected)
{
    auto* link = new QObject;
    QPointer<FakeBtSocket> s = new FakeBtSocket(BtState::ConnectedState, link);
    retireBtSocket(s.data(), 60000);
    delete link;
    runDeletions();
    ASSERT_FALSE(s.isNull()) << "deleted with the link, or at once";
    EXPECT_EQ(s->parent(), nullptr);
    s->setState(BtState::ClosingState);
    runDeletions();
    ASSERT_FALSE(s.isNull()) << "deleted before it said unconnected";
    s->setState(BtState::UnconnectedState);
    runDeletions();
    EXPECT_TRUE(s.isNull());
}

// A reader that never stops: the fallback ends it
TEST(BridgeTest, BtRetireFallsBackWhenItNeverSaysUnconnected)
{
    QPointer<FakeBtSocket> s = new FakeBtSocket(BtState::ConnectedState);
    retireBtSocket(s.data(), 50);
    runDeletions();
    ASSERT_FALSE(s.isNull());
    runDeletions(500);
    EXPECT_TRUE(s.isNull());
}

// ---- The adapter relay (ADR-0047): a fake adapter, real loopback TCP ----
namespace {

// What the relay did to its adapter, kept past the adapter, which the
// relay owns and deletes at stop
struct RelayFakeState {
    int opens = 0;
    int closes = 0;
    QByteArray written;
    RelayAdapter* live = nullptr; // the adapter, while it exists
};

class FakeRelayAdapter : public RelayAdapter
{
public:
    explicit FakeRelayAdapter(std::shared_ptr<RelayFakeState> s)
        : m_s(std::move(s))
    {
        m_s->live = this;
    }
    ~FakeRelayAdapter() override { m_s->live = nullptr; }
    void open() override { ++m_s->opens; }
    void write(const QByteArray& bytes) override { m_s->written += bytes; }
    void close() override { ++m_s->closes; }

private:
    std::shared_ptr<RelayFakeState> m_s;
};

// A relay whose factory makes fake adapters, listening on a port the
// system picks
struct RelayFixture {
    std::shared_ptr<RelayFakeState> s = std::make_shared<RelayFakeState>();
    QString address;
    AdapterRelay relay;

    RelayFixture()
    {
        relay.setFactory([this](const QString& a) {
            address = a;
            return std::unique_ptr<RelayAdapter>(new FakeRelayAdapter(s));
        });
    }
    QString start() { return relay.start("elm327://bt:00:11:22:33:44:55", 0); }
    void adapterOpens() { s->live->opened(); }
    void adapterSays(const QByteArray& b) { s->live->received(b); }
    void adapterFails(const QString& why) { s->live->failed(why); }
};

// A computer: a socket to the relay's port, connected once the relay
// has taken it
std::unique_ptr<QTcpSocket> computerOn(AdapterRelay& relay)
{
    auto c = std::make_unique<QTcpSocket>();
    c->connectToHost(QHostAddress::LocalHost,
                     static_cast<quint16>(relay.port()));
    EXPECT_TRUE(c->waitForConnected(3000)) << c->errorString().toStdString();
    return c;
}

QByteArray readFor(QTcpSocket& c, int want, int timeout_ms = 3000)
{
    QByteArray got;
    spinUntil(
        [&] {
            got += c.readAll();
            return got.size() >= want;
        },
        timeout_ms, 10);
    return got;
}

bool closedFromTheFarSide(QTcpSocket& c, int timeout_ms = 3000)
{
    return spinUntil(
        [&] { return c.state() == QAbstractSocket::UnconnectedState; },
        timeout_ms, 10);
}

} // namespace

// The computer's bytes reach the adapter and the adapter's the computer,
// in order, counted; the address is the URI's, its query left out
TEST(BridgeTest, RelayPassesBytesBothWaysInOrder)
{
    RelayFixture f;
    ASSERT_EQ(f.start(), QString());
    EXPECT_TRUE(f.relay.running());
    EXPECT_GT(f.relay.port(), 0);
    EXPECT_EQ(f.address, QStringLiteral("00:11:22:33:44:55"));
    EXPECT_EQ(f.s->opens, 1) << "the adapter opens at start";
    f.adapterOpens();
    auto c = computerOn(f.relay);
    ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
    c->write("ATZ\r");
    c->write("ATE0\r");
    ASSERT_TRUE(spinUntil([&] { return f.s->written.size() >= 9; }, 3000, 10));
    EXPECT_EQ(f.s->written, QByteArray("ATZ\rATE0\r"));
    f.adapterSays("ELM327 v1.5\r\r>");
    f.adapterSays("OK\r\r>");
    EXPECT_EQ(readFor(*c, 19),QByteArray("ELM327 v1.5\r\r>OK\r\r>"));
    EXPECT_EQ(f.relay.bytesOut(), 9);
    EXPECT_EQ(f.relay.bytesIn(), 19);
}

// A computer that connects before the adapter is open: its bytes wait for
// the open and then go in order; a computer sending past what a setup
// would is disconnected
TEST(BridgeTest, RelayHoldsAComputersBytesUntilTheAdapterOpens)
{
    RelayFixture f;
    ASSERT_EQ(f.start(), QString());
    auto c = computerOn(f.relay);
    ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
    c->write("ATZ\r");
    spinLoop(100);
    EXPECT_TRUE(f.s->written.isEmpty()) << "nothing before the open";
    EXPECT_EQ(f.s->opens, 1) << "an open under way isn't asked again";
    f.adapterOpens();
    EXPECT_EQ(f.s->written, QByteArray("ATZ\r"));
    c->write("ATI\r");
    ASSERT_TRUE(spinUntil([&] { return f.s->written.size() >= 8; }, 3000, 10));
    EXPECT_EQ(f.s->written, QByteArray("ATZ\rATI\r"));

    RelayFixture g;
    ASSERT_EQ(g.start(), QString());
    auto d = computerOn(g.relay);
    ASSERT_TRUE(spinUntil([&] { return g.relay.computerConnected(); }, 3000, 10));
    d->write(QByteArray(AdapterRelay::kHeldMax + 1, 'A'));
    EXPECT_TRUE(closedFromTheFarSide(*d));
    EXPECT_FALSE(g.relay.computerConnected());
    EXPECT_TRUE(g.s->written.isEmpty());
}

// One computer at a time: a second is closed and the first carries on
TEST(BridgeTest, RelayRefusesASecondComputer)
{
    RelayFixture f;
    ASSERT_EQ(f.start(), QString());
    f.adapterOpens();
    auto first = computerOn(f.relay);
    ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
    auto second = computerOn(f.relay);
    EXPECT_TRUE(closedFromTheFarSide(*second));
    first->write("ATZ\r");
    ASSERT_TRUE(spinUntil([&] { return f.s->written.size() >= 4; }, 3000, 10));
    EXPECT_EQ(f.s->written, QByteArray("ATZ\r"));
    EXPECT_EQ(first->state(), QAbstractSocket::ConnectedState);
}

// The adapter's link dropping ends the computer's, so its core sees the
// port fail and connects again; that computer opens the adapter again
TEST(BridgeTest, RelayEndsTheComputersLinkWithTheAdapters)
{
    RelayFixture f;
    ASSERT_EQ(f.start(), QString());
    f.adapterOpens();
    auto c = computerOn(f.relay);
    ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
    f.adapterFails(QStringLiteral("the adapter disconnected"));
    EXPECT_TRUE(closedFromTheFarSide(*c));
    EXPECT_TRUE(f.relay.status().contains(QStringLiteral("the adapter disconnected")))
        << f.relay.status().toStdString();
    EXPECT_TRUE(f.relay.running()) << "still listening";
    auto again = computerOn(f.relay);
    ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
    EXPECT_EQ(f.s->opens, 2) << "the next computer opens the adapter";
}

// A computer leaving keeps the adapter's link for the next one
TEST(BridgeTest, RelayKeepsTheAdapterWhenTheComputerLeaves)
{
    RelayFixture f;
    ASSERT_EQ(f.start(), QString());
    f.adapterOpens();
    {
        auto c = computerOn(f.relay);
        ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
        c->disconnectFromHost();
        ASSERT_TRUE(spinUntil([&] { return !f.relay.computerConnected(); }, 3000, 10));
    }
    EXPECT_EQ(f.s->closes, 0);
    EXPECT_TRUE(f.relay.status().contains(QStringLiteral("waiting for a computer")))
        << f.relay.status().toStdString();
    f.adapterSays("stray\r"); // nobody to take it
    auto next = computerOn(f.relay);
    ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
    EXPECT_EQ(f.s->opens, 1) << "not opened again";
    next->write("ATZ\r");
    ASSERT_TRUE(spinUntil([&] { return f.s->written.size() >= 4; }, 3000, 10));
    f.adapterSays(">");
    EXPECT_EQ(readFor(*next, 1), QByteArray(">")) << "no stray bytes first";
}

// Stop ends the computer's link, the listening and the adapter's link
TEST(BridgeTest, RelayStopEndsEverything)
{
    RelayFixture f;
    ASSERT_EQ(f.start(), QString());
    f.adapterOpens();
    const int port = f.relay.port();
    auto c = computerOn(f.relay);
    ASSERT_TRUE(spinUntil([&] { return f.relay.computerConnected(); }, 3000, 10));
    f.relay.stop();
    EXPECT_FALSE(f.relay.running());
    EXPECT_EQ(f.relay.port(), 0);
    EXPECT_EQ(f.s->closes, 1);
    EXPECT_EQ(f.s->live, nullptr) << "the adapter goes with the relay";
    EXPECT_TRUE(closedFromTheFarSide(*c));
    // On loopback the kernel can give the client the old port as its own
    // source port, and the connect then reaches itself (TCP's
    // simultaneous open): that too says nothing listens. A connect the
    // kernel took for the listener as it closed is reset at once (macOS
    // has read its far side's port as 0 by then); only one that stays
    // connected means something still listens
    QTcpSocket late;
    late.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    if (late.waitForConnected(500) && late.localPort() != late.peerPort()) {
        const bool dropped = late.state() != QAbstractSocket::ConnectedState ||
                             late.waitForDisconnected(1000);
        EXPECT_TRUE(dropped) << "something listens on " << port
                             << " (connected from " << late.localPort() << ")";
    }
}

// What it relays: a paired classic adapter's URI, in a build that can
TEST(BridgeTest, RelayRefusesWhatItCantRelay)
{
    AdapterRelay none;
    EXPECT_FALSE(none.available());
    EXPECT_FALSE(none.start("elm327://bt:00:11:22:33:44:55", 0).isEmpty());
    EXPECT_FALSE(none.running());

    RelayFixture f;
    for (const char* uri : {"elm327://ble:00:11:22:33:44:55", "elm327://COM5",
                            "elm327://bt:", "mock://demo",
                            "elm327://192.168.0.10:35000"}) {
        EXPECT_FALSE(f.relay.start(uri, 0).isEmpty()) << uri;
        EXPECT_FALSE(f.relay.running()) << uri;
    }
    EXPECT_EQ(f.relay.start("elm327://bt:00:11:22:33:44:55", -1),
              QStringLiteral("The port must be 0 to 65535 (0: the system picks)"));
    EXPECT_EQ(f.relay.start("elm327://bt:00:11:22:33:44:55", 65536),
              QStringLiteral("The port must be 0 to 65535 (0: the system picks)"));
    EXPECT_FALSE(f.relay.running());
    EXPECT_EQ(f.s->opens, 0);
    ASSERT_EQ(f.start(), QString());
    EXPECT_FALSE(f.start().isEmpty()) << "one relay at a time";
}

TEST(BridgeTest, FramesMarshalFromWorkerToGuiThread)
{
    SessionBridge bridge;

    // The frame log's countChanged fires on the thread that appends
    std::atomic<Qt::HANDLE> append_thread{nullptr};
    QObject::connect(
        bridge.model(),
        &FrameListModel::countChanged,
        [&append_thread](int) {
            Qt::HANDLE current = QThread::currentThreadId();
            Qt::HANDLE expected = nullptr;
            append_thread.compare_exchange_strong(expected, current);
        });

    ASSERT_TRUE(bridge.connectSession("mock://test").isEmpty());
    // CONTRACT: frames eventually flow. Poll instead of a fixed 300ms.
    const bool flowing = spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000);
    ASSERT_TRUE(flowing) << "no frames in 5s: stream is dead, not slow";

    // Frames flowed...
    EXPECT_GT(bridge.frameCount(), 0);
    // ...and they landed on the thread running this event loop.
    EXPECT_EQ(append_thread.load(), QThread::currentThreadId());
    EXPECT_NE(append_thread.load(), nullptr);

    const int frames_before = bridge.frameCount();

    // Mid-stream disconnect: late-arriving frames must be dropped,
    // not crash, not append. Equality assertion after a settle window
    // is duration-independent: any arriving frame would break it.
    bridge.disconnectSession();
    spinLoop(300);   // settle: let any in-flight lambdas drain and drop
    EXPECT_EQ(bridge.frameCount(), frames_before);
}

TEST(BridgeTest, ClearFramesResets)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://test").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    ASSERT_GT(bridge.frameCount(), 0);

    bridge.clearFrames();
    EXPECT_EQ(bridge.frameCount(), 0);
    EXPECT_EQ(bridge.model()->rowCount(), 0);

    bridge.disconnectSession();
}

/* Recording is off by default: a session writes nothing. */
TEST(BridgeTest, RecordingIsOffByDefault)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    EXPECT_FALSE(bridge.recording());

    ASSERT_TRUE(bridge.connectSession("mock://off").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    EXPECT_FALSE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());
    EXPECT_TRUE(bridge.recordingFolder().isEmpty());
}

/* One folder per connect, which core writes (ADR-0049): every frame
 * the session dispatched, the log's Pause notwithstanding, each decoded
 * value, core's header with the app's caller lines, and the session's
 * activity lines from Connected through Disconnected as notes, then
 * core's summary. */
TEST(BridgeTest, RecordingWritesFramesValuesAndEvents)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.recording());

    ASSERT_TRUE(bridge.connectSession("mock://rec").isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(spinUntil([&bridge]() {
        return bridge.frameCount() > 3 && bridge.signalBatchCount() > 0;
    }, 5000));
    bridge.setLogPaused(true);
    const int before = bridge.frameCount();
    ASSERT_TRUE(spinUntil([&bridge, before]() { return bridge.frameCount() > before + 2; }, 5000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.recordingActive());
    const int frames = bridge.frameCount();

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString dir = root.path() + QStringLiteral("/") + folders.first();
    EXPECT_TRUE(bridge.recordingFolder().endsWith(folders.first())) << bridge.recordingFolder().toStdString();
    EXPECT_EQ(bridge.recordingFolderUrl(), QUrl::fromLocalFile(dir));

    // frames.csv: the header row, then one row per frame, host time
    // never going backwards
    const QList<QByteArray> frameRows = fileLines(dir + QStringLiteral("/frames.csv"));
    ASSERT_FALSE(frameRows.isEmpty());
    EXPECT_EQ(frameRows.first(), QByteArray("host_us,timestamp_us,id,flags,data"));
    // The worker may dispatch a frame or two more after the bridge
    // stops taking them, as the session closes
    EXPECT_GE(frameRows.size() - 1, frames) << "every frame, paused or not";
    qint64 lastHost = -1;
    bool sawTelemetry = false;
    for (qsizetype i = 1; i < frameRows.size(); ++i) {
        const QList<QByteArray> cols = frameRows[i].split(',');
        ASSERT_EQ(cols.size(), 5) << frameRows[i].toStdString();
        const qint64 host = cols[0].toLongLong();
        EXPECT_GE(host, lastHost) << frameRows[i].toStdString();
        lastHost = host;
        sawTelemetry = sawTelemetry || cols[2] == "0xC0";
    }
    EXPECT_TRUE(sawTelemetry) << "no 0x0C0 telemetry frame recorded";

    // signals.csv: a byte-order mark, the header row, decoded values
    const QByteArray signalsCsv = readFile(dir + QStringLiteral("/signals.csv"));
    EXPECT_TRUE(signalsCsv.startsWith("\xEF\xBB\xBFhost_us,signal,value,unit"));
    EXPECT_TRUE(signalsCsv.contains(",RPM,")) << "no RPM value recorded";

    // CRLF on Windows (ADR-0017 takes either)
    const QByteArray events =
        readFile(dir + QStringLiteral("/events.log")).replace("\r\n", "\n");
    for (const char* expected :
         {"ecuio session recording, format 3 (ADR-0049)\n",
          "\nuri  mock://rec\n", "\nsignal  RPM,", "\ncaller  Havne ",
          "\ncaller  surrogate frame logging  ",
          "  note  Connected: mock://rec\n",
          "  note  Disconnecting: mock://rec\n", "  note  Delivery: ",
          "  note  Disconnected: mock://rec\n", "  end  disconnected: "}) {
        EXPECT_TRUE(events.contains(expected)) << expected << "\n" << events.toStdString();
    }
}

/* The app's Mark (ADR-0049 section 4): a marker in markers.csv at the
 * time it's tapped, "Mark N" unless labeled, counted per recording, said
 * in the activity log; nothing to mark without a recording. */
TEST(BridgeTest, AMarkGoesIntoTheRecording)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    EXPECT_FALSE(bridge.mark()) << "no recording";
    EXPECT_EQ(bridge.markCount(), 0);

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://marks").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    EXPECT_TRUE(bridge.mark());
    EXPECT_TRUE(bridge.mark(QStringLiteral("  launch, \"go\"  ")));
    EXPECT_TRUE(bridge.mark(QStringLiteral(" ")));
    EXPECT_EQ(bridge.markCount(), 3);
    EXPECT_TRUE(log.contains(QStringLiteral("Marked: Mark 1")));
    EXPECT_TRUE(log.contains(QStringLiteral("Marked: launch, \"go\"")));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.mark()) << "the recording ended";
    EXPECT_EQ(bridge.markCount(), 3) << "the last recording's";

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString dir = root.path() + QStringLiteral("/") + folders.first();
    const QList<QByteArray> rows =
        fileLines(dir + QStringLiteral("/markers.csv"));
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(rows[0], QByteArray("host_us,label"));
    EXPECT_TRUE(rows[1].endsWith(",Mark 1")) << rows[1].toStdString();
    EXPECT_TRUE(rows[2].endsWith(",\"launch, \"\"go\"\"\""))
        << rows[2].toStdString();
    EXPECT_TRUE(rows[3].endsWith(",Mark 3")) << rows[3].toStdString();
    EXPECT_LE(rows[1].split(',')[0].toLongLong(),
              rows[3].split(',')[0].toLongLong());
    const QByteArray events = readFile(dir + QStringLiteral("/events.log"));
    EXPECT_FALSE(events.contains("Marked:"))
        << "markers.csv holds them, not notes";

    // A new recording counts from one
    ASSERT_TRUE(bridge.connectSession("mock://marks-again").isEmpty());
    EXPECT_EQ(bridge.markCount(), 0);
    EXPECT_TRUE(bridge.mark());
    EXPECT_EQ(log.count(QStringLiteral("Marked: Mark 1")), 2);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* Live's plot: the rows with these ids ticked and every other numeric
 * row unticked, as a vehicle's choices set them */
void plotOnly(SignalListModel* model, const std::vector<uint16_t>& ids)
{
    QStringList plotted;
    QStringList unplotted;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r);
        if (model->data(i, SignalListModel::IsTextRole).toBool()) {
            continue;
        }
        const auto id = static_cast<uint16_t>(
            model->data(i, SignalListModel::IdRole).toUInt());
        const QString name =
            model->data(i, SignalListModel::NameRole).toString();
        (std::find(ids.begin(), ids.end(), id) != ids.end() ? plotted
                                                            : unplotted)
            << name;
    }
    model->setPlotChoices(plotted, unplotted);
}

/* The names of the model's rows with these ids, in the model's order */
QStringList namesOf(SignalListModel* model, const std::vector<uint16_t>& ids)
{
    QStringList names;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r);
        const auto id = static_cast<uint16_t>(
            model->data(i, SignalListModel::IdRole).toUInt());
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
            names << model->data(i, SignalListModel::NameRole).toString();
        }
    }
    return names;
}

/* A folder's files' sizes summed */
qint64 folderBytes(const QString& folder)
{
    qint64 bytes = 0;
    for (const QFileInfo& f :
         QDir(folder).entryInfoList(QDir::Files | QDir::Hidden)) {
        bytes += f.size();
    }
    return bytes;
}

} // namespace

/* Start log is refused, with its reason in the activity log and no
 * folder made: while not connected, while Record sessions' recorder is
 * live (one recorder a session), with no row plotted, and on a replay,
 * which plays a recording already. */
TEST(BridgeTest, StartLogIsRefusedWithItsReason)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(QStringLiteral("Log not started: not connected")))
        << log.join('\n').toStdString();

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://recorded").isEmpty());
    ASSERT_TRUE(bridge.recordingActive());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: Recording this session already")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);
    EXPECT_EQ(recordingFolders(root.path()).size(), 1)
        << "Record sessions' alone";

    ASSERT_TRUE(bridge.connectSession("mock://unticked").isEmpty());
    plotOnly(bridge.signalModel(), {});
    ASSERT_EQ(bridge.signalModel()->subscribedCount(), 0);
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: no signal is ticked on Live")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(recordingFolders(root.path()).size(), 1);

    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("replay://%1/2026-10-25-engine-start"
                                           "?profile=obd2&speed=100")
                                .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES)))
            .isEmpty());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: a replay plays a recording already")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(recordingFolders(root.path()).size(), 1);
}

/* A start the session refuses after its focus took (a recorder of the
 * test's own holds the session's one slot): the whole set is asked
 * again, and the folder the start made, holding no frame, is gone. */
TEST(BridgeTest, AStartLogTheSessionRefusesLeavesNoFolderAndNoFocus)
{
    QTemporaryDir root;
    QTemporaryDir own;
    ASSERT_TRUE(root.isValid() && own.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession("mock://refused").isEmpty());
    ASSERT_GT(bridge.signalModel()->subscribedCount(), 0);

    const QByteArray own8 = own.path().toUtf8();
    const auto attach = [&bridge, &own8]() {
        ecuio_record_options_t o{};
        o.root_utf8 = own8.constData();
        ecuio_recorder_t* r = nullptr;
        EXPECT_EQ(ecuio_recorder_open(&o, &r), ECUIO_OK);
        EXPECT_EQ(ecuio_session_record(bridge.sessionForTesting(), r),
                  ECUIO_OK);
        return r;
    };
    ecuio_recorder_t* holder = attach();
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(QStringLiteral("Log not started: %1")
                                 .arg(QString::fromUtf8(ecuio_status_string(
                                     ECUIO_ERR_INVALID_ARGUMENT)))))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty())
        << recordingFolders(root.path()).join(',').toStdString();
    (void)ecuio_recorder_close(holder, "the test's");

    // A recorder attached now writes the focus it finds: none
    ecuio_recorder_t* probe = attach();
    char folder[4096];
    uint32_t length = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(probe, folder, sizeof folder, &length),
              ECUIO_OK);
    (void)ecuio_recorder_close(probe, "the test's");
    const QByteArray events =
        readFile(QString::fromUtf8(folder) + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.contains("\nuri  mock://refused"))
        << events.toStdString();
    EXPECT_FALSE(events.contains("\nfocus  ")) << events.toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A write that fails during a log ends it as it ends a Record sessions
 * recording: "Recording stopped: ..." in the activity log, logStopped
 * not the user's, the session running on. */
TEST(BridgeTest, AFailedWriteEndsALogNotAsTheUsers)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QList<bool> byUser;
    QObject::connect(&bridge, &SessionBridge::logStopped,
                     [&byUser](const QString&, bool user) { byUser << user; });
    ASSERT_TRUE(bridge.connectSession("mock://failing").isEmpty());
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    ASSERT_NE(bridge.recorderForTesting(), nullptr);
    ecuio_recorder_test_fail_writes(bridge.recorderForTesting());
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_EQ(byUser, QList<bool>{false});
    EXPECT_TRUE(
        log.contains(QStringLiteral("Recording stopped: the disk is full")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(bridge.connected());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(byUser, QList<bool>{false}) << "once";
}

/* The Dashboard header's notice: a refusal's "Log not started: <why>"
 * (startLog's own reasons and the recorder's), an end the user didn't
 * ask for as "Log ended: <why>", nothing for Stop log's, a log that
 * starts clearing it, Dismiss clearing it, and a change signal for
 * each change alone. */
TEST(BridgeTest, TheLogNoticeTellsRefusalsAndUnaskedEnds)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::logNoticeChanged,
                     [&changes]() { ++changes; });

    EXPECT_EQ(bridge.logNotice(), QString());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_EQ(bridge.logNotice(),
              QStringLiteral("Log not started: not connected"));
    EXPECT_EQ(changes, 1);
    EXPECT_FALSE(bridge.startLog());
    EXPECT_EQ(changes, 1) << "the same text again is no change";
    bridge.clearLogNotice();
    EXPECT_EQ(bridge.logNotice(), QString());
    EXPECT_EQ(changes, 2);
    bridge.clearLogNotice();
    EXPECT_EQ(changes, 2) << "clearing nothing is no change";

    // The recorder's refusal, the root a file: openRecorder's reason
    const QString file = root.path() + QStringLiteral("/not-a-folder");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    bridge.setRecordingsRootForTesting(file);
    ASSERT_TRUE(bridge.connectSession("mock://failing").isEmpty());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(bridge.logNotice().startsWith(
        QStringLiteral("Log not started: can't create ")))
        << bridge.logNotice().toStdString();
    EXPECT_TRUE(log.contains(bridge.logNotice()))
        << "the activity log's own line: " << log.join('\n').toStdString();
    EXPECT_EQ(
        log.filter(QStringLiteral("Log not started: can't create ")).size(), 1)
        << "once, not twice";
    bridge.setRecordingsRootForTesting(root.path());

    // A log that starts clears it; a failed write ends it, told
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_EQ(bridge.logNotice(), QString());
    ASSERT_NE(bridge.recorderForTesting(), nullptr);
    ecuio_recorder_test_fail_writes(bridge.recorderForTesting());
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_EQ(bridge.logNotice(), QStringLiteral("Log ended: the disk is full"))
        << "the recorder's own words, not the end note's";

    // Stop log's end says nothing: the viewer opens on it
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_EQ(bridge.logNotice(), QString());
    const int before = changes;
    bridge.stopLog();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_EQ(bridge.logNotice(), QString());
    EXPECT_EQ(changes, before);

    // The session's end closes a running log, told as "disconnected"
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.logNotice(), QStringLiteral("Log ended: disconnected"));
}

/* A connect started from disconnectingChanged's end (as a QML handler
 * may) finds the last session's recording closed: that one ends with
 * the teardown, and the new session's runs on. */
TEST(BridgeTest, AConnectAtTheTeardownsEndFindsTheRecordingClosed)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://first").isEmpty());
    const QString first = bridge.recordingFolderUrl().toLocalFile();
    ASSERT_FALSE(first.isEmpty());
    bool again = false;
    QObject::connect(
        &bridge, &SessionBridge::disconnectingChanged, &bridge,
        [&bridge, &again]() {
            if (!bridge.disconnecting() && !again) {
                again = true;
                EXPECT_TRUE(bridge.connectSession("mock://second").isEmpty());
            }
        });
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(again);
    EXPECT_TRUE(bridge.connected());
    EXPECT_TRUE(bridge.recordingActive());
    const QString second = bridge.recordingFolderUrl().toLocalFile();
    EXPECT_NE(second, first);
    EXPECT_TRUE(readFile(first + QStringLiteral("/events.log"))
                    .contains("  end  disconnected: "));
    EXPECT_FALSE(
        readFile(second + QStringLiteral("/events.log")).contains("  end  "));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(readFile(second + QStringLiteral("/events.log"))
                    .contains("  end  disconnected: "));
}

/* A log on a running session (ADR-0056 section 1): its own folder, the
 * plotted rows its focus line, a readout that counts the folder's bytes
 * and its time, Stop log's folder in logStopped as the user's; a second
 * log in the same session, which the session's end closes and reports
 * with its folder, not as the user's. */
/* ADR-0056 section 4: Settings' minimum time takes only its choices,
 * and on a session that asks nothing (the mock), where it changes
 * nothing, neither the log's line nor its header name it; a session
 * that asks is ALogOnARequestingSessionNamesItsMinimumTime */
TEST(BridgeTest, ALogTakesItsMinimumTimeBetweenSweeps)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    EXPECT_EQ(bridge.minSweepMs(), 0);
    EXPECT_EQ(bridge.minSweepChoices(), (QVariantList{0, 100, 250, 500, 1000}));
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::minSweepMsChanged,
                     [&changes]() { ++changes; });
    bridge.setMinSweepMs(250);
    EXPECT_EQ(bridge.minSweepMs(), 250);
    EXPECT_EQ(changes, 1);
    bridge.setMinSweepMs(251);
    bridge.setMinSweepMs(-100);
    bridge.setMinSweepMs(250);
    EXPECT_EQ(bridge.minSweepMs(), 250) << "anything but a choice is ignored";
    EXPECT_EQ(changes, 1);

    ASSERT_TRUE(bridge.connectSession("mock://paced").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    std::vector<uint16_t> picks;
    for (int r = 0; r < model->rowCount() && picks.size() < 2; ++r) {
        const QModelIndex i = model->index(r);
        if (!model->data(i, SignalListModel::IsTextRole).toBool()) {
            picks.push_back(static_cast<uint16_t>(
                model->data(i, SignalListModel::IdRole).toUInt()));
        }
    }
    ASSERT_EQ(picks.size(), 2u);
    plotOnly(model, picks);

    const auto header = [](const QString& folder) {
        QFile f(folder + QStringLiteral("/events.log"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                           : QString();
    };
    const auto stoppedFolder = [&bridge]() {
        return bridge.recordingFolderUrl().toLocalFile();
    };

    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Log started: %1 (2 signals)")
                                 .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    QString folder = stoppedFolder();
    bridge.stopLog();
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_FALSE(header(folder).contains(QLatin1String("min sweep")))
        << header(folder).toStdString();

    bridge.setMinSweepMs(0);
    log.clear();
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Log started: %1 (2 signals)")
                                 .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    folder = stoppedFolder();
    bridge.stopLog();
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_FALSE(header(folder).contains(QLatin1String("min sweep")))
        << header(folder).toStdString();
    disconnectAndWait(bridge);
}

TEST(BridgeTest, ALogRecordsTheRunningSessionUntilStopped)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QStringList stopped;
    QList<bool> byUser;
    QObject::connect(&bridge, &SessionBridge::logStopped,
                     [&stopped, &byUser](const QString& folder, bool user) {
                         stopped << folder;
                         byUser << user;
                     });

    ASSERT_TRUE(bridge.connectSession("mock://logged").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    std::vector<uint16_t> picks;
    for (int r = 0; r < model->rowCount() && picks.size() < 2; ++r) {
        const QModelIndex i = model->index(r);
        if (!model->data(i, SignalListModel::IsTextRole).toBool()) {
            picks.push_back(static_cast<uint16_t>(
                model->data(i, SignalListModel::IdRole).toUInt()));
        }
    }
    ASSERT_EQ(picks.size(), 2u);
    plotOnly(model, picks);
    ASSERT_EQ(model->subscribedCount(), 2);

    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(bridge.logActive());
    EXPECT_TRUE(bridge.recordingActive());
    const QString first = bridge.recordingFolderUrl().toLocalFile();
    EXPECT_TRUE(log.contains(QStringLiteral("Log started: %1 (2 signals)")
                                 .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: a log is running already")));

    // The readout's second: the folder's bytes and the time so far
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.logBytes() > 0 && bridge.logElapsedMs() >= 1000;
        },
        10000))
        << bridge.logBytes() << " bytes at " << bridge.logElapsedMs();
    EXPECT_LE(bridge.logBytes(), folderBytes(first));

    bridge.stopLog();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    ASSERT_EQ(stopped, QStringList{first});
    EXPECT_EQ(byUser, QList<bool>{true}) << "Stop log's own";
    EXPECT_EQ(bridge.logBytes(), folderBytes(first))
        << "the closed folder's size";
    const qint64 elapsed = bridge.logElapsedMs();
    EXPECT_GE(elapsed, 1000);
    spinLoop(50);
    EXPECT_EQ(bridge.logElapsedMs(), elapsed) << "the last log's, held";
    const QByteArray events =
        readFile(first + QStringLiteral("/events.log")).replace("\r\n", "\n");
    for (const QString& expected :
         {QStringLiteral("\nfocus  ") + namesOf(model, picks).join(',') +
              QStringLiteral("\n"),
          QStringLiteral("  note  Log stopped: "),
          QStringLiteral("  end  Stop log: ")}) {
        EXPECT_TRUE(events.contains(expected.toUtf8()))
            << expected.toStdString() << "\n"
            << events.toStdString();
    }

    // The slot is free again, and the session's end closes the next
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    const QString second = bridge.recordingFolderUrl().toLocalFile();
    EXPECT_NE(second, first);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.logActive());
    ASSERT_EQ(stopped, (QStringList{first, second}));
    EXPECT_EQ(byUser, (QList<bool>{true, false}))
        << "the session's end isn't the user's";
    EXPECT_TRUE(readFile(second + QStringLiteral("/events.log"))
                    .contains("  end  disconnected: "));
    EXPECT_EQ(recordingFolders(root.path()).size(), 2);
}

/* An app closed while connected: the bridge's destructor destroys the
 * session itself and ends the recording, with no disconnect first. */
TEST(BridgeTest, DestroyedWhileRecordingClosesSessionAndRecording)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    {
        SessionBridge bridge;
        bridge.setRecordingsRootForTesting(root.path());
        bridge.setRecording(true);
        ASSERT_TRUE(bridge.connectSession("mock://closing").isEmpty());
        ASSERT_TRUE(
            spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
        ASSERT_TRUE(bridge.recordingActive());
    }

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray events =
        readFile(root.path() + QStringLiteral("/") + folders.first() +
                 QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains("  end  the app closed: "))
        << events.toStdString();
    EXPECT_FALSE(events.contains("Disconnecting:")) << events.toStdString();
}

/* A recording that can't start (the root is a file, so no folder can
 * be made under it) says why in the activity log and leaves the
 * session running unrecorded. */
TEST(BridgeTest, RecordingThatCantStartIsLoggedAndTheSessionRuns)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString file = root.path() + QStringLiteral("/not-a-folder");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(file);
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    ASSERT_TRUE(bridge.connectSession("mock://unrecorded").isEmpty());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(
        log.filter(QStringLiteral("Recording failed: can't create ")).size() ==
        1)
        << log.join('\n').toStdString();
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A write that fails (a full disk) stops the recording, never the
 * session: the activity log says why, once, with no path, the folder is
 * no longer active, and frames keep coming. */
TEST(BridgeTest, AFailedWriteStopsTheRecordingAndTheSessionRuns)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession("mock://failing").isEmpty());
    ASSERT_TRUE(bridge.recordingActive());
    ASSERT_NE(bridge.recorderForTesting(), nullptr);
    ecuio_recorder_test_fail_writes(bridge.recorderForTesting());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return !bridge.recordingActive(); }, 5000));
    const QStringList stopped =
        log.filter(QStringLiteral("Recording stopped: "));
    ASSERT_EQ(stopped.size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(stopped.first(),
              QStringLiteral("Recording stopped: the disk is full"));
    EXPECT_TRUE(bridge.connected());
    const int before = bridge.frameCount();
    ASSERT_TRUE(spinUntil(
        [&bridge, before]() { return bridge.frameCount() > before + 2; }, 5000))
        << "the session runs on";
    EXPECT_EQ(log.filter(QStringLiteral("Recording stopped: ")).size(), 1)
        << "once";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The switch holds while a session is up, so a recording covers a whole
 * session; each connect gets its own folder. */
TEST(BridgeTest, RecordingSwitchHoldsDuringASession)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());

    ASSERT_TRUE(bridge.connectSession("mock://late").isEmpty());
    bridge.setRecording(true);
    EXPECT_FALSE(bridge.recording());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.recording());
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(bridge.connectSession("mock://twice").isEmpty());
        bridge.setRecording(false);
        EXPECT_TRUE(bridge.recording());
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
    EXPECT_EQ(recordingFolders(root.path()).size(), 2);
}

/* Recording turns on the surrogate's frame logging for the spawns it
 * records, in the OS environment the child inherits, and takes it off
 * again at the next connect without recording. A value set at launch
 * stays. */
TEST(BridgeTest, RecordingTurnsOnSurrogateFrameLogging)
{
    const EnvRestore restore("ECUIO_SUR_LOG_FRAMES");
    qunsetenv("ECUIO_SUR_LOG_FRAMES");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    EXPECT_EQ(osEnv("ECUIO_SUR_LOG_FRAMES"), QByteArray("1"));
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    EXPECT_TRUE(readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/events.log"))
                    .contains("surrogate frame logging  on, set by this recording"));

    bridge.setRecording(false);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    EXPECT_TRUE(osEnv("ECUIO_SUR_LOG_FRAMES").isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));

    qputenv("ECUIO_SUR_LOG_FRAMES", "1");   // set at launch
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    EXPECT_EQ(osEnv("ECUIO_SUR_LOG_FRAMES"), QByteArray("1"));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The frame log belongs to a session: rows stay readable after a
 * disconnect, and the next connect starts an empty table. Checked as
 * the synchronous connect returns, before any queued frame can land. */
TEST(BridgeTest, ConnectStartsAnEmptyFrameLog)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://first").isEmpty());
    FrameListModel* proxy = bridge.model();
    ASSERT_TRUE(spinUntil([proxy]() { return proxy->rowCount() > 0; }, 5000));

    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_GT(proxy->rowCount(), 0) << "disconnect cleared the log";

    ASSERT_TRUE(bridge.connectSession("mock://second").isEmpty());
    EXPECT_EQ(proxy->rowCount(), 0);
    EXPECT_EQ(bridge.frameCount(), 0);

    bridge.disconnectSession();
}

/* Rows sort by arrival, not by frame timestamp: a J2534 timestamp is the
 * vendor's raw 32-bit stamp, which can wrap. A frame stamped earlier but
 * arriving later is still the newest row. */
TEST(BridgeTest, FrameLogOrdersByArrivalNotTimestamp)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://arrival").isEmpty());

    ecuio_frame_t first{};
    first.timestamp_us = 900000;
    first.arbitration_id = 0x123;   // not the mock's 0x0C0
    first.data_length = 1;
    ecuio_frame_t second = first;
    second.timestamp_us = 100;      // a wrapped stamp, say
    second.arbitration_id = 0x124;
    bridge.dispatchFrameForTesting(first);
    bridge.dispatchFrameForTesting(second);

    FrameListModel* proxy = bridge.model();
    auto row_of = [proxy](quint32 id) {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->data(proxy->index(r, 0),
                            FrameListModel::ArbitrationIdRole).toUInt() == id) {
                return r;
            }
        }
        return -1;
    };
    ASSERT_TRUE(spinUntil([&row_of]() {
        return row_of(0x123) >= 0 && row_of(0x124) >= 0;
    }, 5000));

    EXPECT_LT(row_of(0x124), row_of(0x123)) << "newest first";
    proxy->setNewestFirst(false);
    EXPECT_LT(row_of(0x123), row_of(0x124)) << "oldest first";

    // A row answers only its roles, and only rows that exist answer
    EXPECT_FALSE(proxy->data(proxy->index(0, 0), Qt::DisplayRole).isValid());
    EXPECT_FALSE(proxy
                     ->data(proxy->index(proxy->rowCount(), 0),
                            FrameListModel::ArbitrationIdRole)
                     .isValid());
    EXPECT_EQ(proxy->count(), proxy->rowCount());
    EXPECT_EQ(bridge.maxFrameCount(), FrameListModel::MaxFrames);

    bridge.disconnectSession();
}

TEST(BridgeTest, FrameLogOrdersNewestFirstByDefault)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-default").isEmpty());

    FrameListModel* proxy = bridge.model();
    // CONTRACT: more than two frames arrive sorted. Polled, not a fixed
    // sleep: starved CI runners can deliver only 2 in 300 ms.
    ASSERT_TRUE(spinUntil(
        [proxy]() { return proxy->rowCount() > 2; }, 5000))
        << "only " << proxy->rowCount() << " rows in 5s";
    EXPECT_TRUE(proxy->newestFirst());

    const quint64 first = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 last = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();
    EXPECT_GT(first, last);

    bridge.disconnectSession();
}

TEST(BridgeTest, FrameLogTogglesToOldestFirst)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-toggle").isEmpty());

    FrameListModel* proxy = bridge.model();
    ASSERT_TRUE(spinUntil(
        [proxy]() { return proxy->rowCount() > 2; }, 5000));

    proxy->setNewestFirst(false);
    EXPECT_FALSE(proxy->newestFirst());

    const quint64 first = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 last = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();
    EXPECT_LT(first, last);

    // Toggle back: must re-sort to descending without reconnect
    proxy->setNewestFirst(true);

    // Capture BOTH ends fresh
    const quint64 firstAgain = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 lastAgain = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();

    // Top is the newest we've ever seen (>= because the stream kept ticking)
    EXPECT_GE(firstAgain, last);
    // Bottom is the oldest: at most the original ascending-order top,
    // and strictly older than the top of the list
    EXPECT_LE(lastAgain, first);      // 'first' = oldest captured earlier
    EXPECT_GT(firstAgain, lastAgain); // reversal actually happened

    bridge.disconnectSession();
}

TEST(BridgeTest, NotifyCountTracksFrameProcessing)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://notify").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.guiNotifyCount() > 0; }, 5000));
    EXPECT_EQ(bridge.guiNotifyCount(), bridge.frameCount());
    EXPECT_GT(bridge.guiNotifyCount(), 0);
    bridge.disconnectSession();
}

TEST(BridgeTest, SignalBatchesAreCoalesced)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://coalesce").isEmpty());

    // CONTRACT: batches accumulate while the stream runs. No upper
    // bound on the count: a wall-clock bound is only a proxy for
    // "coalescing merges frames," which the ratio assertion below
    // states directly.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.signalBatchCount() > 4; }, 5000))
        << "only " << bridge.signalBatchCount() << " batches in 5s";

    EXPECT_GT(bridge.signalBatchCount(), 4);
    EXPECT_GT(bridge.frameCount(), 0);
    // Ratio contract: at ~20fps
    // frames vs 10Hz batches, coalescing means fewer batches than
    // frames. Per-frame emission (the regression this guards) yields
    // exact equality: one emission per frame.
    EXPECT_LE(bridge.signalBatchCount(), bridge.frameCount());

    // Values actually flowed through the model
    EXPECT_GT(bridge.signalModel()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, AWindowShowsItsNewestAndChartsItsEnvelope)
{
    /* Three frames' values inside one coalesce window, as core's worker
     * hands them to onValues, at 1, 2 and 3 ms of session time: 20, then
     * a peak of 30, then 10. The row shows the newest, numeric and text
     * alike, and the window's highest and lowest beside it; the chart
     * gets those at their own times, the peak included (ADR-0005's
     * appendix). The mock's own
     * frames (0x0C0) match neither def. */
    static ecuio_signal_def_t defs[2] = {};
    defs[0] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].message_id = 0x123u;
    defs[0].bit_length = 8;
    defs[0].scale = 1.0;
    strncpy(defs[0].name, "Byte", sizeof(defs[0].name) - 1);
    defs[1] = {};
    defs[1].id = 2;
    defs[1].kind = ECUIO_VALUE_HEX;
    defs[1].message_id = 0x123u;
    defs[1].byte_length = 1;
    defs[1].scale = 1.0;
    strncpy(defs[1].name, "Byte as hex", sizeof(defs[1].name) - 1);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-newest", &set, nullptr), ECUIO_OK);

    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://newest?profile=test-newest").isEmpty());
    spinLoop(150);   // a tick or more, so the window below starts clean
    const int batches = bridge.signalBatchCount();
    uint64_t at = 0;
    for (const char* hex : {"14", "1E", "0A"}) {
        ecuio_value_t values[2] = {};
        values[0].signal_id = 1;
        values[0].kind = ECUIO_VALUE_UINT;
        values[0].value = static_cast<double>(std::strtol(hex, nullptr, 16));
        values[1].signal_id = 2;
        values[1].kind = ECUIO_VALUE_HEX;
        values[1].text = hex;
        values[1].text_len = 2;
        ecuio_value_batch_t batch = {};
        batch.timestamp_us = at += 1000;
        batch.arbitration_id = 0x123;
        batch.count = 2;
        batch.values = values;
        bridge.dispatchValuesForTesting(batch);
    }
    ASSERT_TRUE(spinUntil([&]() { return bridge.signalBatchCount() > batches; }, 2000));

    SignalListModel* model = bridge.signalModel();
    ASSERT_EQ(model->rowCount(), 2);
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::ValueRole).toDouble(), 10.0)
        << "the newest";
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::WindowHighRole).toDouble(), 30.0)
        << "the Dashboard's peak hold sees the window's highest";
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::WindowLowRole).toDouble(), 10.0);
    EXPECT_EQ(model->roleNames().value(SignalListModel::WindowHighRole), "windowHigh");
    EXPECT_EQ(model->roleNames().value(SignalListModel::WindowLowRole), "windowLow");
    EXPECT_EQ(model->data(model->index(1, 0), SignalListModel::TextRole).toString(),
              QStringLiteral("0A"));
    const QVariantList chart = model->chartData();
    ASSERT_EQ(chart.size(), 1);
    QList<double> samples;
    for (const QVariant& v : chart.at(0).toMap().value("samples").toList())
        samples << v.toDouble();
    EXPECT_EQ(samples, (QList<double>{2, 30, 3, 10}))
        << "the peak at 2 ms and the low, which is the newest, at 3 ms";
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(ecuio_profile_unregister("test-newest"), ECUIO_OK);
}

namespace {

/* Dispatches n frames with ID 0x123 and stamps first, first + 1, ...,
 * letting each burst drain so none is dropped at the in-flight gate. */
void dispatchNumbered(SessionBridge& bridge, int first, int n)
{
    ecuio_frame_t f{};
    f.arbitration_id = 0x123;   // not the mock's 0x0C0
    f.data_length = 2;
    const int burst = SessionBridge::maxInFlightForTesting() / 2;
    for (int i = 0; i < n; ++i) {
        f.timestamp_us = static_cast<uint64_t>(first + i);
        f.data[0] = static_cast<uint8_t>((first + i) >> 8);
        f.data[1] = static_cast<uint8_t>(first + i);
        bridge.dispatchFrameForTesting(f);
        if ((i + 1) % burst == 0) {
            spinLoop(5);
        }
    }
    spinLoop(50);
}

/* The 0x123 rows' stamps, top to bottom; mock frames between them are
 * counted in others. */
std::vector<quint64> numberedRows(const FrameListModel* model, int* others)
{
    std::vector<quint64> out;
    *others = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        if (model->data(idx, FrameListModel::ArbitrationIdRole).toUInt() == 0x123u) {
            out.push_back(model->data(idx, FrameListModel::TimestampRole).toULongLong());
        } else {
            ++*others;
        }
    }
    return out;
}

} // namespace

/* Past the cap the log keeps the newest MaxFrames in arrival order, in
 * either direction, each row still showing its own frame: the ring's
 * slot arithmetic, through the overwrite of the oldest and a toggle.
 * A view sees the oldest row leave from its end and the new one arrive
 * at the other, which is what keeps a ListView's rows right. */
TEST(BridgeTest, FullFrameLogKeepsTheNewestInOrder)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://ring").isEmpty());
    FrameListModel* model = bridge.model();
    int wrongRemovals = 0;
    int wrongInserts = 0;
    QObject::connect(model, &QAbstractItemModel::rowsAboutToBeRemoved,
                     [model, &wrongRemovals](const QModelIndex&, int first, int last) {
        const int oldest = model->newestFirst() ? model->rowCount() - 1 : 0;
        if (first != oldest || last != oldest) ++wrongRemovals;
    });
    QObject::connect(model, &QAbstractItemModel::rowsInserted,
                     [model, &wrongInserts](const QModelIndex&, int first, int last) {
        const int newest = model->newestFirst() ? 0 : model->rowCount() - 1;
        if (first != newest || last != newest) ++wrongInserts;
    });
    const int total = FrameListModel::MaxFrames + 250;
    dispatchNumbered(bridge, 1, total);
    ASSERT_EQ(bridge.droppedFrames(), 0);

    ASSERT_EQ(model->rowCount(), FrameListModel::MaxFrames);
    int others = 0;
    std::vector<quint64> rows = numberedRows(model, &others);
    ASSERT_EQ(static_cast<int>(rows.size()) + others, FrameListModel::MaxFrames);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front(), static_cast<quint64>(total)) << "the newest on top";
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] - 1) << "row " << i << " out of order";
    }
    // Each row's payload is its own frame's, not a stale slot's: every
    // numbered row carries its stamp in its two bytes, and the mock's
    // rows between them keep their own length
    int checked = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        if (model->data(idx, FrameListModel::ArbitrationIdRole).toUInt() != 0x123u) {
            EXPECT_NE(model->data(idx, FrameListModel::DlcRole).toUInt(), 2u)
                << "row " << r << " has a numbered frame's length";
            continue;
        }
        const quint64 n = model->data(idx, FrameListModel::TimestampRole).toULongLong();
        ASSERT_EQ(model->data(idx, FrameListModel::PayloadRole).toString(),
                  QStringLiteral("%1 %2")
                      .arg((n >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                      .arg(n & 0xFF, 2, 16, QLatin1Char('0')).toUpper())
            << "row " << r;
        ASSERT_EQ(model->data(idx, FrameListModel::DlcRole).toUInt(), 2u) << "row " << r;
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<int>(rows.size()));

    model->setNewestFirst(false);
    rows = numberedRows(model, &others);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back(), static_cast<quint64>(total)) << "the newest at the bottom";
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] + 1) << "row " << i << " out of order";
    }

    // And it keeps going round while oldest-first
    dispatchNumbered(bridge, total + 1, 300);
    ASSERT_EQ(model->rowCount(), FrameListModel::MaxFrames);
    rows = numberedRows(model, &others);
    EXPECT_EQ(rows.back(), static_cast<quint64>(total + 300));
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] + 1) << "row " << i << " out of order";
    }
    EXPECT_EQ(wrongRemovals, 0) << "a row other than the oldest left";
    EXPECT_EQ(wrongInserts, 0) << "a frame arrived at the wrong end";
    bridge.disconnectSession();
}

/* A payload up to InlinePayload bytes lives in its row; a longer one
 * holds a heap block only while its row is shown, so a run of long
 * replies leaves nothing behind once short frames take the slots. Clear
 * keeps the slots (no reallocation on the next connect) and frees the
 * blocks. */
TEST(BridgeTest, FrameLogHoldsLongPayloadsOnlyWhileShown)
{
    FrameListModel model;
    auto frame = std::make_unique<ecuio_frame_t>();
    *frame = ecuio_frame_t{};
    quint64 stamp = 0;
    auto append = [&](uint16_t len) {
        frame->timestamp_us = ++stamp;
        for (uint16_t i = 0; i < len; ++i) {
            frame->data[i] = static_cast<uint8_t>(stamp + i);
        }
        model.appendFrame(*frame, len);
    };
    // The newest row's bytes, as the Log page shows them
    auto newestHex = [&]() {
        return model.data(model.index(0, 0), FrameListModel::PayloadRole)
            .toString();
    };
    auto expectedHex = [](quint64 s, int len) {
        QByteArray bytes;
        for (int i = 0; i < len; ++i)
            bytes.append(static_cast<char>(s + i));
        return QString::fromLatin1(bytes.toHex(' ').toUpper());
    };
    const int cap = FrameListModel::MaxFrames;

    for (int i = 0; i < cap; ++i)
        append(200);
    EXPECT_GE(model.spilledBytesForTesting(), static_cast<size_t>(cap) * 200);
    EXPECT_EQ(newestHex(), expectedHex(stamp, 200));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::DlcRole).toUInt(),
              200u);

    // Short frames round the ring: every long block is gone
    for (int i = 0; i < cap; ++i)
        append(27);
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    EXPECT_EQ(newestHex(), expectedHex(stamp, 27));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::DlcRole).toUInt(),
              27u);

    // InlinePayload itself stays inline; one byte past it spills
    append(FrameListModel::InlinePayload);
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    EXPECT_EQ(newestHex(), expectedHex(stamp, FrameListModel::InlinePayload));
    append(FrameListModel::InlinePayload + 1);
    EXPECT_GE(model.spilledBytesForTesting(),
              static_cast<size_t>(FrameListModel::InlinePayload + 1));
    EXPECT_EQ(newestHex(),
              expectedHex(stamp, FrameListModel::InlinePayload + 1));

    // Clear keeps the slots and frees the blocks
    model.clear();
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    for (int i = 0; i < 3; ++i)
        append(8);
    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    for (int r = 0; r < 3; ++r) {
        EXPECT_EQ(model.data(model.index(r, 0), FrameListModel::TimestampRole)
                      .toULongLong(),
                  stamp - static_cast<quint64>(r))
            << "row " << r;
    }

    // A ring cleared before it filled grows only as far as it's used
    FrameListModel partial;
    for (int i = 0; i < 5; ++i)
        partial.appendFrame(*frame, 8);
    partial.clear();
    for (int i = 0; i < 7; ++i) {
        frame->timestamp_us = 100 + static_cast<quint64>(i);
        partial.appendFrame(*frame, 8);
    }
    EXPECT_EQ(partial.slotsForTesting(), 7u);
    ASSERT_EQ(partial.rowCount(), 7);
    for (int r = 0; r < 7; ++r) {
        EXPECT_EQ(
            partial.data(partial.index(r, 0), FrameListModel::TimestampRole)
                .toULongLong(),
            106u - static_cast<quint64>(r))
            << "row " << r;
    }
}

/* The Log page's selection holds frames, not row numbers: it stays on
 * them as rows arrive, through a flip of the order, and a frame leaves
 * it as it leaves the ring. A shift-click selects from the anchor; the
 * copied text is the selected rows in view order, under a header line. */
TEST(BridgeTest, FrameLogSelectionFollowsItsFrames)
{
    FrameListModel model;
    auto frame = std::make_unique<ecuio_frame_t>();
    *frame = ecuio_frame_t{};
    quint64 stamp = 0;
    auto append = [&]() {
        frame->timestamp_us = ++stamp * 1000;
        frame->arbitration_id = 0x7E8;
        frame->data[0] = static_cast<uint8_t>(stamp);
        frame->data[1] = 0x0C;
        model.appendFrame(*frame, 2);
    };
    auto selectedAt = [&](int row) {
        return model.data(model.index(row, 0), FrameListModel::SelectedRole).toBool();
    };
    auto stampAt = [&](int row) {
        return model.data(model.index(row, 0), FrameListModel::TimestampRole)
            .toULongLong();
    };
    int selectionSignals = 0;
    QObject::connect(&model, &FrameListModel::selectionChanged,
                     [&selectionSignals]() { ++selectionSignals; });

    for (int i = 0; i < 5; ++i)
        append();
    EXPECT_EQ(model.selectedText(), QString());
    model.toggleSelected(1);   // newest first: the 4th frame
    ASSERT_EQ(model.selectedCount(), 1);
    EXPECT_TRUE(selectedAt(1));
    EXPECT_EQ(selectionSignals, 1);

    // A new frame pushes the row down; the selection goes with it
    append();
    EXPECT_FALSE(selectedAt(1));
    EXPECT_TRUE(selectedAt(2));
    EXPECT_EQ(stampAt(2), 4000u);

    // Shift-click from the anchor (row 2) to row 4: frames 4, 3 and 2
    model.selectThrough(4);
    EXPECT_EQ(model.selectedCount(), 3);
    EXPECT_EQ(model.selectedText(),
              QStringLiteral("time_s\tdlc\text\tid\tdata\n"
                             "0.004000\t2\t-\t0x7E8\t04 0C\n"
                             "0.003000\t2\t-\t0x7E8\t03 0C\n"
                             "0.002000\t2\t-\t0x7E8\t02 0C\n"));

    // Oldest first, the same frames, in the view's new order
    model.setNewestFirst(false);
    EXPECT_EQ(model.selectedCount(), 3);
    EXPECT_TRUE(selectedAt(1) && selectedAt(2) && selectedAt(3));
    EXPECT_FALSE(selectedAt(0) || selectedAt(4) || selectedAt(5));
    EXPECT_TRUE(model.selectedText().startsWith(
        QStringLiteral("time_s\tdlc\text\tid\tdata\n0.002000\t")));

    // A tap on a selected row lets it go
    model.toggleSelected(1);
    EXPECT_EQ(model.selectedCount(), 2);
    EXPECT_FALSE(selectedAt(1));

    // The ring wraps: the selected frames leave it with their rows
    const int before = selectionSignals;
    for (int i = 0; i < FrameListModel::MaxFrames; ++i)
        append();
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_EQ(model.selectedText(), QString());
    EXPECT_EQ(selectionSignals, before + 2) << "once for each frame that left";

    // The anchor left too: a shift-click selects just its own row
    model.selectThrough(7);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_TRUE(selectedAt(7));

    // Clear and Select none empty it
    model.clearSelection();
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_FALSE(selectedAt(7));
    model.toggleSelected(0);
    model.clear();
    EXPECT_EQ(model.selectedCount(), 0);
    append();
    EXPECT_FALSE(selectedAt(0));
}

/* frameCount and guiNotifyCount reach QML on the 100 ms tick, not once
 * per frame: a burst of 400 frames changes each a handful of times. The
 * disconnect reports the last of them, so what QML last saw is exact. */
TEST(BridgeTest, FrameCountsAreReportedOnTheTick)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://counts").isEmpty());
    int countSignals = 0;
    int notifySignals = 0;
    int seenFrames = -1;
    int seenNotify = -1;
    QObject::connect(&bridge, &SessionBridge::frameCountChanged,
                     [&]() { ++countSignals; seenFrames = bridge.frameCount(); });
    QObject::connect(&bridge, &SessionBridge::notifyCountChanged,
                     [&](int n) { ++notifySignals; seenNotify = n; });

    const int before = bridge.frameCount();
    dispatchNumbered(bridge, 1, 400);
    spinLoop(250);   // two ticks past the burst
    EXPECT_GE(bridge.frameCount(), before + 400);
    EXPECT_GT(countSignals, 0) << "the frame count never reached QML";
    EXPECT_GT(notifySignals, 0) << "the notify count never reached QML";
    EXPECT_LT(countSignals, 40) << "reported per frame, not per tick";
    EXPECT_LT(notifySignals, 40) << "reported per frame, not per tick";

    bridge.disconnectSession();   // no frame is counted after this
    EXPECT_EQ(seenFrames, bridge.frameCount());
    EXPECT_EQ(seenNotify, bridge.guiNotifyCount());
    EXPECT_EQ(bridge.guiNotifyCount(), bridge.frameCount())
        << "one GUI-thread delivery per frame";
}

/* Frames flagged ECUIO_FRAME_FLAG_OVERFLOW (the adapter lost frames
 * before them) are counted on the tick, the first with one activity-log
 * line; a new session starts the count again */
TEST(BridgeTest, FramesAfterALossAtTheAdapterAreCountedAndLoggedOnce)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::overflowFramesChanged,
                     [&changes]() { ++changes; });
    ASSERT_TRUE(bridge.connectSession("mock://overflow").isEmpty());
    ecuio_frame_t f{};
    f.arbitration_id = 0x123;
    f.data_length = 1;
    bridge.dispatchFrameForTesting(f);
    f.flags = ECUIO_FRAME_FLAG_OVERFLOW;
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 1);
    bridge.dispatchFrameForTesting(f);
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 3);
    EXPECT_GE(changes, 2);
    const QString line =
        QStringLiteral("The adapter's buffer overflowed: frames were lost");
    EXPECT_EQ(log.count(line), 1) << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.overflowFrames(), 3) << "readable after the disconnect";
    ASSERT_TRUE(bridge.connectSession("mock://overflow-again").isEmpty());
    EXPECT_EQ(bridge.overflowFrames(), 0);
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 1);
    EXPECT_EQ(log.count(line), 2) << "once a session";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, StreamingContinuesPastTenThousandFrames)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://longrun").isEmpty());

    // CONTRACT: at least one frame flows first. Poll, no fixed 150ms.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    ASSERT_GT(bridge.frameCount(), 0);

    // Simulate the old freeze boundary: cumulative count just below 10k.
    // Under the OLD gate, the next frame would hit >= MaxFrames and
    // every subsequent frame would be dropped at source, forever.
    bridge.forceFrameCountForTesting(9999);

    // CONTRACT: the count advances past the boundary. Was a fixed
    // 600ms wait (needs ~2 frames): poll until it's demonstrably past.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 10000; }, 5000))
        << "stream stalled at " << bridge.frameCount();
    EXPECT_GT(bridge.frameCount(), 10000);

    // And no drops occurred: nothing was rejected at the boundary
    EXPECT_EQ(bridge.droppedFrames(), 0);

    // Liveness, not coincidence: the log model kept receiving rows too
    EXPECT_GT(bridge.model()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, DispatchedShortFrameHonorsDataLength)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://short").isEmpty());

    // Unique timestamp so we can find OUR row among mock frames
    ecuio_frame_t f{};
    f.timestamp_us = 777777;   // steady-clock us from the mock won't collide
    f.arbitration_id = 0x0C0;  // telemetry ID: decodes (gate-consistent)
    f.channel_id = 0;
    f.data_length = 4;          // the whole point: NOT 8
    f.flags = ECUIO_FRAME_FLAG_EXTENDED;    // was: f.is_extended = 1
    f.data[0] = 0x34;           // arbitrary RPM-looking bytes
    f.data[1] = 0x12;
    f.data[2] = 0xAA;
    f.data[3] = 0x55;

    bridge.dispatchFrameForTesting(f);

    // CONTRACT: our dispatched frame reaches the model. Poll on the
    // finding condition instead of a fixed 200ms settle: the row must
    // appear through the queued-invocation path regardless of runner speed.
    FrameListModel* proxy = bridge.model();
    auto row_finder = [proxy]() {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->data(proxy->index(r, 0),
                    FrameListModel::TimestampRole).toULongLong() == 777777) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(spinUntil(row_finder, 5000))
        << "dispatched frame never reached the model";

    int foundRow = -1;
    for (int r = 0; r < proxy->rowCount(); ++r) {
        if (proxy->data(proxy->index(r, 0),
                FrameListModel::TimestampRole).toULongLong() == 777777) {
            foundRow = r;
            break;
        }
    }
    ASSERT_NE(foundRow, -1) << "row vanished between poll and assert";

    const QModelIndex idx = proxy->index(foundRow, 0);
    // DLC must reflect the ACTUAL length, not an assumed 8
    EXPECT_EQ(proxy->data(idx, FrameListModel::DlcRole).toUInt(), 4u);
    // Exactly the 4 bytes, not 8, as spaced uppercase hex
    EXPECT_EQ(proxy->data(idx, FrameListModel::PayloadRole).toString(),
              QStringLiteral("34 12 AA 55"));
    EXPECT_TRUE(proxy->data(idx, FrameListModel::ExtendedRole).toBool());
    EXPECT_EQ(proxy->data(idx, FrameListModel::ArbitrationIdRole).toUInt(), 0x0C0u);

    bridge.disconnectSession();
}

/* LogPage binds frame columns by role NAME. A name missing here isn't a
 * build error: the column renders empty and every delegate throws. Read
 * through Bridge.model, what QML gets. */
TEST(BridgeTest, FrameRoleNamesMatchLogPageBindings)
{
    SessionBridge bridge;
    const QHash<int, QByteArray> roles = bridge.model()->roleNames();
    EXPECT_EQ(roles.value(FrameListModel::TimestampRole), QByteArray("timestampUs"));
    EXPECT_EQ(roles.value(FrameListModel::DlcRole), QByteArray("dlc"));
    EXPECT_EQ(roles.value(FrameListModel::ExtendedRole), QByteArray("extended"));
    EXPECT_EQ(roles.value(FrameListModel::PayloadRole), QByteArray("payloadHex"));
    EXPECT_EQ(roles.value(FrameListModel::ArbitrationIdRole), QByteArray("arbId"));
    EXPECT_EQ(roles.value(FrameListModel::SelectedRole), QByteArray("selected"));
}

TEST(BridgeTest, LogPauseStopsFrameLogNotDecode)
{
    /* Pause holds the frame log still to read: nothing is appended while
     * paused, but frames are still counted and decoded, so Live and the
     * Dashboard carry on. Resume appends again, and pause belongs to a
     * session: disconnect clears it and a new session starts unpaused. */
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://pause").isEmpty());
    FrameListModel* proxy = bridge.model();
    ASSERT_TRUE(spinUntil([proxy]() { return proxy->rowCount() > 0; }, 5000));

    bridge.setLogPaused(true);
    ASSERT_TRUE(bridge.logPaused());
    const int rows = proxy->rowCount();
    const int frames = bridge.frameCount();
    const int batches = bridge.signalBatchCount();

    // CONTRACT: frames and batches keep coming while the log holds. The
    // row count is exact: pause is set on the GUI thread, where every
    // append happens.
    ASSERT_TRUE(spinUntil([&bridge, frames, batches]() {
        return bridge.frameCount() > frames + 2 && bridge.signalBatchCount() > batches;
    }, 5000)) << "stream stopped while paused";
    EXPECT_EQ(proxy->rowCount(), rows);

    bridge.setLogPaused(false);
    ASSERT_TRUE(spinUntil([proxy, rows]() { return proxy->rowCount() > rows; }, 5000))
        << "log did not resume";

    bridge.setLogPaused(true);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.logPaused());

    bridge.setLogPaused(true);
    ASSERT_TRUE(bridge.connectSession("mock://pause-again").isEmpty());
    EXPECT_FALSE(bridge.logPaused());
    bridge.disconnectSession();
}

TEST(BridgeTest, BackpressureDropsAreCountedAndReported)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://drops").isEmpty());

    // Lambda-counter instead of QSignalSpy: avoids a new Qt6::Test dependency
    int notifyCount = 0;
    int lastReported = -1;
    QObject::connect(&bridge, &SessionBridge::droppedFramesChanged,
                     [&](int dropped) { ++notifyCount; lastReported = dropped; });

    ecuio_frame_t f{};
    f.data_length = 8;

    const int inject = SessionBridge::maxInFlightForTesting() + 500;

    // Synchronous flood from the test thread: no event processing happens,
    // so in-flight climbs monotonically to the cap and stays there.
    for (int i = 0; i < inject; ++i) {
        f.timestamp_us = 1000000 + static_cast<uint64_t>(i);
        bridge.dispatchFrameForTesting(f);
    }

    // Drops happened during saturation (loop 1001..1500 hit the gate).
    // GE not EQ: the mock worker may contribute a frame or two mid-loop.
    EXPECT_GE(bridge.droppedFrames(), 500);

    // Nothing could have been reported yet: the queue hasn't been processed
    EXPECT_EQ(notifyCount, 0);

    // CONTRACT: the backlog drains. Was a fixed 800ms; poll until the
    // overwhelming majority of the queued invocations processed.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 900; }, 10000))
        << "drain stalled at " << bridge.frameCount() << " frames";

    // Accounting survived the drain
    EXPECT_GE(bridge.droppedFrames(), 500);
    // The consolidated notification arrived with the final value
    EXPECT_GE(notifyCount, 1);
    EXPECT_EQ(lastReported, bridge.droppedFrames());
    // The queued frames were actually processed, not silently lost
    EXPECT_GT(bridge.frameCount(), 900);
    // Drained backlog frees the gate again: no permanent wedge
    // (the running mock resumes normal delivery after this)

    bridge.disconnectSession();
}

// ---------------------------------------------------------------------------
// Strip chart: per-signal rolling history ring in SignalListModel
// ---------------------------------------------------------------------------

namespace {

/* Minimal two-signal set, built test-side. Plain assignments only:
   designated initializers are C++20, and MSVC accepts them where GCC
   rejects them. */
void buildTestSignalSet(ecuio_signal_def_t (&defs)[2], ecuio_signal_set_t& set)
{
    ecuio_signal_def_t& rpm = defs[0];
    rpm = {};
    rpm.id = 1001;
    strncpy(rpm.name, "Engine_RPM", sizeof(rpm.name) - 1);
    rpm.kind = ECUIO_VALUE_UINT;
    strncpy(rpm.unit, "RPM", sizeof(rpm.unit) - 1);

    ecuio_signal_def_t& temp = defs[1];
    temp = {};
    temp.id = 2001;
    strncpy(temp.name, "Coolant_Temp", sizeof(temp.name) - 1);
    temp.kind = ECUIO_VALUE_INT;
    strncpy(temp.unit, "degC", sizeof(temp.unit) - 1);

    set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
}

} // namespace

TEST(BridgeTest, HistoryRingEvictsOldestAtCapacity)
{
    /* Deterministic: injected timestamps + values equal to the timestamp,
       so retention/ordering asserts are exact arithmetic, not timing. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    // Push 10 MORE one-point batches than the ring holds.
    std::vector<std::pair<uint16_t, double>> batch;
    for (qint64 t = 0; t < SignalListModel::HistoryCapacity + 10; ++t) {
        batch.clear();
        batch.emplace_back(uint16_t(1001), double(t));
        batch.emplace_back(uint16_t(2001), double(t) - 40.0);
        model.updateValues(batch, t);
    }

    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 2);   // both signals subscribed with history

    for (int s = 0; s < 2; ++s) {
        const QVariantMap m = data.at(s).toMap();
        const QVariantList samples = m.value("samples").toList();
        // Exactly capacity survives: ring size is a hard bound, no leaks
        ASSERT_EQ(samples.size(), SignalListModel::HistoryCapacity * 2)
            << "wrong sample count for " << m.value("name").toString().toStdString();

        // Oldest retained = push #10; newest = the last push.
        // Exact ints: injected timestamps make wrap position deterministic.
        const double newest = SignalListModel::HistoryCapacity + 9;
        EXPECT_EQ(samples.at(0).toDouble(), 10.0);
        EXPECT_EQ(samples.at(samples.size() - 2).toDouble(), newest);

        // Values track their timestamps (order preserved through the wrap)
        const double offset = (m.value("id").toInt() == 1001) ? 0.0 : -40.0;
        EXPECT_NEAR(samples.at(1).toDouble(), 10.0 + offset, 0.01);
        EXPECT_NEAR(samples.at(samples.size() - 1).toDouble(), newest + offset, 0.01);

        // Monotonic timestamps oldest->newest (wrap-around correctness)
        double prev = -1.0;
        for (int i = 0; i < samples.size(); i += 2) {
            EXPECT_GE(samples.at(i).toDouble(), prev);
            prev = samples.at(i).toDouble();
        }
    }
}

/* A window's points (ADR-0005's appendix): its lowest, highest and
 * newest value in time order, each once; the row's age counts from the
 * value's arrival, whatever its sample time */
TEST(BridgeTest, AWindowChartsItsLowestHighestAndNewestInTimeOrder)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto window = [](double last, qint64 lastMs, double min, qint64 minMs,
                           double max, qint64 maxMs) {
        SignalListModel::WindowValue w;
        w.id = 1001;
        w.last = last;
        w.lastMs = lastMs;
        w.min = min;
        w.minMs = minMs;
        w.max = max;
        w.maxMs = maxMs;
        return w;
    };
    const auto points = [&model]() {
        QList<double> out;
        for (const QVariant& v : model.chartData().at(0).toMap().value("samples").toList())
            out << v.toDouble();
        return out;
    };
    model.updateWindow({window(25, 30, 5, 10, 50, 20)});
    EXPECT_EQ(points(), (QList<double>{10, 5, 20, 50, 30, 25})) << "three apart";
    model.updateWindow({window(7, 40, 7, 40, 7, 40)});
    EXPECT_EQ(points().size(), 8) << "one value: one point";
    model.updateWindow({window(1, 60, 1, 60, 9, 50)});
    EXPECT_EQ(points().mid(8), (QList<double>{50, 9, 60, 1}))
        << "the high first; the low is the newest";
    EXPECT_EQ(model.data(model.index(0, 0), SignalListModel::ValueRole).toDouble(), 1.0);

    /* Sample times far past the model's clock, as a replay's can run */
    model.updateWindow({window(3, 1000000000, 3, 1000000000, 3, 1000000000)});
    const int age = model.data(model.index(0, 0), SignalListModel::AgeMsRole).toInt();
    EXPECT_GE(age, 0);
    EXPECT_LT(age, 5000) << "from arrival, not sample time";
}

TEST(BridgeTest, ChartDataFiltersUnsubscribedAndEmptyBatchesDoNotEmit)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    int emissions = 0;
    QObject::connect(&model, &SignalListModel::valuesUpdated,
                     [&emissions]() { ++emissions; });

    std::vector<std::pair<uint16_t, double>> batch;
    for (int t = 0; t < 5; ++t) {
        batch.clear();
        batch.emplace_back(uint16_t(1001), double(t));
        batch.emplace_back(uint16_t(2001), double(t));
        model.updateValues(batch, t);
    }
    ASSERT_EQ(emissions, 5);   // one per non-empty batch, per contract

    // Empty batch = silent: no dataChanged, no valuesUpdated
    model.updateValues({}, 999);
    EXPECT_EQ(emissions, 5);

    // Unknown IDs = silent too (defensive: staging should never send them)
    batch.clear();
    batch.emplace_back(uint16_t(9999), 1.0);
    model.updateValues(batch, 1000);
    EXPECT_EQ(emissions, 5);
    EXPECT_EQ(model.chartData().size(), 2);

    // Unsubscribe signal 0: excluded from chartData
    model.toggleSubscribed(0);
    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data.at(0).toMap().value("id").toInt(), 2001);
    // `row` is the model row, not the position in chartData(), so the
    // trace keeps its color when a signal before it is unsubscribed
    EXPECT_EQ(data.at(0).toMap().value("row").toInt(), 1);
    EXPECT_EQ(data.at(0).toMap().value("samples").toList().size(), 10); // still 5 samples

    // Resubscribed signal keeps its accumulated history (non-destructive)
    model.toggleSubscribed(0);
    EXPECT_EQ(model.chartData().size(), 2);
    EXPECT_EQ(model.chartData().at(0).toMap().value("samples").toList().size(), 10);
}

TEST(BridgeTest, UnsubscribedSignalKeepsUpdating)
{
    /* A subscription chooses what the chart plots, nothing else. An
     * unsubscribed row still takes values, so the Dashboard's gauge for
     * it stays live instead of dimming as stale while its frames arrive,
     * and its history keeps growing, so the trace is whole when it's
     * plotted again. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    model.toggleSubscribed(0);   // row 0 is id 1001

    bool row0Notified = false;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&row0Notified](const QModelIndex& topLeft, const QModelIndex& bottomRight,
                        const QList<int>& roles) {
            if (roles.contains(SignalListModel::ValueRole)
                && topLeft.row() <= 0 && bottomRight.row() >= 0) {
                row0Notified = true;
            }
        });

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(1001), 3000.0);
    batch.emplace_back(uint16_t(2001), 7.0);
    model.updateValues(batch, 10);

    const QModelIndex row0 = model.index(0, 0);
    EXPECT_TRUE(row0Notified);
    EXPECT_FALSE(model.data(row0, SignalListModel::SubscribedRole).toBool());
    EXPECT_DOUBLE_EQ(model.data(row0, SignalListModel::ValueRole).toDouble(), 3000.0);
    EXPECT_TRUE(model.data(row0, SignalListModel::HasValueRole).toBool());

    // Only the subscribed row is plotted
    const QVariantList plotted = model.chartData();
    ASSERT_EQ(plotted.size(), 1);
    EXPECT_EQ(plotted.at(0).toMap().value("id").toInt(), 2001);

    // Plotted again: the sample taken while unsubscribed is in its trace
    model.toggleSubscribed(0);
    const QVariantList both = model.chartData();
    ASSERT_EQ(both.size(), 2);
    EXPECT_EQ(both.at(0).toMap().value("id").toInt(), 1001);
    EXPECT_EQ(both.at(0).toMap().value("samples").toList().size(), 2);   // one [ts, val]
}

/* A text row (a TEXT or HEX def) carries its latest text, counts as
 * having a value once one arrived, and is never plotted: not
 * subscribed, not toggled, not in chartData. */
TEST(BridgeTest, TextRowsShowTheirTextAndNeverPlot)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[1].kind = ECUIO_VALUE_HEX;   /* row 1, id 2001, a text row */
    model.setSignalSet(&set);

    const QModelIndex row0 = model.index(0, 0);
    const QModelIndex row1 = model.index(1, 0);
    EXPECT_FALSE(model.data(row0, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model.data(row1, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model.data(row0, SignalListModel::SubscribedRole).toBool());
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.subscribedCount(), 1);
    model.toggleSubscribed(1);   /* nothing to plot: a no-op */
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.subscribedCount(), 1);
    EXPECT_FALSE(model.data(row1, SignalListModel::HasValueRole).toBool());
    EXPECT_EQ(model.data(row1, SignalListModel::TextRole).toString(), QString());

    QList<int> notified;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&notified](const QModelIndex& topLeft, const QModelIndex&, const QList<int>& roles) {
            if (topLeft.row() == 1) notified = roles;
        });
    model.updateTexts({{uint16_t(2001), QStringLiteral("62 F1 90")}}, 10);
    EXPECT_EQ(model.data(row1, SignalListModel::TextRole).toString(), QStringLiteral("62 F1 90"));
    EXPECT_TRUE(model.data(row1, SignalListModel::HasValueRole).toBool());
    EXPECT_NE(model.data(row1, SignalListModel::AgeMsRole).toInt(), -1);   /* arrived */
    EXPECT_TRUE(notified.contains(SignalListModel::TextRole));
    EXPECT_TRUE(notified.contains(SignalListModel::HasValueRole));
    EXPECT_TRUE(notified.contains(SignalListModel::AgeMsRole));
    EXPECT_TRUE(model.chartData().isEmpty()) << "a text row was plotted";

    /* A numeric row ignores a text batch */
    model.updateTexts({{uint16_t(1001), QStringLiteral("no")}}, 11);
    EXPECT_FALSE(model.data(row0, SignalListModel::HasValueRole).toBool());
    EXPECT_EQ(model.data(row0, SignalListModel::TextRole).toString(), QString());
}

/* An enum row (ADR-0051) shows its label for the newest raw value, and
 * a raw value with no label its number; the label is notified with the
 * value, and a row of another kind has none */
TEST(BridgeTest, AnEnumRowShowsItsLabel)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    static const ecuio_enum_entry_t kGears[] = {
        {0, "Park"}, {3, "Drive"}, {7, "Low"}};
    defs[1].kind = ECUIO_VALUE_ENUM; /* row 1, id 2001 */
    defs[1].table_first = 1;         /* Drive and Low */
    defs[1].table_count = 2;
    set.enums = kGears;
    set.enum_count = 3;
    model.setSignalSet(&set);

    const QModelIndex row0 = model.index(0, 0);
    const QModelIndex row1 = model.index(1, 0);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString())
        << "a label before any value";
    QList<int> notified;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&notified](const QModelIndex& topLeft, const QModelIndex&,
                                 const QList<int>& roles) {
                         if (topLeft.row() <= 1)
                             notified = roles;
                     });
    model.updateValues({{uint16_t(2001), 3.0}, {uint16_t(1001), 3.0}}, 10);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QStringLiteral("Drive"));
    EXPECT_TRUE(notified.contains(SignalListModel::LabelRole));
    EXPECT_EQ(model.data(row0, SignalListModel::LabelRole).toString(),
              QString())
        << "a number row has no label";
    model.updateValues({{uint16_t(2001), 7.0}}, 11);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QStringLiteral("Low"));
    /* Park is the set's, but not the def's: past its table_first */
    model.updateValues({{uint16_t(2001), 0.0}}, 12);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString());
    model.updateValues({{uint16_t(2001), 5.0}}, 13);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString())
        << "a raw value with no label shows its number";
    EXPECT_DOUBLE_EQ(model.data(row1, SignalListModel::ValueRole).toDouble(),
                     5.0);
}

/* A text def takes the bridge's whole path like a number: decoded
 * from the frame (here two bytes of the mock's telemetry as hex),
 * staged, shown on its row, and written to signals.csv. */
TEST(BridgeTest, TextSignalsReachTheRowAndTheRecording)
{
    static ecuio_signal_def_t defs[2] = {};
    defs[0] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_HEX;
    defs[0].message_id = 0x0C0u;   /* the mock's telemetry frame */
    defs[0].byte_length = 2;
    defs[0].scale = 1.0;
    strncpy(defs[0].name, "Raw bytes", sizeof(defs[0].name) - 1);
    defs[1] = {};
    defs[1].id = 2;
    defs[1].kind = ECUIO_VALUE_UINT;
    defs[1].message_id = 0x0C0u;
    defs[1].bit_length = 8;
    defs[1].scale = 1.0;
    strncpy(defs[1].name, "First byte", sizeof(defs[1].name) - 1);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-text", &set, nullptr), ECUIO_OK);

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://text?profile=test-text").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.signalBatchCount() > 1; }, 5000));

    SignalListModel* model = bridge.signalModel();
    ASSERT_EQ(model->rowCount(), 2);
    const QModelIndex raw = model->index(0, 0);
    const QModelIndex first = model->index(1, 0);
    EXPECT_TRUE(model->data(raw, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model->data(raw, SignalListModel::HasValueRole).toBool());
    const QString text = model->data(raw, SignalListModel::TextRole).toString();
    const auto hexPair = [](const QString& s) {
        if (s.size() != 5 || s[2] != QLatin1Char(' ')) return false;
        for (int i : {0, 1, 3, 4}) {
            const QChar c = s[i];
            if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                  (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) return false;
        }
        return true;
    };
    EXPECT_TRUE(hexPair(text)) << text.toStdString();
    EXPECT_TRUE(model->data(first, SignalListModel::HasValueRole).toBool());
    EXPECT_FALSE(model->data(first, SignalListModel::IsTextRole).toBool());
    EXPECT_EQ(model->subscribedCount(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray signalsCsv =
        readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/signals.csv"));
    bool recorded = false;
    for (const QByteArray& line : signalsCsv.split('\n')) {
        const QList<QByteArray> cols = line.split(',');
        if (cols.size() == 4 && cols[1] == "Raw bytes" && hexPair(QString::fromUtf8(cols[2]))) {
            recorded = true;
            break;
        }
    }
    EXPECT_TRUE(recorded) << "no hex text row in signals.csv:\n" << signalsCsv.toStdString();
    EXPECT_EQ(ecuio_profile_unregister("test-text"), ECUIO_OK);
}

/* LiveView's delegate and SignalHub's Entry take these roles as
 * required properties, by NAME. A missing name isn't a build error: the
 * delegate fails at creation and its row never shows. */
TEST(BridgeTest, SignalRoleNamesMatchQmlBindings)
{
    SignalListModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    EXPECT_EQ(roles.value(SignalListModel::NameRole), QByteArray("name"));
    EXPECT_EQ(roles.value(SignalListModel::UnitRole), QByteArray("unit"));
    EXPECT_EQ(roles.value(SignalListModel::ValueRole), QByteArray("value"));
    EXPECT_EQ(roles.value(SignalListModel::SubscribedRole), QByteArray("subscribed"));
    EXPECT_EQ(roles.value(SignalListModel::HasValueRole), QByteArray("hasValue"));
    EXPECT_EQ(roles.value(SignalListModel::AgeMsRole), QByteArray("ageMs"));
    EXPECT_EQ(roles.value(SignalListModel::DecimalsRole), QByteArray("decimals"));
    EXPECT_EQ(roles.value(SignalListModel::TextRole), QByteArray("text"));
    EXPECT_EQ(roles.value(SignalListModel::IsTextRole), QByteArray("isText"));
    EXPECT_EQ(roles.value(SignalListModel::RefusedRole), QByteArray("refused"));
    EXPECT_EQ(roles.value(SignalListModel::RefusedCodeRole),
              QByteArray("refusedCode"));
    EXPECT_EQ(roles.value(SignalListModel::RefRole), QByteArray("ref"));
    EXPECT_EQ(roles.value(SignalListModel::RangeLowRole),
              QByteArray("rangeLow"));
    EXPECT_EQ(roles.value(SignalListModel::RangeHighRole),
              QByteArray("rangeHigh"));
    EXPECT_EQ(roles.value(SignalListModel::IsEnumRole), QByteArray("isEnum"));

    // A row answers only its roles, and only rows that exist answer
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    EXPECT_EQ(model.data(model.index(1, 0), SignalListModel::IdRole).toInt(),
              2001);
    EXPECT_FALSE(model.data(model.index(0, 0), Qt::DisplayRole).isValid());
    EXPECT_FALSE(
        model.data(model.index(2, 0), SignalListModel::NameRole).isValid());
}

/* Live's count of the refusals past a report's list: each ECU's, one
 * request said as one */
TEST(BridgeTest, TheRefusalsLiveCantNameAreCounted)
{
    EXPECT_EQ(SessionBridge::refusedUnnamedText({}), QString());
    EXPECT_EQ(SessionBridge::refusedUnnamedText({{0x7E8, 3}}),
              QStringLiteral("ECU 0x7E8 refused 3 more requests"));
    EXPECT_EQ(SessionBridge::refusedUnnamedText({{0x7E8, 1}, {0x7E9, 2}}),
              QStringLiteral("ECU 0x7E8 refused 1 more request; "
                             "ECU 0x7E9 refused 2 more requests"));
}

/* ADR-0051 section 3: a refused signal's words name the code, with the
 * reply in frames.csv's form; a code with no name gives the reply alone */
TEST(BridgeTest, ARefusalNamesItsCode)
{
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x31),
              QStringLiteral("Refused: requestOutOfRange (7F 22 31)"));
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x33),
              QStringLiteral("Refused: securityAccessDenied (7F 22 33)"));
    EXPECT_EQ(SessionBridge::refusalText(0x01, 0x12),
              QStringLiteral("Refused: subFunctionNotSupported (7F 01 12)"));
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x99),
              QStringLiteral("Refused: 7F 22 99"));
    EXPECT_EQ(SessionBridge::refusalCode(0x22, 0x31),
              QStringLiteral("7F 22 31"));
}

/* The model takes each refusal by id, judges every row afresh on each
 * call, and a new set refuses none */
TEST(BridgeTest, ARefusedRowSaysSoUntilItIsntRefused)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto refused = [&model](int row) {
        return model.data(model.index(row, 0), SignalListModel::RefusedRole)
            .toString();
    };
    QList<int> changed;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&changed](const QModelIndex& first, const QModelIndex&,
                                const QList<int>& roles) {
                         if (roles.contains(SignalListModel::RefusedRole)) {
                             changed << first.row();
                         }
                     });
    const uint16_t second = static_cast<uint16_t>(defs[1].id);
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()},
         {9999, QStringLiteral("x"), QString()}});
    EXPECT_EQ(refused(0), QString());
    EXPECT_EQ(refused(1), QStringLiteral("Refused: 7F 22 31"));
    EXPECT_EQ(changed, QList<int>{1}) << "only the row that changed";
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()}});
    EXPECT_EQ(changed, QList<int>{1}) << "the same words change nothing";
    model.applyRefused({});
    EXPECT_EQ(refused(1), QString());
    // The reply's bytes come with the words, for a row too narrow for them
    const auto code = [&model](int row) {
        return model.data(model.index(row, 0), SignalListModel::RefusedCodeRole)
            .toString();
    };
    model.applyRefused({{second, QStringLiteral("Refused: 7F 22 31"),
                         QStringLiteral("7F 22 31")}});
    EXPECT_EQ(code(1), QStringLiteral("7F 22 31"));
    EXPECT_EQ(code(0), QString());
    model.setSignalSet(&set);
    EXPECT_EQ(code(1), QString()) << "a new set's rows carry no code";
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()}});
    model.setSignalSet(&set);
    EXPECT_EQ(refused(1), QString()) << "a new set refuses none";
}

TEST(BridgeTest, DecimalsComeFromTheDefAndReachTheChart)
{
    /* Display precision has one source, the def's decimal_places:
     * LiveView and the Dashboard format with DecimalsRole, the strip
     * chart's value labels with chartData's "decimals". */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[0].decimal_places = 0;
    defs[1].decimal_places = 2;
    model.setSignalSet(&set);

    EXPECT_EQ(model.data(model.index(0, 0), SignalListModel::DecimalsRole).toInt(), 0);
    EXPECT_EQ(model.data(model.index(1, 0), SignalListModel::DecimalsRole).toInt(), 2);
    EXPECT_FALSE(model.data(model.index(1, 0), SignalListModel::HasValueRole).toBool());

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(2001), 21.5);
    model.updateValues(batch, 0);
    EXPECT_TRUE(model.data(model.index(1, 0), SignalListModel::HasValueRole).toBool());

    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data.at(0).toMap().value("decimals").toInt(), 2);
}

TEST(BridgeTest, UnitSystemConvertsRowsAndChartAndKeepsHistory)
{
    /* The model keeps decoded values; imperial converts value, unit,
     * decimals and the chart's whole history on the way out, and metric
     * gives back exactly what was decoded. A unit with no imperial form
     * passes through. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    std::memset(defs[1].unit, 0, sizeof defs[1].unit);
    std::memcpy(defs[1].unit, "\xC2\xB0" "C", 3);
    defs[1].decimal_places = 1;
    model.setSignalSet(&set);
    int redraws = 0;
    QObject::connect(&model, &SignalListModel::valuesUpdated, [&redraws]() { ++redraws; });

    for (int i = 0; i < 3; ++i) {
        std::vector<std::pair<uint16_t, double>> batch;
        batch.emplace_back(uint16_t(1001), 100.0 + i);
        batch.emplace_back(uint16_t(2001), 90.0 + i);
        model.updateValues(batch, i * 100);
    }
    const int before = redraws;
    const QModelIndex speed = model.index(0, 0);
    const QModelIndex temp = model.index(1, 0);

    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(redraws, before + 1) << "the chart must redraw in the new units";
    EXPECT_NEAR(model.data(speed, SignalListModel::ValueRole).toDouble(), 102.0 * 0.621371192237334, 1e-9);
    EXPECT_EQ(model.data(speed, SignalListModel::UnitRole).toString(), QStringLiteral("mph"));
    EXPECT_NEAR(model.data(temp, SignalListModel::ValueRole).toDouble(), 92.0 * 1.8 + 32.0, 1e-9);
    EXPECT_EQ(model.data(temp, SignalListModel::UnitRole).toString(), QString::fromUtf8("\xC2\xB0" "F"));
    EXPECT_EQ(model.data(temp, SignalListModel::DecimalsRole).toInt(), 1);

    const QVariantList chart = model.chartData();
    ASSERT_EQ(chart.size(), 2);
    for (const QVariant& v : chart) {
        const QVariantMap m = v.toMap();
        const QVariantList samples = m.value("samples").toList();
        ASSERT_EQ(samples.size(), 6) << "history lost on a unit change";
        if (m.value("id").toInt() == 1001) {
            EXPECT_EQ(m.value("unit").toString(), QStringLiteral("mph"));
            EXPECT_NEAR(samples.at(1).toDouble(), 100.0 * 0.621371192237334, 1e-9);
        } else {
            EXPECT_NEAR(samples.at(5).toDouble(), 92.0 * 1.8 + 32.0, 1e-9);
        }
    }

    model.setUnitSystem(ECUIO_UNITS_METRIC);
    EXPECT_EQ(model.data(speed, SignalListModel::ValueRole).toDouble(), 102.0);
    EXPECT_EQ(model.data(speed, SignalListModel::UnitRole).toString(), QStringLiteral("km/h"));
    EXPECT_EQ(model.chartData().at(1).toMap().value("samples").toList().at(1).toDouble(), 90.0);
}

TEST(BridgeTest, UnitSystemShowsImperialAndRecordsDecodedUnits)
{
    /* The property takes metric or imperial only. The Dashboard's
     * helpers convert through core. A recording keeps what the ECU sent,
     * whatever the screen shows. */
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    EXPECT_EQ(bridge.unitSystem(), 0);
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::unitSystemChanged, [&changes]() { ++changes; });
    bridge.setUnitSystem(7);
    EXPECT_EQ(bridge.unitSystem(), 0);
    bridge.setUnitSystem(1);
    bridge.setUnitSystem(1);
    EXPECT_EQ(bridge.unitSystem(), 1);
    EXPECT_EQ(changes, 1);
    EXPECT_NEAR(bridge.displayValue(QStringLiteral("km/h"), 160.0), 99.4193907579734, 1e-9);
    EXPECT_EQ(bridge.displayUnit(QStringLiteral("km/h")), QStringLiteral("mph"));
    EXPECT_EQ(bridge.displayValue(QStringLiteral("%"), 42.0), 42.0);

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://units").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.signalBatchCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    bool sawMph = false;
    for (int r = 0; r < model->rowCount(); ++r) {
        sawMph = sawMph || model->data(model->index(r, 0), SignalListModel::UnitRole).toString() ==
                                QStringLiteral("mph");
    }
    EXPECT_TRUE(sawMph) << "no row shows mph under imperial";
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray signalsCsv =
        readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/signals.csv"));
    EXPECT_TRUE(signalsCsv.contains(",Vehicle Speed,")) << signalsCsv.left(400).toStdString();
    EXPECT_TRUE(signalsCsv.contains(",km/h")) << "the recording should keep km/h";
    EXPECT_FALSE(signalsCsv.contains("mph")) << "the recording followed the display";
}

TEST(BridgeTest, StripChartDataPopulatedByLiveStream)
{
    /* End-to-end through the coalescer: real mock frames -> decode ->
       staging -> updateValues -> ring. Poll to the condition instead
       of a fixed 750ms window: the CONTRACT is history accumulates,
       not that 7.5 windows elapse. Monotonicity assertions are timing-
       independent once the data exists. */
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://strips").isEmpty());

    // CONTRACT: every subscribed signal has at least 4 samples
    auto chart_ready = [&bridge]() {
        const QVariantList data = bridge.signalModel()->chartData();
        if (data.isEmpty()) {
            return false;
        }
        for (const QVariant& v : data) {
            if (v.toMap().value("samples").toList().size() < 4) {
                return false;
            }
        }
        return true;
    };
    ASSERT_TRUE(spinUntil(chart_ready, 10000))
        << "strip chart history did not accumulate in 10s";

    const QVariantList data = bridge.signalModel()->chartData();
    ASSERT_GT(data.size(), 0);

    for (const QVariant& v : data) {
        const QVariantMap m = v.toMap();
        const QVariantList samples = m.value("samples").toList();

        // Sample count floor satisfied by the poll; upper bound sanity
        // (poll exits promptly, so a healthy stream sits near 4-8)
        EXPECT_GE(samples.size(), 4)
            << "no history for " << m.value("name").toString().toStdString();

        // Timestamps monotonic non-decreasing across the window
        double prev = -1.0;
        for (int i = 0; i < samples.size(); i += 2) {
            EXPECT_GE(samples.at(i).toDouble(), prev);
            prev = samples.at(i).toDouble();
        }
    }

    bridge.disconnectSession();
}

// ---------------------------------------------------------------------------
// J2534 picker surface: discovery marshaling + currentUri contract
// ---------------------------------------------------------------------------

TEST(BridgeTest, DiscoveredDevicesHaveWellFormedEntries)
{
    /* Marshaling only: the discovery itself is covered by
       test_j2534_discovery against the live registry. Count is
       registry-dependent (0..16, empty on non-Windows), so this pins
       SHAPE: every entry carries string vendorName/dllPath/architecture.
       An entry with a missing or non-string field would feed garbage
       into the SettingsPage picker's bindings. */
    SessionBridge bridge;
    const QVariantList devices = bridge.discoverJ2534Devices();

    EXPECT_GE(devices.size(), 0);   // documents the legitimate empty case
    for (const QVariant& v : devices) {
        ASSERT_TRUE(v.canConvert<QVariantMap>()) << "entry not a map";
        const QVariantMap m = v.toMap();
        ASSERT_TRUE(m.contains("vendorName")) << "missing vendorName";
        ASSERT_TRUE(m.contains("dllPath")) << "missing dllPath";
        ASSERT_TRUE(m.contains("architecture")) << "missing architecture";
        ASSERT_TRUE(m.value("vendorName").canConvert<QString>());
        ASSERT_TRUE(m.value("dllPath").canConvert<QString>());
        ASSERT_TRUE(m.value("architecture").canConvert<QString>());
    }
}

/* The serial ports as core lists them, name for name and in its order
 * (most machines have none, and the call itself is SerialPortsTest's) */
TEST(BridgeTest, SerialPortsAreListedAsCoreFindsThem)
{
    SessionBridge bridge;
    const QVariantList listed = bridge.discoverSerialPorts();

    uint32_t count = 0;
    ASSERT_EQ(ecuio_serial_ports(nullptr, 0, &count), ECUIO_OK);
    std::vector<ecuio_serial_port_t> ports(count);
    ASSERT_EQ(ecuio_serial_ports(ports.data(), count, &count), ECUIO_OK);
    ASSERT_EQ(listed.size(), static_cast<qsizetype>(count))
        << "a port came or went between the two calls";
    for (uint32_t i = 0; i < count; ++i) {
        const QVariantMap m = listed[static_cast<qsizetype>(i)].toMap();
        EXPECT_EQ(m.value("name").toString().toStdString(), ports[i].name);
        EXPECT_EQ(m.value("device").toString().toStdString(), ports[i].device);
    }
}

TEST(BridgeTest, CurrentUriTracksConnectState)
{
    /* currentUri: empty while disconnected, set after a successful
       connect, cleared by disconnect. The QML picker displays it as
       the ground truth of what's live: staleness here lies to the
       user. Failed connects must not leave a residue either. */
    SessionBridge bridge;
    EXPECT_TRUE(bridge.currentUri().isEmpty());

    ASSERT_TRUE(bridge.connectSession("mock://uri-track").isEmpty());
    EXPECT_EQ(bridge.currentUri(), QStringLiteral("mock://uri-track"));

    bridge.disconnectSession();
    EXPECT_TRUE(bridge.currentUri().isEmpty());   // cleared at once
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.disconnecting(); }, 5000, 10));

    // Failed connect leaves no stale URI behind
    EXPECT_FALSE(bridge.connectSession("j2534://definitely-not-a-vendor").isEmpty());
    EXPECT_TRUE(bridge.currentUri().isEmpty());
    EXPECT_FALSE(bridge.connected());
}

TEST(BridgeTest, ConnectTrimsSpacesAroundTheUri)
{
    // A URI pasted with spaces around it connects, as the URI without
    // them; core alone refuses it (the scheme is matched at the first
    // character)
    SessionBridge bridge;
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("  mock://padded \t")).isEmpty());
    EXPECT_TRUE(bridge.connected());
    EXPECT_EQ(bridge.currentUri(), QStringLiteral("mock://padded"));
    ASSERT_TRUE(disconnectAndWait(bridge));

    bridge.connectSessionAsync(QStringLiteral(" mock://padded-async "));
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.connected(); }, 5000, 10));
    EXPECT_EQ(bridge.currentUri(), QStringLiteral("mock://padded-async"));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A Bluetooth adapter's address is personal data, and the label reaches
 * the phone's lock screen in the session's notification: the last two
 * octets stay, the query too. A TCP address, a serial port, a short or
 * empty address and another stream kind pass */
TEST(BridgeTest, UriLabelKeepsABluetoothAddresssLastOctets)
{
    const QString dots = QString(QChar(0x2026));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("elm327://bt:00:11:22:33:44:55")),
              QStringLiteral("elm327://bt:") + dots + QStringLiteral(":44:55"));
    EXPECT_EQ(SessionBridge::uriLabelFor(QStringLiteral(
                  "elm327://ble:00:11:22:33:44:55?profile=raw&filter=0x7E8")),
              QStringLiteral("elm327://ble:") + dots +
                  QStringLiteral(":44:55?profile=raw&filter=0x7E8"));
    for (const char* uri :
         {"elm327://192.168.4.1:23", "elm327://COM7?serial=115200",
          "elm327:///dev/rfcomm0", "elm327://bt:44:55",
          "elm327://bt:", "elm327://usb:00:11:22:33:44:55", "elm327://"}) {
        EXPECT_EQ(SessionBridge::uriLabelFor(QString::fromLatin1(uri)),
                  QString::fromLatin1(uri));
    }
}

TEST(BridgeTest, UriLabelCutsADirectLoadPathToItsFileName)
{
    const QString dots = QString(QChar(0x2026));
    // A path selector, either separator, a drive or UNC root, the query kept
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("j2534://E:/dev/build/fake_j2534.dll")),
              QStringLiteral("j2534://") + dots +
                  QStringLiteral("/fake_j2534.dll"));
    EXPECT_EQ(SessionBridge::uriLabelFor(QStringLiteral(
                  "j2534://C:\\Program "
                  "Files\\Vendor\\vendor32.dll?profile=raw&filter=0x7E8")),
              QStringLiteral("j2534://") + dots +
                  QStringLiteral("/vendor32.dll?profile=raw&filter=0x7E8"));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("j2534://\\\\server\\share\\pt.dll")),
              QStringLiteral("j2534://") + dots + QStringLiteral("/pt.dll"));

    // A '/' in the query isn't a path
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("j2534://OBDX Pro - VX?did=a/b")),
              QStringLiteral("j2534://OBDX Pro - VX?did=a/b"));
    // Vendor names, the first device, the mock and nothing pass unchanged
    for (const char* uri :
         {"j2534://OBDX Pro - VX", "j2534://", "j2534://?probe=0",
          "mock://demo", "mock://a/b", ""}) {
        EXPECT_EQ(SessionBridge::uriLabelFor(QString::fromLatin1(uri)),
                  QString::fromLatin1(uri));
    }

    // The property follows currentUri
    SessionBridge bridge;
    EXPECT_TRUE(bridge.uriLabel().isEmpty());
    int changed = 0;
    QObject::connect(&bridge, &SessionBridge::currentUriChanged,
                     [&changed]() { ++changed; });
    ASSERT_TRUE(bridge.connectSession("mock://label").isEmpty());
    EXPECT_EQ(bridge.uriLabel(), QStringLiteral("mock://label"));
    EXPECT_EQ(changed, 1);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.uriLabel().isEmpty());
    EXPECT_EQ(changed, 2);
}

TEST(BridgeTest, AsyncConnectCompletesOffGuiThread)
{
    SessionBridge bridge;
    // Run a spinner on the GUI thread DURING the connect: if the
    // blocking core calls ran on this thread, the spinner would be
    // starved past its deadline. The mock's connect is fast, so give
    // a generous-but-finite margin.
    std::atomic<bool> done{false};
    QString gotErr = "unset";
    QObject::connect(&bridge, &SessionBridge::connectResult,
                     [&](const QString&, const QString& error) {
                         gotErr = error;
                         done.store(true);
                     });

    bridge.connectSessionAsync(QStringLiteral("mock://demo"));

    QElapsedTimer timer; timer.start();
    while (!done.load() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_TRUE(done.load());
    EXPECT_EQ(gotErr.toStdString(), "");
    EXPECT_TRUE(bridge.connected());
    bridge.disconnectSession();
    EXPECT_FALSE(bridge.connected());
}

/* Every emit while a connect is applied runs QML handlers synchronously,
 * after the session's worker has started. A connect a handler starts
 * there is refused, so the first session stays the bridge's; let through,
 * it would replace m_session and leave the first worker posting to the
 * bridge with nothing to close it. Sync connect: currentUriChanged, one
 * of the apply's emits. */
TEST(BridgeTest, AConnectFromAHandlerDuringTheApplyIsRefused)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bool tried = false;
    QObject::connect(&bridge, &SessionBridge::currentUriChanged, [&]() {
        if (!tried && !bridge.currentUri().isEmpty()) {
            tried = true;
            (void)bridge.connectSession(QStringLiteral("mock://second"));
        }
    });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://first")).isEmpty());
    ASSERT_TRUE(tried);
    EXPECT_EQ(bridge.currentUri().toStdString(), "mock://first");
    EXPECT_EQ(log.filter(QStringLiteral("Connected:")).size(), 1)
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The async completion: connectingChanged, as connecting clears, is a
 * handler's chance too */
TEST(BridgeTest, AConnectFromConnectingChangedIsRefused)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bool tried = false;
    QObject::connect(&bridge, &SessionBridge::connectingChanged, [&]() {
        if (!tried && !bridge.connecting()) {
            tried = true;
            bridge.connectSessionAsync(QStringLiteral("mock://second"));
        }
    });
    bridge.connectSessionAsync(QStringLiteral("mock://first"));
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.connected(); }, 5000));
    spinLoop(300); // a second connect let through completes in here
    ASSERT_TRUE(tried);
    EXPECT_EQ(bridge.currentUri().toStdString(), "mock://first");
    EXPECT_TRUE(log.filter(QStringLiteral("mock://second")).isEmpty())
        << log.join('\n').toStdString();
    EXPECT_EQ(log.filter(QStringLiteral("Connected:")).size(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {
void countFramesCb(const ecuio_frame_t*, void* user_data)
{
    static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
}
}  // namespace

/* An async connect whose completion never runs, because the bridge is
 * destroyed first, must not leave a running session calling onFrame on
 * the freed bridge. The connect thread creates and registers but doesn't
 * start, and the destructor destroys what it left pending. The event
 * loop isn't spun while the bridge lives, so the completion can't run;
 * a started mock session would reach the probe callback every 50 ms. */
TEST(BridgeTest, DestroyedMidConnectLeavesNoRunningSession)
{
    auto bridge = std::make_unique<SessionBridge>();
    bridge->connectSessionAsync(QStringLiteral("mock://demo"));

    ecuio_session_t* pending = nullptr;
    for (int i = 0; i < 500 && pending == nullptr; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pending = bridge->pendingSessionForTesting();
    }
    ASSERT_NE(pending, nullptr) << "the connect thread produced no session";

    std::atomic<int> frames{0};
    ASSERT_EQ(ecuio_register_frame_callback(pending, countFramesCb, &frames), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(frames.load(), 0) << "the session started before the GUI thread took it";
    ASSERT_EQ(ecuio_unregister_frame_callback(pending, countFramesCb, &frames), ECUIO_OK);

    bridge.reset();   // the queued completion is dropped; the destructor destroys `pending`
    spinLoop(50);     // nothing queued may run against the freed bridge
    EXPECT_EQ(frames.load(), 0);
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Connect and disconnect threads, against the fake J2534 DLL: its delay
// knobs stall the vendor call, so a call made on the GUI thread would
// block it, and its control page counts the teardown's calls.
// ---------------------------------------------------------------------------

namespace {

/* The fake's control page; created before any session opens, so a
 * surrogate child takes its name at spawn. */
fake_control::Page* fakePage()
{
    return fake_control::create_for_this_process();
}

/* Sets one of the fake's knobs for one test, on its page (which the
 * fake in the child reads on every call), and clears it on exit,
 * so a failing ASSERT can't leak it into the next case. */
class FakeEnvArm
{
public:
    FakeEnvArm(const char* name, const char* value) : m_name(name)
    {
        EXPECT_TRUE(fake_control::set(fakePage(), name, value)) << name;
    }
    ~FakeEnvArm() { fake_control::set(fakePage(), m_name, nullptr); }
    FakeEnvArm(const FakeEnvArm&) = delete;
    FakeEnvArm& operator=(const FakeEnvArm&) = delete;

private:
    const char* m_name;
};

/* What the fake publishes on its page, reset for one test. */
struct FakeHooks
{
    bool load()
    {
        fake_control::Page* page = fakePage();
        if (page == nullptr) return false;
        fake_control::reset(page);
        return true;
    }
    ~FakeHooks()
    {
        if (fakePage() != nullptr) fake_control::reset(fakePage());
    }
    unsigned long disconnect_count() const
    {
        return fake_control::disconnects(fakePage());
    }
};

/* A passive raw session: no requests, so the vendor calls under test
 * are the open, the engine ECU's filter and the teardown. */
QString fakeUri()
{
    return QStringLiteral("j2534://%1?profile=raw")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
}

}  // namespace

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

} // namespace

/* Start log over the fake ECU (ADR-0056 sections 2 and 3): the session
 * asks the plotted PIDs alone, ticks on Live meanwhile leave that so,
 * the recording's time starts at Start (not at the connect), and Stop
 * asks the whole set again. */
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

    bridge.stopLog();
    ASSERT_EQ(stopped, QStringList{folder});
    ASSERT_TRUE(spinUntil(
        []() {
            const std::vector<uint8_t> pids = nextPids();
            return !pids.empty() && !onlyThese(pids, {0x0C, 0x0D, 0x05});
        },
        15000, 10))
        << "the whole set never came back";
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QList<QByteArray> rows =
        fileLines(folder + QStringLiteral("/frames.csv"));
    ASSERT_GE(rows.size(), 2) << "no frame in the log";
    const qint64 firstHostUs = rows[1].split(',')[0].toLongLong();
    EXPECT_LT(firstHostUs, ageAtStartMs * 1000 / 2)
        << "timed from the connect, " << ageAtStartMs << " ms before Start";
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains(
        (QStringLiteral("focus  ") + namesOf(model, picks).join(',')).toUtf8()))
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

// ---------------------------------------------------------------------------
// ADR-0013: the bridge renders the SESSION's profile, not a hardcoded
// set
// ---------------------------------------------------------------------------

TEST(BridgeTest, RawProfileLeavesSignalModelEmpty)
{
    /* A raw session after a mock session must CLEAR the signal model.
     * Failure shape guarded: a setSignalSet that early-returns on NULL
     * without clearing leaves a raw session displaying the PREVIOUS
     * session's rows with no live updates: stale data posing as live.
     * Frames still flow to the Log page; decode alone is silenced. */
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://profile-then-raw").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.signalModel()->rowCount() > 0; }, 5000));
    EXPECT_GT(bridge.signalModel()->rowCount(), 0);

    ASSERT_TRUE(disconnectAndWait(bridge));

    ASSERT_TRUE(bridge.connectSession("mock://profile-raw-stage2?profile=raw")
                    .isEmpty());
    /* Frames keep arriving (mock streams regardless of profile):
     * the Log page stays live while LiveView stays empty. */
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    EXPECT_EQ(bridge.signalModel()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, AgeMsRoleEmittedAndRefreshesWhileStalled)
{
    /* Freshness contract: (a) updateValues emits dataChanged carrying
     * AgeMsRole; (b) refreshAges emits over all rows for AgeMsRole
     * ONLY: the stalled-row dimming path; (c) ageMs is -1 before the
     * first sample and a sane clock delta after. ageMs's wall-clock
     * basis is nondeterministic by nature, so assert ranges, not
     * exact values. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    // -1 before any sample
    EXPECT_EQ(model.data(model.index(0, 0),
                         SignalListModel::AgeMsRole).toInt(), -1);

    int ageEmissions = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&ageEmissions](const QModelIndex&, const QModelIndex&,
                        const QList<int>& roles) {
            if (roles.contains(SignalListModel::AgeMsRole)) ++ageEmissions;
        });

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(1001), 1500.0);
    model.updateValues(batch, 0);   // deterministic stamp of 0
    EXPECT_GE(ageEmissions, 1);

    const int age = model.data(model.index(0, 0),
                               SignalListModel::AgeMsRole).toInt();
    EXPECT_GE(age, 0);
    EXPECT_LT(age, 5000);   // clock basis sanity, not timing

    const int before = ageEmissions;
    model.refreshAges();   // the stalled-row path
    EXPECT_EQ(ageEmissions, before + 1);

    SignalListModel empty;
    empty.refreshAges();   // no rows: silent, no crash
}

/* The "Session:" line's wording, from hand-built infos: every field
 * it names, and what it says when a reading is missing. */
TEST(BridgeTest, SessionInfoTextNamesTheAdapter)
{
    ecuio_session_info_t mock{};
    mock.backend = ECUIO_BACKEND_MOCK;
    qstrncpy(mock.profile, "mock", sizeof(mock.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(mock).toStdString(),
              "Session: mock transport; profile mock");

    ecuio_session_info_t vx{};
    vx.backend = ECUIO_BACKEND_J2534_SURROGATE;
    vx.dll_arch = ECUIO_DLL_ARCH_X86;
    vx.protocol_id = 6;
    vx.baud = 500000;
    vx.channel_count = 2;
    vx.rx_ids[0] = 0x7E8;
    vx.tx_ids[0] = 0x7E0;
    vx.rx_ids[1] = 0x7E9;
    vx.tx_ids[1] = 0x7E1;
    vx.flags = ECUIO_SESSION_INFO_VBATT | ECUIO_SESSION_INFO_VERSIONS;
    vx.vbatt_mv = 12640;
    qstrncpy(vx.firmware_version, "1.2.3", sizeof(vx.firmware_version));
    qstrncpy(vx.dll_version, "4.5", sizeof(vx.dll_version));
    qstrncpy(vx.api_version, "04.04", sizeof(vx.api_version));
    qstrncpy(vx.profile, "obd2", sizeof(vx.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(vx).toStdString(),
              "Session: x86 DLL, through the surrogate; ISO 15765 at 500000 "
              "baud; profile obd2, channels 0x7E8/0x7E0, 0x7E9/0x7E1 (rx/tx); "
              "battery 12.6 V; firmware 1.2.3, DLL 4.5, API 04.04");
    EXPECT_EQ(SessionBridge::versionsText(vx).toStdString(),
              "firmware 1.2.3, DLL 4.5, API 04.04");
    EXPECT_TRUE(SessionBridge::versionsText(mock).isEmpty());

    ecuio_session_info_t bare{};
    bare.backend = ECUIO_BACKEND_J2534_SURROGATE;
    bare.dll_arch = ECUIO_DLL_ARCH_X64;
    bare.protocol_id = 6;
    bare.baud = 250000;
    bare.connect_flags = 0x100;
    bare.channel_count = 1;
    bare.rx_ids[0] = 0x18DAF110;
    bare.tx_ids[0] = 0x18DA10F1;
    qstrncpy(bare.profile, "raw", sizeof(bare.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(bare).toStdString(),
              "Session: x64 DLL, through the surrogate; ISO 15765 at 250000 baud, flags 0x100; "
              "profile raw, channel 0x18DAF110/0x18DA10F1 (rx/tx); "
              "no battery reading; no version reading");
    EXPECT_TRUE(SessionBridge::versionsText(bare).isEmpty());

    /* An ELM327 on an STN chip, its port in the "Connected:" line */
    ecuio_session_info_t elm{};
    elm.backend = ECUIO_BACKEND_ELM327;
    elm.protocol_id = 6;
    elm.baud = 500000;
    elm.channel_count = 1;
    elm.rx_ids[0] = 0x7E8;
    elm.tx_ids[0] = 0x7E0;
    elm.flags = ECUIO_SESSION_INFO_VBATT | ECUIO_SESSION_INFO_VERSIONS;
    elm.vbatt_mv = 13800;
    qstrncpy(elm.firmware_version, "ELM327 v1.4b, STN2255 v9.9.9",
             sizeof(elm.firmware_version));
    qstrncpy(elm.profile, "obd2", sizeof(elm.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(elm).toStdString(),
              "Session: ELM327 adapter (ELM327 v1.4b, STN2255 v9.9.9); "
              "ISO 15765 at 500000 baud; profile obd2, channel "
              "0x7E8/0x7E0 (rx/tx); battery 13.8 V");
    EXPECT_EQ(SessionBridge::versionsText(elm).toStdString(),
              "ELM327 v1.4b, STN2255 v9.9.9");

    /* Set not to wait past a pending reply, and no readings */
    elm.flags = ECUIO_SESSION_INFO_PENDING_OFF;
    elm.baud = 250000;
    EXPECT_EQ(SessionBridge::sessionInfoText(elm).toStdString(),
              "Session: ELM327 adapter (no version reading); ISO 15765 "
              "at 250000 baud; profile obd2, channel 0x7E8/0x7E0 "
              "(rx/tx); no battery reading; the adapter is set not to "
              "wait past 7F xx 78 (STN parameter 2A), so a pending "
              "reply's answer is lost");
    EXPECT_TRUE(SessionBridge::versionsText(elm).isEmpty());

    /* SocketCAN (ADR-0032, ADR-0040): a listen-only controller, vcan,
     * and a requesting profile on a controller and on vcan */
    ecuio_session_info_t can{};
    can.backend = ECUIO_BACKEND_SOCKETCAN;
    can.baud = 500000;
    can.flags = ECUIO_SESSION_INFO_LISTEN_ONLY;
    qstrncpy(can.profile, "raw", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a CAN controller in listen-only mode at "
              "500000 bit/s, listening; profile raw");
    can.baud = 0;
    can.flags = 0;
    qstrncpy(can.profile, "mock", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a virtual CAN bus, listening; profile mock");
    EXPECT_TRUE(SessionBridge::versionsText(can).isEmpty());
    can.baud = 500000;
    can.channel_count = 2;
    qstrncpy(can.profile, "obd2", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a CAN controller at 500000 bit/s, "
              "requesting on 2 channels; profile obd2");
    can.baud = 0;
    can.channel_count = 1;
    qstrncpy(can.profile, "user.sim-car-own", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a virtual CAN bus, requesting on 1 "
              "channel; profile user.sim-car-own");

    ecuio_session_info_t later{};
    later.backend = 7; // a backend this build doesn't know
    EXPECT_EQ(SessionBridge::sessionInfoText(later).toStdString(),
              "Session: backend 7");
    later.backend = ECUIO_BACKEND_J2534_INPROC; // retired, never reported
    EXPECT_EQ(SessionBridge::sessionInfoText(later).toStdString(),
              "Session: backend 2");
}

/* The "ECU 0x…:" line's wording, from hand-built reports: the first
 * vehicle's three bitmaps (docs/bench-sessions/2026-10-22-vehicle.md:
 * 44 data PIDs to 0x60, whose bit says a fourth exists), a chain
 * answered to its end, one unanswered, and requests dropped for each
 * reason, past the eight a report lists. */
/* Finding a signal among many (ADR-0050): every word in the name or the
 * unit, ignoring case; plotted rows only; the chosen groups, a code
 * core doesn't name counted as Other; the choices in code order */
TEST(BridgeTest, SignalFilterMatchesWordsPlottedRowsAndGroups)
{
    SignalFilter f;
    int changes = 0;
    QObject::connect(&f, &SignalFilter::changed, [&changes]() { ++changes; });
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, false));

    f.setText(QStringLiteral("  COOL   temp "));
    EXPECT_TRUE(f.active());
    EXPECT_TRUE(f.matches(QStringLiteral("Coolant Temp"), QStringLiteral("°C"),
                          1, false));
    EXPECT_FALSE(f.matches(QStringLiteral("Engine Oil Temp"),
                           QStringLiteral("°C"), 1, false))
        << "every word";
    f.setText(QStringLiteral("temp °c"));
    EXPECT_TRUE(f.matches(QStringLiteral("Engine Oil Temp"),
                          QStringLiteral("°C"), 1, false))
        << "a word in the unit";
    f.setText(QStringLiteral("psi"));
    EXPECT_TRUE(f.matches(QStringLiteral("Fuel Pressure"),
                          QStringLiteral("psi"), 3, false));
    EXPECT_FALSE(f.matches(QStringLiteral("Fuel Pressure"),
                           QStringLiteral("kPa"), 3, false));
    f.setText(QString());
    EXPECT_FALSE(f.active());

    f.setPlottedOnly(true);
    EXPECT_FALSE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, false));
    EXPECT_TRUE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, true));
    f.setPlottedOnly(false);

    f.toggleGroup(ECUIO_SIGNAL_GROUP_ENGINE);
    EXPECT_TRUE(f.chosen(ECUIO_SIGNAL_GROUP_ENGINE));
    EXPECT_TRUE(f.matches(QStringLiteral("RPM"), QString(),
                          ECUIO_SIGNAL_GROUP_ENGINE, false));
    EXPECT_FALSE(f.matches(QStringLiteral("MAF"), QString(),
                           ECUIO_SIGNAL_GROUP_AIR, false));
    f.toggleGroup(ECUIO_SIGNAL_GROUP_AIR);
    EXPECT_TRUE(f.matches(QStringLiteral("MAF"), QString(),
                          ECUIO_SIGNAL_GROUP_AIR, false))
        << "either chosen group";
    f.toggleGroup(200);
    EXPECT_TRUE(f.chosen(ECUIO_SIGNAL_GROUP_OTHER))
        << "an unknown code is Other's";
    EXPECT_TRUE(f.matches(QStringLiteral("DID 1E1C"), QString(), 0, false));
    EXPECT_TRUE(
        f.matches(QStringLiteral("Next year's"), QString(), 250, false));
    EXPECT_EQ(f.groups(), (QVariantList{0, 1, 2}));
    f.toggleGroup(ECUIO_SIGNAL_GROUP_ENGINE);
    EXPECT_FALSE(f.chosen(ECUIO_SIGNAL_GROUP_ENGINE))
        << "a second tap lets it go";

    const QVariantList choices = f.groupChoices(QVariantList{3, 1, 1, 200, 0});
    ASSERT_EQ(choices.size(), 3);
    EXPECT_EQ(choices[0].toMap().value(QStringLiteral("code")).toInt(), 0);
    EXPECT_EQ(choices[0].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Other"));
    EXPECT_EQ(choices[1].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine"));
    EXPECT_EQ(choices[2].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Fuel"));

    f.clear();
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(f.groups().isEmpty());
    EXPECT_EQ(changes, 11);
}

/* Live's list through its filter (ADR-0050): the mock's rows by word,
 * group and plotted; a tick under "Plotted only" re-filters; the groups
 * and the count before the filter leave out the rows core hides */
TEST(BridgeTest, LiveListFollowsItsFilter)
{
    SignalListModel model;
    model.setSignalSet(ecuio_mock_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    ASSERT_EQ(live.rowCount(), rows);
    EXPECT_EQ(live.shownCount(), rows);
    EXPECT_EQ(live.groups(), (QVariantList{1, 2, 4, 6}))
        << "Engine, Air, Oxygen sensors, Vehicle";
    const auto names = [&live]() {
        QStringList n;
        for (int r = 0; r < live.rowCount(); ++r) {
            n << live.data(live.index(r, 0), SignalListModel::NameRole)
                     .toString();
        }
        return n;
    };

    live.filter()->setText(QStringLiteral("temp"));
    EXPECT_EQ(names(), (QStringList{QStringLiteral("Coolant Temp"),
                                    QStringLiteral("Intake Air Temp"),
                                    QStringLiteral("Engine Oil Temp")}));
    live.filter()->toggleGroup(ECUIO_SIGNAL_GROUP_AIR);
    EXPECT_EQ(names(), QStringList{QStringLiteral("Intake Air Temp")});
    live.filter()->clear();
    EXPECT_EQ(live.rowCount(), rows);

    live.filter()->setPlottedOnly(true);
    EXPECT_EQ(live.rowCount(), rows) << "the mock plots every row";
    model.toggleSubscribed(0); /* RPM */
    EXPECT_EQ(live.rowCount(), rows - 1);
    EXPECT_FALSE(names().contains(QStringLiteral("RPM")));
    EXPECT_EQ(live.shownCount(), rows) << "the count before the filter";
    live.filter()->clear();

    /* obd2's Diagnostics rows hidden by core: no choice, not counted */
    model.setSignalSet(ecuio_obd2_signal_set());
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> diag;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].group == ECUIO_SIGNAL_GROUP_DIAGNOSTICS) {
            diag.push_back(set->defs[i].id);
        }
    }
    ASSERT_FALSE(diag.empty());
    EXPECT_TRUE(live.groups().contains(QVariant(8)));
    model.applyAbsent(diag);
    EXPECT_FALSE(live.groups().contains(QVariant(8)));
    EXPECT_EQ(live.shownCount(),
              model.rowCount() - static_cast<int>(diag.size()));
}

/* Live leaves out the rows core judges unsupported (ADR-0039 section 6;
 * the rule is PidSupportTest's): on the first vehicle's bitmaps (00, 20
 * and 40 answered), 0x5C among them; a PID past an answered range, and a
 * row no report names, stay. Each list is applied afresh, so a rescan
 * that finds a PID shows its row again. */
TEST(BridgeTest, LiveHidesTheRowsTheirEcuMarksUnsupported)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    ASSERT_GT(rows, 0);
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);
    int hiddenSignals = 0;
    QObject::connect(&model, &SignalListModel::hiddenCountChanged,
                     [&hiddenSignals]() { ++hiddenSignals; });

    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    model.applyUnsupported(unsupportedIds(&car, 1));

    /* The rule, from the bitmaps' bytes: PID p is supported when bit
     * 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8 is set */
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    int expectHidden = 0;
    int pastTheRanges = 0;
    int row5C = -1;
    int row0C = -1;
    for (int r = 0; r < rows; ++r) {
        const unsigned pid = set->defs[r].mux_key & 0xFFu;
        const bool clear = pid <= 0x60 && (bitmaps[(pid - 1) / 8] &
                                           (0x80u >> ((pid - 1) % 8))) == 0;
        const QModelIndex i = model.index(r);
        EXPECT_EQ(model.data(i, SignalListModel::HiddenRole).toBool(), clear)
            << "PID " << std::hex << pid;
        EXPECT_EQ(model.data(i, SignalListModel::RowRole).toInt(), r);
        expectHidden += clear ? 1 : 0;
        pastTheRanges += pid > 0x60 ? 1 : 0;
        if (pid == 0x5C)
            row5C = r;
        if (pid == 0x0C)
            row0C = r;
    }
    ASSERT_GE(row5C, 0);
    ASSERT_GE(row0C, 0);
    EXPECT_TRUE(
        model.data(model.index(row5C), SignalListModel::HiddenRole).toBool());
    EXPECT_GT(expectHidden, 1);
    EXPECT_GT(pastTheRanges, 0);
    EXPECT_EQ(model.hiddenCount(), expectHidden);
    EXPECT_EQ(hiddenSignals, 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted)
        << "a ranked set plots its top ranked rows";

    /* Live's rows are the shown ones in order, each naming its model row */
    ASSERT_EQ(live.rowCount(), rows - expectHidden);
    int last = -1;
    for (int r = 0; r < live.rowCount(); ++r) {
        const QModelIndex i = live.index(r, 0);
        const int source = live.data(i, SignalListModel::RowRole).toInt();
        EXPECT_GT(source, last);
        last = source;
        EXPECT_FALSE(live.data(i, SignalListModel::HiddenRole).toBool());
        EXPECT_EQ(live.data(i, SignalListModel::NameRole),
                  model.data(model.index(source), SignalListModel::NameRole));
    }

    /* A hidden row is never charted, and its plot box counts nothing */
    model.updateValues(
        {{set->defs[row5C].id, 80.0}, {set->defs[row0C].id, 2000.0}}, 100);
    QList<int> charted;
    for (const QVariant& v : model.chartData()) {
        charted << v.toMap().value(QStringLiteral("row")).toInt();
    }
    EXPECT_TRUE(charted.contains(row0C));
    EXPECT_FALSE(charted.contains(row5C));
    model.toggleSubscribed(row5C);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);
    model.toggleSubscribed(row5C);

    /* A rescan that finds 0x5C shows it again */
    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&rescan, 1));
    EXPECT_FALSE(
        model.data(model.index(row5C), SignalListModel::HiddenRole).toBool());
    EXPECT_EQ(model.hiddenCount(), expectHidden - 1);
    EXPECT_EQ(live.rowCount(), rows - expectHidden + 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    /* Another ECU's report, or one with nothing learned, hides nothing */
    ecuio_channel_support_t other = car;
    other.rx_id = 0x7E9;
    ecuio_channel_support_t nothing = car;
    nothing.generation = 0;
    const ecuio_channel_support_t neither[2] = {other, nothing};
    model.applyUnsupported(unsupportedIds(neither, 2));
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);

    /* A new set shows every row */
    model.applyUnsupported(unsupportedIds(&car, 1));
    ASSERT_GT(model.hiddenCount(), 0);
    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);
}

namespace {

/* The obd2 set's row for signal id, or -1 */
int obd2RowOf(uint16_t id)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

/* ADR-0039 section 6: a row core reports absent (its field past every reply)
 * is hidden beside the ones the bitmaps rule out, and shown again when
 * core no longer reports it; a PID the bitmaps list keeps its other
 * rows. A new set shows all. */
TEST(BridgeTest, LiveHidesTheFieldsCoreReportsAbsent)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    const int b1 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 0));
    const int b3 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ASSERT_GE(b1, 0);
    ASSERT_GE(b3, 0);
    const auto hidden = [&model](int row) {
        return model.data(model.index(row), SignalListModel::HiddenRole)
            .toBool();
    };

    model.applyAbsent({ECUIO_OBD2_PID_SIG_ID(0x55, 1)});
    EXPECT_TRUE(hidden(b3));
    EXPECT_FALSE(hidden(b1));
    EXPECT_EQ(model.hiddenCount(), 1);
    EXPECT_EQ(live.rowCount(), rows - 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted)
        << "an unranked row's hiding moves no default";

    /* The bitmaps hide theirs beside it; neither undoes the other */
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x1; /* 0x01-0x20: all clear but 0x0C */
    car.pid_bitmap[(0x0C - 1) / 8] = 0x80u >> ((0x0C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&car, 1));
    int unsupported = 0;
    for (int r = 0; r < rows; ++r) {
        const unsigned pid = ecuio_obd2_signal_set()->defs[r].mux_key & 0xFFu;
        unsupported += (pid <= 0x20 && pid != 0x0C) ? 1 : 0;
    }
    ASSERT_GT(unsupported, 0);
    EXPECT_TRUE(hidden(b3)) << "still absent after the support's judgment";
    EXPECT_EQ(model.hiddenCount(), unsupported + 1);

    model.applyAbsent({});
    EXPECT_FALSE(hidden(b3)) << "core no longer reports it";
    EXPECT_EQ(model.hiddenCount(), unsupported);

    model.applyAbsent({ECUIO_OBD2_PID_SIG_ID(0x55, 1)});
    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.hiddenCount(), 0) << "a new set shows every row";
}

/* The bridge reads core's absent signals on its tick: a replay of the
 * test car's one-byte PID 55 replies hides bank 3's row, and only it */
TEST(BridgeTest, LiveHidesAFieldTheRepliesComeWithout)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray text = "host_us,timestamp_us,id,flags,data\n";
    for (int i = 0; i < 5; ++i) {
        const QByteArray t = QByteArray::number(10000 * i);
        text += t + ',' + t + ",0x7E8,0x0,41 55 80\n";
    }
    QFile f(dir.filePath(QStringLiteral("frames.csv")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(text), text.size());
    f.close();

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("replay://%1?profile=obd2&speed=max")
                                .arg(dir.path()))
            .isEmpty())
        << log.join('\n').toStdString();
    SignalListModel* model = bridge.signalModel();
    const int b1 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 0));
    const int b3 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ASSERT_TRUE(
        spinUntil([model]() { return model->hiddenCount() > 0; }, 5000));
    spinLoop(200);
    EXPECT_EQ(model->hiddenCount(), 1);
    EXPECT_TRUE(
        model->data(model->index(b3), SignalListModel::HiddenRole).toBool());
    EXPECT_FALSE(
        model->data(model->index(b1), SignalListModel::HiddenRole).toBool());
    EXPECT_TRUE(
        model->data(model->index(b1), SignalListModel::HasValueRole).toBool());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The counts are core's (pids_supported, pids_through: PidSupportTest);
 * the line says them */
TEST(BridgeTest, ChannelSupportTextNamesTheEcuAndWhatItDropped)
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.requests = 10;
    car.ranges = 0x7;
    car.pids_supported = 44;
    car.pids_through = 0x60;
    car.dropped = 1;
    car.dropped_list[0] = {{0x01, 0x5C, 0x00}, 2, ECUIO_DROP_BITMAP, 0, {0, 0}};
    EXPECT_EQ(SessionBridge::channelSupportText(car).toStdString(),
              "ECU 0x7E8: supports 44 PIDs up to 0x60, no answer past that; "
              "10 requests, 1 dropped: 01 5C (not in its supported PIDs)");

    ecuio_channel_support_t later = car; // a reason this build doesn't know
    later.dropped_list[0].reason = 99;
    EXPECT_EQ(SessionBridge::channelSupportText(later).toStdString(),
              "ECU 0x7E8: supports 44 PIDs up to 0x60, no answer past that; "
              "10 requests, 1 dropped: 01 5C (reason 99)");

    ecuio_channel_support_t all{};
    all.flags = ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE;
    all.rx_id = 0x18DAF110;
    all.requests = 1;
    all.ranges = 0x1;
    all.pids_supported = 2;
    all.pids_through = 0x20;
    EXPECT_EQ(SessionBridge::channelSupportText(all).toStdString(),
              "ECU 0x18DAF110: supports 2 PIDs; 1 request, none dropped");

    /* Packing (ADR-0039 Appendix A): answered, then given up */
    ecuio_channel_support_t packed = all;
    packed.flags |= ECUIO_SUPPORT_PACKED;
    EXPECT_EQ(SessionBridge::channelSupportText(packed).toStdString(),
              "ECU 0x18DAF110: supports 2 PIDs; 1 request, none dropped; "
              "several PIDs per request");
    packed.flags |= ECUIO_SUPPORT_UNPACKED;
    EXPECT_EQ(SessionBridge::channelSupportText(packed).toStdString(),
              "ECU 0x18DAF110: supports 2 PIDs; 1 request, none dropped; "
              "one PID per request, several went unanswered");

    ecuio_channel_support_t part = all;
    part.flags = ECUIO_SUPPORT_PROBED;
    part.ranges = 0;
    part.pids_supported = 0;
    part.pids_through = 0;
    EXPECT_EQ(SessionBridge::channelSupportText(part).toStdString(),
              "ECU 0x18DAF110: no answer to the supported-PID query; "
              "1 request, none dropped");

    ecuio_channel_support_t dids{};
    dids.rx_id = 0x7E9;
    dids.requests = 12;
    dids.dropped = 10;
    dids.dropped_list[0] = {{0x22, 0x1E, 0x1D}, 3, ECUIO_DROP_NOT_SUPPORTED, 0x31, {0, 0}};
    dids.dropped_list[1] = {{0x22, 0x1E, 0x1E}, 3, ECUIO_DROP_ACCESS, 0x33, {0, 0}};
    for (int i = 2; i < ECUIO_SUPPORT_MAX_DROPPED; ++i) {
        dids.dropped_list[i] = {{0x22, 0x40, static_cast<uint8_t>(i)}, 3,
                                ECUIO_DROP_SERVICE, 0x11, {0, 0}};
    }
    EXPECT_EQ(SessionBridge::channelSupportText(dids).toStdString(),
              "ECU 0x7E9: 12 requests, 10 dropped: "
              "22 1E 1D (not supported: 7F 22 31), "
              "22 1E 1E (needs security access or another session: 7F 22 33), "
              "22 40 02 (service not supported: 7F 22 11), "
              "22 40 03 (service not supported: 7F 22 11), "
              "22 40 04 (service not supported: 7F 22 11), "
              "22 40 05 (service not supported: 7F 22 11), "
              "22 40 06 (service not supported: 7F 22 11), "
              "22 40 07 (service not supported: 7F 22 11), and 2 more");

    /* A DID scan (ADR-0020): the fake's 1E18-1E20 range, answered to its
     * end; then one still running whose table filled before every found
     * DID could join the rotation. */
    ecuio_channel_support_t scan{};
    scan.flags = ECUIO_SUPPORT_SCAN_COMPLETE;
    scan.rx_id = 0x7E8;
    scan.requests = 1;
    scan.scan_total = 9;
    scan.scan_done = 9;
    scan.scan_found = 1;
    scan.scan_refused = 8;
    scan.scan_polled = 1;
    EXPECT_EQ(SessionBridge::channelSupportText(scan).toStdString(),
              "ECU 0x7E8: 1 request, none dropped; scan: 9 DIDs, 1 found, 8 refused");

    ecuio_channel_support_t running{};
    running.rx_id = 0x7E9;
    running.requests = 128;
    running.scan_total = 256;
    running.scan_done = 250;
    running.scan_found = 130;
    running.scan_refused = 120;
    running.scan_polled = 128;
    EXPECT_EQ(SessionBridge::channelSupportText(running).toStdString(),
              "ECU 0x7E9: 128 requests, none dropped; "
              "scan: 256 DIDs, 130 found (128 polled), 120 refused, 6 unanswered");
}
TEST(BridgeTest, NewSaltIsSixteenRandomBytes)
{
    SessionBridge bridge;
    const QString a = bridge.newSalt();
    const QString b = bridge.newSalt();
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$"))
                    .match(a)
                    .hasMatch())
        << a.toStdString();
    EXPECT_NE(a, b);
}

/* ADR-0027 section 2: a label as a recording folder may take it */
TEST(BridgeTest, FolderLabelKeepsWhatAFolderNameMay)
{
    const auto f = [](const char* label) {
        return SessionBridge::folderLabel(QString::fromUtf8(label))
            .toStdString();
    };
    EXPECT_EQ(f("Daily driver"), "Daily driver");
    EXPECT_EQ(f("  Weekend   car  "), "Weekend car");
    EXPECT_EQ(f("a/b:c*?<>|d"), "abcd");
    EXPECT_EQ(f("v1.2_track-day"), "v1.2_track-day");
    EXPECT_EQ(f("Car..."), "Car") << "Windows refuses a trailing dot";
    EXPECT_EQ(f("..."), "");
    EXPECT_EQ(f(std::string(60, 'x').c_str()), std::string(40, 'x'));
}

/* ADR-0027 section 1: a vehicle's plotted signals are the rows ticked
 * past the default and unticked from it, read and set by name. The
 * mock's set ranks none, so every numeric row is its default. */
/* The chart's window (LiveViewPage's 10 to 120 s): chartData keeps the
 * samples of the last windowMs before the newest plotted one, and with
 * columns, each slice's lowest and highest in time order and the newest
 * sample, never more than two a slice plus it. Slices are counted from
 * time 0, so the window's first may be partial */
TEST(BridgeTest, ChartDataKeepsTheWindowAndEachSlicesExtremes)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    /* 1001 rises and falls every second; 2001 tracks the time */
    for (qint64 t = 0; t < 20000; t += 10) {
        const double wave = static_cast<double>((t / 10) % 100 < 50 ? (t / 10) % 50
                                                                    : 50 - (t / 10) % 50);
        model.updateValues({{uint16_t(1001), wave}, {uint16_t(2001), double(t)}}, t);
    }
    const auto samples = [](const QVariantList& data, int s) {
        return data.at(s).toMap().value("samples").toList();
    };

    const QVariantList all = model.chartData();
    ASSERT_EQ(all.size(), 2);
    EXPECT_EQ(samples(all, 1).size(), 2 * 2000) << "no window: every sample";

    const QVariantList win = model.chartData(5000);
    const QVariantList w = samples(win, 1);
    EXPECT_EQ(w.at(0).toDouble(), 19990.0 - 5000.0) << "from the window's start";
    EXPECT_EQ(w.at(w.size() - 2).toDouble(), 19990.0) << "to the newest";
    EXPECT_EQ(w.size(), 2 * 501);

    const int columns = 10;
    const QVariantList cut = model.chartData(5000, columns);
    for (int s = 0; s < 2; ++s) {
        const QVariantList c = samples(cut, s);
        EXPECT_LE(c.size(), 2 * (2 * (columns + 1) + 1)) << s;
        for (int i = 2; i < c.size(); i += 2) {
            EXPECT_LT(c.at(i - 2).toDouble(), c.at(i).toDouble()) << "in time order";
        }
        EXPECT_EQ(c.at(c.size() - 2).toDouble(), 19990.0) << "the newest kept";
    }
    /* Each slice of the wave keeps its own lowest and highest, as the
     * whole window's samples show them */
    const QVariantList full = samples(model.chartData(5000), 0);
    const QVariantList wave = samples(cut, 0);
    const double slice = 5000.0 / columns;
    std::map<int, std::pair<double, double>> want, got;
    const auto note = [&](std::map<int, std::pair<double, double>>& m,
                          const QVariantList& v) {
        for (int i = 0; i < v.size(); i += 2) {
            const int k =
                static_cast<int>(std::floor(v.at(i).toDouble() / slice));
            const double x = v.at(i + 1).toDouble();
            auto it = m.find(k);
            if (it == m.end()) {
                m[k] = {x, x};
            } else {
                it->second.first = std::min(it->second.first, x);
                it->second.second = std::max(it->second.second, x);
            }
        }
    };
    note(want, full);
    note(got, wave);
    EXPECT_EQ(got, want) << "each slice's lowest and highest";
}

/* A window that slides keeps its points: a sample stays in its slice
 * however far the window has moved, so between two updates every slice
 * away from the window's ends keeps the same lowest and highest, and a
 * noisy trace scrolls instead of shimmering */
TEST(BridgeTest, ChartDataKeepsEachSlicesPointsAsTheWindowSlides)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    /* A noisy trace: a value that jumps sample to sample */
    const auto noise = [](qint64 t) {
        return static_cast<double>((t * 7919) % 97);
    };
    qint64 t = 0;
    const auto feed = [&](qint64 until) {
        for (; t < until; t += 10)
            model.updateValues({{uint16_t(1001), noise(t)}}, t);
    };
    const qint64 windowMs = 10000;
    const int columns = 300; /* a phone's chart: 33.3 ms a slice */
    const auto points = [&]() {
        std::map<double, double> out;
        const QVariantList v = model.chartData(windowMs, columns)
                                   .at(0)
                                   .toMap()
                                   .value("samples")
                                   .toList();
        for (int i = 0; i < v.size(); i += 2)
            out[v.at(i).toDouble()] = v.at(i + 1).toDouble();
        return out;
    };
    feed(20000);
    const std::map<double, double> before = points();
    feed(20130); /* one 100 ms update and a bit: not a slice's width */
    const std::map<double, double> after = points();

    /* The stretch both windows hold whole, a slice clear of either end */
    const double slice = static_cast<double>(windowMs) / columns;
    const double lo = 20120.0 - windowMs + slice;
    const double hi = 19990.0 - slice;
    const auto inside = [&](const std::map<double, double>& m) {
        std::map<double, double> out;
        for (const auto& p : m)
            if (p.first >= lo && p.first <= hi)
                out.insert(p);
        return out;
    };
    ASSERT_GT(inside(before).size(), 100u);
    EXPECT_EQ(inside(after), inside(before))
        << "the same points, slice for slice";
}

TEST(BridgeTest, UnplottedNamesReadAndSetTheChartsRows)
{
    SessionBridge bridge;
    SignalListModel* m = bridge.signalModel();
    int countNotified = 0;
    QObject::connect(m, &SignalListModel::subscribedCountChanged,
                     [&countNotified]() { ++countNotified; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    EXPECT_GE(countNotified, 1) << "a new set's count is notified";
    ASSERT_GE(m->rowCount(), 4);
    const auto name = [m](int row) {
        return m->data(m->index(row), SignalListModel::NameRole).toString();
    };
    const auto plotted = [m](int row) {
        return m->data(m->index(row), SignalListModel::SubscribedRole).toBool();
    };
    const int all = m->subscribedCount();
    EXPECT_TRUE(m->unplottedNames().isEmpty())
        << "every numeric row starts plotted";
    EXPECT_TRUE(m->plottedNames().isEmpty()) << "and none past its default";

    m->setPlotChoices({name(1)},
                      {name(0), name(2), name(1), QStringLiteral("Not a signal")});
    EXPECT_EQ(m->unplottedNames(), (QStringList{name(0), name(1), name(2)}))
        << "a name in both lists is unplotted";
    EXPECT_TRUE(m->plottedNames().isEmpty());
    EXPECT_EQ(m->subscribedCount(), all - 3);
    EXPECT_FALSE(plotted(0));
    EXPECT_TRUE(plotted(3));

    m->setPlotChoices({}, {});
    EXPECT_TRUE(m->unplottedNames().isEmpty());
    EXPECT_EQ(m->subscribedCount(), all);

    m->toggleSubscribed(1);
    EXPECT_EQ(m->unplottedNames(), QStringList{name(1)})
        << "a click is read back";
    m->toggleSubscribed(1);
    EXPECT_TRUE(m->unplottedNames().isEmpty())
        << "a click back to the default clears the choice";
    EXPECT_TRUE(m->plottedNames().isEmpty()) << "rather than turning it over";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* The names of the rows the chart plots, in set order */
QStringList plottedRows(const SignalListModel& model)
{
    QStringList names;
    for (int r = 0; r < model.rowCount(); ++r) {
        const QModelIndex i = model.index(r);
        if (model.data(i, SignalListModel::SubscribedRole).toBool()
                && !model.data(i, SignalListModel::HiddenRole).toBool()) {
            names << model.data(i, SignalListModel::NameRole).toString();
        }
    }
    return names;
}

/* The test car's first two ranges with PID 0x5C clear, as its
 * bitmaps read (docs/bench-sessions/2026-10-31-elm327-vehicle.md) */
ecuio_channel_support_t carWithoutOilTemp()
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    return car;
}

} // namespace

/* ADR-0039 section 8: a ranked set plots the DefaultPlotted shown rows
 * of the lowest plot_rank; a row Live hides gives its place to the
 * next, and a rescan that shows it takes the place back */
TEST(BridgeTest, ARankedSetPlotsTheTopRankedRowsItShows)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    ASSERT_EQ(SignalListModel::DefaultPlotted, 6);
    const QStringList topSix = {
        QStringLiteral("Coolant Temp"),    QStringLiteral("RPM"),
        QStringLiteral("Vehicle Speed"),   QStringLiteral("Intake Air Temp"),
        QStringLiteral("MAF"),             QStringLiteral("Engine Oil Temp")};
    EXPECT_EQ(plottedRows(model), topSix) << "ranks 1 to 6, in set order";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    const ecuio_channel_support_t car = carWithoutOilTemp();
    model.applyUnsupported(unsupportedIds(&car, 1));
    QStringList withLoad = topSix;
    withLoad.removeOne(QStringLiteral("Engine Oil Temp"));
    withLoad.prepend(QStringLiteral("Engine Load"));
    EXPECT_EQ(plottedRows(model), withLoad) << "rank 7 takes the hidden row's place";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&rescan, 1));
    EXPECT_EQ(plottedRows(model), topSix) << "and gives it back";

    /* A car with fewer ranked rows than that plots all it has */
    ecuio_channel_support_t sparse{};
    sparse.flags = ECUIO_SUPPORT_PROBED;
    sparse.rx_id = 0x7E8;
    sparse.generation = 3;
    sparse.ranges = 0x7; /* 0x01-0x60: only RPM and speed */
    sparse.pid_bitmap[(0x0C - 1) / 8] |= 0x80u >> ((0x0C - 1) % 8);
    sparse.pid_bitmap[(0x0D - 1) / 8] |= 0x80u >> ((0x0D - 1) % 8);
    model.applyUnsupported(unsupportedIds(&sparse, 1));
    EXPECT_EQ(plottedRows(model),
              (QStringList{QStringLiteral("RPM"), QStringLiteral("Vehicle Speed")}));
}

/* A refused row never has a value to draw: it gives its place among the
 * default plotted rows to the next, as a hidden one does, and takes it
 * back once it isn't refused; a tick still plots it. In a set that ranks
 * none, it is no default either */
TEST(BridgeTest, ARefusedRowGivesItsDefaultPlotToTheNext)
{
    SignalListModel model;
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    model.setSignalSet(set);
    const QStringList topSix = plottedRows(model);
    int oilRow = -1;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (std::strcmp(set->defs[i].name, "Engine Oil Temp") == 0) {
            oilRow = static_cast<int>(i);
        }
    }
    ASSERT_GE(oilRow, 0);
    ASSERT_TRUE(topSix.contains(QStringLiteral("Engine Oil Temp")));
    const SignalListModel::Refusal oil{
        static_cast<uint16_t>(set->defs[oilRow].id),
        QStringLiteral("Refused: subFunctionNotSupported (7F 01 12)"),
        QStringLiteral("7F 01 12")};

    model.applyRefused({oil});
    QStringList withLoad = topSix;
    withLoad.removeOne(QStringLiteral("Engine Oil Temp"));
    withLoad.prepend(QStringLiteral("Engine Load"));
    EXPECT_EQ(plottedRows(model), withLoad)
        << "rank 7 takes the refused row's place";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);
    model.applyRefused({});
    EXPECT_EQ(plottedRows(model), topSix) << "and gives it back";

    model.applyRefused({oil});
    model.toggleSubscribed(oilRow);
    EXPECT_TRUE(plottedRows(model).contains(QStringLiteral("Engine Oil Temp")))
        << "a tick plots it all the same";

    SignalListModel plain;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t two;
    buildTestSignalSet(defs, two);
    plain.setSignalSet(&two);
    ASSERT_EQ(plottedRows(plain).size(), 2)
        << "an unranked set plots every row";
    plain.applyRefused(
        {{static_cast<uint16_t>(defs[1].id),
          QStringLiteral("Refused: requestOutOfRange (7F 22 31)"),
          QStringLiteral("7F 22 31")}});
    EXPECT_EQ(plottedRows(plain), QStringList{QStringLiteral("Engine_RPM")});
    EXPECT_EQ(plain.subscribedCount(), 1);
}

/* The user's ticks and unticks outlive the defaults moving, and a click
 * back to the default clears one */
TEST(BridgeTest, APlotChoiceOutlivesTheDefaultsMoving)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    const int rpm = obd2RowOf(ECUIO_OBD2_SIG_RPM_ID);
    const int load = obd2RowOf(ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    const int pedal = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x49, 0));
    ASSERT_GE(rpm, 0);
    ASSERT_GE(load, 0);
    ASSERT_GE(pedal, 0);
    const auto plotted = [&model](int row) {
        return model.data(model.index(row), SignalListModel::SubscribedRole)
            .toBool();
    };

    model.toggleSubscribed(rpm);
    model.toggleSubscribed(load);
    EXPECT_FALSE(plotted(rpm));
    EXPECT_TRUE(plotted(load));
    EXPECT_EQ(model.unplottedNames(), QStringList{QStringLiteral("RPM")});
    EXPECT_EQ(model.plottedNames(), QStringList{QStringLiteral("Engine Load")});
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    /* Load's default turns on and RPM's stays on: neither choice moves */
    const ecuio_channel_support_t car = carWithoutOilTemp();
    model.applyUnsupported(unsupportedIds(&car, 1));
    EXPECT_FALSE(plotted(rpm)) << "an untick holds against its default";
    EXPECT_TRUE(plotted(load));
    EXPECT_FALSE(plotted(pedal)) << "an untick leaves its place empty";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted - 1);

    model.toggleSubscribed(rpm);
    EXPECT_TRUE(model.unplottedNames().isEmpty()) << "back to its default";
    EXPECT_EQ(model.plottedNames(), QStringList{QStringLiteral("Engine Load")})
        << "with no choice of its own";

    /* A vehicle's lists, applied whole: every other row to its default */
    model.setPlotChoices({QStringLiteral("Accelerator Pedal D")}, {});
    EXPECT_TRUE(plotted(pedal));
    EXPECT_TRUE(plotted(load)) << "its default now, with no choice";
    EXPECT_TRUE(model.unplottedNames().isEmpty());
    EXPECT_EQ(model.plottedNames(),
              QStringList{QStringLiteral("Accelerator Pedal D")});

    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_TRUE(model.plottedNames().isEmpty()) << "a new set starts with no choices";
}

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

TEST(BridgeTest, ConnectLogsTheMockSession)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_GE(log.size(), 2);
    EXPECT_EQ(log.at(log.size() - 2), QStringLiteral("Connected: mock://demo"));
    EXPECT_EQ(log.last(), QStringLiteral("Session: mock transport; profile mock"));
    EXPECT_TRUE(bridge.adapterVersions().isEmpty()) << "no adapter, nothing to keep";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, LinkStateMapsEveryPublishedCode)
{
    /* The state-string mapping, pinned session-free via the static
     * helper. Covers all four link-health codes plus the forward-
     * compatible fallback; QML's Theme keys colors and labels on
     * these strings. */
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_OK), QStringLiteral("ok"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_VEHICLE_TIMEOUT), QStringLiteral("quiet"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_REQUEST_FAILED), QStringLiteral("requests"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_TRANSPORT_HARD_FAILURE), QStringLiteral("fault"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_RECORDING_CHANGED), QStringLiteral("stopped"));
    EXPECT_EQ(SessionBridge::linkStateForError(static_cast<ecuio_status_t>(999)), QStringLiteral("degraded"));
}

/* A replay whose recording changes under it stops, and the bridge says
 * why: the "stopped" state and one log line with the frames sent, never
 * "Replay finished". The file is rewritten while it loops, so the next
 * pass (or the one under way) meets a row it can't read. */
TEST(BridgeTest, ReplayThatStopsEarlySaysSo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString csv = dir.filePath(QStringLiteral("frames.csv"));
    const auto write = [&csv](const QByteArray& text) {
        QFile f(csv);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               f.write(text) == text.size();
    };
    QByteArray rows = "host_us,timestamp_us,id,flags,data\n";
    for (int i = 0; i < 5; ++i) {
        rows += QByteArray::number(10000 * i) + ',' + QByteArray::number(10000 * i) +
                ",0x7E8,0x0,41 0C 1F 40\n";
    }
    ASSERT_TRUE(write(rows));

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("replay://") + csv +
                                      QStringLiteral("?loop=1"))
                    .isEmpty())
        << log.join('\n').toStdString();
    EXPECT_EQ(bridge.linkState().toStdString(), "ok");
    ASSERT_TRUE(write("host_us,timestamp_us,id,flags,data\nnot a row\n"));

    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.linkState() == QStringLiteral("stopped"); },
                          5000))
        << log.join('\n').toStdString();
    EXPECT_EQ(bridge.linkErrorCode(), static_cast<int>(ECUIO_ERR_RECORDING_CHANGED));
    EXPECT_EQ(bridge.errorEpisodes(), 1);
    spinLoop(300);
    const QStringList stopped = log.filter(QStringLiteral("Replay stopped after "));
    ASSERT_EQ(stopped.size(), 1) << log.join('\n').toStdString();
    EXPECT_TRUE(stopped.first().endsWith(
        QStringLiteral(" frames: the recording changed after it was opened")))
        << stopped.first().toStdString();
    EXPECT_TRUE(log.filter(QStringLiteral("Replay finished")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* ADR-0023's round trip: a recorded mock session, replayed at full speed
 * under its profile, delivers every frame (the first included, though a
 * replay sends it the moment the session starts) and decodes every value
 * the recording holds; the replay's own recording matches the first one
 * row for row, its times rebased to the first frame. The end of the file
 * is logged once. */
TEST(BridgeTest, ReplayOfAMockRecordingDecodesWhatWasRecorded)
{
    QTemporaryDir first, second;
    ASSERT_TRUE(first.isValid() && second.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    bridge.setRecordingsRootForTesting(first.path());
    ASSERT_TRUE(bridge.connectSession("mock://roundtrip").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 30; }, 10000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList recorded = recordingFolders(first.path());
    ASSERT_EQ(recorded.size(), 1);
    const QString a = first.path() + QStringLiteral("/") + recorded.first();
    const QList<QByteArray> framesA =
        fileLines(a + QStringLiteral("/frames.csv"));
    const int n = static_cast<int>(framesA.size()) - 1;
    ASSERT_GT(n, 30);

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setRecordingsRootForTesting(second.path());
    const QString uri = QStringLiteral("replay://") + a +
                        QStringLiteral("?profile=mock&speed=max");
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString finished =
        QStringLiteral("Replay finished: %1 frames; the session stays open")
            .arg(n);
    ASSERT_TRUE(spinUntil(
        [&log, &finished]() { return log.contains(finished); }, 10000))
        << log.join(QLatin1Char('\n')).toStdString();
    spinLoop(300); // the last frames' queued calls, and a tick or two
    EXPECT_EQ(bridge.droppedFrames(), 0)
        << "the count below is whole only without drops";
    EXPECT_EQ(bridge.frameCount(), n) << "every frame, the first included";
    EXPECT_EQ(log.count(finished), 1) << "logged once";
    const QString session =
        QStringLiteral("Session: replay of a recording, %1 frames over ")
            .arg(n);
    EXPECT_TRUE(std::any_of(
        log.cbegin(), log.cend(),
        [&session](const QString& l) { return l.startsWith(session); }))
        << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList replayed = recordingFolders(second.path());
    ASSERT_EQ(replayed.size(), 1);
    const QString b = second.path() + QStringLiteral("/") + replayed.first();
    const QList<QByteArray> framesB =
        fileLines(b + QStringLiteral("/frames.csv"));
    ASSERT_EQ(framesB.size(), framesA.size());
    const qulonglong t0 = framesA.at(1).split(',').at(1).toULongLong();
    for (int i = 1; i < framesA.size(); ++i) {
        const QList<QByteArray> ra = framesA.at(i).split(',');
        const QList<QByteArray> rb = framesB.at(i).split(',');
        ASSERT_EQ(ra.size(), 5);
        ASSERT_EQ(rb.size(), 5);
        EXPECT_EQ(rb.at(1).toULongLong(), ra.at(1).toULongLong() - t0)
            << "row " << i;
        EXPECT_EQ(rb.at(2), ra.at(2)) << "row " << i;
        EXPECT_EQ(rb.at(3), ra.at(3)) << "row " << i;
        ASSERT_EQ(rb.at(4), ra.at(4)) << "row " << i;
    }

    // Each decoded value, signal by signal in order (host_us aside)
    const auto values = [](const QString& dir) {
        QList<QByteArray> out;
        const QList<QByteArray> rows =
            fileLines(dir + QStringLiteral("/signals.csv"));
        for (int i = 1; i < rows.size(); ++i) {
            out << rows.at(i).mid(rows.at(i).indexOf(',') + 1);
        }
        return out;
    };
    const QList<QByteArray> valuesA = values(a);
    ASSERT_FALSE(valuesA.isEmpty());
    EXPECT_EQ(values(b), valuesA);
}

/* Settings' recordings list: a recording replays under the profile it
 * was recorded with, so a tap leaves nothing to type */
/* A sleep while connected (ADR-0033 section 6) is one activity line saying how
 * long; a rise under a second, one before the connect, or one after the
 * disconnect says nothing. The line is never a note in the recording:
 * core writes its own asleep line (ADR-0049 section 2,
 * RecorderTest.ASleepIsAnAsleepLineWithItsLength). No runner can sleep,
 * so the seam adds to the count the bridge's tick reads */
TEST(BridgeTest, ASleepWhileConnectedIsLoggedWithItsLength)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    // Before the connect: not this session's
    bridge.addSleptForTesting(5000000);
    ASSERT_TRUE(bridge.connectSession("mock://asleep").isEmpty());
    spinLoop(300);
    EXPECT_TRUE(log.filter(QStringLiteral("Asleep")).isEmpty())
        << log.join(QLatin1Char('\n')).toStdString();

    bridge.addSleptForTesting(98400000);
    const QString line =
        QStringLiteral("Asleep for 98.4 s: nothing was read meanwhile");
    ASSERT_TRUE(spinUntil([&log, &line]() { return log.contains(line); }, 5000))
        << log.join(QLatin1Char('\n')).toStdString();
    bridge.addSleptForTesting(900000);
    spinLoop(300);
    EXPECT_EQ(log.filter(QStringLiteral("Asleep")).size(), 1)
        << "told once; 0.9 s is no line";

    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.addSleptForTesting(3000000);
    spinLoop(300);
    EXPECT_EQ(log.filter(QStringLiteral("Asleep")).size(), 1)
        << "nothing while disconnected";

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QList<QByteArray> events =
        fileLines(root.path() + QStringLiteral("/") + folders.first() +
                  QStringLiteral("/events.log"));
    ASSERT_FALSE(events.isEmpty());
    EXPECT_TRUE(std::none_of(
        events.cbegin(), events.cend(),
        [](const QByteArray& e) { return e.contains("Asleep for"); }))
        << "the activity line is no note";
}

/* A replay with faults injected (ADR-0041) still finishes, and says what
 * the faults did; the Session line says they were injected; the
 * recording holds the frames delivered, row for row, and the overflows
 * the bridge counts are the frames flagged in it. Seed 3 over the engine
 * start: the CLI's golden, replay-faults.jsonl, has the same counts.
 * Paced at 100x: at full speed the worker can outrun the GUI thread,
 * and frames dropped at the bridge's gate would leave rows out. */
TEST(BridgeTest, AFaultedReplayFinishesAndRecordsWhatItDelivered)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordReplaysForTesting(true);
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("replay://") + QString::fromUtf8(ECUIO_REPLAY_FIXTURES) +
        QStringLiteral("/2026-10-25-engine-start?profile=obd2&speed=100&"
                       "faults=drop:10,overflow:5,corrupt:2,truncate:2,"
                       "stall:1:5&seed=3");
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString finished = QStringLiteral(
        "Replay finished: 7074 frames, faults injected: 735 dropped, 296 lost "
        "to an overflow, 119 corrupted, 109 cut short, 70 held back; the "
        "session stays open");
    ASSERT_TRUE(spinUntil(
        [&log, &finished]() { return log.contains(finished); }, 20000))
        << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(log.filter(QStringLiteral("frames over 297.4 s at 100x, with "
                                           "faults injected"))
                     .isEmpty())
        << log.join(QLatin1Char('\n')).toStdString();
    spinLoop(300);
    ASSERT_EQ(bridge.droppedFrames(), 0)
        << "the rows below are whole only without drops";
    const int overflows = bridge.overflowFrames();
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList recorded = recordingFolders(root.path());
    ASSERT_EQ(recorded.size(), 1);
    const QList<QByteArray> rows =
        fileLines(root.path() + QStringLiteral("/") + recorded.first() +
                  QStringLiteral("/frames.csv"));
    ASSERT_EQ(rows.size(), 1 + 6043) << "the header, then each frame delivered";
    int flagged = 0;
    for (qsizetype i = 1; i < rows.size(); ++i) {
        const QList<QByteArray> cols = rows[i].split(',');
        ASSERT_GE(cols.size(), 4) << i;
        bool ok = false;
        const uint32_t flags = cols[3].toUInt(&ok, 0); /* 0x80 */
        ASSERT_TRUE(ok) << cols[3].toStdString();
        flagged += (flags & ECUIO_FRAME_FLAG_OVERFLOW) != 0 ? 1 : 0;
    }
    EXPECT_GT(flagged, 0);
    EXPECT_LE(flagged, 296) << "losses in a row flag one frame";
    EXPECT_EQ(overflows, flagged);
}

TEST(BridgeTest, ReplayUriForTakesTheRecordedProfile)
{
    // A folder as QFileInfo gives it, '/' on every platform
    const auto r = [](const char* recorded) {
        return SessionBridge::replayUriFor(
                   QStringLiteral("C:/rec/20261028-143012 Daily driver"),
                   QString::fromUtf8(recorded))
            .toStdString();
    };
    const std::string base =
        "replay://C:/rec/20261028-143012 Daily driver?profile=";
    EXPECT_EQ(r("elm327://192.168.4.1:23?profile=raw&poll=0x0C"), base + "raw")
        << "an adapter's keys stay behind";
    EXPECT_EQ(r("elm327://COM5"), base + "obd2");
    EXPECT_EQ(r("j2534://OBDX Pro - VX"), base + "obd2");
    EXPECT_EQ(r("mock://demo"), base + "mock");
    EXPECT_EQ(r("replay://C:/rec/x?speed=10"), base + "raw");
    EXPECT_EQ(r(""), base + "raw")
        << "no source named: what can't decode wrongly";
    EXPECT_EQ(r("mock://demo?profile=uds-generic&did=F190:text,1E1C:u16"),
              base + "uds-generic&did=F190:text,1E1C:u16");
    EXPECT_EQ(r("mock://demo?did=F190&profile=obd2"), base + "obd2")
        << "did= only under uds-generic, which core insists on";
    EXPECT_EQ(r("mock://demo?profile=raw&profile=obd2"), base + "obd2")
        << "the last profile= wins, as in core";
}

TEST(BridgeTest, RecentRecordingsListTheNewestFirst)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto make = [&root](const char* name, const char* uriLine) {
        const QString dir =
            root.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        QFile frames(dir + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
        if (uriLine != nullptr) {
            QFile events(dir + QStringLiteral("/events.log"));
            ASSERT_TRUE(events.open(QIODevice::WriteOnly));
            events.write("Havne session recording (ADR-0017)\n"
                         "started  2026-10-28 14:30:12.000 (local time)\n"
                         "core     0.3.2\n");
            events.write(uriLine);
            events.write("\n");
        }
    };
    make("20261027-090000", "uri      mock://demo");
    make("20261028-143012 Daily driver",
         "uri      elm327://192.168.4.1:23?profile=raw&poll=0x0C");
    make("20261028-143012-2 Daily driver", nullptr);
    ASSERT_TRUE(QDir().mkpath(root.path() + QStringLiteral("/20261029-080000")))
        << "no frames.csv: not a recording";

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 3);
    const auto field = [&rows](int i, const char* key) {
        return rows.at(i)
            .toMap()
            .value(QString::fromUtf8(key))
            .toString()
            .toStdString();
    };
    const std::string at = "replay://" + root.path().toStdString() + "/";
    EXPECT_EQ(field(0, "title"), "2026-10-28 14:30:12 \xC2\xB7 Daily driver");
    EXPECT_EQ(field(0, "uri"),
              at + "20261028-143012-2 Daily driver?profile=raw");
    EXPECT_EQ(field(0, "source"), "") << "no events.log";
    EXPECT_EQ(field(0, "folder"),
              root.path().toStdString() + "/20261028-143012-2 Daily driver");
    EXPECT_EQ(field(1, "title"), "2026-10-28 14:30:12 \xC2\xB7 Daily driver");
    EXPECT_EQ(field(1, "source"),
              "elm327://192.168.4.1:23?profile=raw&poll=0x0C");
    EXPECT_EQ(field(1, "profile"), "raw");
    EXPECT_EQ(field(2, "title"), "2026-10-27 09:00:00");
    EXPECT_EQ(field(2, "uri"), at + "20261027-090000?profile=mock");
    EXPECT_EQ(field(2, "profile"), "mock");

    EXPECT_EQ(bridge.recentRecordings(2).size(), 2);
    EXPECT_TRUE(bridge.recentRecordings(0).isEmpty());
    bridge.setRecordingsRootForTesting(root.path() + QStringLiteral("/none"));
    EXPECT_TRUE(bridge.recentRecordings(10).isEmpty()) << "no folder yet";
}

/* The list takes what the viewer opens, the rule recordingRow keeps: a
 * folder holding a signals.csv (a CSV import's values alone, ADR-0054's
 * appendix "the CSV import as built") or a frames.csv; each row says
 * whether it has frames to replay */
TEST(BridgeTest, TheRecordingsListTakesAFolderOfValuesAlone)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto make = [&root](const char* name,
                              std::initializer_list<const char*> files) {
        const QString dir =
            root.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        for (const char* f : files) {
            QFile file(dir + QLatin1Char('/') + QString::fromUtf8(f));
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        }
    };
    make("20261107-090000", {"signals.csv", "events.log"});
    make("20261106-090000", {"frames.csv"});
    make("20261105-090000", {"frames.csv", "signals.csv"});
    make("20261104-090000", {"events.log"});
    make("20261103-090000", {});

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 3) << "an events.log alone or nothing isn't one";
    const auto row = [&rows](int i) { return rows.at(i).toMap(); };
    EXPECT_EQ(row(0).value(QStringLiteral("title")).toString(),
              QStringLiteral("2026-11-07 09:00:00"));
    EXPECT_FALSE(row(0).value(QStringLiteral("frames")).toBool())
        << "values alone: nothing to replay";
    EXPECT_TRUE(row(1).value(QStringLiteral("frames")).toBool());
    EXPECT_TRUE(row(2).value(QStringLiteral("frames")).toBool());
    for (int i = 0; i < rows.size(); ++i) {
        const QString folder =
            row(i).value(QStringLiteral("folder")).toString();
        EXPECT_EQ(bridge.recordingRow(folder), row(i))
            << "the list's rule is recordingRow's: " << folder.toStdString();
    }
    EXPECT_TRUE(
        bridge.recordingRow(root.path() + QStringLiteral("/20261104-090000"))
            .isEmpty());
}

/* A vehicle's baseline (ADR-0049's appendix of 2026-11-06): one folder's
 * row, its title and URI as the list gives them, its folder as stored,
 * while it holds a signals.csv or a frames.csv; nothing once it's gone */
TEST(BridgeTest, ARecordingRowIsTheListsRowWhileItsFolderHoldsItsValues)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString dir =
        root.path() + QStringLiteral("/20261106-091500 Weekend car");
    ASSERT_TRUE(QDir().mkpath(dir));
    for (const char* name : {"/frames.csv", "/signals.csv"}) {
        QFile f(dir + QString::fromUtf8(name));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    QFile events(dir + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::WriteOnly));
    events.write("ecuio session recording, format 3 (ADR-0049)\n"
                 "uri  mock://demo\n");
    events.close();

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantMap listed = bridge.recentRecordings(1).value(0).toMap();
    const QVariantMap row = bridge.recordingRow(dir);
    EXPECT_EQ(row, listed);
    EXPECT_EQ(row.value(QStringLiteral("title")).toString().toStdString(),
              "2026-11-06 09:15:00 \xC2\xB7 Weekend car");
    EXPECT_EQ(row.value(QStringLiteral("profile")).toString(),
              QStringLiteral("mock"));

    EXPECT_TRUE(bridge.recordingRow(QString()).isEmpty());
    ASSERT_TRUE(QFile::remove(dir + QStringLiteral("/signals.csv")));
    EXPECT_EQ(bridge.recordingRow(dir), row) << "its frames still open";
    ASSERT_TRUE(QFile::remove(dir + QStringLiteral("/frames.csv")));
    EXPECT_TRUE(bridge.recordingRow(dir).isEmpty()) << "nothing to open";
    ASSERT_TRUE(QDir(dir).removeRecursively());
    EXPECT_TRUE(bridge.recordingRow(dir).isEmpty()) << "the folder is gone";
}

/* With Record on, a replay isn't recorded: it would copy the recording
 * it plays (the third phone run's Record left on made one) */
TEST(BridgeTest, AReplayIsNotRecorded)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_EQ(bridge.connectSession(uri), QString());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Not recording: a replay plays a recording already")))
        << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());
    EXPECT_TRUE(bridge.recording())
        << "the switch stays on for the next session";
}

/* The list's URI connects as it stands, uds-generic's DIDs and all */
// ---- What the product's earlier name left (ADR-0001's appendix of
// 2026-10-28): its preferences copied once, its recordings read --------

namespace {

// QSettings' default format and the user scope's ini path, pointed at a
// scratch directory for one test, then back to the native format
struct ScratchSettings {
    QTemporaryDir dir;
    ScratchSettings()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           dir.path());
    }
    ~ScratchSettings() { QSettings::setDefaultFormat(QSettings::NativeFormat); }
};

QVariant storedValue(const char* org, const char* app, const char* key)
{
    QSettings store(QSettings::IniFormat, QSettings::UserScope,
                    QString::fromUtf8(org), QString::fromUtf8(app));
    return store.value(QString::fromUtf8(key));
}

void storeValue(const char* org, const char* app, const char* key,
                const QVariant& value)
{
    QSettings store(QSettings::IniFormat, QSettings::UserScope,
                    QString::fromUtf8(org), QString::fromUtf8(app));
    store.setValue(QString::fromUtf8(key), value);
}

} // namespace

TEST(BridgeTest, CarryOverCopiesTheEarlierPreferencesOnce)
{
    ScratchSettings scratch;
    ASSERT_TRUE(scratch.dir.isValid());
    storeValue("Old", "Old", "ui/zoom", 1.25);
    storeValue("Old", "Old", "session/uri", QStringLiteral("mock://demo"));
    storeValue("Old", "Old", "vehicles/document", QStringLiteral("{\"list\":[]}"));

    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("New"), QStringLiteral("New")),
              3);
    EXPECT_EQ(storedValue("New", "New", "ui/zoom").toDouble(), 1.25);
    EXPECT_EQ(storedValue("New", "New", "session/uri").toString(),
              QStringLiteral("mock://demo"));
    EXPECT_EQ(storedValue("New", "New", "vehicles/document").toString(),
              QStringLiteral("{\"list\":[]}"));

    // Once: a later change to the earlier store isn't copied again
    storeValue("Old", "Old", "ui/zoom", 2.0);
    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("New"), QStringLiteral("New")),
              0);
    EXPECT_EQ(storedValue("New", "New", "ui/zoom").toDouble(), 1.25);
    // and the earlier store is left as it was, for an older build
    EXPECT_EQ(storedValue("Old", "Old", "session/uri").toString(),
              QStringLiteral("mock://demo"));
}

TEST(BridgeTest, CarryOverNeverWritesOverAStoreThatHasPreferences)
{
    ScratchSettings scratch;
    ASSERT_TRUE(scratch.dir.isValid());
    storeValue("Old", "Old", "ui/zoom", 1.25);
    storeValue("Old", "Old", "session/uri", QStringLiteral("mock://demo"));
    storeValue("New", "New", "ui/zoom", 1.5);

    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("New"), QStringLiteral("New")),
              0);
    EXPECT_EQ(storedValue("New", "New", "ui/zoom").toDouble(), 1.5);
    EXPECT_FALSE(storedValue("New", "New", "session/uri").isValid());

    // The same name, or nothing to carry: nothing happens
    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("Old"), QStringLiteral("Old")),
              0);
    EXPECT_EQ(carryOverSettings(QStringLiteral("None"), QStringLiteral("None"),
                                QStringLiteral("Empty"), QStringLiteral("Empty")),
              0);
}

TEST(BridgeTest, RecentRecordingsReadTheEarlierNamesFolderToo)
{
    QTemporaryDir current;
    QTemporaryDir earlier;
    ASSERT_TRUE(current.isValid());
    ASSERT_TRUE(earlier.isValid());
    const auto make = [](const QTemporaryDir& root, const char* name) {
        const QString dir =
            root.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        QFile frames(dir + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
    };
    make(current, "20261029-090000");
    make(earlier, "20261028-100000");
    make(earlier, "20261030-080000 Weekend car");

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(current.path());
    bridge.setEarlierRecordingsRootForTesting(earlier.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 3) << "both folders are read";
    const auto uri = [&rows](int i) {
        return rows.at(i).toMap().value(QStringLiteral("uri")).toString().toStdString();
    };
    // Newest first across the two, each replaying from its own folder
    EXPECT_EQ(uri(0), "replay://" + earlier.path().toStdString() +
                          "/20261030-080000 Weekend car?profile=raw");
    EXPECT_EQ(uri(1), "replay://" + current.path().toStdString() +
                          "/20261029-090000?profile=raw");
    EXPECT_EQ(uri(2), "replay://" + earlier.path().toStdString() +
                          "/20261028-100000?profile=raw");
    EXPECT_EQ(bridge.recentRecordings(2).size(), 2) << "the limit spans both";
}

TEST(BridgeTest, RecentRecordingsReplayUnderTheirOwnProfile)
{
    QTemporaryDir first, second;
    ASSERT_TRUE(first.isValid() && second.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordingsRootForTesting(first.path());
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral(
                "mock://recent?profile=uds-generic&did=F190:text,1E1C:u16"))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 3; }, 10000));
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QVariantList rows = bridge.recentRecordings(5);
    ASSERT_EQ(rows.size(), 1);
    const QString uri =
        rows.first().toMap().value(QStringLiteral("uri")).toString();
    EXPECT_TRUE(uri.endsWith(
        QStringLiteral("?profile=uds-generic&did=F190:text,1E1C:u16")))
        << uri.toStdString();

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setRecordingsRootForTesting(second.path());
    ASSERT_EQ(bridge.connectSession(uri), QString());
    EXPECT_TRUE(std::any_of(log.cbegin(), log.cend(), [](const QString& l) {
        return l.startsWith(QStringLiteral("Session: replay of a recording")) &&
               l.endsWith(QStringLiteral("; profile uds-generic"));
    })) << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* What core's values callback delivers, as signals.csv rows: name and
 * value, formatted as core's recorder writes them */
struct CoreRows {
    QHash<uint16_t, QByteArray> names;
    std::vector<std::pair<QByteArray, QByteArray>> rows;
    std::atomic<int> frames{0};
};

void core_rows_frame_cb(const ecuio_frame_t*, void* user_data)
{
    ++static_cast<CoreRows*>(user_data)->frames;
}

void core_rows_values_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* c = static_cast<CoreRows*>(user_data);
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        c->rows.emplace_back(c->names.value(v.signal_id),
                             v.text != nullptr
                                 ? QByteArray(v.text)
                                 : QByteArray::number(v.value, 'g', 12));
    }
}

} // namespace

/* The chart keeps a peak (ADR-0005's appendix): the vehicle fixture's
 * engine start peaks at 1,261 rpm, and replayed at 100x under obd2,
 * ten seconds of the recording to a window, the chart reaches it. Each
 * sample sits at its frame's time in the recording, in order */
TEST(BridgeTest, TheChartKeepsTheFixturesPeak)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&speed=100")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    double peak = 0.0;
    bool ordered = true;
    qint64 lastMs = -1;
    QObject::connect(model, &SignalListModel::valuesUpdated, model, [&]() {
        for (const QVariant& s : model->chartData()) {
            const QVariantMap m = s.toMap();
            if (m.value("name").toString() != QStringLiteral("RPM"))
                continue;
            const QVariantList samples = m.value("samples").toList();
            qint64 prev = -1;
            for (qsizetype i = 0; i + 1 < samples.size(); i += 2) {
                const qint64 t = samples.at(i).toLongLong();
                ordered = ordered && t >= prev && t <= 297446;
                prev = t;
                peak = std::max(peak, samples.at(i + 1).toDouble());
            }
            lastMs = prev;
        }
    });
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() + bridge.droppedFrames() >= 7074; },
        20000));
    spinLoop(300);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_NEAR(peak, 1261.25, 0.01) << "the recording's peak, the sheet's 1,261";
    EXPECT_TRUE(ordered) << "times in order, inside the recording";
    EXPECT_GT(lastMs, 290000) << "the last sample near the recording's end";
}

/* LatencyStats reads a quantile back at the top of the bucket holding
 * it: exact below 16 us, at most an eighth above the true value past
 * that, never below it, and the maximum exactly */
TEST(BridgeTest, LatencyStatsReadsQuantilesWithinABucket)
{
    LatencyStats empty;
    EXPECT_EQ(empty.count(), 0u);
    EXPECT_EQ(empty.quantileUs(0.5), 0u);

    LatencyStats small;
    for (int us : {0, 3, 7, 15})
        small.add(us);
    small.add(-5);   // a clock step backwards counts as 0
    EXPECT_EQ(small.count(), 5u);
    EXPECT_EQ(small.quantileUs(0.0), 0u);
    EXPECT_EQ(small.quantileUs(0.5), 3u) << "exact below 16 µs";
    EXPECT_EQ(small.quantileUs(1.0), 15u);

    LatencyStats l;
    for (int us = 1; us <= 100000; ++us)
        l.add(us);
    EXPECT_EQ(l.count(), 100000u);
    EXPECT_EQ(l.maxUs(), 100000u);
    for (double q : {0.01, 0.25, 0.5, 0.9, 0.99, 0.999}) {
        const double truth = q * 100000.0;
        const double got = static_cast<double>(l.quantileUs(q));
        EXPECT_GE(got, truth) << q;
        EXPECT_LE(got, truth * 1.125 + 1.0) << q;
    }
    EXPECT_EQ(l.quantileUs(1.0), 100000u) << "the maximum, exactly";

    LatencyStats huge;
    huge.add(int64_t(1) << 50);   // past the last bucket: kept, and the max
    EXPECT_EQ(huge.quantileUs(0.5), uint64_t(1) << 50);
    l.clear();
    EXPECT_EQ(l.count(), 0u);
    EXPECT_EQ(l.maxUs(), 0u);
}

/* The activity log's "Delivery:" line: frames and drops, what was still
 * queued, then each path's median, p99 and max (us below a millisecond,
 * ms with a decimal below 10, whole ms past that); a session that
 * decoded nothing has no values clause */
TEST(BridgeTest, DeliveryTextNamesFramesDropsAndTimes)
{
    SessionBridge::DeliveryStats none;
    EXPECT_EQ(SessionBridge::deliveryText(none).toStdString(), "Delivery: no frames");

    SessionBridge::DeliveryStats raw;
    raw.frames = 3;
    for (int us : {12, 12, 900})
        raw.frameToGui.add(us);
    EXPECT_EQ(SessionBridge::deliveryText(raw).toStdString(),
              "Delivery: 3 frames, none dropped; to the GUI thread "
              "in median 12 µs, p99 900 µs, max 900 µs");

    SessionBridge::DeliveryStats busy;
    busy.frames = 2000;
    busy.dropped = 500;
    busy.queuedAtEnd = 7;
    for (int i = 0; i < 1493; ++i)
        busy.frameToGui.add(2500);
    busy.values = 4000;
    busy.valuesMissed = 10;
    for (int i = 0; i < 3990; ++i)
        busy.valueToModel.add(i < 3900 ? 52000 : 99000);
    /* 52 ms reads 53: its bucket's top is 53.247 ms, an eighth of
     * 32.768 ms past its floor. 99 ms reads exact, capped at the max. */
    EXPECT_EQ(SessionBridge::deliveryText(busy).toStdString(),
              "Delivery: 2000 frames, 500 dropped at the in-flight cap "
              "(25.0 %), 7 still queued at the end; to the GUI thread "
              "in median 2.5 ms, p99 2.5 ms, max 2.5 ms; 4000 values, "
              "to the model in median 53 ms, p99 99 ms, max 99 ms "
              "(10 past the staging's limit, not timed)");
}

/* The activity log's "Reply timing:" line (ADR-0042): the requests asked,
 * their outcomes, the spread of their median latencies, the intervals'
 * clock and ISO-TP's counts where core reassembles; nothing when no
 * request was asked */
TEST(BridgeTest, TimingTextNamesTheRequestsAndTheClock)
{
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(3);
    EXPECT_TRUE(SessionBridge::timingText(t, rows).isEmpty());

    t.interval_clock = ECUIO_TIMING_CLOCK_ADAPTER;
    rows[0].answered = 100;
    rows[0].latency_p50_us = 4200;
    rows[1].answered = 50;
    rows[1].timeouts = 2;
    rows[1].latency_p50_us = 12400;
    EXPECT_EQ(
        SessionBridge::timingText(t, rows).toStdString(),
        "Reply timing: 2 requests, 150 answered, none refused, 2 timed out; "
        "latency P50 4.2 ms to 12 ms; intervals on the adapter's clock");

    t.interval_clock = ECUIO_TIMING_CLOCK_KERNEL;
    t.flags = ECUIO_TIMING_ISOTP;
    t.isotp_completed = 640;
    t.isotp_first_frames = 40;
    t.isotp_dropped = 1;
    rows[1] = ecuio_request_timing_t{};
    rows[2].refused = 1;
    rows[2].latency_p50_us = 4200;
    EXPECT_EQ(
        SessionBridge::timingText(t, rows).toStdString(),
        "Reply timing: 2 requests, 100 answered, 1 refused, none timed out; "
        "latency P50 4.2 ms; intervals on the kernel's receive times; "
        "ISO-TP: 640 messages, 40 segmented, 1 dropped, none unusable");
}

#ifdef _WIN32

/* A log on a session that asks takes the minimum, and its line and its
 * header say so (ADR-0056 section 4) */
TEST(BridgeTest, ALogOnARequestingSessionNamesItsMinimumTime)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setMinSweepMs(250);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri = QStringLiteral("j2534://%1?identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    std::vector<uint16_t> picks;
    for (int r = 0; r < model->rowCount() && picks.size() < 2; ++r) {
        const QModelIndex i = model->index(r);
        if (!model->data(i, SignalListModel::IsTextRole).toBool()) {
            picks.push_back(static_cast<uint16_t>(
                model->data(i, SignalListModel::IdRole).toUInt()));
        }
    }
    ASSERT_EQ(picks.size(), 2u);
    plotOnly(model, picks);

    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(
        QStringLiteral(
            "Log started: %1 (2 signals, at least 250 ms between sweeps)")
            .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    const QString folder = bridge.recordingFolderUrl().toLocalFile();
    bridge.stopLog();
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    QFile f(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString header = QString::fromUtf8(f.readAll());
    EXPECT_TRUE(header.contains(QLatin1String("min sweep  250 ms")))
        << header.toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A recording of a requesting session holds latency.csv (ADR-0042
 * section 3), which core writes (ADR-0049): its header is
 * ECUIO_TIMING_COLUMNS, then the worker's final snapshot at the
 * teardown, a row per request on the engine's ECU, the intervals on the
 * fake's own clock (core's 10 s snapshots, RecorderTest); the activity
 * log names the timing at the disconnect */
TEST(BridgeTest, ARecordingKeepsReplyTimingsSnapshots)
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
    const QString uri = QStringLiteral("j2534://%1?identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    spinLoop(3500);
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(spinUntil([&]() { return !bridge.recordingActive(); }, 5000));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QList<QByteArray> lines =
        fileLines(root.path() + QStringLiteral("/") + folders.first() +
                  QStringLiteral("/latency.csv"));
    ASSERT_GT(lines.size(), 2);
    EXPECT_EQ(lines.first(), QByteArray(ECUIO_TIMING_COLUMNS));
    const int columns =
        static_cast<int>(QByteArray(ECUIO_TIMING_COLUMNS).count(',')) + 1;
    QSet<QByteArray> snapshots;
    QHash<QByteArray, qint64> answered;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QList<QByteArray> cols = lines[i].split(',');
        ASSERT_EQ(cols.size(), columns) << lines[i].toStdString();
        snapshots.insert(cols[0]);
        EXPECT_EQ(cols[1], QByteArray("0x7E8"));
        EXPECT_EQ(cols.last(), QByteArray("adapter"));
        answered[cols[2]] = cols[3].toLongLong();
    }
    EXPECT_EQ(snapshots.size(), 1) << "the last, at the teardown";
    EXPECT_GT(answered.size(), 10);
    bool named = false;
    for (const QString& line : log) {
        named = named || (line.startsWith(QStringLiteral("Reply timing: ")) &&
                          line.contains(QStringLiteral("the adapter's clock")));
    }
    EXPECT_TRUE(named) << log.join(QLatin1Char('\n')).toStdString();
}

#endif // _WIN32

/* At disconnect the activity log says how the session's frames and
 * values were delivered: over the engine-start fixture at full speed,
 * every frame accounted for (handled or dropped at the cap), each
 * handled frame and each staged value timed, and the line is the
 * figures the bridge kept */
TEST(BridgeTest, DisconnectLogsTheDelivery)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    constexpr quint64 kFrames = 7074;
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() + bridge.droppedFrames() >= int(kFrames); },
        20000));
    spinLoop(300);   // past two ticks: every value drained

    const SessionBridge::DeliveryStats s = bridge.deliveryStats();
    EXPECT_EQ(s.frames, kFrames);
    EXPECT_EQ(s.dropped + static_cast<quint64>(bridge.frameCount()), kFrames);
    EXPECT_EQ(s.queuedAtEnd, 0u);
    EXPECT_EQ(s.frameToGui.count(), static_cast<quint64>(bridge.frameCount()))
        << "every handled frame timed";
    EXPECT_GT(s.values, 0u);
    EXPECT_EQ(s.valuesMissed, 0u);
    EXPECT_EQ(s.valueToModel.count(), s.values) << "every staged value timed";

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList delivery = log.filter(QStringLiteral("Delivery: "));
    ASSERT_EQ(delivery.size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(delivery.first(), SessionBridge::deliveryText(s));
    EXPECT_TRUE(delivery.first().startsWith(QStringLiteral("Delivery: 7074 frames, ")));
    EXPECT_TRUE(delivery.first().contains(QStringLiteral("values, to the model in median ")));
}

/* The recording holds every value core decodes (ADR-0024): over the
 * vehicle fixture under obd2, the bridge's signals.csv is, row for row,
 * what core's values callback delivers to a session of its own for the
 * same recording */
TEST(BridgeTest, RecordingHoldsEveryValueCoreDecodes)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&speed=100")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    constexpr int kFrames = 7074;

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.frameCount() + bridge.droppedFrames() >= kFrames;
        },
        20000));
    spinLoop(300);   // the last frames' values staged, a tick or more
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray csv = readFile(root.path() + QStringLiteral("/") +
                                    folders.first() + QStringLiteral("/signals.csv"));
    std::vector<std::pair<QByteArray, QByteArray>> recorded;
    const QList<QByteArray> lines = csv.split('\n');
    for (qsizetype i = 1; i < lines.size(); ++i) {   // past the header
        const QList<QByteArray> cols = lines[i].split(',');
        if (cols.size() == 4)
            recorded.emplace_back(cols[1], cols[2]);
    }

    ecuio_session_t* session = nullptr;
    const QByteArray utf8 = uri.toUtf8();
    ASSERT_EQ(ecuio_session_create(utf8.constData(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    CoreRows core;
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    for (uint32_t i = 0; i < set->count; ++i) {
        core.names.insert(set->defs[i].id,
                          QByteArray(set->defs[i].name,
                                     qstrnlen(set->defs[i].name,
                                              sizeof set->defs[i].name)));
    }
    EXPECT_EQ(ecuio_register_frame_callback(session, core_rows_frame_cb, &core),
              ECUIO_OK);
    EXPECT_EQ(ecuio_register_values_callback(session, core_rows_values_cb, &core),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    const bool reached =
        spinUntil([&core]() { return core.frames.load() >= kFrames; }, 20000);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    ASSERT_TRUE(reached) << core.frames.load() << " frames";

    ASSERT_GT(recorded.size(), 1000u);
    ASSERT_EQ(core.rows.size(), recorded.size());
    for (size_t i = 0; i < recorded.size(); ++i) {
        ASSERT_EQ(core.rows[i], recorded[i])
            << "row " << i << ": core " << core.rows[i].first.toStdString()
            << " " << core.rows[i].second.toStdString() << ", bridge "
            << recorded[i].first.toStdString() << " "
            << recorded[i].second.toStdString();
    }
}

namespace {

/* n rows of 0x7E8 RPM replies, 10 ms of session time apart; row i reads
 * 1000 + i rpm (41 0C, a quarter rpm a count) */
QString writeRpmRows(const QString& folder, int n)
{
    QByteArray text = "host_us,timestamp_us,id,flags,data\n";
    for (int i = 0; i < n; ++i) {
        const QByteArray t = QByteArray::number(qint64{10000} * i);
        const int raw = (1000 + i) * 4;
        text += t + ',' + t + ",0x7E8,0x0,41 0C " +
                QByteArray::number(raw >> 8, 16).rightJustified(2, '0').toUpper() + ' ' +
                QByteArray::number(raw & 0xFF, 16).rightJustified(2, '0').toUpper() + '\n';
    }
    QFile f(folder + QStringLiteral("/frames.csv"));
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(text);
    return QStringLiteral("replay://%1?profile=obd2&speed=max").arg(folder);
}

/* signals.csv's RPM values, in order */
std::vector<double> recordedRpm(const QString& root)
{
    std::vector<double> out;
    const QStringList folders = recordingFolders(root);
    if (folders.size() != 1)
        return out;
    const QByteArray csv = readFile(root + QStringLiteral("/") + folders.first() +
                                    QStringLiteral("/signals.csv"));
    for (const QByteArray& line : csv.split('\n')) {
        const QList<QByteArray> cols = line.split(',');
        if (cols.size() == 4 && cols[1] == "RPM")
            out.push_back(cols[2].toDouble());
    }
    return out;
}

} // namespace

/* Values come from core's worker, not from the frames the GUI thread
 * handles, so a frame dropped at the in-flight cap keeps its values
 * (ADR-0024 section 7): with the event loop held while a replay floods in,
 * thousands of frames drop, yet the recording holds every row's RPM in
 * order and the row shows the last one. Core's recording (ADR-0049)
 * holds the dropped frames too, and its last note counts them */
TEST(BridgeTest, ValuesSurviveFramesDroppedAtTheCap)
{
    constexpr int kRows = 5000;
    QTemporaryDir recording, root;
    ASSERT_TRUE(recording.isValid() && root.isValid());
    const QString uri = writeRpmRows(recording.path(), kRows);
    ASSERT_FALSE(uri.isEmpty());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    // No event processing: the queued frames pile up to the cap
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.frameCount() + bridge.droppedFrames() >= kRows;
        },
        10000));
    EXPECT_GT(bridge.droppedFrames(), 1000) << "the cap dropped frames";
    spinLoop(300);
    SignalListModel* model = bridge.signalModel();
    int rpmRow = -1;
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->data(model->index(r, 0), SignalListModel::NameRole).toString() ==
            QStringLiteral("RPM"))
            rpmRow = r;
    }
    ASSERT_GE(rpmRow, 0);
    EXPECT_EQ(model->data(model->index(rpmRow, 0), SignalListModel::ValueRole).toDouble(),
              1000.0 + kRows - 1)
        << "the last row's value, though its frame may have dropped";
    const int dropped = bridge.droppedFrames();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const std::vector<double> rpm = recordedRpm(root.path());
    ASSERT_EQ(rpm.size(), static_cast<size_t>(kRows)) << "every row's value recorded";
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(rpm[static_cast<size_t>(i)], 1000.0 + i) << "row " << i;
    }
    // Core records the frames the bridge dropped too (ADR-0049), and the
    // recording's last note says how many the Log page skipped
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString dir = root.path() + QStringLiteral("/") + folders.first();
    EXPECT_EQ(fileLines(dir + QStringLiteral("/frames.csv")).size() - 1, kRows)
        << "every frame, dropped by the bridge or not";
    const QByteArray events = readFile(dir + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains(
        QStringLiteral(
            "  note  the Log page skipped %1 frames; the recording has them")
            .arg(dropped)
            .toUtf8()))
        << events.toStdString();
}

/* One window holds a bounded number of samples, so the worker never
 * allocates: past the limit a value goes untimed and the Delivery line
 * counts it, while the row still shows the newest value and the
 * recording, which core writes (ADR-0049), holds every one */
TEST(BridgeTest, ValuesPastTheWindowsLimitAreCountedAndLogged)
{
    constexpr int kRows = 3000;
    constexpr int kLimit = 100;
    QTemporaryDir recording, root;
    ASSERT_TRUE(recording.isValid() && root.isValid());
    const QString uri = writeRpmRows(recording.path(), kRows);
    ASSERT_FALSE(uri.isEmpty());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    bridge.setValueSampleLimitForTesting(kLimit);
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    // Held, so the whole replay lands in one window
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.frameCount() + bridge.droppedFrames() >= kRows;
        },
        10000));
    spinLoop(300);
    SignalListModel* model = bridge.signalModel();
    double shown = 0.0;
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->data(model->index(r, 0), SignalListModel::NameRole).toString() ==
            QStringLiteral("RPM"))
            shown = model->data(model->index(r, 0), SignalListModel::ValueRole).toDouble();
    }
    EXPECT_EQ(shown, 1000.0 + kRows - 1) << "the newest value, past the limit";
    ASSERT_TRUE(disconnectAndWait(bridge));
    const SessionBridge::DeliveryStats s = bridge.deliveryStats();
    const std::vector<double> rpm = recordedRpm(root.path());
    ASSERT_EQ(rpm.size(), static_cast<size_t>(kRows)) << "every value recorded";
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(rpm[static_cast<size_t>(i)], 1000.0 + i) << "row " << i;
    }
    EXPECT_GE(s.valuesMissed, static_cast<quint64>(kRows - 2 * kLimit));
    const QStringList delivery = log.filter(QStringLiteral("Delivery: "));
    ASSERT_EQ(delivery.size(), 1) << log.join('\n').toStdString();
    EXPECT_TRUE(delivery.first().contains(
        QStringLiteral("(%1 past the staging's limit").arg(s.valuesMissed)))
        << delivery.first().toStdString();
}

namespace {

/* A recording's signals.csv rows past the header, each without its
 * host_us column: signal, value, unit */
QList<QByteArray> signalRows(const QString& folder)
{
    QList<QByteArray> out;
    const QList<QByteArray> lines = readFile(folder + QStringLiteral("/signals.csv")).split('\n');
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype comma = line.indexOf(',');
        if (comma > 0)
            out << line.mid(comma + 1);
    }
    return out;
}

/* The replay URI that decodes a recording as it was decoded live: the
 * recorded URI's profile and DIDs, or its scheme's default profile */
QString replayUriFor(const QString& folder)
{
    QString uri;
    for (const QByteArray& line : readFile(folder + QStringLiteral("/events.log")).split('\n')) {
        if (line.startsWith("uri ")) {
            uri = QString::fromUtf8(line.mid(4).trimmed());
            break;
        }
    }
    if (uri.isEmpty())
        return {};
    QString profile = uri.startsWith(QStringLiteral("mock://")) ? QStringLiteral("mock")
                                                                : QStringLiteral("obd2");
    QStringList keep;
    const qsizetype q = uri.indexOf(QLatin1Char('?'));
    if (q >= 0) {
        for (const QString& pair : uri.mid(q + 1).split(QLatin1Char('&'))) {
            if (pair.startsWith(QStringLiteral("profile=")))
                profile = pair.mid(8);
            else if (pair.startsWith(QStringLiteral("did=")))
                keep << pair;
        }
    }
    keep.prepend(QStringLiteral("profile=") + profile);
    return QStringLiteral("replay://%1?%2&speed=max").arg(folder, keep.join(QLatin1Char('&')));
}

} // namespace

/* Local only: ECUIO_BENCH_RAW names a folder of real-car recordings
 * (Testing/bench-raw, kept out of the repository by the bench-session
 * rules). Each recording's frames.csv, replayed through the bridge
 * under the profile its URI chose, records the values its own
 * signals.csv holds, row for row: the decode on the worker against the
 * one that ran on the GUI thread in the car. A mismatch names the
 * recording, the row and the signal, never a value, which can be a
 * vehicle's identity. */
TEST(BridgeTest, VehicleRecordingsReplayToTheirOwnValues)
{
    const QString root = qEnvironmentVariable("ECUIO_BENCH_RAW");
    if (root.isEmpty())
        GTEST_SKIP() << "set ECUIO_BENCH_RAW to a folder of recordings";
    QStringList folders;
    QDirIterator it(root, {QStringLiteral("frames.csv")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = QFileInfo(it.next()).absolutePath();
        if (QFile::exists(dir + QStringLiteral("/signals.csv")) &&
            QFile::exists(dir + QStringLiteral("/events.log")))
            folders << dir;
    }
    folders.sort();
    ASSERT_FALSE(folders.isEmpty()) << "no recordings under ECUIO_BENCH_RAW";

    qsizetype compared = 0;
    for (const QString& folder : folders) {
        const QString name = QDir(folder).dirName();
        SCOPED_TRACE(name.toStdString());
        const QString uri = replayUriFor(folder);
        ASSERT_FALSE(uri.isEmpty()) << "no uri line in events.log";
        const QList<QByteArray> frames = readFile(folder + QStringLiteral("/frames.csv")).split('\n');
        int rows = 0;
        for (qsizetype i = 1; i < frames.size(); ++i) {
            if (!frames[i].trimmed().isEmpty())
                ++rows;
        }
        if (rows == 0)
            continue;   // a connect that heard nothing; replay refuses it

        QTemporaryDir out;
        ASSERT_TRUE(out.isValid());
        SessionBridge bridge;
        bridge.setRecordingsRootForTesting(out.path());
        bridge.setRecording(true);
        // A replay, compared with its source
        bridge.setRecordReplaysForTesting(true);
        bridge.setValueSampleLimitForTesting(1 << 20);
        const QString err = bridge.connectSession(uri);
        if (!err.isEmpty()) {
            ADD_FAILURE() << "replay refused: " << err.toStdString();
            continue;
        }
        EXPECT_TRUE(spinUntil(
            [&]() { return bridge.frameCount() + bridge.droppedFrames() >= rows; },
            30000))
            << bridge.frameCount() + bridge.droppedFrames() << " of " << rows << " frames";
        spinLoop(300);
        ASSERT_TRUE(disconnectAndWait(bridge));
        const QStringList made = recordingFolders(out.path());
        ASSERT_EQ(made.size(), 1);

        const QList<QByteArray> live = signalRows(folder);
        const QList<QByteArray> replayed = signalRows(out.path() + QStringLiteral("/") + made.first());
        EXPECT_EQ(replayed.size(), live.size()) << "rows";
        const qsizetype n = std::min(live.size(), replayed.size());
        for (qsizetype i = 0; i < n; ++i) {
            if (replayed[i] != live[i]) {
                ADD_FAILURE() << "row " << i << ": " << live[i].left(live[i].indexOf(',')).toStdString()
                              << " decodes differently";
                break;
            }
        }
        compared += n;
    }
    EXPECT_GT(compared, 0);
    std::printf("compared %lld rows over %lld recordings\n", static_cast<long long>(compared),
                static_cast<long long>(folders.size()));
}

TEST(BridgeTest, ReplayUriLabelKeepsTheRecordingsName)
{
    const QString dots = QString(QChar(0x2026));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("replay://C:/Users/me/Documents/Havne/"
                                 "recordings/20261025-165100?profile=obd2")),
              QStringLiteral("replay://") + dots +
                  QStringLiteral("/20261025-165100?profile=obd2"));
    EXPECT_EQ(
        SessionBridge::uriLabelFor(QStringLiteral(
            "replay://C:\\Users\\me\\recordings\\20261025-165100\\frames.csv")),
        QStringLiteral("replay://") + dots +
            QStringLiteral("/20261025-165100\\frames.csv"));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("replay:///home/me/rec/20261025-165100/")),
              QStringLiteral("replay://") + dots +
                  QStringLiteral("/20261025-165100"));
    for (const char* uri :
         {"replay://", "replay://?speed=max", "replay://frames.csv"}) {
        EXPECT_EQ(SessionBridge::uriLabelFor(QString::fromLatin1(uri)),
                  QString::fromLatin1(uri));
    }
}

TEST(BridgeTest, SessionInfoTextDescribesAReplay)
{
    ecuio_session_info_t info{};
    info.backend = ECUIO_BACKEND_REPLAY;
    info.replay_frames = 7194;
    info.replay_span_ms = 275224;
    info.replay_speed_milli = 1000;
    qstrncpy(info.profile, "obd2", sizeof(info.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at 1x; "
              "profile obd2");
    info.replay_speed_milli = 2500;
    info.flags = ECUIO_SESSION_INFO_REPLAY_LOOP;
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at "
              "2.5x, looping; "
              "profile obd2");
    info.replay_speed_milli = 0;
    info.flags = 0;
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at "
              "full speed; "
              "profile obd2");
    info.replay_rows_torn = 1;
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at "
              "full speed, its last row cut short and left out; "
              "profile obd2");
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

// ---------------------------------------------------------------------------
// Read once (ADR-0051's form, app/readonce.h): the draft into a request
// and a def, the URI that asks for it, and what a reply says. Session-free.
// ---------------------------------------------------------------------------

namespace {

// A DID read as a signal file holds it, the module the engine ECU
QVariantMap didSignal(const QString& did, const QVariantMap& field,
                      const QVariantMap& more = {})
{
    QVariantMap s{
        {QStringLiteral("name"), QStringLiteral("Test")},
        {QStringLiteral("read"),
         QVariantMap{{QStringLiteral("service"), QStringLiteral("22")},
                     {QStringLiteral("number"), did}}},
        {QStringLiteral("field"), field}};
    for (auto it = more.begin(); it != more.end(); ++it) {
        s.insert(it.key(), it.value());
    }
    return s;
}

QVariantMap pidSignal(const QString& pid, const QVariantMap& field,
                      const QVariantMap& more = {})
{
    QVariantMap s = didSignal(QString(), field, more);
    s.insert(QStringLiteral("read"),
             QVariantMap{{QStringLiteral("service"), QStringLiteral("01")},
                         {QStringLiteral("number"), pid}});
    return s;
}

QVariantMap engineEcu()
{
    return {{QStringLiteral("rx"), QStringLiteral("7E8")}};
}

QVariantMap field(int byte, int bits, const QVariantMap& more = {})
{
    QVariantMap f{{QStringLiteral("byte"), byte},
                  {QStringLiteral("bits"), bits}};
    for (auto it = more.begin(); it != more.end(); ++it) {
        f.insert(it.key(), it.value());
    }
    return f;
}

ReadOnce::Request request(const QVariantMap& signal,
                          const QVariantMap& module = engineEcu())
{
    ReadOnce::Request r;
    QString why;
    EXPECT_TRUE(ReadOnce::requestFrom(signal, module, &r, &why))
        << why.toStdString();
    return r;
}

QVariantMap evaluateBytes(const ReadOnce::Request& r,
                          std::initializer_list<uint8_t> bytes)
{
    const std::vector<uint8_t> v(bytes);
    return ReadOnce::evaluate(r, v.data(), static_cast<uint16_t>(v.size()));
}

} // namespace

TEST(ReadOnceTest, TheRequestIdIsTheStandardsWhereItGivesOne)
{
    EXPECT_EQ(ReadOnce::standardTx(0x7E8), 0x7E0u);
    EXPECT_EQ(ReadOnce::standardTx(0x7EF), 0x7E7u);
    EXPECT_EQ(ReadOnce::standardTx(0x7E7), 0u);
    EXPECT_EQ(ReadOnce::standardTx(0x7B8), 0u);
    EXPECT_EQ(ReadOnce::standardTx(0x18DAF110), 0x18DA10F1u);
    EXPECT_EQ(ReadOnce::standardTx(0x18DA10F1), 0u);
}

/* The adapter's own keys stay; what a session asks for is Read once's */
TEST(ReadOnceTest, TheUriKeepsTheAdaptersKeysAndAsksForTheOneRequest)
{
    QString why;
    const ReadOnce::Request did =
        request(didSignal(QStringLiteral("1E1C"), field(0, 16)));
    EXPECT_EQ(
        ReadOnce::uriFor(
            QStringLiteral(" j2534://C:/v/x.dll?profile=obd2&baud=500000"
                           "&did=F190&poll=0C&identify=1 "),
            did, &why)
            .toStdString(),
        "j2534://C:/v/x.dll?baud=500000&profile=uds-generic&did=7E8:1E1C:hex"
        "&identify=0&diagnose=0&probe=0");

    const ReadOnce::Request pid =
        request(pidSignal(QStringLiteral("0B"), field(0, 8)));
    EXPECT_EQ(
        ReadOnce::uriFor(QStringLiteral("elm327://COM6"), pid, &why)
            .toStdString(),
        "elm327://COM6?profile=raw&poll=0x0B&filter=0x7E8&txid=0x7E0&probe=0");

    const ReadOnce::Request body =
        request(pidSignal(QStringLiteral("0C"), field(0, 16)),
                {{QStringLiteral("rx"), QStringLiteral("7B8")},
                 {QStringLiteral("tx"), QStringLiteral("7B0")}});
    EXPECT_EQ(ReadOnce::uriFor(QStringLiteral("socketcan://can0?baud=500000"),
                               body, &why)
                  .toStdString(),
              "socketcan://"
              "can0?baud=500000&profile=obd2&poll=0x0C&filter=0x7B8&txid=0x7B0"
              "&probe=0");
    // uds-generic's own rule addresses an 11-bit module off the default
    const ReadOnce::Request bodyDid =
        request(didSignal(QStringLiteral("D101"), field(0, 8)),
                {{QStringLiteral("rx"), QStringLiteral("7B8")},
                 {QStringLiteral("tx"), QStringLiteral("7B0")}});
    EXPECT_EQ(ReadOnce::uriFor(QStringLiteral("elm327://COM6"), bodyDid, &why)
                  .toStdString(),
              "elm327://"
              "COM6?profile=uds-generic&did=7B8:D101:hex&identify=0&diagnose=0"
              "&probe=0");
    const ReadOnce::Request wide =
        request(didSignal(QStringLiteral("D101"), field(0, 8)),
                {{QStringLiteral("rx"), QStringLiteral("18DAF110")}});
    EXPECT_EQ(
        ReadOnce::uriFor(QStringLiteral("elm327://COM6"), wide, &why)
            .toStdString(),
        "elm327://"
        "COM6?profile=uds-generic&did=18DAF110:D101:hex&identify=0&diagnose=0"
        "&probe=0");
}

TEST(ReadOnceTest, AUriItCantAskOnIsRefused)
{
    QString why;
    const ReadOnce::Request did =
        request(didSignal(QStringLiteral("1E1C"), field(0, 16)));
    EXPECT_TRUE(
        ReadOnce::uriFor(QStringLiteral("mock://demo"), did, &why).isEmpty());
    EXPECT_EQ(
        why.toStdString(),
        "Read once needs an adapter: j2534://, elm327:// or socketcan://");
    EXPECT_TRUE(
        ReadOnce::uriFor(QStringLiteral("replay://C:/r/frames.csv"), did, &why)
            .isEmpty());

    // A DID's module off uds-generic's rule: it has no way to address it
    const ReadOnce::Request off =
        request(didSignal(QStringLiteral("1E1C"), field(0, 16)),
                {{QStringLiteral("rx"), QStringLiteral("7E8")},
                 {QStringLiteral("tx"), QStringLiteral("7E3")}});
    why.clear();
    EXPECT_TRUE(
        ReadOnce::uriFor(QStringLiteral("elm327://COM6"), off, &why).isEmpty());
    EXPECT_TRUE(
        why.startsWith(QStringLiteral("Read once asks for a DID only where")))
        << why.toStdString();
}

/* Each key a draft gets wrong is named; nothing reaches a session */
TEST(ReadOnceTest, TheDraftsMistakesAreNamed)
{
    struct Case {
        const char* what;
        QVariantMap signal;
        QVariantMap module;
        const char* starts;
    };
    const QVariantMap f16 = field(0, 16);
    const QString d = QStringLiteral("1E1C");
    const QVariantMap rx7b8{{QStringLiteral("rx"), QStringLiteral("7B8")}};
    const QVariantMap mixed{{QStringLiteral("rx"), QStringLiteral("18DAF110")},
                            {QStringLiteral("tx"), QStringLiteral("7E0")}};
    QVariantMap noName = didSignal(d, f16);
    noName.insert(QStringLiteral("name"), QStringLiteral("  "));
    QVariantMap longName = didSignal(d, f16);
    longName.insert(QStringLiteral("name"), QString(32, QLatin1Char('n')));
    QVariantMap service = didSignal(d, f16);
    service.insert(
        QStringLiteral("read"),
        QVariantMap{{QStringLiteral("service"), QStringLiteral("23")},
                    {QStringLiteral("number"), d}});
    QVariantMap noByte = didSignal(d, f16);
    noByte.insert(QStringLiteral("field"),
                  QVariantMap{{QStringLiteral("bits"), 8}});
    const std::vector<Case> cases = {
        {"no reply ID", didSignal(d, f16), {}, "The module needs its reply ID"},
        {"a reply ID not hex",
         didSignal(d, f16),
         {{QStringLiteral("rx"), QStringLiteral("7G8")}},
         "The module needs its reply ID"},
        {"no request ID off the rule", didSignal(d, f16), rx7b8,
         "The module needs its request ID"},
        {"two widths", didSignal(d, f16), mixed,
         "The module's two IDs need the same width"},
        {"another service", service, engineEcu(), "The service is 01"},
        {"a DID of three digits", didSignal(QStringLiteral("E1C"), f16),
         engineEcu(), "A DID is four hex digits"},
        {"a PID of three digits", pidSignal(QStringLiteral("00C"), f16),
         engineEcu(), "A PID is two hex digits"},
        {"a blank name", noName, engineEcu(), "A signal needs a name"},
        {"a name of 32 bytes", longName, engineEcu(),
         "The signal's name is 32 bytes; it takes 1 to 31"},
        {"no first byte", noByte, engineEcu(),
         "The signal's field needs \"byte\""},
        {"no bits", didSignal(d, field(0, 0)), engineEcu(),
         "The signal's bits takes a whole number from 1 to 32"},
        {"33 bits", didSignal(d, field(0, 33)), engineEcu(),
         "The signal's bits takes a whole number from 1 to 32"},
        {"half a bit",
         didSignal(d, field(0, 8,
                            {{QStringLiteral("bit"), 0.5},
                             {QStringLiteral("order"), QStringLiteral("le")}})),
         engineEcu(), "The signal's bit takes a whole number from 0 to 7"},
        {"bit 8",
         didSignal(d, field(0, 1,
                            {{QStringLiteral("bit"), 8},
                             {QStringLiteral("order"), QStringLiteral("le")}})),
         engineEcu(), "The signal's bit takes a whole number from 0 to 7"},
        {"a big-endian 12 bits", didSignal(d, field(0, 12)), engineEcu(),
         "The signal's field is big-endian, so whole bytes"},
        {"another order",
         didSignal(
             d, field(0, 8, {{QStringLiteral("order"), QStringLiteral("me")}})),
         engineEcu(), "The signal's order 'me' isn't \"be\" or \"le\""},
        {"scale 0", didSignal(d, f16, {{QStringLiteral("scale"), 0}}),
         engineEcu(), "The signal's scale is 0"},
        {"a unit of 16 bytes",
         didSignal(d, f16,
                   {{QStringLiteral("unit"), QString(16, QLatin1Char('u'))}}),
         engineEcu(), "The signal's unit is 16 bytes; it takes 0 to 15"},
        {"4 decimals", didSignal(d, f16, {{QStringLiteral("decimals"), 4}}),
         engineEcu(), "The signal's decimals takes a whole number from 0 to 3"},
        {"a computed signal",
         didSignal(d, f16,
                   {{QStringLiteral("kind"), QStringLiteral("computed")}}),
         engineEcu(), "Read once reads a number"},
        {"a signed enum",
         didSignal(d, field(0, 8, {{QStringLiteral("signed"), true}}),
                   {{QStringLiteral("kind"), QStringLiteral("enum")}}),
         engineEcu(), "Unknown key 'signed' in the signal's field"},
        {"a formula's name",
         didSignal(d, f16,
                   {{QStringLiteral("name"), QStringLiteral("=SUM(A1)")}}),
         engineEcu(),
         "The signal's name '=SUM(A1)' starts as a spreadsheet formula"},
    };
    for (const Case& c : cases) {
        ReadOnce::Request r;
        QString why;
        EXPECT_FALSE(ReadOnce::requestFrom(c.signal, c.module, &r, &why))
            << c.what;
        EXPECT_TRUE(why.startsWith(QString::fromUtf8(c.starts)))
            << c.what << ": " << why.toStdString();
    }
}

/* Core decodes the reply with the draft's def: each kind, each order */
TEST(ReadOnceTest, AReplyDecodesThroughTheDraft)
{
    const ReadOnce::Request kpa =
        request(didSignal(QStringLiteral("1E1C"), field(0, 16),
                          {{QStringLiteral("scale"), 0.1},
                           {QStringLiteral("unit"), QStringLiteral("kPa")},
                           {QStringLiteral("decimals"), 1}}));
    QVariantMap out = evaluateBytes(kpa, {0x62, 0x1E, 0x1C, 0x12, 0x34});
    EXPECT_TRUE(out.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(out.value(QStringLiteral("request")).toString().toStdString(),
              "22 1E 1C");
    EXPECT_EQ(out.value(QStringLiteral("reply")).toString().toStdString(),
              "62 1E 1C 12 34");
    EXPECT_EQ(out.value(QStringLiteral("value")).toString().toStdString(),
              "466.0 kPa");
    EXPECT_DOUBLE_EQ(out.value(QStringLiteral("number")).toDouble(), 466.0);

    const ReadOnce::Request map =
        request(pidSignal(QStringLiteral("0B"), field(0, 8),
                          {{QStringLiteral("unit"), QStringLiteral("kPa")}}));
    out = evaluateBytes(map, {0x41, 0x0B, 0x23});
    EXPECT_EQ(out.value(QStringLiteral("request")).toString().toStdString(),
              "01 0B");
    EXPECT_EQ(out.value(QStringLiteral("value")).toString().toStdString(),
              "35 kPa");

    const ReadOnce::Request flag = request(
        didSignal(QStringLiteral("1E1C"),
                  field(1, 1,
                        {{QStringLiteral("bit"), 3},
                         {QStringLiteral("order"), QStringLiteral("le")}})));
    EXPECT_EQ(evaluateBytes(flag, {0x62, 0x1E, 0x1C, 0x00, 0x08})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "1");
    EXPECT_EQ(evaluateBytes(flag, {0x62, 0x1E, 0x1C, 0xFF, 0xF7})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "0");

    const ReadOnce::Request little = request(didSignal(
        QStringLiteral("1E1C"),
        field(0, 16, {{QStringLiteral("order"), QStringLiteral("le")}})));
    EXPECT_EQ(evaluateBytes(little, {0x62, 0x1E, 0x1C, 0x34, 0x12})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "4660");

    const ReadOnce::Request trim =
        request(didSignal(QStringLiteral("1E1C"),
                          field(0, 16, {{QStringLiteral("signed"), true}}),
                          {{QStringLiteral("offset"), 10}}));
    EXPECT_EQ(evaluateBytes(trim, {0x62, 0x1E, 0x1C, 0xFF, 0x38})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "-190");

    const ReadOnce::Request text = request(didSignal(
        QStringLiteral("F187"), QVariantMap{{QStringLiteral("byte"), 0}},
        {{QStringLiteral("kind"), QStringLiteral("text")}}));
    EXPECT_EQ(evaluateBytes(text, {0x62, 0xF1, 0x87, 'P', 'N', '-', 0x01})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "PN-.");

    const ReadOnce::Request hex = request(didSignal(
        QStringLiteral("1E1C"),
        QVariantMap{{QStringLiteral("byte"), 1}, {QStringLiteral("bytes"), 1}},
        {{QStringLiteral("kind"), QStringLiteral("hex")}}));
    EXPECT_EQ(evaluateBytes(hex, {0x62, 0x1E, 0x1C, 0x12, 0x34, 0x56})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "34");

    // A raw value with no label shows as its number
    const ReadOnce::Request gear = request(didSignal(
        QStringLiteral("D101"), field(0, 8),
        {{QStringLiteral("kind"), QStringLiteral("enum")},
         {QStringLiteral("values"),
          QVariantMap{{QStringLiteral("3"), QStringLiteral("Drive")}}}}));
    EXPECT_EQ(evaluateBytes(gear, {0x62, 0xD1, 0x01, 0x03})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "Drive (3)");
    EXPECT_EQ(evaluateBytes(gear, {0x62, 0xD1, 0x01, 0x07})
                  .value(QStringLiteral("value"))
                  .toString()
                  .toStdString(),
              "7");
}

TEST(ReadOnceTest, ARefusalSaysItsCodesName)
{
    const ReadOnce::Request r =
        request(didSignal(QStringLiteral("1E1C"), field(0, 16)));
    QVariantMap out = evaluateBytes(r, {0x7F, 0x22, 0x31});
    EXPECT_FALSE(out.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(out.value(QStringLiteral("nrc")).toInt(), 0x31);
    EXPECT_EQ(out.value(QStringLiteral("reply")).toString().toStdString(),
              "7F 22 31");
    EXPECT_EQ(out.value(QStringLiteral("words")).toString().toStdString(),
              "Refused: requestOutOfRange (7F 22 31)");
    EXPECT_EQ(evaluateBytes(r, {0x7F, 0x22, 0x33})
                  .value(QStringLiteral("words"))
                  .toString()
                  .toStdString(),
              "Refused: securityAccessDenied (7F 22 33)");
    EXPECT_EQ(evaluateBytes(r, {0x7F, 0x22, 0x99})
                  .value(QStringLiteral("words"))
                  .toString()
                  .toStdString(),
              "Refused: 7F 22 99");
}

TEST(ReadOnceTest, AFieldPastTheReplySaysSo)
{
    const ReadOnce::Request r =
        request(didSignal(QStringLiteral("1E1C"), field(1, 16)));
    const QVariantMap out = evaluateBytes(r, {0x62, 0x1E, 0x1C, 0x12, 0x34});
    EXPECT_FALSE(out.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(out.value(QStringLiteral("reply")).toString().toStdString(),
              "62 1E 1C 12 34");
    EXPECT_EQ(out.value(QStringLiteral("words")).toString().toStdString(),
              "The field runs past the reply's 2 data bytes");
}

/* Only the module's reply to this request ends the wait; response
 * pending doesn't */
TEST(ReadOnceTest, OnlyItsOwnReplyAnswers)
{
    const ReadOnce::Request did =
        request(didSignal(QStringLiteral("1E1C"), field(0, 16)));
    const auto answers = [](const ReadOnce::Request& r, uint32_t rx,
                            std::initializer_list<uint8_t> bytes) {
        const std::vector<uint8_t> v(bytes);
        return ReadOnce::answers(r, rx, v.data(),
                                 static_cast<uint16_t>(v.size()));
    };
    EXPECT_TRUE(answers(did, 0x7E8, {0x62, 0x1E, 0x1C, 0x12, 0x34}));
    EXPECT_FALSE(answers(did, 0x7E9, {0x62, 0x1E, 0x1C, 0x12, 0x34}));
    EXPECT_FALSE(answers(did, 0x7E8, {0x62, 0x1E, 0x1D, 0x12, 0x34}));
    EXPECT_TRUE(answers(did, 0x7E8, {0x7F, 0x22, 0x31}));
    EXPECT_FALSE(answers(did, 0x7E8, {0x7F, 0x22, 0x78}));
    EXPECT_FALSE(answers(did, 0x7E8, {0x7F, 0x01, 0x12}));
    EXPECT_FALSE(answers(did, 0x7E8, {0x62}));
    const ReadOnce::Request pid =
        request(pidSignal(QStringLiteral("0B"), field(0, 8)));
    EXPECT_TRUE(answers(pid, 0x7E8, {0x41, 0x0B, 0x23}));
    EXPECT_FALSE(answers(pid, 0x7E8, {0x41, 0x0C, 0x1F, 0x40}));
    EXPECT_TRUE(answers(pid, 0x7E8, {0x7F, 0x01, 0x12}));
}

TEST(BridgeTest, TheStaleAgeIsCores)
{
    EXPECT_EQ(SessionBridge::valueStaleMs(),
              static_cast<int>(ECUIO_VALUE_STALE_US / 1000u));
}

TEST(BridgeTest, ReadOnceRefusesWhatItCantAsk)
{
    SessionBridge bridge;
    EXPECT_EQ(
        bridge
            .readOnce(QStringLiteral("mock://demo"),
                      didSignal(QStringLiteral("1E1C"), field(0, 16)),
                      engineEcu())
            .toStdString(),
        "Read once needs an adapter: j2534://, elm327:// or socketcan://");
    EXPECT_EQ(
        bridge
            .readOnce(QStringLiteral("elm327://COM6"),
                      didSignal(QStringLiteral("1E1C"), field(0, 12)),
                      engineEcu())
            .toStdString(),
        "The signal's field is big-endian, so whole bytes: bits a multiple "
        "of 8 and no bit (\"order\": \"le\" for a bit field)");
    EXPECT_FALSE(bridge.readingOnce());
}

#ifdef _WIN32

namespace {

// Read once against the fake DLL's ECU: the result, once it comes
struct ReadOnceRun {
    explicit ReadOnceRun(SessionBridge& b) : bridge(b)
    {
        QObject::connect(&bridge, &SessionBridge::readOnceResult, &context,
                         [this](const QVariantMap& r) {
                             result = r;
                             done = true;
                         });
        QObject::connect(&bridge, &SessionBridge::logMessage, &context,
                         [this](const QString& line) { log << line; });
    }
    bool wait()
    {
        return spinUntil([this]() { return done; }, 15000, 10);
    }

    // The connections end with the run
    QObject context;
    SessionBridge& bridge;
    QVariantMap result;
    bool done = false;
    QStringList log;
};

QString fakeObd2Uri()
{
    return QStringLiteral("j2534://%1?profile=obd2&identify=1")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
}

} // namespace

/* A DID through uds-generic: the fake's 1E1C answers 12 34 */
TEST(BridgeTest, ReadOnceReadsADidFromTheFakeEcu)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    ReadOnceRun run(bridge);
    ASSERT_EQ(bridge
                  .readOnce(fakeObd2Uri(),
                            didSignal(QStringLiteral("1E1C"), field(0, 16)),
                            engineEcu())
                  .toStdString(),
              "");
    EXPECT_TRUE(bridge.readingOnce());
    EXPECT_EQ(bridge
                  .readOnce(fakeObd2Uri(),
                            didSignal(QStringLiteral("1E1C"), field(0, 16)),
                            engineEcu())
                  .toStdString(),
              "A read is running");
    ASSERT_TRUE(run.wait());
    EXPECT_FALSE(bridge.readingOnce());
    EXPECT_TRUE(run.result.value(QStringLiteral("ok")).toBool())
        << run.result.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(
        run.result.value(QStringLiteral("reply")).toString().toStdString(),
        "62 1E 1C 12 34");
    EXPECT_EQ(
        run.result.value(QStringLiteral("value")).toString().toStdString(),
        "4660");
    EXPECT_TRUE(
        run.result.value(QStringLiteral("uri"))
            .toString()
            .contains(QStringLiteral("profile=uds-generic&did=7E8:1E1C:hex")));
    EXPECT_TRUE(
        run.log.contains(QStringLiteral("Read once: 22 1E 1C answered")))
        << run.log.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(bridge.connected());
}

/* A PID through the raw profile's poll: the fake's MAP, 35 kPa */
TEST(BridgeTest, ReadOnceReadsAPidFromTheFakeEcu)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    ReadOnceRun run(bridge);
    ASSERT_EQ(bridge
                  .readOnce(fakeObd2Uri(),
                            pidSignal(QStringLiteral("0B"), field(0, 8),
                                      {{QStringLiteral("unit"),
                                        QStringLiteral("kPa")}}),
                            engineEcu())
                  .toStdString(),
              "");
    ASSERT_TRUE(run.wait());
    EXPECT_EQ(
        run.result.value(QStringLiteral("reply")).toString().toStdString(),
        "41 0B 23");
    EXPECT_EQ(
        run.result.value(QStringLiteral("value")).toString().toStdString(),
        "35 kPa");
}

/* A DID the ECU doesn't know: its refusal, by name; another code from
 * the fake's knob */
TEST(BridgeTest, ReadOnceShowsTheEcusRefusal)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    {
        ReadOnceRun run(bridge);
        ASSERT_EQ(bridge
                      .readOnce(fakeObd2Uri(),
                                didSignal(QStringLiteral("1E1D"), field(0, 8)),
                                engineEcu())
                      .toStdString(),
                  "");
        ASSERT_TRUE(run.wait());
        EXPECT_FALSE(run.result.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(run.result.value(QStringLiteral("nrc")).toInt(), 0x31);
        EXPECT_EQ(
            run.result.value(QStringLiteral("words")).toString().toStdString(),
            "Refused: requestOutOfRange (7F 22 31)");
        EXPECT_TRUE(run.log.contains(QStringLiteral(
            "Read once: 22 1E 1D Refused: requestOutOfRange (7F 22 31)")));
    }
    FakeEnvArm denied("ECUIO_FAKE_UNSUPPORTED_NRC", "0x33");
    ReadOnceRun run(bridge);
    ASSERT_EQ(bridge
                  .readOnce(fakeObd2Uri(),
                            didSignal(QStringLiteral("1E1D"), field(0, 8)),
                            engineEcu())
                  .toStdString(),
              "");
    ASSERT_TRUE(run.wait());
    EXPECT_EQ(
        run.result.value(QStringLiteral("words")).toString().toStdString(),
        "Refused: securityAccessDenied (7F 22 33)");
}

/* An ECU that stays silent: the wait ends it */
TEST(BridgeTest, ReadOnceWithNoReplySaysSo)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");
    SessionBridge bridge;
    bridge.setReadOnceWaitForTesting(500);
    ReadOnceRun run(bridge);
    ASSERT_EQ(bridge
                  .readOnce(fakeObd2Uri(),
                            didSignal(QStringLiteral("1E1D"), field(0, 8)),
                            engineEcu())
                  .toStdString(),
              "");
    ASSERT_TRUE(run.wait());
    EXPECT_FALSE(run.result.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(
        run.result.value(QStringLiteral("reply")).toString().toStdString(), "");
    EXPECT_EQ(
        run.result.value(QStringLiteral("words")).toString().toStdString(),
        "No reply in 0.5 s");
}

/* The adapter is one session's at a time: no Read once while one is
 * connected, no connect while a Read once runs */
TEST(BridgeTest, ReadOnceAndASessionTakeTurns)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    EXPECT_EQ(bridge
                  .readOnce(fakeObd2Uri(),
                            didSignal(QStringLiteral("1E1C"), field(0, 16)),
                            engineEcu())
                  .toStdString(),
              "Read once reads only while no session is connected");
    EXPECT_FALSE(bridge.readingOnce());
    ASSERT_TRUE(disconnectAndWait(bridge));

    ReadOnceRun run(bridge);
    ASSERT_EQ(bridge
                  .readOnce(fakeObd2Uri(),
                            didSignal(QStringLiteral("1E1C"), field(0, 16)),
                            engineEcu())
                  .toStdString(),
              "");
    EXPECT_EQ(bridge.connectSession(fakeUri()).toStdString(),
              "Read once is using the adapter");
    QString asyncErr;
    QObject::connect(
        &bridge, &SessionBridge::connectResult,
        [&asyncErr](const QString&, const QString& e) { asyncErr = e; });
    bridge.connectSessionAsync(fakeUri());
    EXPECT_EQ(asyncErr.toStdString(), "Read once is using the adapter");
    EXPECT_FALSE(bridge.connecting());
    ASSERT_TRUE(run.wait());
    EXPECT_TRUE(run.result.value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty())
        << "the adapter is free again";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

#endif // _WIN32

// ---------------------------------------------------------------------------
// The vehicles' signal files (ADR-0051, app/signalstore.h): each case's
// store in a HAVNE_SETTINGS_DIR of its own.
// ---------------------------------------------------------------------------

namespace {

// HAVNE_SETTINGS_DIR at a scratch folder for one case, put back after
class SettingsDirScope
{
public:
    SettingsDirScope() : m_before(qgetenv("HAVNE_SETTINGS_DIR"))
    {
        qputenv("HAVNE_SETTINGS_DIR", m_dir.path().toUtf8());
    }
    ~SettingsDirScope() { qputenv("HAVNE_SETTINGS_DIR", m_before); }
    SettingsDirScope(const SettingsDirScope&) = delete;
    SettingsDirScope& operator=(const SettingsDirScope&) = delete;

    bool valid() const { return m_dir.isValid(); }
    QString store() const { return m_dir.path() + QStringLiteral("/signals"); }

private:
    QTemporaryDir m_dir;
    QByteArray m_before;
};

// A signal file of one DID on the engine ECU, named name
QByteArray signalFile(const QString& name,
                      const QString& did = QStringLiteral("D100"))
{
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},)"
               R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
               R"({"key":"a","name":"A","read":{"service":"22","number":"%2"},)"
               R"("field":{"byte":0,"bits":8}}]})")
        .arg(name, did)
        .toUtf8();
}

QString sha256Of(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QUrl writeScratch(const QString& folder, const QString& name,
                  const QByteArray& bytes)
{
    QDir().mkpath(folder);
    QFile f(folder + QLatin1Char('/') + name);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    return QUrl::fromLocalFile(f.fileName());
}

bool registered(const char* profile)
{
    const ecuio_signal_set_t* set = nullptr;
    return ecuio_profile_lookup(profile, &set) == ECUIO_OK;
}

QStringList storeFiles(const QString& store)
{
    return QDir(store).entryList(QDir::Files, QDir::Name);
}

} // namespace

/* A file that loads is kept as <sha256>.json, its bytes as given; the
 * same bytes from elsewhere reuse that copy */
TEST(BridgeTest, SignalFileImportKeepsACopyByItsHash)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray bytes = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(bytes);

    QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), sha);
    EXPECT_EQ(r.value(QStringLiteral("name")).toString().toStdString(),
              "Extras");
    EXPECT_EQ(r.value(QStringLiteral("profile")).toString().toStdString(),
              "user.extras");
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 1);
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha + QStringLiteral(".json")});
    QFile copy(dir.store() + QLatin1Char('/') + sha + QStringLiteral(".json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), bytes);
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file: Extras (user.extras, 1 signal)")))
        << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(registered("user.extras")) << "import registers nothing";

    // The same bytes under another name, from a fresh bridge too
    SessionBridge again;
    r = again.importSignalFile(writeScratch(from.path(), "b.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(storeFiles(dir.store()).size(), 1);
}

/* A file says the built-in set it extends and how many of its signals
 * are its own, at import and in use: the page offers one extending obd2
 * in obd2's place (ADR-0052) */
TEST(BridgeTest, SignalFileSaysWhatItExtendsAndHowManyAreItsOwn)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray extending = signalFile(QStringLiteral("Plus"));
    extending.replace(R"("source":{"name":"t"},)",
                      R"("source":{"name":"t"},"extends":"obd2",)");
    const QByteArray alone = signalFile(QStringLiteral("Alone"));
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    const int base = static_cast<int>(obd2->count);

    QVariantMap r = bridge.importSignalFile(
        writeScratch(from.path(), "plus.json", extending));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(),
              "obd2");
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), base + 1);
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 1);
    r = bridge.importSignalFile(writeScratch(from.path(), "alone.json", alone));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(), "");
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 1);

    const QVariantList used =
        bridge.useSignalFiles({sha256Of(extending), sha256Of(alone)});
    ASSERT_EQ(used.size(), 2);
    for (const QVariant& v : used) {
        const QVariantMap e = v.toMap();
        ASSERT_TRUE(e.value(QStringLiteral("ok")).toBool());
        const bool plus =
            e.value(QStringLiteral("sha256")).toString() == sha256Of(extending);
        EXPECT_EQ(e.value(QStringLiteral("extends")).toString().toStdString(),
                  plus ? "obd2" : "");
        EXPECT_EQ(e.value(QStringLiteral("own")).toInt(), 1);
        EXPECT_EQ(e.value(QStringLiteral("count")).toInt(),
                  plus ? base + 1 : 1);
    }
    bridge.useSignalFiles({});
}

/* A refusal is the loader's words, never the path it came from; nothing
 * is kept */
TEST(BridgeTest, SignalFileRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    // On Windows the scratch folder is under the account's home, so its
    // name is in the path too
    const QString folder = from.path() + QStringLiteral("/jdoe-private");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QByteArray bad = signalFile(QStringLiteral("Extras"));
    bad.replace("\"bits\"", "\"bitz\"");
    QVariantMap r =
        bridge.importSignalFile(writeScratch(folder, "extras.json", bad));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.contains(QStringLiteral("bitz"))) << words.toStdString();
    EXPECT_TRUE(words.contains(QStringLiteral("line"))) << words.toStdString();

    r = bridge.importSignalFile(
        QUrl::fromLocalFile(folder + QStringLiteral("/gone.json")));
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't read the file");

    for (const QString& line : log + QStringList{words}) {
        EXPECT_FALSE(line.contains(QStringLiteral("jdoe")))
            << line.toStdString();
        EXPECT_FALSE(line.contains(from.path())) << line.toStdString();
        EXPECT_FALSE(line.contains(QStringLiteral("extras.json")))
            << line.toStdString();
    }
    EXPECT_TRUE(storeFiles(dir.store()).isEmpty());
}

/* use() registers the files it's given and unregisters the rest */
TEST(BridgeTest, SignalFileUseRegistersItsProfile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes))
            .value(QStringLiteral("ok"))
            .toBool());
    QVariantList used = bridge.useSignalFiles({sha});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("user.extras"));
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo?profile=user.extras", &s),
              ECUIO_OK)
        << "a URI can choose it";
    ecuio_session_destroy(s);

    EXPECT_TRUE(bridge.useSignalFiles({}).isEmpty());
    EXPECT_FALSE(registered("user.extras"));
}

/* One profile, one file: two files of one name don't both register */
TEST(BridgeTest, SignalFileOneProfileHasOneFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    const QByteArray b =
        signalFile(QStringLiteral("Extras"), QStringLiteral("D101"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "a.json", a))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", b))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantList used = bridge.useSignalFiles({sha256Of(a), sha256Of(b)});
    ASSERT_EQ(used.size(), 2);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(used[1].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(
        used[1].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "user.extras is made by another file already (Extras)");
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("user.extras", &set), ECUIO_OK);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->defs[0].mux_key, 0x62D100u)
        << "the first file's, never replaced";
}

/* At start-up each copy is read again: a missing, changed or no longer
 * loading copy says so and isn't offered; a good one registers */
TEST(BridgeTest, SignalFileThatNoLongerLoadsAtStartUpSaysSo)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray good = signalFile(QStringLiteral("Extras"));
    QByteArray stale = signalFile(QStringLiteral("Stale"));
    stale.replace("\"havne_signals\":1", "\"havne_signals\":9");
    writeScratch(dir.store(), sha256Of(good) + QStringLiteral(".json"), good);
    writeScratch(dir.store(), sha256Of(stale) + QStringLiteral(".json"), stale);
    const QString changed = sha256Of("other bytes");
    writeScratch(dir.store(), changed + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    const QString missing = QString(64, QLatin1Char('e'));

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QVariantList used = bridge.useSignalFiles(
        {sha256Of(stale), missing, changed, sha256Of(good)});
    ASSERT_EQ(used.size(), 4);
    const QString staleWords =
        used[0].toMap().value(QStringLiteral("words")).toString();
    EXPECT_TRUE(staleWords.startsWith(QStringLiteral("It no longer loads: ")))
        << staleWords.toStdString();
    EXPECT_EQ(
        used[1].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "Its copy is missing: import the file again");
    EXPECT_EQ(
        used[2].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "Its copy has changed: import the file again");
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(used[i].toMap().value(QStringLiteral("ok")).toBool()) << i;
    }
    EXPECT_FALSE(registered("user.stale"));
    EXPECT_TRUE(used[3].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("user.extras"));
    EXPECT_EQ(log.filter(QStringLiteral(" not offered: ")).size(), 3)
        << log.join(QLatin1Char('\n')).toStdString();
}

/* A copy no vehicle keeps goes, its profile first; the folder with the
 * last one */
TEST(BridgeTest, SignalFileIsDeletedOnlyOnceItsProfileIsUnregistered)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    const QByteArray b = signalFile(QStringLiteral("Body"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "a.json", a))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", b))
                    .value(QStringLiteral("ok"))
                    .toBool());
    (void)bridge.useSignalFiles({sha256Of(a), sha256Of(b)});
    ASSERT_TRUE(registered("user.extras"));

    EXPECT_EQ(bridge.deleteUnusedSignalFiles({sha256Of(b)}), 1);
    EXPECT_FALSE(registered("user.extras"));
    EXPECT_TRUE(registered("user.body"));
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha256Of(b) + QStringLiteral(".json")});

    EXPECT_EQ(bridge.deleteUnusedSignalFiles({}), 1);
    EXPECT_FALSE(registered("user.body"));
    EXPECT_FALSE(QDir(dir.store()).exists()) << "the folder with the last copy";
}

/* The form's file goes in as text and comes back as text; a refusal is
 * the loader's, nothing kept */
TEST(BridgeTest, SignalFileTextGoesInAndComesBack)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString text = QString::fromUtf8(signalFile(QStringLiteral("Mine")));
    const QVariantMap r = bridge.importSignalText(text);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();
    EXPECT_EQ(sha, sha256Of(text.toUtf8()));
    EXPECT_EQ(bridge.signalFileText(sha), text);
    EXPECT_TRUE(bridge.signalFileText(QString(64, QLatin1Char('e'))).isEmpty());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file saved: Mine (user.mine, 1 signal)")))
        << log.join(QLatin1Char('\n')).toStdString();
    /* Two signals: the count's plural */
    QString two = QString::fromUtf8(signalFile(QStringLiteral("Pair")));
    two.replace(QStringLiteral("}}]}"),
                QStringLiteral("}},{\"key\":\"b\",\"name\":\"B\",\"read\":"
                               "{\"service\":\"22\",\"number\":\"D101\"},"
                               "\"field\":{\"byte\":0,\"bits\":8}}]}"));
    ASSERT_TRUE(
        bridge.importSignalText(two).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file saved: Pair (user.pair, 2 signals)")))
        << log.join(QLatin1Char('\n')).toStdString();

    QString bad = text;
    bad.replace(QStringLiteral("\"bits\""), QStringLiteral("\"bitz\""));
    const QVariantMap refused = bridge.importSignalText(bad);
    EXPECT_FALSE(refused.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(refused.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("bitz")));
    EXPECT_EQ(storeFiles(dir.store()).size(), 2);
}

/* A recording of a session a vehicle's file decodes holds the store's
 * copy as signals.json, named by the copy's hash on events.log's header;
 * a session no file decodes has none (ADR-0051 section 4) */
TEST(BridgeTest, SignalFileRecordingHoldsTheStoresCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QByteArray bytes = signalFile(QStringLiteral("Recorded"));
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes))
            .value(QStringLiteral("ok"))
            .toBool());
    ASSERT_EQ(bridge.useSignalFiles({sha}).size(), 1);
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);

    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("mock://rec?profile=user.recorded"))
            .isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://plain")).isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 2);
    folders.sort();
    const QString first = root.path() + QStringLiteral("/") + folders[0];
    const QString second = root.path() + QStringLiteral("/") + folders[1];
    QFile copy(first + QStringLiteral("/signals.json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), bytes);
    QFile events(first + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_TRUE(events.readAll().contains(
        ("\nsignal file  signals.json," + sha + "\n").toUtf8()));
    EXPECT_FALSE(QFile::exists(second + QStringLiteral("/signals.json")));
}

namespace {

/* The value Live shows for the signal named name, NaN with no row or no
 * value yet */
double liveValue(SessionBridge& bridge, const QString& name)
{
    SignalListModel* model = bridge.signalModel();
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r, 0);
        if (model->data(i, SignalListModel::NameRole).toString() == name) {
            // ValueRole reads 0 before any value: HasValueRole says whether
            // one came
            if (!model->data(i, SignalListModel::HasValueRole).toBool()) {
                return std::nan("");
            }
            return model->data(i, SignalListModel::ValueRole).toDouble();
        }
    }
    return std::nan("");
}

/* A mock session decoded by the store's file `bytes` (profile
 * user.recorded), recorded under root until signal A has a value; the
 * recording's folder */
QString recordWithSignalFile(SessionBridge& bridge, const QString& root,
                             const QByteArray& bytes)
{
    QTemporaryDir from;
    const QString sha = sha256Of(bytes);
    EXPECT_TRUE(
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes))
            .value(QStringLiteral("ok"))
            .toBool());
    EXPECT_EQ(bridge.useSignalFiles({sha}).size(), 1);
    bridge.setRecordingsRootForTesting(root);
    bridge.setRecording(true);
    EXPECT_TRUE(
        bridge
            .connectSession(QStringLiteral("mock://rec?profile=user.recorded"))
            .isEmpty());
    EXPECT_TRUE(spinUntil(
        [&bridge]() {
            return !std::isnan(liveValue(bridge, QStringLiteral("A")));
        },
        5000));
    EXPECT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);
    const QStringList folders = recordingFolders(root);
    EXPECT_EQ(folders.size(), 1);
    return folders.isEmpty() ? QString()
                             : root + QLatin1Char('/') + folders.first();
}

/* The app's Replay of folder, at max speed: connected, and signal A
 * given a value, or the connect's error */
QString replayFolder(SessionBridge& bridge, const QString& folder,
                     bool expectValue = true)
{
    const QString uri =
        SessionBridge::replayUriFor(
            folder, QStringLiteral("mock://rec?profile=user.recorded")) +
        QStringLiteral("&speed=max");
    const QString err = bridge.connectSession(uri);
    if (err.isEmpty() && expectValue) {
        EXPECT_TRUE(spinUntil(
            [&bridge]() {
                return !std::isnan(liveValue(bridge, QStringLiteral("A")));
            },
            5000))
            << "signal A decoded in the replay";
    }
    return err;
}

} // namespace

/* A replay decodes with the recording's own signals.json (ADR-0051
 * section 4): under the recorded profile while the store registers the
 * same bytes there, and as rec.<16 hex> once the vehicle's file has been
 * removed or changed, so the replay still decodes what was recorded */
TEST(BridgeTest, ReplayDecodesWithTheRecordingsOwnSignalFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    QTemporaryDir from;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString sha1 = sha256Of(v1);
    const QString own = QStringLiteral("rec.") + sha1.left(16);
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());

    /* The same bytes in the store: its profile, as recorded */
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(
        QStringLiteral("?profile=user.recorded&speed=max")))
        << bridge.currentUri().toStdString();
    EXPECT_FALSE(registered(own.toUtf8().constData()));
    ASSERT_TRUE(disconnectAndWait(bridge));

    /* The file changed under the same name (another DID, which the
     * recording's frames don't hold): the copy, under its own name */
    const QByteArray v2 =
        signalFile(QStringLiteral("Recorded"), QStringLiteral("D101"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", v2))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.useSignalFiles({sha256Of(v2)}).size(), 1);
    ASSERT_TRUE(registered("user.recorded"));
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(
        QStringLiteral("?speed=max&profile=") + own))
        << bridge.currentUri().toStdString();
    EXPECT_TRUE(registered(own.toUtf8().constData()));
    EXPECT_TRUE(
        log.contains(QStringLiteral("Replay: decoding with the recording's own "
                                    "signal file, Recorded (") +
                     own + QLatin1Char(')')))
        << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));

    /* The file removed: the copy again, and a replay of the replay holds
     * the copy as its own signals.json */
    EXPECT_GE(bridge.deleteUnusedSignalFiles({}), 1);
    ASSERT_FALSE(registered("user.recorded"));
    QTemporaryDir again;
    ASSERT_TRUE(again.isValid());
    bridge.setRecordingsRootForTesting(again.path());
    bridge.setRecordReplaysForTesting(true);
    bridge.setRecording(true);
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList replayed = recordingFolders(again.path());
    ASSERT_EQ(replayed.size(), 1);
    EXPECT_EQ(readFile(again.path() + QLatin1Char('/') + replayed.first() +
                       QStringLiteral("/signals.json")),
              v1);

    /* Its own name, as the URI the last replay left in the field */
    bridge.setRecording(false);
    const QString err = bridge.connectSession(
        QStringLiteral("replay://") + QDir::fromNativeSeparators(folder) +
        QStringLiteral("?profile=") + own + QStringLiteral("&speed=max"));
    EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A copy that no longer matches the hash its recording names isn't
 * loaded: the replay decodes with the profile as loaded now, and says so */
TEST(BridgeTest, ReplayOfAChangedSignalFileCopyDecodesAsLoaded)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());
    {
        QFile copy(folder + QStringLiteral("/signals.json"));
        ASSERT_TRUE(copy.open(QIODevice::Append));
        copy.write(" ");
    }
    EXPECT_GE(bridge.deleteUnusedSignalFiles({}), 1);
    const QString err = replayFolder(bridge, folder, false);
    EXPECT_FALSE(err.isEmpty()) << "user.recorded is registered by nothing";
    EXPECT_FALSE(
        registered(("rec." + sha256Of(v1).left(16)).toUtf8().constData()));
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Replay: the recording's signals.json doesn't match its hash; "
        "decoding with user.recorded as loaded now")))
        << log.join(QLatin1Char('\n')).toStdString();

    QFile::remove(folder + QStringLiteral("/signals.json"));
    (void)replayFolder(bridge, folder, false);
    EXPECT_TRUE(log.contains(
        QStringLiteral("Replay: the recording's signals.json is missing; "
                       "decoding with user.recorded as loaded now")))
        << log.join(QLatin1Char('\n')).toStdString();
}

// ---- ADR-0057's refs and ranges: the model's rows and profileSignals ----

namespace {

// The rows of the bridge's signal model, each a map of its role names
QVariantList modelRows(SessionBridge& bridge)
{
    SignalListModel* model = bridge.signalModel();
    const QHash<int, QByteArray> roles = model->roleNames();
    QVariantList rows;
    for (int i = 0; i < model->rowCount(); ++i) {
        QVariantMap row;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            row.insert(QString::fromUtf8(it.value()),
                       model->data(model->index(i, 0), it.key()));
        }
        rows.append(row);
    }
    return rows;
}

// The row named name, an empty map if none
QVariantMap rowNamed(const QVariantList& rows, const QString& name)
{
    for (const QVariant& r : rows) {
        if (r.toMap().value(QStringLiteral("name")).toString() == name) {
            return r.toMap();
        }
    }
    return {};
}

QStringList refsOf(const QVariantList& rows)
{
    QStringList refs;
    for (const QVariant& r : rows) {
        refs << r.toMap().value(QStringLiteral("ref")).toString();
    }
    return refs;
}

// Imports bytes into the store and uses it alone; its profile
QString useFile(SessionBridge& bridge, const QString& folder,
                const QByteArray& bytes)
{
    const QVariantMap r =
        bridge.importSignalFile(writeScratch(folder, "f.json", bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(bridge.useSignalFiles({sha256Of(bytes)}).size(), 1);
    return r.value(QStringLiteral("profile")).toString();
}

// A signal file extending obd2: an 8-bit temperature at -40 and one at
// -50 (whose low end differs between the unit systems), an enum, a
// text, and a computed signal over the temperature
QByteArray rangesFile()
{
    return QByteArray(
        R"({"havne_signals":1,"name":"Ranges","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"temp","name":"Temp","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8},"offset":-40,"unit":"\u00b0C"},)"
        R"({"key":"gear","name":"Gear","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"kind":"enum","values":{"0":"Park"}},)"
        R"({"key":"part","name":"Part","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0},"kind":"text"},)"
        R"({"key":"cold","name":"Cold","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":8},"offset":-50,"unit":"\u00b0C"},)"
        R"({"key":"twice","name":"Twice","kind":"computed","expr":"temp * 2"}]})");
}

} // namespace

/* The mock's and obd2's rows are theirs by core's def ID; a row's ref
 * is the same in the model and in profileSignals */
TEST(BridgeTest, ABuiltInSetsRowsAreReferencedByTheirIds)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    const QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QVariant& r : rows) {
        const QVariantMap m = r.toMap();
        EXPECT_EQ(
            m.value(QStringLiteral("ref")).toString(),
            QStringLiteral("id:%1").arg(m.value(QStringLiteral("id")).toInt()))
            << m.value(QStringLiteral("name")).toString().toStdString();
    }
    EXPECT_EQ(
        rowNamed(rows, QStringLiteral("RPM")).value(QStringLiteral("ref")),
        QStringLiteral("id:1001"));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("mock://demo"))),
              refsOf(rows));
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QString obd2 =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_TRUE(bridge.connectSession(obd2).isEmpty());
    const QVariantList obd2Rows = modelRows(bridge);
    EXPECT_TRUE(refsOf(obd2Rows).contains(QStringLiteral("id:7003")));
    for (const QString& ref : refsOf(obd2Rows)) {
        EXPECT_TRUE(ref.startsWith(QStringLiteral("id:"))) << ref.toStdString();
    }
    EXPECT_EQ(refsOf(bridge.profileSignals(obd2)), refsOf(obd2Rows));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("elm327://COM9"))),
              refsOf(obd2Rows))
        << "an adapter's default, obd2";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* uds-generic's rows and a set registered with no signal file (the
 * mock's, copied) have no ref, though they carry IDs */
TEST(BridgeTest, ASetWithNoFileHasNoRefs)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://demo?profile=uds-generic&did=1E1C:u16"))
                    .isEmpty());
    QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));

    // The registry keeps the set's pointer until it's unregistered
    static std::vector<ecuio_signal_def_t> defs;
    static ecuio_signal_set_t copy{};
    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    defs.assign(mock->defs, mock->defs + mock->count);
    copy = *mock;
    copy.defs = defs.data();
    ASSERT_EQ(ecuio_profile_register("copied-mock", &copy, nullptr), ECUIO_OK);
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=copied-mock"))
            .isEmpty());
    rows = modelRows(bridge);
    ASSERT_EQ(rows.size(), static_cast<int>(mock->count));
    EXPECT_EQ(rowNamed(rows, QStringLiteral("RPM"))
                  .value(QStringLiteral("id"))
                  .toInt(),
              1001);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    const QVariantList copied = bridge.profileSignals(
        QStringLiteral("mock://demo?profile=copied-mock"));
    ASSERT_EQ(copied.size(), static_cast<int>(mock->count));
    for (const QString& ref : refsOf(copied)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(ecuio_profile_unregister("copied-mock"), ECUIO_OK);
}

/* A signal file's own defs are theirs by key, its extends base's by
 * the base's ID; a file extending nothing has keys alone */
TEST(BridgeTest, ASignalFilesRowsAreReferencedByTheirKeys)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray extending = signalFile(QStringLiteral("Extending"));
    extending.replace(R"("source":{"name":"t"},)",
                      R"("source":{"name":"t"},"extends":"obd2",)");
    const QString profile = useFile(bridge, from.path(), extending);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QVariantList rows = modelRows(bridge);
    EXPECT_EQ(rowNamed(rows, QStringLiteral("A")).value(QStringLiteral("ref")),
              QStringLiteral("key:a"));
    const QStringList refs = refsOf(rows);
    EXPECT_TRUE(refs.contains(QStringLiteral("id:7003"))) << "obd2's RPM";
    EXPECT_EQ(refs.filter(QStringLiteral("key:")).size(), 1);
    EXPECT_FALSE(refs.contains(QString()));
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)), refs);

    // A store change under a session leaves its rows' refs; the file's
    // profile is gone from the registry, so profileSignals gives []
    bridge.useSignalFiles({});
    EXPECT_EQ(refsOf(modelRows(bridge)), refs);
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QString alone =
        useFile(bridge, from.path(), signalFile(QStringLiteral("Alone")));
    ASSERT_FALSE(alone.isEmpty());
    EXPECT_EQ(refsOf(bridge.profileSignals(
                  QStringLiteral("mock://demo?profile=") + alone)),
              QStringList{QStringLiteral("key:a")});
    bridge.useSignalFiles({});
}

/* A plugin variant's rows have refs, its file being the plugin
 * store's, in profileSignals and in a session's model alike */
TEST(BridgeTest, APluginVariantsRowsHaveRefs)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_FALSE(bytes.isEmpty());
    ASSERT_TRUE(bridge
                    .importPlugin(
                        writeScratch(from.path(), "p.havne-plugin.json", bytes))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString uri =
        QStringLiteral("mock://demo?profile=plugin.example-sim.base");
    const QVariantList rows = bridge.profileSignals(uri);
    ASSERT_GT(rows.size(), 0);
    const QStringList refs = refsOf(rows);
    EXPECT_FALSE(refs.contains(QString())) << refs.join(' ').toStdString();
    EXPECT_FALSE(refs.filter(QStringLiteral("key:")).isEmpty());
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_EQ(refsOf(modelRows(bridge)), refs);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A def's range follows the unit system in the model and in
 * profileSignals; an enum's is scaled as the decoder scales it, and a
 * text or computed def has none (NaN) */
TEST(BridgeTest, ARowsRangeIsShownInTheUnitSystem)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QString profile = useFile(bridge, from.path(), rangesFile());
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());

    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        const bool metric = units == 0;
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
            ASSERT_FALSE(temp.isEmpty());
            EXPECT_EQ(temp.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("°C") : QStringLiteral("°F"));
            EXPECT_NEAR(temp.value(QStringLiteral("rangeLow")).toDouble(),
                        -40.0, 1e-9);
            EXPECT_NEAR(temp.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 215.0 : 419.0, 1e-9);
            EXPECT_FALSE(temp.value(QStringLiteral("isEnum")).toBool());
            const QVariantMap cold = rowNamed(rows, QStringLiteral("Cold"));
            ASSERT_FALSE(cold.isEmpty());
            EXPECT_NEAR(cold.value(QStringLiteral("rangeLow")).toDouble(),
                        metric ? -50.0 : -58.0, 1e-9);
            EXPECT_NEAR(cold.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 205.0 : 401.0, 1e-9);

            const QVariantMap gear = rowNamed(rows, QStringLiteral("Gear"));
            EXPECT_TRUE(gear.value(QStringLiteral("isEnum")).toBool());
            EXPECT_EQ(gear.value(QStringLiteral("rangeLow")).toDouble(), 0.0);
            EXPECT_EQ(gear.value(QStringLiteral("rangeHigh")).toDouble(),
                      255.0);

            for (const char* none : {"Part", "Twice"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(none));
                ASSERT_FALSE(row.isEmpty()) << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << none;
                EXPECT_FALSE(row.value(QStringLiteral("isEnum")).toBool())
                    << none;
            }
            const QVariantMap speed =
                rowNamed(rows, QStringLiteral("Vehicle Speed"));
            ASSERT_FALSE(speed.isEmpty()) << "obd2's, the base";
            EXPECT_EQ(speed.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("km/h") : QStringLiteral("mph"));
            EXPECT_NEAR(speed.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 255.0 : 255.0 / 1.609344, 1e-3);
        }

        // profileSignals also gives the def's own unit and range, the
        // same in every unit system
        const QVariantList rows = bridge.profileSignals(uri);
        const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
        EXPECT_EQ(temp.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("°C"));
        EXPECT_NEAR(temp.value(QStringLiteral("decodedLow")).toDouble(), -40.0,
                    1e-9);
        EXPECT_NEAR(temp.value(QStringLiteral("decodedHigh")).toDouble(), 215.0,
                    1e-9);
        const QVariantMap speed =
            rowNamed(rows, QStringLiteral("Vehicle Speed"));
        EXPECT_EQ(speed.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("km/h"));
        EXPECT_NEAR(speed.value(QStringLiteral("decodedHigh")).toDouble(),
                    255.0, 1e-9);
        const QVariantMap part = rowNamed(rows, QStringLiteral("Part"));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedLow")).toDouble()));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedHigh")).toDouble()));
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* A range whose end isn't a finite number (a 32-bit field at scale
 * 1e300), whose ends are one number (a byte at offset 1e300, past the
 * double's precision), whose end converts past a double (deg C near
 * DBL_MAX, times 1.8 in deg F), or whose ends meet once converted (a bit
 * at 1.75 km/h and one ulp more, one number in mph) is no range, NaN, in
 * the model and in profileSignals */
TEST(BridgeTest, ARangeOfAnEndPastADoubleIsNoRange)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes(
        R"({"havne_signals":1,"name":"Huge","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"huge","name":"Huge","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":32},"scale":1e300},)"
        R"({"key":"flat","name":"Flat","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"offset":1e300},)"
        R"({"key":"edge","name":"Edge","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0,"bits":32},"scale":3.49e298,"unit":"\u00b0C"},)"
        R"({"key":"meet","name":"Meet","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":1,"order":"le"},"offset":1.75,)"
        R"("scale":2.220446049250313e-16,"unit":"km/h"}]})");
    const QString profile = useFile(bridge, from.path(), bytes);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            for (const char* name : {"Huge", "Flat"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(name));
                ASSERT_FALSE(row.isEmpty()) << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << name;
            }
            // About 1.499e308 in deg C; deg F's 2.698e308 is inf
            const QVariantMap edge = rowNamed(rows, QStringLiteral("Edge"));
            ASSERT_FALSE(edge.isEmpty());
            const double lo = edge.value(QStringLiteral("rangeLow")).toDouble();
            const double hi =
                edge.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(lo, 0.0);
                EXPECT_GT(hi, 1.4e308);
                EXPECT_TRUE(std::isfinite(hi));
            } else {
                EXPECT_TRUE(std::isnan(lo)) << lo;
                EXPECT_TRUE(std::isnan(hi)) << hi;
            }
            // 1.75 and 1.75 + 2^-52 km/h are both 1.0873995864153345 mph
            const QVariantMap meet = rowNamed(rows, QStringLiteral("Meet"));
            ASSERT_FALSE(meet.isEmpty());
            const double mlo =
                meet.value(QStringLiteral("rangeLow")).toDouble();
            const double mhi =
                meet.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(mlo, 1.75);
                EXPECT_EQ(mhi, std::nextafter(1.75, 2.0));
            } else {
                EXPECT_TRUE(std::isnan(mlo)) << mlo;
                EXPECT_TRUE(std::isnan(mhi)) << mhi;
            }
        }
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* profileSignals gives the rows of the profile a URI selects, and []
 * for one with no rows to bind or none registered */
TEST(BridgeTest, ProfileSignalsGivesTheProfilesRows)
{
    SessionBridge bridge;
    const QVariantList mock =
        bridge.profileSignals(QStringLiteral("mock://demo"));
    ASSERT_EQ(mock.size(), static_cast<int>(ecuio_mock_signal_set()->count));
    const QVariantMap rpm = rowNamed(mock, QStringLiteral("RPM"));
    EXPECT_EQ(rpm.value(QStringLiteral("id")).toInt(), 1001);
    EXPECT_EQ(rpm.value(QStringLiteral("ref")).toString(),
              QStringLiteral("id:1001"));
    EXPECT_EQ(rpm.value(QStringLiteral("unit")).toString(),
              QStringLiteral("RPM"));
    EXPECT_FALSE(rpm.value(QStringLiteral("isText")).toBool());
    EXPECT_FALSE(rpm.value(QStringLiteral("isEnum")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("group")).toInt(),
              static_cast<int>(ECUIO_SIGNAL_GROUP_ENGINE));
    EXPECT_EQ(rpm.value(QStringLiteral("decimals")).toInt(), 0);
    EXPECT_NEAR(rpm.value(QStringLiteral("rangeHigh")).toDouble(), 65535 * 0.25,
                1e-9);

    for (const char* none :
         {"mock://demo?profile=uds-generic", "mock://demo?profile=no-such",
          "mock://demo?profile=raw", "replay://somewhere", "bogus://x",
          "mock://demo?profile="}) {
        EXPECT_TRUE(bridge.profileSignals(QString::fromLatin1(none)).isEmpty())
            << none;
    }
}

/* A replay's own signals.json is registered when it connects, so its
 * profile has no rows before and the file's after */
TEST(BridgeTest, ProfileSignalsOfAReplaysOwnFileFollowTheConnect)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString own = QStringLiteral("rec.") + sha256Of(v1).left(16);
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());
    EXPECT_GE(bridge.deleteUnusedSignalFiles({}), 1);
    const QString uri = QStringLiteral("replay://") +
                        QDir::fromNativeSeparators(folder) +
                        QStringLiteral("?profile=") + own;
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty()) << "before the connect";
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(own))
        << bridge.currentUri().toStdString();
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)),
              QStringList{QStringLiteral("key:a")});
    EXPECT_EQ(refsOf(modelRows(bridge)), QStringList{QStringLiteral("key:a")});
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* profileOfUri is core's choice of profile: each URI core takes gives
 * the profile its session reports, and one core refuses for its scheme
 * or its query gives "" */
TEST(BridgeTest, ProfileOfUriIsTheProfileCoreChooses)
{
    const QString fixture =
        QStringLiteral("replay://%1/2026-10-25-engine-start")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    const QStringList taken = {
        QStringLiteral("mock://demo"),
        QStringLiteral("mock://demo?profile=obd2"),
        QStringLiteral("mock://demo?profile=raw&profile=mock"),
        QStringLiteral("mock://demo?profile=uds-generic&did=1E1C:u16"),
        fixture,
        fixture + QStringLiteral("?profile=obd2&speed=max"),
    };
    for (const QString& uri : taken) {
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
        EXPECT_EQ(SessionBridge::profileOfUri(uri).toStdString(),
                  std::string(info.profile))
            << uri.toStdString();
        ecuio_session_destroy(s);
    }

    const QString tooLong =
        QStringLiteral("mock://demo?profile=") +
        QString(ECUIO_PROFILE_NAME_MAX + 1, QLatin1Char('x'));
    const QStringList refused = {
        QStringLiteral("Mock://demo"),
        QStringLiteral("bogus://x"),
        QStringLiteral("mock://demo?profile"),
        QStringLiteral("mock://demo?profile="),
        QStringLiteral("mock://demo?&profile=obd2"),
        tooLong,
    };
    for (const QString& uri : refused) {
        ecuio_session_t* s = nullptr;
        EXPECT_NE(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_destroy(s);
        EXPECT_EQ(SessionBridge::profileOfUri(uri), QString())
            << uri.toStdString();
    }

    // The longest name core takes, and the transports this test can't open
    const QString longest(ECUIO_PROFILE_NAME_MAX, QLatin1Char('x'));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("mock://demo?profile=") + longest),
              longest);
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("elm327://COM9")),
              QStringLiteral("obd2"));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("elm327://COM9?profile=mock")),
              QStringLiteral("mock"));
#ifdef _WIN32
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QStringLiteral("obd2"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QString());
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QStringLiteral("raw"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QString());
#endif
}

// ---- The app's plugins (ADR-0052, PluginStore): the example plugin
// packed, as a phone imports it ----

namespace {

QStringList pluginCopies()
{
    return QDir(PluginStore::root()).entryList(QDir::Files, QDir::Name);
}

} // namespace

/* An import keeps the package's bytes by their hash and registers every
 * variant as plugin.<id>.<variant>; a later bridge loads it from its copy
 * at its first list */
TEST(BridgeTest, PluginImportKeepsACopyAndRegistersEveryVariant)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_FALSE(bytes.isEmpty()) << "the example plugin is readable";
    const QString sha = sha256Of(bytes);
    {
        SessionBridge bridge;
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [&log](const QString& line) { log << line; });
        EXPECT_TRUE(bridge.plugins().isEmpty());
        const QVariantMap r = bridge.importPlugin(
            writeScratch(from.path(), "example-sim.havne-plugin.json", bytes));
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), sha);
        EXPECT_EQ(r.value(QStringLiteral("id")).toString().toStdString(),
                  "example-sim");
        EXPECT_EQ(r.value(QStringLiteral("version")).toString().toStdString(),
                  "1.0.0");
        EXPECT_EQ(
            r.value(QStringLiteral("variants")).toStringList(),
            (QStringList{QStringLiteral("base"), QStringLiteral("my2019")}));
        EXPECT_EQ(r.value(QStringLiteral("profiles")).toStringList(),
                  (QStringList{QStringLiteral("plugin.example-sim.base"),
                               QStringLiteral("plugin.example-sim.my2019")}));
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("replaced")).toString(), QString());
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_TRUE(registered("plugin.example-sim.my2019"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha + QStringLiteral(".havne-plugin.json")});
        QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
                   QStringLiteral(".havne-plugin.json"));
        ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
        EXPECT_EQ(copy.readAll(), bytes);
        EXPECT_TRUE(log.contains(QStringLiteral(
            "Plugin: Example: the simulated car 1.0.0 "
            "(plugin.example-sim.base, plugin.example-sim.my2019)")))
            << log.join(QLatin1Char('\n')).toStdString();
        EXPECT_EQ(bridge.plugins().size(), 1);
    }
    EXPECT_FALSE(registered("plugin.example-sim.base")) << "freed at close";

    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("plugin.example-sim.my2019"));
}

/* A refusal is core's words, never the path it came from; nothing is
 * kept */
TEST(BridgeTest, PluginRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), "broken.havne-plugin.json",
                     packed_plugin::example().replace("\"havne_plugin\": 1",
                                                      "\"havne_plugin\": 2")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.contains(QStringLiteral("line"))) << words.toStdString();
    EXPECT_FALSE(words.contains(QStringLiteral("broken")))
        << words.toStdString();
    EXPECT_FALSE(words.contains(from.path())) << words.toStdString();
    EXPECT_TRUE(pluginCopies().isEmpty());
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* A kept copy is loaded at start only while its bytes are the ones its
 * name hashes: one changed since says so, registers nothing, and goes
 * with Remove */
TEST(BridgeTest, PluginCopyThatChangedSaysSo)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QString sha = sha256Of(bytes);
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
    }
    QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
               QStringLiteral(".havne-plugin.json"));
    ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
    copy.write(QByteArray(bytes).replace("Oil Temp", "Oil Tamp"));
    copy.close();

    SessionBridge bridge;
    const QVariantList all = bridge.plugins();
    ASSERT_EQ(all.size(), 1);
    const QVariantMap e = all.at(0).toMap();
    EXPECT_FALSE(e.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(e.value(QStringLiteral("words")).toString().toStdString(),
              "Its copy has changed: import the plugin again");
    EXPECT_FALSE(registered("plugin.example-sim.base"));
    EXPECT_TRUE(bridge.removePlugin(sha).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(pluginCopies().isEmpty());
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* One version per id: an import of an id loaded replaces it, its copy
 * going with it */
TEST(BridgeTest, PluginOfAnIdLoadedReplacesIt)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QByteArray newer = packed_plugin::example("1.1.0");
    const QVariantMap r =
        bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("replaced")).toString().toStdString(),
              "1.0.0");
    EXPECT_EQ(
        pluginCopies(),
        QStringList{sha256Of(newer) + QStringLiteral(".havne-plugin.json")});
    const QVariantList all = bridge.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all.at(0)
                  .toMap()
                  .value(QStringLiteral("version"))
                  .toString()
                  .toStdString(),
              "1.1.0");
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A replace whose copy can't be kept (a folder where its file goes)
 * is refused with the loaded version still loaded, registered and kept,
 * so a later bridge loads it too */
TEST(BridgeTest, PluginReplaceWhoseCopyCantBeKeptKeepsTheLoadedOne)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QByteArray newer = packed_plugin::example("1.1.0");
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
        ASSERT_TRUE(QDir().mkpath(PluginStore::root() + QLatin1Char('/') +
                                  sha256Of(newer) +
                                  QStringLiteral(".havne-plugin.json")));
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
                  "Couldn't keep a copy of the plugin");
        const QVariantList all = bridge.plugins();
        ASSERT_EQ(all.size(), 1);
        EXPECT_EQ(all.at(0)
                      .toMap()
                      .value(QStringLiteral("version"))
                      .toString()
                      .toStdString(),
                  "1.0.0");
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha256Of(bytes) +
                              QStringLiteral(".havne-plugin.json")});
    }
    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A replace whose new version doesn't load (the store's seam, as a
 * registration refused would) gives way to the version it replaced:
 * loaded again from its copy, registered and kept, so a later bridge
 * loads it too; the new version's copy goes */
TEST(BridgeTest, PluginReplaceThatDoesntLoadLoadsTheReplacedOneAgain)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QByteArray newer = packed_plugin::example("1.1.0");
    const auto onlyVersion = [](const QVariantList& all) {
        return all.size() == 1 &&
                       all.at(0).toMap().value(QStringLiteral("ok")).toBool()
                   ? all.at(0)
                         .toMap()
                         .value(QStringLiteral("version"))
                         .toString()
                         .toStdString()
                   : std::string("not one loaded plugin");
    };
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
        bridge.failPluginLoadForTesting(sha256Of(newer));
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
                  "a load failed for the test");
        EXPECT_EQ(onlyVersion(bridge.plugins()), "1.0.0");
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha256Of(bytes) +
                              QStringLiteral(".havne-plugin.json")});
    }
    SessionBridge again;
    EXPECT_EQ(onlyVersion(again.plugins()), "1.0.0");
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A session on a variant decodes its signals; while it does, the plugin
 * is neither replaced nor removed, and once it has ended both go ahead */
TEST(BridgeTest, PluginInUseIsNeitherReplacedNorRemoved)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_TRUE(bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://plugin?profile=plugin.example-sim.base"))
                    .isEmpty());
    SignalListModel* m = bridge.signalModel();
    QStringList names;
    for (int row = 0; row < m->rowCount(); ++row) {
        names << m->data(m->index(row), SignalListModel::NameRole).toString();
    }
    EXPECT_TRUE(names.contains(QStringLiteral("Oil Temp")))
        << names.join(',').toStdString();
    // The mock answers each of the variant's DIDs with its frame count,
    // so Oil Temp reads a whole raw number less the variant's offset, 40,
    // and moves on frame by frame
    const int oil = static_cast<int>(names.indexOf(QStringLiteral("Oil Temp")));
    ASSERT_GE(oil, 0);
    const auto oilValue = [m, oil]() {
        return m->data(m->index(oil), SignalListModel::ValueRole);
    };
    // ValueRole reads 0 before any value: wait for HasValueRole
    ASSERT_TRUE(spinUntil(
        [m, oil]() {
            return m->data(m->index(oil), SignalListModel::HasValueRole)
                .toBool();
        },
        5000, 10));
    const double first = oilValue().toDouble();
    EXPECT_GE(first, -40.0);
    EXPECT_LE(first, 215.0);
    EXPECT_EQ(first, std::floor(first)) << first;
    EXPECT_TRUE(
        spinUntil([&]() { return oilValue().toDouble() != first; }, 5000, 10))
        << first;

    QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), "b.json", packed_plugin::example("1.1.0")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "A session decodes with Example: the simulated car 1.0.0: "
              "disconnect, then import it again");
    r = bridge.removePlugin(sha256Of(bytes));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(pluginCopies().size(), 1) << "its copy kept";
    EXPECT_TRUE(registered("plugin.example-sim.base"));

    ASSERT_TRUE(disconnectAndWait(bridge));
    r = bridge.removePlugin(sha256Of(bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_FALSE(registered("plugin.example-sim.base"));
    EXPECT_FALSE(QDir(PluginStore::root()).exists())
        << "the empty folder goes too";
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* The VIN proposes variants (ADR-0052 section 5): the example matches
 * ZZZ from 2015 to 2030, its my2019 variant from 2019; a typed VIN or,
 * with none typed, the session's; no VIN, no match */
TEST(BridgeTest, PluginMatchesProposeVariantsByTheVin)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const auto variants = [&bridge](const QString& vin) {
        QStringList out;
        for (const QVariant& v : bridge.pluginMatches(vin)) {
            out << v.toMap().value(QStringLiteral("profile")).toString();
        }
        return out;
    };
    /* Model year 2021: M, on the later cycle (a letter at position 7) */
    EXPECT_EQ(variants(QStringLiteral("ZZZFAKE11M0000001")),
              (QStringList{QStringLiteral("plugin.example-sim.base"),
                           QStringLiteral("plugin.example-sim.my2019")}));
    EXPECT_EQ(variants(QStringLiteral("zzz fake11g0000001")),
              QStringList{QStringLiteral("plugin.example-sim.base")})
        << "2016: before my2019's years; spaces and lower case taken";
    EXPECT_TRUE(variants(QStringLiteral("ZZZFAKE11A0000001")).isEmpty())
        << "2010: before the plugin's years";
    EXPECT_TRUE(variants(QStringLiteral("1FAFAKE11M0000001")).isEmpty())
        << "another maker";
    EXPECT_TRUE(variants(QStringLiteral("ZZZFAKE")).isEmpty()) << "not a VIN";
    EXPECT_TRUE(variants(QString()).isEmpty()) << "no session, no VIN";
    const QVariantMap first =
        bridge.pluginMatches(QStringLiteral("ZZZFAKE11M0000001")).at(0).toMap();
    EXPECT_EQ(first.value(QStringLiteral("name")).toString().toStdString(),
              "Example: the simulated car");
    EXPECT_EQ(first.value(QStringLiteral("variant")).toString().toStdString(),
              "base");
}

/* Read once asks a PID on SocketCAN through a profile that requests: a
 * profile with no channels only listens there and is refused the request
 * keys (BAD_URI before the link is looked at, so the URI is judged with
 * or without vcan0) */
TEST(ReadOnceTest, APidsUriIsOneSocketCanTakes)
{
#if !defined(__linux__) || defined(__ANDROID__)
    GTEST_SKIP() << "socketcan:// is Linux's";
#else
    QString why;
    const ReadOnce::Request pid =
        request(pidSignal(QStringLiteral("0C"), field(0, 16)));
    const QString uri =
        ReadOnce::uriFor(QStringLiteral("socketcan://vcan0"), pid, &why);
    ASSERT_FALSE(uri.isEmpty()) << why.toStdString();
    ecuio_session_t* session = nullptr;
    const ecuio_status_t rc =
        ecuio_session_create(uri.toUtf8().constData(), &session);
    EXPECT_NE(rc, ECUIO_ERR_BAD_URI) << ecuio_last_error_detail();
    if (session != nullptr) {
        ecuio_session_destroy(session);
    }
#endif
}

/* A copy whose bytes changed since it was kept is written again by an
 * import of the right bytes, so the next launch loads it: the words that
 * refused it say to import it again */
TEST(BridgeTest, SignalFileImportRepairsAChangedCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray good = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(good);
    writeScratch(dir.store(), sha + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    QTemporaryDir from;
    {
        SessionBridge bridge;
        const QVariantList before = bridge.useSignalFiles({sha});
        ASSERT_EQ(before.size(), 1);
        EXPECT_EQ(before[0]
                      .toMap()
                      .value(QStringLiteral("words"))
                      .toString()
                      .toStdString(),
                  "Its copy has changed: import the file again");
        const QVariantMap r =
            bridge.importSignalFile(writeScratch(from.path(), "e.json", good));
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool())
            << "a copy that held other bytes is written, not kept";
    }
    SessionBridge later;
    const QVariantList used = later.useSignalFiles({sha});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool())
        << used[0]
               .toMap()
               .value(QStringLiteral("words"))
               .toString()
               .toStdString();
    EXPECT_TRUE(registered("user.extras"));
}

TEST(BridgeTest, PluginImportRepairsAChangedCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(QDir().mkpath(PluginStore::root()));
    {
        QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
                   QStringLiteral(".havne-plugin.json"));
        ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
        copy.write(QByteArray(bytes).replace("Oil Temp", "Oil Tamp"));
    }
    {
        SessionBridge bridge;
        const QVariantList all = bridge.plugins();
        ASSERT_EQ(all.size(), 1);
        EXPECT_FALSE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes));
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    }
    SessionBridge later;
    const QVariantList all = later.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool())
        << all.at(0)
               .toMap()
               .value(QStringLiteral("words"))
               .toString()
               .toStdString();
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A session a plugin variant decodes records the variant's files merged
 * as signals.json, with the plugin's line (ADR-0052 section 4), as
 * havne-cli stream --plugin --record does; the mock answers the variant's
 * DIDs */
TEST(BridgeTest, PluginRecordingHoldsTheVariantsFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://rec?profile=plugin.example-sim.base"))
                    .isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QStringLiteral("/") + folders[0];
    QFile copy(folder + QStringLiteral("/signals.json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly)) << "no signals.json";
    const QByteArray text = copy.readAll();
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.constData(),
                                     static_cast<size_t>(text.size()), &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_NE(ecuio_signal_file_find(file, "oil_temp"), nullptr);
    ecuio_signal_file_free(file);
    QFile events(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray log = events.readAll();
    EXPECT_TRUE(log.contains("\nplugin  example-sim,1.0.0,base\n"))
        << log.left(600).toStdString();
    EXPECT_TRUE(log.contains(
        ("\nsignal file  signals.json," + sha256Of(text) + "\n").toUtf8()));
}

/* The bridge's own refusal of a file it can't read names no path: the
 * folder can hold the account's name */
TEST(BridgeTest, AFileThatCantBeReadIsRefusedWithoutItsPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QUrl missing = QUrl::fromLocalFile(
        from.path() + QStringLiteral("/missing-extras.json"));
    SessionBridge bridge;
    for (const QVariantMap& r :
         {bridge.importSignalFile(missing), bridge.importPlugin(missing)}) {
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        const QString words = r.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), "Couldn't read the file");
        EXPECT_FALSE(words.contains(QStringLiteral("missing")));
    }
}

/* A Save out of the app writes the store's copy byte for byte, so
 * havne-cli --signals and another vehicle read the file the vehicle has;
 * a copy changed since it was kept, a sha that isn't one and a target
 * that can't be written are each refused in words naming no path, and a
 * file already at the target is replaced whole */
TEST(BridgeTest, SignalFileSavedOutIsItsCopyByteForByte)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    // Its own spacing and order, which a writer of its own would change
    const QByteArray bytes =
        signalFile(QStringLiteral("Extras")).replace(",", ",\r\n  ");
    SessionBridge bridge;
    const QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "e.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();

    const QString target = out.path() + QStringLiteral("/extras.json");
    writeScratch(out.path(), "extras.json", QByteArray(5000, 'x'));
    const QVariantMap saved =
        bridge.saveSignalFile(sha, QUrl::fromLocalFile(target));
    EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    QFile f(target);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), bytes) << "the copy, byte for byte, and only it";
    f.close();

    const auto refused = [&](const QString& which, const QUrl& to,
                             const char* expected) {
        const QVariantMap no = bridge.saveSignalFile(which, to);
        EXPECT_FALSE(no.value(QStringLiteral("ok")).toBool()) << expected;
        const QString words = no.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), expected);
        EXPECT_FALSE(words.contains(out.path()) || words.contains(dir.store()));
    };
    refused(QStringLiteral("../signals/x"), QUrl::fromLocalFile(target),
            "No such signal file");
    refused(QString(64, QLatin1Char('a')), QUrl::fromLocalFile(target),
            "Its copy is missing: import the file again");
    refused(sha, QUrl::fromLocalFile(out.path()), "Couldn't write the file");
    writeScratch(dir.store(), sha + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    refused(sha, QUrl::fromLocalFile(out.path() + QStringLiteral("/b.json")),
            "Its copy has changed since it was kept: import the file again");
    EXPECT_FALSE(QFile::exists(out.path() + QStringLiteral("/b.json")));
}

// ---- An OBDb set imported in the app (ADR-0051's appendix of 2026-11-06)

namespace {

QByteArray obdbFixture()
{
    return readFile(QStringLiteral(ECUIO_OBDB_FIXTURE));
}

QUrl obdbUrl()
{
    return QUrl::fromLocalFile(QStringLiteral(ECUIO_OBDB_FIXTURE));
}

// Whether the registered profile reads this DID
bool profileReadsDid(const char* profile, uint32_t did)
{
    const ecuio_signal_set_t* set = nullptr;
    if (ecuio_profile_lookup(profile, &set) != ECUIO_OK || set == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].mux_key == (0x620000u | did)) {
            return true;
        }
    }
    return false;
}

QStringList fieldsOf(const QVariantList& notes, const QString& key)
{
    QStringList out;
    for (const QVariant& v : notes) {
        out << v.toMap().value(key).toString();
    }
    return out;
}

} // namespace

/* The model year picks the commands OBDb gives for some years only, and
 * the notes come sorted: converted counted, each field that differs,
 * each signal left out, the set's own keys */
TEST(ObdbImportTest, TheYearPicksTheCommands)
{
    const QByteArray set = obdbFixture();
    ASSERT_FALSE(set.isEmpty());

    const ObdbImport::Preview none = ObdbImport::read(set, 0, QString());
    ASSERT_TRUE(none.ok) << none.words.toStdString();
    EXPECT_EQ(none.converted, 6);
    EXPECT_EQ(none.changed.size(), 5);
    EXPECT_EQ(none.skipped.size(), 6);
    EXPECT_EQ(fieldsOf(none.file, QStringLiteral("field")),
              (QStringList{QStringLiteral("signalGroups"),
                           QStringLiteral("freq")}));
    EXPECT_FALSE(none.json.contains("D200"));
    EXPECT_FALSE(none.json.contains("D201"));
    const QVariantMap first = none.skipped.value(0).toMap();
    EXPECT_EQ(first.value(QStringLiteral("signal")).toString().toStdString(),
              "EXAMPLE_BOOST_TARGET");
    EXPECT_EQ(first.value(QStringLiteral("line")).toInt(), 46);
    EXPECT_EQ(first.value(QStringLiteral("text")).toString().toStdString(),
              "its command is for some model years only: give the "
              "vehicle's model year");
    const QVariantMap changed = none.changed.value(0).toMap();
    EXPECT_EQ(changed.value(QStringLiteral("key")).toString().toStdString(),
              "example_speed");
    EXPECT_EQ(changed.value(QStringLiteral("signal")).toString().toStdString(),
              "EXAMPLE_SPEED");
    EXPECT_EQ(changed.value(QStringLiteral("field")).toString().toStdString(),
              "max");
    EXPECT_EQ(changed.value(QStringLiteral("line")).toInt(), 10);

    const ObdbImport::Preview y2019 = ObdbImport::read(set, 2019, QString());
    ASSERT_TRUE(y2019.ok);
    EXPECT_EQ(y2019.converted, 7);
    EXPECT_EQ(y2019.skipped.size(), 5);
    EXPECT_TRUE(y2019.json.contains("\"D200\""));
    EXPECT_FALSE(y2019.json.contains("\"D201\""));

    const ObdbImport::Preview y2021 =
        ObdbImport::read(set, 2021, QStringLiteral("Daily driver OBDb"));
    ASSERT_TRUE(y2021.ok);
    EXPECT_EQ(y2021.converted, 7);
    EXPECT_TRUE(y2021.json.contains("\"D201\""));
    EXPECT_FALSE(y2021.json.contains("\"D200\""));
    EXPECT_TRUE(y2021.json.contains("\"name\": \"Daily driver OBDb\""));
    EXPECT_TRUE(y2021.json.contains("\"extends\": \"obd2\""))
        << "the vehicle keeps Service 01's signals";

    const QVariantMap r = ObdbImport::report(
        y2021, QStringLiteral("Daily driver OBDb"), 2021);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("name")).toString().toStdString(),
              "Daily driver OBDb");
    EXPECT_EQ(r.value(QStringLiteral("year")).toInt(), 2021);
    EXPECT_EQ(r.value(QStringLiteral("converted")).toInt(), 7);
    EXPECT_EQ(r.value(QStringLiteral("changed")).toList().size(), 6);
    EXPECT_EQ(r.value(QStringLiteral("skipped")).toList().size(), 5);
    EXPECT_EQ(r.value(QStringLiteral("file")).toList().size(), 2);
}

/* Only the importer's own source block marks a file as a conversion */
TEST(ObdbImportTest, AConversionIsKnownByItsSourceBlock)
{
    const ObdbImport::Preview p = ObdbImport::read(obdbFixture(), 0, QString());
    ASSERT_TRUE(p.ok);
    EXPECT_TRUE(ObdbImport::isConversion(p.json));
    EXPECT_FALSE(ObdbImport::isConversion(signalFile(QStringLiteral("X"))));
    QByteArray byHand = p.json;
    byHand.replace("Converted to Havne's signal-file format", "By hand");
    EXPECT_FALSE(ObdbImport::isConversion(byHand));
    QByteArray otherSource = p.json;
    otherSource.replace("OBDb editors and contributors", "Someone");
    EXPECT_FALSE(ObdbImport::isConversion(otherSource));
    EXPECT_FALSE(ObdbImport::isConversion(QByteArray()));
}

/* What isn't an OBDb set is refused with core's words; more than core
 * reads is refused before core sees it */
TEST(ObdbImportTest, NotASetIsRefused)
{
    ObdbImport::Preview p = ObdbImport::read("{\"ecu\": 1}", 0, QString());
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(
        p.words.startsWith(QStringLiteral("Not an OBDb set Havne can read: ")))
        << p.words.toStdString();
    EXPECT_TRUE(p.json.isEmpty());
    p = ObdbImport::read(QByteArray(static_cast<qsizetype>(ECUIO_OBDB_MAX_BYTES) + 1, ' '), 0,
                         QString());
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.words.toStdString(),
              "The file is over 1 MiB, an OBDb set's most");
    const QVariantMap r = ObdbImport::report(p, QStringLiteral("X"), 0);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(r.contains(QStringLiteral("converted")));
}

/* ADR-0043's year: a typed VIN's, spaces dropped and upper-cased; none
 * for a position 10 that isn't a year code or a text that isn't a VIN */
TEST(ObdbImportTest, AVinGivesItsModelYear)
{
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("1FAFAKE10M0000001"), 2027),
              2021);
    EXPECT_EQ(
        ObdbImport::modelYear(QStringLiteral("1fa fake10m 0000001"), 2027),
        2021);
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("1FAFAKE13Y0000001"), 2027),
              2000)
        << "2030 is still ahead";
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("ZZZFAKE10U0000001"), 2027),
              0);
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("1FAFAKE10M"), 2027), 0);
    EXPECT_EQ(ObdbImport::modelYear(QString(), 2027), 0);
}

/* The bridge's year: the typed VIN's, or with "" the session's */
TEST(BridgeTest, ObdbYearComesFromTheTypedVinOrTheSessions)
{
    SessionBridge bridge;
    EXPECT_EQ(bridge.vinModelYear(QString()), 0);
    EXPECT_EQ(bridge.vinModelYear(QStringLiteral("1FAFAKE10M0000001")), 2021);
    bridge.identifyForTesting(QStringLiteral("1FAFAKE13Y0000001"));
    EXPECT_EQ(bridge.vinModelYear(QString()), 2000);
    EXPECT_EQ(bridge.vinModelYear(QStringLiteral("  ")), 2000);
    EXPECT_EQ(bridge.vinModelYear(QStringLiteral("1FAFAKE10M0000001")), 2021)
        << "a typed VIN is the one asked about";
}

/* Preview converts and holds the file; only keep puts it in the store,
 * once, as any signal file's copy */
TEST(BridgeTest, ObdbPreviewKeepsNothingUntilKept)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    QVariantMap r = bridge.previewObdb(obdbUrl(), 2021,
                                       QStringLiteral("Daily driver OBDb"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("name")).toString().toStdString(),
              "Daily driver OBDb");
    EXPECT_EQ(r.value(QStringLiteral("year")).toInt(), 2021);
    EXPECT_EQ(r.value(QStringLiteral("converted")).toInt(), 7);
    EXPECT_TRUE(storeFiles(dir.store()).isEmpty()) << "nothing kept yet";
    EXPECT_TRUE(log.contains(QStringLiteral(
        "OBDb set read for Daily driver OBDb: 7 converted, 6 fields differ, "
        "5 left out (model year 2021)")))
        << log.join(QLatin1Char('\n')).toStdString();

    r = bridge.keepObdb(QString());
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();
    EXPECT_EQ(r.value(QStringLiteral("profile")).toString().toStdString(),
              "user.daily-driver-obdb");
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(),
              "obd2");
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 7);
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha + QStringLiteral(".json")});
    EXPECT_TRUE(bridge.isObdbImport(sha));
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file from OBDb: Daily driver OBDb "
                       "(user.daily-driver-obdb, %1 signals)")
            .arg(r.value(QStringLiteral("count")).toInt())))
        << log.join(QLatin1Char('\n')).toStdString();

    // Kept once: a second keep, or one after a drop, has nothing to keep
    r = bridge.keepObdb(QString());
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "No OBDb set is waiting: import it again");
    ASSERT_TRUE(
        bridge.previewObdb(obdbUrl(), 2019, QStringLiteral("Other OBDb"))
            .value(QStringLiteral("ok"))
            .toBool());
    bridge.dropObdb();
    EXPECT_FALSE(
        bridge.keepObdb(QString()).value(QStringLiteral("ok")).toBool());
    // A refused preview leaves none pending either
    ASSERT_TRUE(
        bridge.previewObdb(obdbUrl(), 2019, QStringLiteral("Other OBDb"))
            .value(QStringLiteral("ok"))
            .toBool());
    QTemporaryDir from;
    EXPECT_FALSE(bridge
                     .previewObdb(writeScratch(from.path(), "x.json", "[]"), 0,
                                  QStringLiteral("Other OBDb"))
                     .value(QStringLiteral("ok"))
                     .toBool());
    EXPECT_FALSE(
        bridge.keepObdb(QString()).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(storeFiles(dir.store()).size(), 1);
    EXPECT_FALSE(bridge.isObdbImport(QString(64, QLatin1Char('a'))));
}

/* A refusal is core's words or the bridge's, never the path */
TEST(BridgeTest, ObdbRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QString folder = from.path() + QStringLiteral("/jdoe-private");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QVariantMap r = bridge.previewObdb(
        writeScratch(folder, "car.json", "{\"commands\": 3}"), 0,
        QStringLiteral("A OBDb"));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.startsWith(QStringLiteral("Not an OBDb set")))
        << words.toStdString();
    r = bridge.previewObdb(
        QUrl::fromLocalFile(folder + QStringLiteral("/gone.json")), 0,
        QStringLiteral("A OBDb"));
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't read the file");
    ASSERT_EQ(log.size(), 2);
    for (const QString& line : log + QStringList{words}) {
        EXPECT_FALSE(line.contains(QStringLiteral("jdoe")))
            << line.toStdString();
        EXPECT_FALSE(line.contains(QStringLiteral("car.json")))
            << line.toStdString();
    }
    EXPECT_TRUE(storeFiles(dir.store()).isEmpty());
}

/* The set read again for another year replaces the earlier import: the
 * profile's defs are the new year's */
TEST(BridgeTest, ObdbReimportReplacesTheEarlierImport)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const QString name = QStringLiteral("Daily driver OBDb");
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2019, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString earlier =
        bridge.keepObdb(QString()).value(QStringLiteral("sha256")).toString();
    ASSERT_FALSE(earlier.isEmpty());
    ASSERT_EQ(bridge.useSignalFiles({earlier}).size(), 1);
    EXPECT_TRUE(profileReadsDid("user.daily-driver-obdb", 0xD200));
    EXPECT_FALSE(profileReadsDid("user.daily-driver-obdb", 0xD201));

    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2021, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantMap r = bridge.keepObdb(earlier);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString later = r.value(QStringLiteral("sha256")).toString();
    EXPECT_NE(later, earlier);
    // The book swaps them; the store registers the one it's given
    const QVariantList used = bridge.useSignalFiles({later});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(profileReadsDid("user.daily-driver-obdb", 0xD201));
    EXPECT_FALSE(profileReadsDid("user.daily-driver-obdb", 0xD200));
    EXPECT_EQ(bridge.deleteUnusedSignalFiles({later}), 1);
    bridge.useSignalFiles({});
}

/* While a session decodes with the earlier import, keep refuses to
 * replace it and changes nothing; the import waits for the disconnect */
TEST(BridgeTest, ObdbReplaceWhileItsSessionReadsIsRefused)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const QString name = QStringLiteral("Daily driver OBDb");
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2019, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString earlier =
        bridge.keepObdb(QString()).value(QStringLiteral("sha256")).toString();
    ASSERT_EQ(bridge.useSignalFiles({earlier}).size(), 1);

    // A session on another profile doesn't hold it
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://plain")).isEmpty());
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2019, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    QVariantMap r = bridge.keepObdb(earlier);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), earlier)
        << "the same year's bytes again";
    ASSERT_TRUE(disconnectAndWait(bridge));

    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://demo?profile=user.daily-driver-obdb"))
                    .isEmpty());
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2021, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    r = bridge.keepObdb(earlier);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(
        r.value(QStringLiteral("words")).toString().toStdString(),
        "The session reads the earlier import: disconnect, then keep it");
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{earlier + QStringLiteral(".json")})
        << "nothing changed";
    EXPECT_TRUE(profileReadsDid("user.daily-driver-obdb", 0xD200));
    ASSERT_TRUE(disconnectAndWait(bridge));

    r = bridge.keepObdb(earlier);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_NE(r.value(QStringLiteral("sha256")).toString(), earlier);
    EXPECT_EQ(storeFiles(dir.store()).size(), 2);
    bridge.useSignalFiles({});
}

// ---- The Log page's filter and By ID view (framefilter.h) ----

namespace {

// A frame for the model: its ID, flags, time and payload
struct FrameMaker {
    std::unique_ptr<ecuio_frame_t> frame = std::make_unique<ecuio_frame_t>();
    FrameMaker() { *frame = ecuio_frame_t{}; }
    void add(FrameListModel& model, uint32_t id, std::initializer_list<uint8_t> bytes,
             quint64 timeUs, uint32_t flags = 0)
    {
        frame->arbitration_id = id;
        frame->flags = flags;
        frame->timestamp_us = timeUs;
        uint16_t n = 0;
        for (uint8_t b : bytes) {
            frame->data[n++] = b;
        }
        frame->data_length = n;
        model.appendFrame(*frame, n);
    }
};

// Counts rows as the model's signals say, so a view that trusts them
// sees what rowCount says
struct RowMirror {
    int rows = 0;
    explicit RowMirror(QAbstractItemModel& m)
    {
        rows = m.rowCount();
        QObject::connect(&m, &QAbstractItemModel::rowsInserted,
                         [this](const QModelIndex&, int first, int last) {
                             rows += last - first + 1;
                         });
        QObject::connect(&m, &QAbstractItemModel::rowsRemoved,
                         [this](const QModelIndex&, int first, int last) {
                             rows -= last - first + 1;
                         });
        QObject::connect(&m, &QAbstractItemModel::modelReset,
                         [this, &m]() { rows = m.rowCount(); });
    }
};

QStringList idsShown(const FrameListModel& model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row) {
        out << QString::number(
                   model.data(model.index(row, 0), FrameListModel::ArbitrationIdRole)
                       .toUInt(),
                   16)
                   .toUpper();
    }
    return out;
}

bool passes(const QString& ids, const QString& data, uint32_t id,
            std::initializer_list<uint8_t> bytes, uint32_t flags = 0)
{
    FrameFilter f;
    EXPECT_EQ(FrameFilter::parseIds(ids, &f.ids).toStdString(), "");
    EXPECT_EQ(FrameFilter::parseData(data, &f.data, &f.anchored).toStdString(), "");
    std::vector<uint8_t> payload(bytes);
    return f.matches(id, flags, 0, payload.data(),
                     static_cast<int>(payload.size()));
}

} // namespace

/* The ID field: hex IDs and ranges, commas between, "0x" optional;
 * what doesn't parse says which item and why */
TEST(FrameFilterTest, TheIdFieldTakesIdsAndRanges)
{
    std::vector<FrameFilter::IdRange> r;
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral(" 7E8, 0x7e0-7EF ,18DAF110"), &r)
                  .toStdString(),
              "");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].first, 0x7E8u);
    EXPECT_EQ(r[0].last, 0x7E8u);
    EXPECT_EQ(r[1].first, 0x7E0u);
    EXPECT_EQ(r[1].last, 0x7EFu);
    EXPECT_EQ(r[2].first, 0x18DAF110u);
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("  "), &r).toStdString(), "");
    EXPECT_TRUE(r.empty());

    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7E8,,7E9"), &r).toStdString(),
              "an empty item between commas");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7G8"), &r).toStdString(),
              "\"7G8\" isn't a hex ID (up to 1FFFFFFF) or a range like 7E0-7EF");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("20000000"), &r).toStdString(),
              "\"20000000\" isn't a hex ID (up to 1FFFFFFF) or a range like 7E0-7EF");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7EF-7E0"), &r).toStdString(),
              "\"7EF-7E0\" runs backward");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7E0-"), &r).toStdString(),
              "\"7E0-\" isn't a hex ID (up to 1FFFFFFF) or a range like 7E0-7EF");
    EXPECT_TRUE(r.empty()) << "a refusal leaves nothing";
}

/* The data field: hex bytes and ??, anywhere unless ^ anchors them */
TEST(FrameFilterTest, TheDataFieldTakesBytesAndWildcards)
{
    std::vector<int> d;
    bool anchored = true;
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("62 f1 ??"), &d, &anchored)
                  .toStdString(),
              "");
    EXPECT_EQ(d, (std::vector<int>{0x62, 0xF1, -1}));
    EXPECT_FALSE(anchored);
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("^ 620D"), &d, &anchored)
                  .toStdString(),
              "");
    EXPECT_EQ(d, (std::vector<int>{0x62, 0x0D}));
    EXPECT_TRUE(anchored);
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("^"), &d, &anchored).toStdString(),
              "\"^\" needs bytes after it");
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("62 F"), &d, &anchored).toStdString(),
              "\"F\" isn't whole bytes: two hex digits each, or ??");
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("6Z"), &d, &anchored).toStdString(),
              "\"6Z\" isn't a hex byte or ??");
    EXPECT_TRUE(d.empty());
}

/* What passes: the IDs (an ID past 0x7FF only an extended frame's), the
 * bytes anywhere or at byte 0, the direction and the one flag */
TEST(FrameFilterTest, AFrameMatchesEveryFieldInForce)
{
    EXPECT_TRUE(passes(QStringLiteral("7E0-7EF"), QString(), 0x7E8, {0x41}));
    EXPECT_FALSE(passes(QStringLiteral("7E0-7EF"), QString(), 0x7DF, {0x41}));
    EXPECT_TRUE(passes(QStringLiteral("7E8"), QString(), 0x7E8, {0x41},
                       ECUIO_FRAME_FLAG_EXTENDED))
        << "a 29-bit 0x7E8 is still 0x7E8";
    EXPECT_FALSE(passes(QStringLiteral("18DAF110"), QString(), 0x18DAF110, {0x41}))
        << "no 11-bit frame carries a 29-bit ID";
    EXPECT_TRUE(passes(QStringLiteral("18DAF110"), QString(), 0x18DAF110, {0x41},
                       ECUIO_FRAME_FLAG_EXTENDED));

    EXPECT_TRUE(passes(QString(), QStringLiteral("F1 ?? 01"), 0x7E8, {0x62, 0xF1, 0x90, 0x01}));
    EXPECT_FALSE(passes(QString(), QStringLiteral("^F1"), 0x7E8, {0x62, 0xF1}));
    EXPECT_TRUE(passes(QString(), QStringLiteral("^62 F1"), 0x7E8, {0x62, 0xF1}));
    EXPECT_FALSE(passes(QString(), QStringLiteral("62 F1 90"), 0x7E8, {0x62, 0xF1}))
        << "longer than the payload";
    EXPECT_TRUE(passes(QString(), QStringLiteral("??"), 0x7E8, {0x00}));
    EXPECT_FALSE(passes(QString(), QStringLiteral("??"), 0x7E8, {}));

    FrameFilter f;
    f.direction = FrameFilter::Sent;
    const uint8_t b[] = {0x01};
    EXPECT_FALSE(f.matches(0x7E0, 0, 0, b, 1));
    EXPECT_TRUE(f.matches(0x7E0, ECUIO_FRAME_FLAG_TX_ECHO, 0, b, 1));
    f.direction = FrameFilter::Received;
    EXPECT_TRUE(f.matches(0x7E0, 0, 0, b, 1));
    EXPECT_FALSE(f.matches(0x7E0, ECUIO_FRAME_FLAG_TX_ECHO, 0, b, 1));
    f.direction = FrameFilter::AnyDirection;
    const std::pair<int, uint32_t> flags[] = {
        {FrameFilter::Extended, ECUIO_FRAME_FLAG_EXTENDED},
        {FrameFilter::Fd, ECUIO_FRAME_FLAG_FD},
        {FrameFilter::Reassembled, ECUIO_FRAME_FLAG_REASSEMBLED},
        {FrameFilter::Truncated, ECUIO_FRAME_FLAG_TRUNCATED},
        {FrameFilter::Overflow, ECUIO_FRAME_FLAG_OVERFLOW},
        {FrameFilter::Error, ECUIO_FRAME_FLAG_ERROR}};
    for (const auto& [flag, bit] : flags) {
        f.flag = flag;
        EXPECT_TRUE(f.matches(0x7E0, bit, 0, b, 1)) << flag;
        EXPECT_FALSE(f.matches(0x7E0, 0, 0, b, 1)) << flag;
    }
    f.flag = FrameFilter::Standard;
    EXPECT_TRUE(f.matches(0x7E0, 0, 0, b, 1));
    EXPECT_FALSE(f.matches(0x7E0, ECUIO_FRAME_FLAG_EXTENDED, 0, b, 1));
    EXPECT_EQ(f.terms(), 1);
    f.flag = FrameFilter::AnyFlag;
    EXPECT_EQ(f.terms(), 0);
    EXPECT_TRUE(f.matches(0x7E0, ECUIO_FRAME_FLAG_ERROR, 0, b, 1))
        << "no filter";
}

/* An ID under a mask, candump's can_id:can_mask: the ID's bits where
 * the mask has ones. A value or mask past 0x7FF is an extended frame's;
 * "/" is refused with the ":" form named */
TEST(FrameFilterTest, TheIdFieldTakesAMaskAsCandumpWritesIt)
{
    std::vector<FrameFilter::IdRange> r;
    EXPECT_EQ(FrameFilter::parseIds(
                  QStringLiteral("7E0:7F8, 7DF, 0x18DAF100:0x1FFFFF00"), &r)
                  .toStdString(),
              "");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_TRUE(r[0].masked);
    EXPECT_EQ(r[0].first, 0x7E0u);
    EXPECT_EQ(r[0].last, 0x7E0u);
    EXPECT_EQ(r[0].mask, 0x7F8u);
    EXPECT_FALSE(r[1].masked);
    EXPECT_EQ(r[1].first, 0x7DFu);
    EXPECT_EQ(r[2].mask, 0x1FFFFF00u);

    EXPECT_EQ(
        FrameFilter::parseIds(QStringLiteral("7E8, 7E0/7F8"), &r).toStdString(),
        "\"7E0/7F8\": an ID and its mask take \":\", as in 7E0:7F8");
    for (const char* bad :
         {"7E0:", ":7F8", "7E0-7EF:7F8", "7E0:7F8:1", "7E0:20000000"}) {
        EXPECT_EQ(
            FrameFilter::parseIds(QString::fromLatin1(bad), &r).toStdString(),
            "\"" + std::string(bad) +
                "\" isn't a hex ID and mask like 7E0:7F8");
    }
    EXPECT_TRUE(r.empty());

    for (uint32_t id = 0x7E0; id <= 0x7E7; ++id) {
        EXPECT_TRUE(passes(QStringLiteral("7E0:7F8"), QString(), id, {0x01}))
            << id;
    }
    EXPECT_FALSE(passes(QStringLiteral("7E0:7F8"), QString(), 0x7E8, {0x01}));
    EXPECT_FALSE(passes(QStringLiteral("7E0:7F8"), QString(), 0x7D8, {0x01}));
    EXPECT_TRUE(passes(QStringLiteral("7E5:7F8"), QString(), 0x7E2, {0x01}))
        << "the value's bits outside the mask don't count";
    EXPECT_TRUE(passes(QStringLiteral("7E0:7F8"), QString(), 0x7E3, {0x01},
                       ECUIO_FRAME_FLAG_EXTENDED))
        << "a 29-bit 0x7E3 is still 0x7E3";
    EXPECT_TRUE(passes(QStringLiteral("18DAF100:1FFFFF00"), QString(),
                       0x18DAF1E8, {0x01}, ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_FALSE(passes(QStringLiteral("18DAF100:1FFFFF00"), QString(),
                        0x18DAF2E8, {0x01}, ECUIO_FRAME_FLAG_EXTENDED));
    // Past 0x7FF, the value or the mask: an extended frame's alone
    EXPECT_FALSE(
        passes(QStringLiteral("7E0:1FFFFFF8"), QString(), 0x7E0, {0x01}));
    EXPECT_TRUE(passes(QStringLiteral("7E0:1FFFFFF8"), QString(), 0x7E0, {0x01},
                       ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_FALSE(
        passes(QStringLiteral("18DA0000:0"), QString(), 0x7E0, {0x01}));
    EXPECT_TRUE(passes(QStringLiteral("18DA0000:0"), QString(), 0x7E0, {0x01},
                       ECUIO_FRAME_FLAG_EXTENDED))
        << "mask 0 passes every ID";
    EXPECT_TRUE(passes(QStringLiteral("7FF:0"), QString(), 0x123, {0x01}));
}

/* The length field: decimal lengths in bytes and ranges, commas between,
 * against the frame's data length */
TEST(FrameFilterTest, TheLengthFieldTakesCountsAndRanges)
{
    std::vector<FrameFilter::LengthRange> r;
    EXPECT_EQ(FrameFilter::parseLengths(QStringLiteral(" 8, 1-3 ,0, 4096"), &r)
                  .toStdString(),
              "");
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0].first, 8);
    EXPECT_EQ(r[0].last, 8);
    EXPECT_EQ(r[1].first, 1);
    EXPECT_EQ(r[1].last, 3);
    EXPECT_EQ(r[2].last, 0);
    EXPECT_EQ(r[3].first, 4096);
    EXPECT_EQ(FrameFilter::parseLengths(QStringLiteral("  "), &r).toStdString(),
              "");
    EXPECT_TRUE(r.empty());

    EXPECT_EQ(
        FrameFilter::parseLengths(QStringLiteral("8,,9"), &r).toStdString(),
        "an empty item between commas");
    for (const char* bad :
         {"x", "4097", "-3", "3-", "1.5", "+8", "0x8", "8 9"}) {
        EXPECT_EQ(
            FrameFilter::parseLengths(QString::fromLatin1(bad), &r)
                .toStdString(),
            "\"" + std::string(bad) +
                "\" isn't a length in bytes (up to 4096) or a range like 1-7");
    }
    EXPECT_EQ(
        FrameFilter::parseLengths(QStringLiteral("8-1"), &r).toStdString(),
        "\"8-1\" runs backward");
    EXPECT_TRUE(r.empty());

    FrameFilter f;
    ASSERT_EQ(FrameFilter::parseLengths(QStringLiteral("2-3, 8"), &f.lengths)
                  .toStdString(),
              "");
    const uint8_t b[8] = {};
    EXPECT_FALSE(f.matches(0x7E8, 0, 0, b, 1));
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 2));
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 3));
    EXPECT_FALSE(f.matches(0x7E8, 0, 0, b, 4));
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 8));
    EXPECT_FALSE(f.matches(0x7E8, 0, 0, b, 0));
    EXPECT_EQ(f.terms(), 1);
}

/* The time field: one range of the table's seconds, both ends in it,
 * either end left open */
TEST(FrameFilterTest, TheTimeFieldTakesARangeOfSeconds)
{
    FrameFilter::TimeRange r;
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral(" 12.5 - 30 "), &r)
                  .toStdString(),
              "");
    EXPECT_TRUE(r.set);
    EXPECT_EQ(r.first, 12500000u);
    EXPECT_EQ(r.last, 30000000u);
    EXPECT_EQ(
        FrameFilter::parseTimes(QStringLiteral("12.5-"), &r).toStdString(), "");
    EXPECT_EQ(r.first, 12500000u);
    EXPECT_EQ(r.last, UINT64_MAX);
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral("-.25"), &r).toStdString(),
              "");
    EXPECT_EQ(r.first, 0u);
    EXPECT_EQ(r.last, 250000u);
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral("1.000001-2."), &r)
                  .toStdString(),
              "");
    EXPECT_EQ(r.first, 1000001u);
    EXPECT_EQ(r.last, 2000000u);
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral("  "), &r).toStdString(),
              "");
    EXPECT_FALSE(r.set);

    for (const char* bad : {"-", "12", "1-2-3", "a-3", "+1-3", "1e3-", "1,2-3",
                            "1-2,3", "3600000001-"}) {
        EXPECT_EQ(
            FrameFilter::parseTimes(QString::fromLatin1(bad), &r).toStdString(),
            "\"" + std::string(bad) +
                "\" isn't a range of seconds like 12.5-30, 12.5- (from then "
                "on) "
                "or -30 (up to then)");
        EXPECT_FALSE(r.set) << bad;
    }
    EXPECT_EQ(
        FrameFilter::parseTimes(QStringLiteral("30-12.5"), &r).toStdString(),
        "\"30-12.5\" runs backward");
    EXPECT_FALSE(r.set);

    FrameFilter f;
    ASSERT_EQ(FrameFilter::parseTimes(QStringLiteral("12.5-30"), &f.time)
                  .toStdString(),
              "");
    const uint8_t b[] = {0x01};
    EXPECT_FALSE(f.matches(0x7E8, 0, 12499999, b, 1));
    EXPECT_TRUE(f.matches(0x7E8, 0, 12500000, b, 1));
    EXPECT_TRUE(f.matches(0x7E8, 0, 30000000, b, 1));
    EXPECT_FALSE(f.matches(0x7E8, 0, 30000001, b, 1));
    EXPECT_EQ(f.terms(), 1);
    ASSERT_EQ(
        FrameFilter::parseTimes(QStringLiteral("-30"), &f.time).toStdString(),
        "");
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 1));
    ASSERT_EQ(
        FrameFilter::parseTimes(QStringLiteral("12.5-"), &f.time).toStdString(),
        "");
    EXPECT_TRUE(f.matches(0x7E8, 0, UINT64_MAX, b, 1));
    ASSERT_EQ(FrameFilter::parseLengths(QStringLiteral("1"), &f.lengths)
                  .toStdString(),
              "");
    EXPECT_EQ(f.terms(), 2) << "the length and the time, a term each";
}

/* The table shows what the filter passes, in either order, as frames
 * come and the ring wraps; the rows its signals report are rowCount's */
TEST(BridgeTest, FrameFilterShowsWhatItPasses)
{
    FrameListModel model;
    RowMirror mirror(model);
    FrameMaker m;
    quint64 t = 0;
    for (int i = 0; i < 6; ++i) {
        m.add(model, i % 2 == 0 ? 0x7E8 : 0x7E9, {0x41, static_cast<uint8_t>(i)}, ++t);
    }
    QVariantMap words = model.setFilter(QStringLiteral("7E9"), QString(), 0, 0);
    EXPECT_EQ(words.value(QStringLiteral("ids")).toString().toStdString(), "");
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 3);
    EXPECT_EQ(model.heldCount(), 6);
    EXPECT_EQ(mirror.rows, 3);
    EXPECT_EQ(idsShown(model), (QStringList{"7E9", "7E9", "7E9"}));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::PayloadRole).toString(),
              QStringLiteral("41 05"))
        << "newest first";

    // A frame the filter passes is a row; one it doesn't isn't
    m.add(model, 0x7E8, {0x41, 6}, ++t);
    EXPECT_EQ(mirror.rows, 3);
    m.add(model, 0x7E9, {0x41, 7}, ++t);
    EXPECT_EQ(mirror.rows, 4);
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::PayloadRole).toString(),
              QStringLiteral("41 07"));

    // A field that doesn't parse keeps its part in force and says why
    words = model.setFilter(QStringLiteral("7E9,"), QStringLiteral("^41"), 0, 0);
    EXPECT_EQ(words.value(QStringLiteral("ids")).toString().toStdString(),
              "an empty item between commas");
    EXPECT_EQ(model.filterTerms(), 2);
    EXPECT_EQ(model.count(), 4);

    // Oldest first, then no filter: every frame again
    model.setNewestFirst(false);
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::PayloadRole).toString(),
              QStringLiteral("41 01"));
    model.setFilter(QString(), QString(), 0, 0);
    EXPECT_EQ(model.filterTerms(), 0);
    EXPECT_EQ(model.count(), 8);
    EXPECT_EQ(mirror.rows, 8);
}

/* The length and time fields through the model: their answer's words,
 * a field that doesn't parse keeping its part, and the four-argument
 * call leaving both out */
TEST(BridgeTest, FrameFilterTakesALengthAndATime)
{
    FrameListModel model;
    RowMirror mirror(model);
    FrameMaker m;
    // A frame a second for 10 s, its length 1 + i % 3
    for (int i = 0; i < 10; ++i) {
        const quint64 t = static_cast<quint64>(i) * 1000000;
        if (i % 3 == 0) {
            m.add(model, 0x7E8, {0x41}, t);
        } else if (i % 3 == 1) {
            m.add(model, 0x7E8, {0x41, 0x0C}, t);
        } else {
            m.add(model, 0x7E8, {0x41, 0x0C, 0x1A}, t);
        }
    }
    QVariantMap words = model.setFilter(QString(), QString(), 0, 0,
                                        QStringLiteral("2-3"), QString());
    EXPECT_EQ(words.value(QStringLiteral("lengths")).toString().toStdString(),
              "");
    EXPECT_EQ(words.value(QStringLiteral("times")).toString().toStdString(),
              "");
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 6);
    EXPECT_EQ(mirror.rows, 6);

    words = model.setFilter(QString(), QString(), 0, 0, QStringLiteral("2-3"),
                            QStringLiteral("2-5"));
    EXPECT_EQ(model.filterTerms(), 2);
    EXPECT_EQ(model.count(), 3) << "2, 4 and 5 s; 3 s is one byte long";
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::TimestampRole)
                  .toULongLong(),
              5000000u)
        << "both ends in the range";

    // A field that doesn't parse keeps its part in force and says why
    words = model.setFilter(QString(), QString(), 0, 0, QStringLiteral("2-"),
                            QStringLiteral("5"));
    EXPECT_EQ(
        words.value(QStringLiteral("lengths")).toString().toStdString(),
        "\"2-\" isn't a length in bytes (up to 4096) or a range like 1-7");
    EXPECT_EQ(words.value(QStringLiteral("times")).toString().toStdString(),
              "\"5\" isn't a range of seconds like 12.5-30, 12.5- (from then "
              "on) or -30 "
              "(up to then)");
    EXPECT_EQ(words.value(QStringLiteral("ids")).toString().toStdString(), "");
    EXPECT_EQ(model.filterTerms(), 2);
    EXPECT_EQ(model.count(), 3);

    // Only the time: from 7 s on
    model.setFilter(QString(), QString(), 0, 0, QString(),
                    QStringLiteral("7-"));
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 3);
    EXPECT_EQ(mirror.rows, 3);

    // The four-argument call: no length and no time
    model.setFilter(QStringLiteral("7E8"), QString(), 0, 0);
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 10);
    EXPECT_EQ(mirror.rows, 10);
}

/* At the cap, the oldest frame leaves the view only when the view shows
 * it; the selection stays on its frames and lets go at a filter change */
TEST(BridgeTest, FrameFilterHoldsAsTheRingWraps)
{
    FrameListModel model;
    RowMirror mirror(model);
    FrameMaker m;
    quint64 t = 0;
    model.setFilter(QStringLiteral("100"), QString(), 0, 0);
    for (int i = 0; i < FrameListModel::MaxFrames; ++i) {
        m.add(model, i % 10 == 0 ? 0x100 : 0x200, {static_cast<uint8_t>(i)}, ++t);
    }
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10);
    EXPECT_EQ(mirror.rows, model.count());
    model.toggleSelected(0);   // the newest 0x100 frame
    const QString selected = model.selectedText();

    // The oldest frame, a 0x100, leaves; the next ones (0x200) leave the
    // ring unseen
    m.add(model, 0x200, {0xAA}, ++t);
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10 - 1);
    EXPECT_EQ(mirror.rows, model.count());
    for (int i = 0; i < 9; ++i) {
        m.add(model, 0x200, {0xAA}, ++t);
    }
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10 - 1);
    // A 0x100 comes as the next 0x100 leaves
    m.add(model, 0x100, {0xBB}, ++t);
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10 - 1);
    EXPECT_EQ(mirror.rows, model.count());
    EXPECT_EQ(model.heldCount(), FrameListModel::MaxFrames);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_EQ(model.selectedText(), selected) << "the selection kept its frame";
    EXPECT_TRUE(
        model.data(model.index(1, 0), FrameListModel::SelectedRole).toBool());

    model.setFilter(QStringLiteral("100"), QStringLiteral("BB"), 0, 0);
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_EQ(model.count(), 1);
    model.clear();
    EXPECT_EQ(model.count(), 0);
    EXPECT_EQ(model.filterTerms(), 2) << "Clear empties the table, not the filter";
}

/* A row's flags in words, for the table's Flags column */
TEST(BridgeTest, FrameFlagsReadAsWords)
{
    EXPECT_EQ(FrameListModel::flagWords(0).toStdString(), "");
    EXPECT_EQ(FrameListModel::flagWords(ECUIO_FRAME_FLAG_EXTENDED).toStdString(), "")
        << "the Ext column says it";
    EXPECT_EQ(FrameListModel::flagWords(ECUIO_FRAME_FLAG_FD |
                                        ECUIO_FRAME_FLAG_BITRATE_SWITCH |
                                        ECUIO_FRAME_FLAG_REASSEMBLED)
                  .toStdString(),
              "FD BRS reassembled");
    EXPECT_EQ(FrameListModel::flagWords(ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_TRUNCATED |
                                        ECUIO_FRAME_FLAG_OVERFLOW | ECUIO_FRAME_FLAG_ERROR |
                                        ECUIO_FRAME_FLAG_TX_ECHO |
                                        ECUIO_FRAME_FLAG_REMOTE)
                  .toStdString(),
              "FD remote truncated after a loss error sent");
    FrameListModel model;
    FrameMaker m;
    m.add(model, 0x7E8, {0x41}, 1, ECUIO_FRAME_FLAG_REASSEMBLED);
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::FlagsTextRole).toString(),
              QStringLiteral("reassembled"));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::FlagsRole).toUInt(),
              ECUIO_FRAME_FLAG_REASSEMBLED);
}

/* By ID: one row per ID and width over the frames shown, its count, its
 * rate over the last 10 s and its newest payload; rows keep their places */
TEST(BridgeTest, FrameIdsCountRateAndNewestPayload)
{
    FrameListModel frames;
    FrameIdModel ids(&frames);
    RowMirror mirror(ids);
    FrameMaker m;
    // 0x7E8 every 100 ms for 20 s; 0x7E9 every second; a 29-bit 0x7E8
    // once
    for (int i = 0; i < 200; ++i) {
        const quint64 t = static_cast<quint64>(i) * 100000;
        m.add(frames, 0x7E8, {0x41, static_cast<uint8_t>(i)}, t);
        if (i % 10 == 0) {
            m.add(frames, 0x7E9, {0x62}, t);
        }
    }
    m.add(frames, 0x7E8, {0x01}, 19900000, ECUIO_FRAME_FLAG_EXTENDED);
    ids.refresh();
    ASSERT_EQ(ids.count(), 3);
    EXPECT_EQ(mirror.rows, 3);
    auto at = [&](int row, int role) { return ids.data(ids.index(row, 0), role); };
    EXPECT_EQ(at(0, FrameIdModel::ArbIdRole).toUInt(), 0x7E8u);
    EXPECT_FALSE(at(0, FrameIdModel::ExtendedRole).toBool());
    EXPECT_EQ(at(0, FrameIdModel::CountRole).toInt(), 200);
    EXPECT_DOUBLE_EQ(at(0, FrameIdModel::RateRole).toDouble(), 10.0)
        << "the last 10 s, its first edge left out: 100 frames";
    EXPECT_EQ(at(0, FrameIdModel::PayloadRole).toString(), QStringLiteral("41 C7"));
    EXPECT_EQ(at(1, FrameIdModel::ArbIdRole).toUInt(), 0x7E9u);
    EXPECT_DOUBLE_EQ(at(1, FrameIdModel::RateRole).toDouble(), 1.0);
    EXPECT_TRUE(at(2, FrameIdModel::ExtendedRole).toBool()) << "29-bit sorts after";

    // The filter narrows it; a new ID is inserted in its place
    frames.setFilter(QStringLiteral("7E9-7EA"), QString(), 0, 0);
    ids.refresh();
    EXPECT_EQ(ids.count(), 1);
    EXPECT_EQ(mirror.rows, 1);
    int resets = 0;
    QObject::connect(&ids, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    m.add(frames, 0x7EA, {0x01}, 20000000);
    ids.refresh();
    EXPECT_EQ(ids.count(), 2);
    EXPECT_EQ(mirror.rows, 2);
    EXPECT_EQ(resets, 0) << "inserted, not reset";
    EXPECT_EQ(at(1, FrameIdModel::ArbIdRole).toUInt(), 0x7EAu);
    EXPECT_EQ(at(1, FrameIdModel::RateRole).toDouble(), 0.1)
        << "one frame in the 10 s window";

    // One frame alone has no span to rate over
    FrameListModel lone;
    FrameIdModel loneIds(&lone);
    m.add(lone, 0x123, {0x01}, 5);
    loneIds.refresh();
    EXPECT_EQ(loneIds.data(loneIds.index(0, 0), FrameIdModel::RateRole).toDouble(), 0.0);
    frames.clear();
    ids.refresh();
    EXPECT_EQ(ids.count(), 0);
    EXPECT_EQ(mirror.rows, 0);
}

/* By ID marks the newest payload's bytes that differ from the frame
 * before it among those shown; a length change marks every byte past
 * the shorter payload; a lone frame marks none */
TEST(BridgeTest, FrameIdsMarkTheBytesThatChanged)
{
    FrameListModel frames;
    FrameIdModel ids(&frames);
    FrameMaker m;
    auto changed = [&](int row) {
        QList<int> out;
        for (const QVariant& v :
             ids.data(ids.index(row, 0), FrameIdModel::ChangedRole).toList()) {
            out.append(v.toInt());
        }
        return out;
    };
    m.add(frames, 0x100, {0x01, 0x02, 0x03}, 1);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>()) << "one frame: nothing before it";

    int changes = 0;
    QObject::connect(&ids, &QAbstractItemModel::dataChanged,
                     [&changes](const QModelIndex&, const QModelIndex&,
                                const QList<int>& roles) {
                         if (roles.contains(FrameIdModel::ChangedRole)) {
                             ++changes;
                         }
                     });
    m.add(frames, 0x100, {0x01, 0x22, 0x03}, 2);
    m.add(frames, 0x200, {0xAA}, 3);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>{1});
    EXPECT_EQ(changed(1), QList<int>()) << "each ID its own";
    EXPECT_EQ(changes, 1);

    m.add(frames, 0x100, {0x01, 0x22, 0x03}, 4);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>()) << "the same payload again";

    m.add(frames, 0x100, {0x01, 0x22, 0x33, 0x04, 0x05}, 5);
    ids.refresh();
    EXPECT_EQ(changed(0), (QList<int>{2, 3, 4}))
        << "longer: the bytes past the old length";

    m.add(frames, 0x100, {0x01, 0x23}, 6);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>{1})
        << "shorter: its own bytes that differ";

    // The frame before among those shown: one the filter hides doesn't count
    m.add(frames, 0x100, {0xFF, 0x23}, 7);
    m.add(frames, 0x100, {0x01, 0x24}, 8);
    frames.setFilter(QString(), QStringLiteral("^01"), 0, 0);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>{1}) << "from 01 23, past the hidden FF 23";
    EXPECT_EQ(ids.roleNames().value(FrameIdModel::ChangedRole),
              QByteArray("changedBytes"));
}

// A log shorter than the window: each ID's frames less one over its own
// span, so a 10 Hz ID reads 10/s from its second frame on
TEST(BridgeTest, FrameIdsRateAShortLogByItsIntervals)
{
    FrameListModel frames;
    FrameIdModel ids(&frames);
    FrameMaker m;
    auto rate = [&](int row) {
        return ids.data(ids.index(row, 0), FrameIdModel::RateRole).toDouble();
    };
    for (int i = 0; i <= 10; ++i) {
        const quint64 t = 3000000 + static_cast<quint64>(i) * 100000;
        m.add(frames, 0x7E8, {0x41, static_cast<uint8_t>(i)}, t);
        if (i % 10 == 0) {
            m.add(frames, 0x7E9, {0x62}, t);
        }
        if (i == 5) {
            m.add(frames, 0x7EA, {0x01}, t);
            ids.refresh();
            ASSERT_EQ(ids.count(), 3);
            EXPECT_DOUBLE_EQ(rate(0), 10.0)
                << "0.5 s in: 6 frames, 5 intervals";
            EXPECT_DOUBLE_EQ(rate(1), 0.0) << "one frame so far";
        }
    }
    ids.refresh();
    ASSERT_EQ(ids.count(), 3);
    EXPECT_DOUBLE_EQ(rate(0), 10.0) << "1 s in: 11 frames, 10 intervals";
    EXPECT_DOUBLE_EQ(rate(1), 1.0) << "two frames a second apart";
    EXPECT_DOUBLE_EQ(rate(2), 0.0) << "a lone frame has no interval";
}

// ---- Export CSV (ADR-0054's appendix "frames in memory") ----

// The VIN reply delivered whole, a value, another ECU's value, the
// tester's own request, then a text signal's DID
void heldForExport(FrameListModel& model, FrameMaker& m)
{
    m.add(model, 0x7E8, {0x49, 0x02, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
                         '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'},
          1000, ECUIO_FRAME_FLAG_REASSEMBLED);
    m.add(model, 0x7E8, {0x04, 0x41, 0x0C, 0x1A, 0xF8}, 2000);
    m.add(model, 0x7E9, {0x03, 0x41, 0x05, 0x7B}, 3000);
    m.add(model, 0x7E0, {0x02, 0x01, 0x0C}, 4000, ECUIO_FRAME_FLAG_TX_ECHO);
    m.add(model, 0x7E8,
          {0x62, 0xD8, 0x02, 'E', 'C', 'M', '-', 'H', 'a', 'v', 'n', 'e'}, 5000,
          ECUIO_FRAME_FLAG_REASSEMBLED);
}

std::string csvHead(int written, const char* filter, int held, int ident,
                    int text)
{
    char line[512];
    std::snprintf(line, sizeof line,
                  "# Havne %s, the Log page's frames, scrubbed (ADR-0054): %d "
                  "written%s; left out of the %d held: %d frames of "
                  "identification replies, %d of text signals' messages, 0 "
                  "segments of messages that couldn't be reassembled\n"
                  "time_s,id,ext,dir,dlc,flags,data\n",
                  ecuio_version_string(), written, filter, held, ident, text);
    return line;
}

// Core's scrub leaves the identification reply out, a text def of a
// session's set leaves its message out (the set noted, the session long
// gone), and the filter picks among the frames kept
TEST(BridgeTest, FrameCsvLeavesIdentificationOutAndWritesWhatTheFilterShows)
{
    FrameListModel model;
    FrameMaker m;
    heldForExport(model, m);

    FrameListModel::ScrubbedCsv csv = model.scrubbedCsv();
    ASSERT_TRUE(csv.ok) << csv.words.toStdString();
    EXPECT_EQ(csv.bytes.toStdString(),
              csvHead(4, "", 5, 1, 0) +
                  "0.002000,0x7E8,0,rx,5,,04 41 0C 1A F8\n"
                  "0.003000,0x7E9,0,rx,4,,03 41 05 7B\n"
                  "0.004000,0x7E0,0,tx,3,sent,02 01 0C\n"
                  "0.005000,0x7E8,0,rx,12,reassembled,62 D8 02 45 43 4D 2D 48 "
                  "61 76 6E 65\n");
    EXPECT_EQ(csv.written, 4);
    EXPECT_EQ(csv.identification, 1u);

    const std::string json =
        R"({"havne_signals": 1, "name": "Scrub test", )"
        R"("source": {"name": "test_bridge"}, )"
        R"("ecus": [{"id": "pcm", "rx": "7E8"}], )"
        R"("signals": [{"key": "ecu_name", "name": "ECU name", "ecu": "pcm", )"
        R"("read": {"service": "22", "number": "D802"}, )"
        R"("field": {"byte": 0}, "kind": "text"}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    model.noteTextDefs(ecuio_signal_file_set(file));
    model.noteTextDefs(ecuio_signal_file_set(file));
    ecuio_signal_file_free(file); // the copies stay

    model.setFilter(QStringLiteral("7E8"), QString(), 0, 0);
    csv = model.scrubbedCsv();
    ASSERT_TRUE(csv.ok) << csv.words.toStdString();
    EXPECT_EQ(csv.bytes.toStdString(),
              csvHead(1, " (those the filter shows)", 5, 1, 1) +
                  "0.002000,0x7E8,0,rx,5,,04 41 0C 1A F8\n");
    EXPECT_EQ(csv.textValues, 1u);

    // A frame echoing the VIN fails the check: no bytes, the class alone
    FrameListModel echo;
    m.add(echo, 0x7E8, {0x62, 0xD8, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
                        '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'},
          1000, ECUIO_FRAME_FLAG_REASSEMBLED);
    csv = echo.scrubbedCsv();
    EXPECT_FALSE(csv.ok);
    EXPECT_TRUE(csv.bytes.isEmpty());
    EXPECT_EQ(csv.words.toStdString(),
              "The check found a VIN-shaped token in a frame the export would "
              "write: nothing was written");
}

// The bridge writes the file whole, answers with the counts and logs one
// line naming no path; a failed check or a target it can't write leaves
// what was there
TEST(BridgeTest, ExportFramesWritesTheFileAndLogsOneLine)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    QStringList lines;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&](const QString& line) { lines << line; });
    FrameMaker m;
    heldForExport(*bridge.model(), m);

    const QString target = out.path() + QStringLiteral("/frames.csv");
    const QVariantMap r = bridge.exportFrames(QUrl::fromLocalFile(target));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("written")).toInt(), 4);
    EXPECT_EQ(r.value(QStringLiteral("leftOut")).toULongLong(), 1u);
    EXPECT_EQ(r.value(QStringLiteral("unattributed")).toULongLong(), 0u);
    QFile f(target);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();
    EXPECT_TRUE(bytes.startsWith(QByteArray(csvHead(4, "", 5, 1, 0).c_str())));
    EXPECT_FALSE(bytes.contains("FAKE11M")) << "the VIN reply left out";
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].toStdString(),
              "Frames exported: 4 written, 1 left out (identification and text "
              "signals), 0 segments unattributed");

    // A frame echoing the VIN: nothing written, the file as it was
    m.add(*bridge.model(), 0x7E8,
          {0x62, 0xD8, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
           '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'},
          6000, ECUIO_FRAME_FLAG_REASSEMBLED);
    lines.clear();
    const QVariantMap no = bridge.exportFrames(QUrl::fromLocalFile(target));
    EXPECT_FALSE(no.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(no.value(QStringLiteral("written")).toInt(), 0);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), bytes);
    f.close();
    ASSERT_EQ(lines.size(), 1);
    EXPECT_TRUE(lines[0].startsWith(
        QStringLiteral("Frames not exported: The check found")))
        << lines[0].toStdString();

    // A folder isn't a file it can write
    bridge.model()->clear();
    heldForExport(*bridge.model(), m);
    const QVariantMap folder =
        bridge.exportFrames(QUrl::fromLocalFile(out.path()));
    EXPECT_FALSE(folder.value(QStringLiteral("ok")).toBool());
    const QString words = folder.value(QStringLiteral("words")).toString();
    EXPECT_EQ(words.toStdString(), "Couldn't write the file");
    for (const QString& line : lines) {
        EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
    }
}

// ---- The health report, Diagnostics' Save report (ADR-0054's appendix
// "the health report as built") ----------------------------------------

/* The temporary folder core writes the report in: TMP, TEMP and TMPDIR
 * pointed at a folder of the test's own while it lives, so a case can
 * see the report's own folder go */
struct TempDirScope {
    EnvRestore tmp{"TMP"};
    EnvRestore temp{"TEMP"};
    EnvRestore tmpdir{"TMPDIR"};
    QTemporaryDir dir;
    TempDirScope()
    {
        const QByteArray path = QDir::toNativeSeparators(dir.path()).toUtf8();
        qputenv("TMP", path);
        qputenv("TEMP", path);
        qputenv("TMPDIR", path);
    }
    QStringList left() const
    {
        return QDir(dir.path())
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    }
};

TEST(BridgeTest, HealthFormatIsTheFiltersOrTheNames)
{
    const QUrl txt = QUrl::fromLocalFile(QStringLiteral("/x/report.txt"));
    const QUrl json = QUrl::fromLocalFile(QStringLiteral("/x/report.JSON"));
    const QUrl content(QStringLiteral("content://provider/document/1234"));
    EXPECT_EQ(healthFormatFor(txt, 0), ECUIO_HEALTH_TEXT);
    EXPECT_EQ(healthFormatFor(txt, 1), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(json, 0), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(content, 0), ECUIO_HEALTH_TEXT);
    EXPECT_EQ(healthFormatFor(content, 1), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(content, -1), ECUIO_HEALTH_TEXT);
}

/* Each count's noun as the count wants it: one, none and many */
TEST(BridgeTest, HealthReportWordsEachCount)
{
    ecuio_health_report_t one{};
    one.ecus = 1;
    one.codes = 1;
    one.values = 1;
    one.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 1;
    EXPECT_EQ(healthSummary(one).toStdString(),
              "1 ECU, 1 trouble code, 1 key value");
    EXPECT_EQ(healthSavedLine(one).toStdString(),
              "Health report saved: 1 ECU, 1 trouble code, 1 key value (1 "
              "identification item left out)");
    ecuio_health_report_t many{};
    many.ecus = 2;
    many.codes = 0;
    many.values = 14;
    many.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 13;
    many.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES] = 1;
    EXPECT_EQ(healthSummary(many).toStdString(),
              "2 ECUs, 0 trouble codes, 14 key values");
    EXPECT_EQ(healthSavedLine(many).toStdString(),
              "Health report saved: 2 ECUs, 0 trouble codes, 14 key values (13 "
              "identification items left out)");
}

/* A live session's report names the picked vehicle's baseline it's
 * given, by its start and never its folder: read, gone (the report still
 * saved, baselineRead false) and none ("") */
TEST(BridgeTest, HealthReportNamesTheBaselineItIsGiven)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://Daily driver")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    const struct {
        QString folder;
        bool read;
    } cases[] = {
        {QStringLiteral(ECUIO_RECORDING_FIXTURES "/findings"), true},
        {out.path() + QStringLiteral("/vanished-folder"), false},
        {QString(), false},
    };
    int n = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder.toStdString());
        const QString target =
            out.path() + QStringLiteral("/report-%1.json").arg(++n);
        const QVariantMap r =
            bridge.saveHealthReport(QUrl::fromLocalFile(target), 1, c.folder);
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(r.value(QStringLiteral("baselineRead")).toBool(), c.read);
        const QByteArray bytes = readFile(target);
        EXPECT_FALSE(bytes.contains("vanished"));
        EXPECT_FALSE(bytes.contains(ECUIO_RECORDING_FIXTURES));
        const QJsonValue b = QJsonDocument::fromJson(bytes).object().value(
            QStringLiteral("baseline"));
        if (c.folder.isEmpty()) {
            EXPECT_TRUE(b.isNull()) << bytes.toStdString();
            continue;
        }
        EXPECT_EQ(b.toObject().value(QStringLiteral("read")).toBool(), c.read);
        if (c.read) {
            EXPECT_EQ(b.toObject().value(QStringLiteral("started")).toString(),
                      QStringLiteral("2026-11-03T16:00:00Z"));
            EXPECT_FALSE(
                b.toObject().value(QStringLiteral("is_source")).toBool());
        }
    }
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The mock's report written where the user chose: text, then JSON by
 * the filter or the name, over a file that exists; its label never in
 * the file or the lines; core's temporary folder gone each time */
TEST(BridgeTest, HealthReportOfTheMockIsWrittenWhereChosen)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    QStringList lines;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&](const QString& line) { lines << line; });

    TempDirScope temp;
    ASSERT_TRUE(temp.dir.isValid());
    const QString target = out.path() + QStringLiteral("/report.txt");
    const QVariantMap none =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 0);
    EXPECT_FALSE(none.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(none.value(QStringLiteral("words")).toString().toStdString(),
              "No session");
    EXPECT_FALSE(QFile::exists(target));

    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://Daily driver")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));

    {
        QFile old(target);
        ASSERT_TRUE(old.open(QIODevice::WriteOnly));
        old.write("the old report");
    }
    lines.clear();
    const QVariantMap r =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 0);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("ecus")).toInt(), 1);
    const int codes = r.value(QStringLiteral("codes")).toInt();
    EXPECT_GT(codes, 0);
    EXPECT_FALSE(r.value(QStringLiteral("vinDecoded")).toBool());
    const QByteArray text = readFile(target);
    EXPECT_FALSE(text.contains("the old report"));
    EXPECT_TRUE(text.contains("mock://<label>")) << text.toStdString();
    EXPECT_FALSE(text.contains("Daily"));
    EXPECT_FALSE(text.startsWith("{"));
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0],
              QStringLiteral("Health report saved: 1 ECU, %1 trouble codes, "
                             "0 key values (0 identification items left out)")
                  .arg(codes));
    EXPECT_EQ(
        r.value(QStringLiteral("summary")).toString(),
        QStringLiteral("1 ECU, %1 trouble codes, 0 key values").arg(codes));

    // JSON by the dialog's filter, a .txt name notwithstanding, and by the
    // name alone
    const QVariantMap byFilter =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 1);
    ASSERT_TRUE(byFilter.value(QStringLiteral("ok")).toBool());
    const QJsonDocument doc = QJsonDocument::fromJson(readFile(target));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value(QStringLiteral("havne_health")).toInt(), 1);
    EXPECT_EQ(doc.object().value(QStringLiteral("ecus")).toArray().size(), 1);
    const QString named = out.path() + QStringLiteral("/report.json");
    ASSERT_TRUE(bridge.saveHealthReport(QUrl::fromLocalFile(named), 0)
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_TRUE(QJsonDocument::fromJson(readFile(named)).isObject());
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();

    // A folder isn't a file it can write: nothing there, the words plain
    lines.clear();
    const QVariantMap folder =
        bridge.saveHealthReport(QUrl::fromLocalFile(out.path()), 0);
    EXPECT_FALSE(folder.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(folder.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].toStdString(),
              "Health report not saved: Couldn't write the file");
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    for (const QString& line : lines) {
        EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
    }
}

/* Live's rows hold each value as core decoded it, in the def's unit:
 * the display's conversion never reaches the report, which converts for
 * itself; each value aged from the window that brought it */
TEST(BridgeTest, HeldValuesAreCoresInTheDefsUnit)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    model.setSignalSet(&set);
    EXPECT_TRUE(model.heldValues().empty()) << "no value yet";
    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    model.updateValues({{uint16_t(1001), 100.0}});
    spinLoop(300);
    const std::vector<ecuio_health_value_t> held = model.heldValues();
    ASSERT_EQ(held.size(), 1u) << "only the row with a value";
    EXPECT_EQ(held[0].def_id, 1001u);
    EXPECT_EQ(held[0].value, 100.0) << "km/h as decoded, not mph";
    EXPECT_GE(held[0].age_us, 250000u);
    EXPECT_LT(held[0].age_us, 10000000u);
    EXPECT_EQ(held[0].flags, 0u);
    EXPECT_EQ(held[0].reserved, 0u);
}

/* A replay under obd2: Live's ranked values are the report's key values,
 * shown in the app's units (core converts them for the file) */
TEST(BridgeTest, HealthReportCarriesLivesValuesInTheAppsUnits)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const QString uri =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=10")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_EQ(bridge.connectSession(uri), QString());
    ASSERT_TRUE(spinUntil(
        [&]() { return bridge.signalModel()->heldValues().size() >= 5; },
        10000));
    const QString target = out.path() + QStringLiteral("/report.json");
    const QVariantMap r =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 1);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_GT(r.value(QStringLiteral("values")).toInt(), 0);
    const QJsonObject doc = QJsonDocument::fromJson(readFile(target)).object();
    const QJsonArray values = doc.value(QStringLiteral("key_values")).toArray();
    ASSERT_FALSE(values.isEmpty());
    QStringList units;
    for (const QJsonValue& v : values) {
        units << v.toObject().value(QStringLiteral("unit")).toString();
    }
    EXPECT_TRUE(units.contains(QStringLiteral("mph")) ||
                units.contains(QString::fromUtf8("\xC2\xB0"
                                                 "F")))
        << units.join(' ').toStdString();
    EXPECT_FALSE(units.contains(QStringLiteral("km/h")));
}

namespace {

QStringList g_themeWarnings;

void collectWarnings(QtMsgType type, const QMessageLogContext&,
                     const QString& message)
{
    if (type == QtWarningMsg) {
        g_themeWarnings << message;
    }
}

} // namespace

// Each of bg, surface and text, changed, applies the theme once, with no
// window to color
TEST(BridgeTest, WindowThemeAppliesOncePerChangeOfEachColor)
{
    FakeTheme theme;
    WindowTheme follower(&theme, nullptr);
    int applied = 0;
    QObject::connect(&follower, &WindowTheme::applied, &follower,
                     [&] { ++applied; });
    theme.setBg(Qt::red);
    EXPECT_EQ(applied, 1);
    theme.setSurface(Qt::green);
    EXPECT_EQ(applied, 2);
    theme.setText(Qt::blue);
    EXPECT_EQ(applied, 3);
}

// A property the theme lacks, or does not notify, is a warning: a rename
// would otherwise leave the window not following the theme
TEST(BridgeTest, WindowThemeWarnsOfAColorItCannotFollow)
{
    g_themeWarnings.clear();
    const QtMessageHandler previous = qInstallMessageHandler(collectWarnings);
    {
        FakeTheme whole;
        WindowTheme follower(&whole, nullptr);
    }
    EXPECT_TRUE(g_themeWarnings.isEmpty()) << "a whole theme says nothing";
    {
        FakeThemeWithoutSurface partial;
        WindowTheme follower(&partial, nullptr);
        int applied = 0;
        QObject::connect(&follower, &WindowTheme::applied, &follower,
                         [&] { ++applied; });
        partial.setBg(Qt::red);
        EXPECT_EQ(applied, 1) << "the colors it has still follow";
    }
    qInstallMessageHandler(previous);
    ASSERT_EQ(g_themeWarnings.size(), 1);
    EXPECT_TRUE(g_themeWarnings[0].contains(QStringLiteral("'surface'")))
        << g_themeWarnings[0].toStdString();
}
