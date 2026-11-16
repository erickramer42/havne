// File/layer: tests/test_bridge_bluetooth.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest.BlePicker*, BtConnect*, BtRetire*, Relay* (test_bridge)
//
// Cases for the Bluetooth picker, connect rules, socket retirement and the adapter relay.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QEventLoop>
#include <QPointer>
#include <QTcpSocket>

#include <cstdlib>
#include <memory>
#include <vector>

#include "adapterrelay.h"
#include "blepicker.h"
#include "btconnect.h"
#include "btretire.h"
#include "fake_bt_socket.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

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
