// File/layer: tests/test_bridge_readonce.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: ReadOnceTest, BridgeTest.ReadOnce* (test_bridge)
//
// Cases for Read once: the draft, the request, the reply, and a read against the fake ECU.

#include <gtest/gtest.h>

#include <QStringList>

#include <cstdlib>
#include <initializer_list>
#include <vector>

#include "readonce.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES

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
#include "bridge_support.h"

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
