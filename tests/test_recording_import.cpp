// File/layer: tests/test_recording_import.cpp | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: BridgeTest.Import* (test_bridge)
//
// The Recordings page's Import (app/recordingimport.{h,cpp}): a candump log
// or a CSV through its column map made a recording by core's ecuio_import,
// on a thread of its own, in a folder named as a recording is.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "recordingimport.h"
#include "sessionbridge.h"

namespace {

// Runs the GUI thread's events until done() or ms pass; done()'s answer
bool waitUntil(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

QString fixture(const char* name)
{
    return QStringLiteral(ECUIO_IMPORT_FIXTURES "/csv/") + QLatin1String(name);
}

QUrl fileUrl(const QString& path)
{
    return QUrl::fromLocalFile(path);
}

// A file of the test's own, its text as given
QString write(const QTemporaryDir& dir, const char* name,
              const QByteArray& text)
{
    const QString path = dir.filePath(QLatin1String(name));
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(text);
    return path;
}

// Two buses, can0 three frames over 0.5 s and can1 two
const QByteArray kTwoBuses = "\n"
                             "(1700000000.000000) can0 7E0#0201050000000000\n"
                             "(1700000000.010000) can1 123#11\n"
                             "(1700000000.250000) can0 7E8#03410550AAAAAAAA\n"
                             "(1700000000.300000) can1 123#22\n"
                             "(1700000000.500000) can0 7E8#03410551AAAAAAAA\n";

const QByteArray kOneBus = "(1700000000.000000) vcan0 7E8#03410550\n"
                           "(1700000001.500000) vcan0 7E8#03410551\n";

// An import run to its end: its result, and how often busy changed
struct Imported {
    QVariantMap result;
    int busyChanges = 0;
    bool started = false;
};

Imported import(RecordingImport& im, const QUrl& file, int kind,
                const QUrl& map = {}, const QString& iface = {})
{
    Imported out;
    bool done = false;
    QObject context;
    QObject::connect(&im, &RecordingImport::busyChanged, &context,
                     [&out] { ++out.busyChanges; });
    QObject::connect(&im, &RecordingImport::finished, &context,
                     [&](const QVariantMap& r) {
                         out.result = r;
                         done = true;
                     });
    out.started = im.start(file, kind, map, iface);
    if (out.started) {
        EXPECT_TRUE(im.busy());
        EXPECT_TRUE(waitUntil([&] { return done; })) << "the import ends";
        EXPECT_FALSE(im.busy());
    }
    return out;
}

QString words(const Imported& i)
{
    return i.result.value(QStringLiteral("words")).toString();
}

bool ok(const Imported& i)
{
    return i.result.value(QStringLiteral("ok")).toBool();
}

QStringList entries(const QString& folder)
{
    return QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                  QDir::Name);
}

const QRegularExpression
    kStamped(QStringLiteral("^[0-9]{8}-[0-9]{6}(-[0-9]+)? (.+)$"));

} // namespace

/* The kind from the file's first line that isn't blank: candump's
 * "(seconds) interface ID#data" is candump, anything else a CSV */
TEST(BridgeTest, ImportGuessesTheKindFromTheFirstLine)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    EXPECT_EQ(im.guessKind(fileUrl(write(dir, "two.log", kTwoBuses))),
              RecordingImport::Candump);
    EXPECT_EQ(im.guessKind(
                  fileUrl(write(dir, "bare.log", "  (12) can0 18DAF110#\n"))),
              RecordingImport::Candump);
    EXPECT_EQ(im.guessKind(fileUrl(fixture("logger-a/log.csv"))),
              RecordingImport::Csv);
    EXPECT_EQ(
        im.guessKind(fileUrl(write(
            dir, "header.csv", "time,(1.0) can0 7E8#00\n(1.0) can0 7E8#00\n"))),
        RecordingImport::Csv)
        << "the first line decides";
    EXPECT_EQ(im.guessKind(fileUrl(write(dir, "noid.log", "(1.0) can0 #00\n"))),
              RecordingImport::Csv);
    EXPECT_EQ(
        im.guessKind(fileUrl(dir.filePath(QStringLiteral("missing.log")))),
        RecordingImport::Csv);
}

/* A CSV through its column map: a folder named and titled as a recording
 * is, "<stamp> CSV import", holding signals.csv and events.log, the root
 * made if it wasn't there */
TEST(BridgeTest, ImportMakesARecordingOfACsvThroughItsMap)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("not yet/recordings"));
    RecordingImport im([root] { return root; });
    const Imported i =
        import(im, fileUrl(fixture("logger-a/log.csv")), RecordingImport::Csv,
               fileUrl(fixture("logger-a/map.json")));
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    EXPECT_EQ(words(i), QString());
    EXPECT_EQ(i.busyChanges, 2);
    EXPECT_GT(i.result.value(QStringLiteral("values")).toDouble(), 0.0);
    EXPECT_EQ(i.result.value(QStringLiteral("columnsIgnored")).toDouble(), 1.0)
        << "Note";
    EXPECT_EQ(i.result.value(QStringLiteral("frames")).toDouble(), 0.0);
    const QStringList made = entries(root);
    ASSERT_EQ(made.size(), 1);
    const QRegularExpressionMatch m = kStamped.match(made[0]);
    ASSERT_TRUE(m.hasMatch()) << made[0].toStdString();
    EXPECT_EQ(m.captured(2), QStringLiteral("CSV import"));
    EXPECT_EQ(i.result.value(QStringLiteral("folder")).toString(),
              root + QLatin1Char('/') + made[0]);
    EXPECT_EQ(entries(root + QLatin1Char('/') + made[0]),
              (QStringList{QStringLiteral("events.log"),
                           QStringLiteral("signals.csv")}));
}

/* A candump log of one bus takes it; one of several is refused in core's
 * words, nothing made, and the bus named takes its own frames alone */
TEST(BridgeTest, ImportTakesACandumpLogsBus)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    Imported i = import(im, fileUrl(write(dir, "one.log", kOneBus)),
                        RecordingImport::Candump);
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    EXPECT_EQ(i.result.value(QStringLiteral("frames")).toDouble(), 2.0);
    EXPECT_EQ(i.result.value(QStringLiteral("durationUs")).toDouble(),
              1500000.0);
    ASSERT_EQ(entries(root).size(), 1);
    EXPECT_EQ(kStamped.match(entries(root)[0]).captured(2),
              QStringLiteral("candump import"));
    EXPECT_EQ(entries(i.result.value(QStringLiteral("folder")).toString()),
              (QStringList{QStringLiteral("events.log"),
                           QStringLiteral("frames.csv")}));

    const QUrl two = fileUrl(write(dir, "two.log", kTwoBuses));
    i = import(im, two, RecordingImport::Candump);
    EXPECT_FALSE(ok(i));
    EXPECT_EQ(words(i),
              QStringLiteral(
                  "the log has several interfaces: name one of can0 and can1"));
    EXPECT_EQ(entries(root).size(), 1) << "nothing made";

    i = import(im, two, RecordingImport::Candump, {}, QStringLiteral(" can1 "));
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    EXPECT_EQ(i.result.value(QStringLiteral("frames")).toDouble(), 2.0)
        << "can1's alone";
    EXPECT_EQ(entries(root).size(), 2);
}

/* A name taken in this second: the next, "-2", as a recording's would */
TEST(BridgeTest, ImportTakesTheNextNameWhenOneIsTaken)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    // This second's name and the next's, so the import meets one taken
    const QDateTime now = QDateTime::currentDateTime();
    for (int s = 0; s < 3; ++s) {
        const QString stamp =
            now.addSecs(s).toString(QStringLiteral("yyyyMMdd-HHmmss"));
        ASSERT_TRUE(QDir().mkpath(root + QLatin1Char('/') + stamp +
                                  QStringLiteral(" candump import")));
    }
    RecordingImport im([root] { return root; });
    const Imported i = import(im, fileUrl(write(dir, "one.log", kOneBus)),
                              RecordingImport::Candump);
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    const QString name =
        QDir(i.result.value(QStringLiteral("folder")).toString()).dirName();
    EXPECT_TRUE(name.endsWith(QStringLiteral("-2 candump import")))
        << name.toStdString();
}

/* A file the app can't hand core by its path (Android's content://) is
 * copied into a temporary folder, removed once the import is done; one
 * that can't be read says so, never with its path */
TEST(BridgeTest, ImportCopiesAFileItCantReadInPlace)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    im.setCopyLocalFilesForTesting(true);
    {
        // Not TempDirScope: QTemporaryDir reads QDir::tempPath, which on
        // Windows is TMP, and test_bridge.cpp keeps its scope to itself
        QTemporaryDir temp;
        const QByteArray was = qgetenv("TMP");
        const QByteArray wasTemp = qgetenv("TEMP");
        const QByteArray wasTmpdir = qgetenv("TMPDIR");
        qputenv("TMP", QDir::toNativeSeparators(temp.path()).toUtf8());
        qputenv("TEMP", QDir::toNativeSeparators(temp.path()).toUtf8());
        qputenv("TMPDIR", QDir::toNativeSeparators(temp.path()).toUtf8());
        const Imported i = import(im, fileUrl(write(dir, "one.log", kOneBus)),
                                  RecordingImport::Candump);
        const QStringList left = entries(temp.path());
        qputenv("TMP", was);
        qputenv("TEMP", wasTemp);
        qputenv("TMPDIR", wasTmpdir);
        ASSERT_TRUE(ok(i)) << words(i).toStdString();
        EXPECT_EQ(i.result.value(QStringLiteral("frames")).toDouble(), 2.0);
        EXPECT_EQ(left, QStringList()) << "the copy is gone";
    }
    const QString missing = dir.filePath(QStringLiteral("missing.log"));
    const Imported i = import(im, fileUrl(missing), RecordingImport::Candump);
    EXPECT_FALSE(ok(i));
    EXPECT_EQ(words(i), QStringLiteral("the file can't be read"));
    EXPECT_EQ(entries(root).size(), 1);
}

/* What stops an import, each in words with no path in them: no map for
 * a CSV, a map that can't be read or is too large, a map core refuses, a
 * file core can't open, no folder for recordings; and one import at a time */
TEST(BridgeTest, ImportSaysWhatStoppedIt)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    const QUrl csv = fileUrl(fixture("logger-a/log.csv"));

    EXPECT_EQ(words(import(im, csv, RecordingImport::Csv)),
              QStringLiteral("choose the logger's column map first"));
    EXPECT_EQ(words(import(im, csv, RecordingImport::Csv,
                           fileUrl(dir.filePath(QStringLiteral("none.json"))))),
              QStringLiteral("the column map can't be read"));
    const QByteArray big(static_cast<int>(RecordingImport::MapMaxBytes) + 1,
                         ' ');
    EXPECT_EQ(words(import(im, csv, RecordingImport::Csv,
                           fileUrl(write(dir, "big.json", big)))),
              QStringLiteral("the column map is larger than 1 MiB"));
    const QByteArray fits(static_cast<int>(RecordingImport::MapMaxBytes), ' ');
    EXPECT_TRUE(words(import(im, csv, RecordingImport::Csv,
                             fileUrl(write(dir, "fits.json", fits))))
                    .startsWith(QStringLiteral("the column map: ")))
        << "1 MiB reaches core";
    const QString refusal = words(import(im, csv, RecordingImport::Csv,
                                         fileUrl(fixture("unknown-key.json"))));
    EXPECT_TRUE(refusal.startsWith(QStringLiteral("the column map: ")))
        << refusal.toStdString();

    const QString missing = dir.filePath(QStringLiteral("missing.log"));
    const QString opened =
        words(import(im, fileUrl(missing), RecordingImport::Candump));
    EXPECT_TRUE(opened.startsWith(QStringLiteral("the log can't be opened")))
        << opened.toStdString();
    EXPECT_FALSE(opened.contains(QStringLiteral("missing.log"))) << "no path";
    EXPECT_EQ(entries(root), QStringList()) << "nothing made";

    RecordingImport nowhere([] { return QString(); });
    EXPECT_EQ(
        words(import(nowhere, csv, RecordingImport::Csv,
                     fileUrl(fixture("logger-a/map.json")))),
        QStringLiteral("there's no Documents folder to keep recordings in"));

    EXPECT_FALSE(im.start(csv, 0, {}, {})) << "a kind core doesn't know";
    EXPECT_FALSE(im.busy());
    bool done = false;
    QObject context;
    QObject::connect(&im, &RecordingImport::finished, &context,
                     [&done] { done = true; });
    ASSERT_TRUE(im.start(fileUrl(write(dir, "one.log", kOneBus)),
                         RecordingImport::Candump, {}, {}));
    EXPECT_FALSE(im.start(csv, RecordingImport::Csv, {}, {}))
        << "one at a time";
    EXPECT_TRUE(waitUntil([&done] { return done; }));
}

/* The bridge's Import writes in its own recordings folder */
TEST(BridgeTest, ImportGoesToTheBridgesRecordingsFolder)
{
    QTemporaryDir dir;
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(dir.path());
    ASSERT_NE(bridge.recordingImport(), nullptr);
    QTemporaryDir logs;
    const Imported i = import(*bridge.recordingImport(),
                              fileUrl(write(logs, "one.log", kOneBus)),
                              RecordingImport::Candump);
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    EXPECT_EQ(entries(dir.path()).size(), 1);
    const QVariantList rows = bridge.recentRecordings(20);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_TRUE(rows[0]
                    .toMap()
                    .value(QStringLiteral("title"))
                    .toString()
                    .endsWith(QStringLiteral("candump import")));
}
