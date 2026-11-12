// File/layer: tests/test_recording_import.cpp | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: BridgeTest.Import* (test_bridge)
//
// The Recordings page's Import (app/recordingimport.{h,cpp}): a candump log
// or a CSV through its column map made a recording by core's
// ecuio_import_with, on a thread of its own, in a folder named as a
// recording is; the importer's destruction stops one under way.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <utility>

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

/* A worker that can't be started (the system out of threads) is a start
 * that says no, with nothing busy and nothing made, for an import and for a
 * guess alike */
TEST(BridgeTest, AnImportWhoseThreadCantStartSaysSo)
{
    QTemporaryDir dir;
    RecordingImport im([&] { return dir.path(); });
    im.setFailThreadForTesting(true);
    int busyChanges = 0;
    QObject::connect(&im, &RecordingImport::busyChanged,
                     [&busyChanges] { ++busyChanges; });
    const QString log = write(dir, "two.log", kTwoBuses);
    EXPECT_FALSE(
        im.start(fileUrl(log), RecordingImport::Candump, QUrl(), QString()));
    EXPECT_FALSE(im.guess(fileUrl(fixture("logger-a/log.csv")), QString()));
    EXPECT_FALSE(im.busy());
    EXPECT_EQ(busyChanges, 0);
    EXPECT_EQ(entries(dir.path()), QStringList{QStringLiteral("two.log")});

    im.setFailThreadForTesting(false);
    const Imported ran = import(im, fileUrl(log), RecordingImport::Candump);
    EXPECT_TRUE(ran.started) << "the importer is still usable";
}

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
    EXPECT_EQ(i.result.value(QStringLiteral("rowsNoTime")).toDouble(), 0.0);
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

/* A CSV's rows with no time are left out by core: the result says how many */
TEST(BridgeTest, ImportCountsACsvsRowsWithNoTime)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    const QString map = write(
        dir, "map.json",
        R"({"havne_column_map": 1, "time": {"column": "time", "unit": "ms"},
            "signals": [{"column": "rpm", "name": "RPM"}]})");
    const Imported i = import(
        im,
        fileUrl(write(dir, "log.csv", "time,rpm\n0,800\n,850\n500,900\n , \n")),
        RecordingImport::Csv, fileUrl(map));
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    EXPECT_EQ(i.result.value(QStringLiteral("rowsNoTime")).toDouble(), 2.0);
    EXPECT_EQ(i.result.value(QStringLiteral("values")).toDouble(), 2.0);
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

/* An import under way when the importer goes (the app quitting) stops at
 * core's next ask: the destructor's wait ends with nothing made, where
 * the same log imported whole makes its folder */
TEST(BridgeTest, ImportStopsWhenTheImporterGoes)
{
    QTemporaryDir dir;
    QByteArray log;
    for (int i = 0; i < 300000; ++i) {
        log += QStringLiteral("(1700000000.%1) can0 7E8#03410C1AF8AAAAAA\n")
                   .arg(i * 3, 6, 10, QLatin1Char('0'))
                   .toLatin1();
    }
    const QUrl file = fileUrl(write(dir, "long.log", log));
    const QString root = dir.filePath(QStringLiteral("recordings"));
    {
        RecordingImport whole([root] { return root; });
        const Imported i = import(whole, file, RecordingImport::Candump);
        ASSERT_TRUE(ok(i)) << words(i).toStdString();
        EXPECT_EQ(i.result.value(QStringLiteral("frames")).toDouble(),
                  300000.0);
    }
    ASSERT_EQ(entries(root).size(), 1);

    auto im = std::make_unique<RecordingImport>([root] { return root; });
    bool finished = false;
    QObject::connect(im.get(), &RecordingImport::finished,
                     [&finished] { finished = true; });
    ASSERT_TRUE(im->start(file, RecordingImport::Candump, {}, {}));
    im.reset();
    EXPECT_EQ(entries(root).size(), 1)
        << "no second folder and no partial: "
        << entries(root).join(QLatin1Char(' ')).toStdString();
    QCoreApplication::processEvents();
    EXPECT_FALSE(finished) << "its end goes with it";
}

/* An import the user cancels ends canceled with nothing made: held before
 * core's call, through the second pass with its partial folder there, and
 * in a provider file's copy. A guess isn't stopped, and the importer
 * imports again after each */
TEST(BridgeTest, ACanceledImportEndsCanceledAndLeavesNoRecording)
{
    QTemporaryDir dir;
    QByteArray log;
    for (int i = 0; i < 300000; ++i) {
        log += QStringLiteral("(1700000000.%1) can0 7E8#03410C1AF8AAAAAA\n")
                   .arg(i * 3, 6, 10, QLatin1Char('0'))
                   .toLatin1();
    }
    const QUrl file = fileUrl(write(dir, "long.log", log));
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    QVariantMap result;
    int ends = 0;
    QObject::connect(&im, &RecordingImport::finished,
                     [&](const QVariantMap& r) {
                         result = r;
                         ++ends;
                     });
    const auto expectCanceled = [&](const char* when) {
        EXPECT_FALSE(result.value(QStringLiteral("ok")).toBool()) << when;
        EXPECT_TRUE(result.value(QStringLiteral("canceled")).toBool()) << when;
        EXPECT_EQ(result.value(QStringLiteral("words")).toString(),
                  QStringLiteral("the import was canceled"))
            << when;
        EXPECT_FALSE(im.busy()) << when;
        EXPECT_TRUE(entries(root).isEmpty())
            << when << ": no folder and no partial: "
            << entries(root).join(QLatin1Char(' ')).toStdString();
    };

    im.cancel(); // with nothing under way: nothing

    // Held before core's call: the cancel ends the hold and core's first
    // ask stops it
    im.setHoldForTesting(60000);
    QElapsedTimer held;
    held.start();
    ASSERT_TRUE(im.start(file, RecordingImport::Candump, {}, {}));
    EXPECT_TRUE(im.busy());
    im.cancel();
    ASSERT_TRUE(waitUntil([&] { return ends == 1; }, 60000));
    EXPECT_LT(held.elapsed(), 30000) << "the cancel ends the hold";
    expectCanceled("held before core's call");
    im.setHoldForTesting(0);

    // Mid-way: the second pass has made its partial folder. An import
    // that outran the look (it ended, or its folder was already in place)
    // is no judgment: its folder goes and the next is tried
    bool caught = false;
    for (int attempt = 0; attempt < 5 && !caught; ++attempt) {
        const int was = ends;
        ASSERT_TRUE(im.start(file, RecordingImport::Candump, {}, {}));
        ASSERT_TRUE(waitUntil(
            [&] { return !entries(root).isEmpty() || ends > was; }, 60000));
        const QStringList seen = entries(root);
        const bool partial = ends == was && seen.size() == 1 &&
                             seen[0].contains(QStringLiteral(".partial-"));
        im.cancel();
        ASSERT_TRUE(waitUntil([&] { return ends > was; }, 60000));
        caught = partial && result.value(QStringLiteral("canceled")).toBool();
        if (caught) {
            expectCanceled("in the second pass");
        } else {
            ASSERT_TRUE(QDir(root).removeRecursively());
        }
    }
    EXPECT_TRUE(caught) << "no import was stopped with its partial folder "
                           "there in five tries";

    // In the copy of a file core can't read in place
    im.setCopyLocalFilesForTesting(true);
    int was = ends;
    ASSERT_TRUE(im.start(file, RecordingImport::Candump, {}, {}));
    im.cancel();
    ASSERT_TRUE(waitUntil([&] { return ends == was + 1; }, 60000));
    expectCanceled("in the copy");
    im.setCopyLocalFilesForTesting(false);

    // A guess runs to its end whatever is asked
    bool guessed = false;
    QVariantMap guess;
    QObject::connect(&im, &RecordingImport::guessFinished,
                     [&](const QVariantMap& g) {
                         guess = g;
                         guessed = true;
                     });
    const QUrl csv =
        fileUrl(write(dir, "small.csv", "time (s),rpm\n0,800\n1,900\n"));
    ASSERT_TRUE(im.guess(csv, QString()));
    im.cancel();
    ASSERT_TRUE(waitUntil([&] { return guessed; }));
    EXPECT_TRUE(guess.value(QStringLiteral("ok")).toBool())
        << guess.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_FALSE(guess.contains(QStringLiteral("canceled")));

    // An earlier cancel is no cancel of the next import
    was = ends;
    const Imported whole = import(im, file, RecordingImport::Candump);
    ASSERT_TRUE(ok(whole)) << words(whole).toStdString();
    EXPECT_FALSE(whole.result.contains(QStringLiteral("canceled")));
    EXPECT_EQ(whole.result.value(QStringLiteral("frames")).toDouble(),
              300000.0);
    EXPECT_EQ(entries(root).size(), 1);
    EXPECT_EQ(ends, was + 1);
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

namespace {

// A guess run to its end
QVariantMap guessed(RecordingImport& im, const QUrl& file,
                    const QString& unit = {})
{
    QVariantMap out;
    bool done = false;
    QObject context;
    QObject::connect(&im, &RecordingImport::guessFinished, &context,
                     [&](const QVariantMap& r) {
                         out = r;
                         done = true;
                     });
    EXPECT_TRUE(im.guess(file, unit));
    EXPECT_TRUE(im.busy());
    EXPECT_TRUE(waitUntil([&] { return done; })) << "the guess ends";
    EXPECT_FALSE(im.busy());
    return out;
}

QVariantMap column(const QVariantMap& g, int i)
{
    return g.value(QStringLiteral("columns")).toList().at(i).toMap();
}

QString text(const QString& path)
{
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    return QString::fromUtf8(f.readAll()).remove(QLatin1Char('\r'));
}

// The edit of a header column the panel hands composeMap
QVariantMap edit(const QString& column, bool include, const QString& name,
                 const QString& unit, const QString& kind)
{
    return {{QStringLiteral("column"), column},
            {QStringLiteral("include"), include},
            {QStringLiteral("name"), name},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("kind"), kind}};
}

} // namespace

/* The guess is core's map for the file, as core wrote it, with each header
 * column's use, why not, name, unit and kind, and the first rows of its
 * cells for display */
TEST(BridgeTest, ImportGuessesAColumnMapForTheFile)
{
    RecordingImport im([] { return QString(); });
    const QVariantMap g = guessed(im, fileUrl(fixture("logger-a/log.csv")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(
        g.value(QStringLiteral("mapText")).toString().remove(QLatin1Char('\r')),
        text(fixture("logger-a/guessed-map.json")).trimmed());
    EXPECT_TRUE(g.value(QStringLiteral("unitKnown")).toBool());
    EXPECT_TRUE(g.value(QStringLiteral("decimalComma")).toBool());
    EXPECT_TRUE(g.value(QStringLiteral("preamble")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("delimiter")).toString(),
              QStringLiteral(";"));
    EXPECT_EQ(g.value(QStringLiteral("headerRow")).toInt(), 3);
    EXPECT_EQ(g.value(QStringLiteral("timeColumn")).toString(),
              QStringLiteral("Time [s]"));
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("s"));

    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 5);
    const QVariantMap time = column(g, 0);
    EXPECT_TRUE(time.value(QStringLiteral("isTime")).toBool());
    EXPECT_EQ(time.value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("0,00"), QStringLiteral("0,25"),
                           QStringLiteral("0,50"), QStringLiteral("0,75"),
                           QStringLiteral("1,00")}))
        << "the first five rows below the header row, as the file has them";
    const QVariantMap rpm = column(g, 1);
    EXPECT_FALSE(rpm.value(QStringLiteral("isTime")).toBool());
    EXPECT_TRUE(rpm.value(QStringLiteral("used")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("column")).toString(),
              QStringLiteral("Engine Speed [rpm]"));
    EXPECT_EQ(rpm.value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine Speed"));
    EXPECT_EQ(rpm.value(QStringLiteral("unit")).toString(),
              QStringLiteral("rpm"));
    EXPECT_EQ(rpm.value(QStringLiteral("kind")).toString(),
              QStringLiteral("number"));
    EXPECT_EQ(column(g, 4).value(QStringLiteral("kind")).toString(),
              QStringLiteral("text"));
}

/* A quoted file is split as its quotes say (a BOM, a quoted comma, a
 * doubled quote); a long cell is cut; the file's path is nowhere in the
 * result */
TEST(BridgeTest, ImportShowsTheFirstRowsAsTheFileHasThem)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QString longCell(40, QLatin1Char('x'));
    const QByteArray csv = "\xEF\xBB\xBF\"time (s)\",\"a, b\",c,note\r\n"
                           "0,1,2,\"he said \"\"hi\"\"\"\r\n"
                           "1,3,4,x\r\n"
                           "2,5,6,y\r\n"
                           "3,4,5," +
                           longCell.toUtf8() + "\r\n";
    const QString path = write(dir, "quoted.csv", csv);
    const QVariantMap g = guessed(im, fileUrl(path));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(column(g, 1).value(QStringLiteral("column")).toString(),
              QStringLiteral("a, b"));
    EXPECT_EQ(column(g, 1).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("1"), QStringLiteral("3"),
                           QStringLiteral("5"), QStringLiteral("4")}));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("2"), QStringLiteral("4"),
                           QStringLiteral("6"), QStringLiteral("5")}));
    EXPECT_EQ(column(g, 3).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("he said \"hi\""),
                           QStringLiteral("x"), QStringLiteral("y"),
                           longCell.left(29) + QStringLiteral("...")}));
    for (const QVariant& v : g)
        EXPECT_FALSE(v.toString().contains(path));
}

/* A padded file's cells show as the import reads them: an unquoted cell
 * trimmed (a blank one empty), a quoted one exact; a label is the header
 * without its padding, and a header of spaces is an empty one */
TEST(BridgeTest, ImportShowsPaddedCellsAsTheImportReadsThem)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QVariantMap g =
        guessed(im, fileUrl(write(dir, "padded.csv",
                                  "Time, \"Engine speed (rpm)\" , Mode, \n"
                                  "0,  900, \" OL \", \n"
                                  "250, 910, , \n"
                                  "500,\t950 , OL, \n")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 4);
    EXPECT_EQ(column(g, 1).value(QStringLiteral("column")).toString(),
              QStringLiteral("Engine speed (rpm)"));
    EXPECT_EQ(column(g, 1).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("900"), QStringLiteral("910"),
                           QStringLiteral("950")}));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("column")).toString(),
              QStringLiteral(" Mode"));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("label")).toString(),
              QStringLiteral("Mode"));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral(" OL "), QString(),
                           QStringLiteral("OL")}));
    const QVariantMap blank = column(g, 3);
    EXPECT_EQ(blank.value(QStringLiteral("label")).toString(), QString());
    EXPECT_EQ(blank.value(QStringLiteral("why")).toString(),
              QStringLiteral("its header cell is empty"));
    EXPECT_FALSE(blank.value(QStringLiteral("canPutBack")).toBool());
    EXPECT_EQ(blank.value(QStringLiteral("samples")).toStringList(),
              (QStringList{QString(), QString(), QString()}));
}

/* A left-out column can be put back unless its header can't name it: an
 * empty header, or one the file has twice (core refuses a map naming it) */
TEST(BridgeTest, ImportGuessPutsBackOnlyAColumnAMapCanName)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QVariantMap g =
        guessed(im, fileUrl(write(dir, "twice.csv",
                                  "time (s),rpm,dup,dup,empty,\n"
                                  "0,800,1,2,,\n1,900,3,4,,\n2,950,5,6,,\n")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 6);
    EXPECT_FALSE(column(g, 2).value(QStringLiteral("used")).toBool());
    EXPECT_FALSE(column(g, 2).value(QStringLiteral("canPutBack")).toBool());
    EXPECT_FALSE(column(g, 3).value(QStringLiteral("canPutBack")).toBool());
    EXPECT_TRUE(column(g, 4).value(QStringLiteral("canPutBack")).toBool());
    EXPECT_FALSE(column(g, 5).value(QStringLiteral("canPutBack")).toBool());
}

/* A name or a shown cell is cut on a character, never between the halves
 * of a surrogate pair */
TEST(BridgeTest, ImportCutsNamesAndCellsOnWholeCharacters)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    // 30 bytes, then a 4-byte character that crosses the 31-byte limit
    const QString header = QString(30, QLatin1Char('a')) + emoji;
    const QString cell =
        QString(28, QLatin1Char('x')) + emoji + QString(10, QLatin1Char('y'));
    const QByteArray csv = "time (s),rpm," + header.toUtf8() +
                           ",note\n0,800,," + cell.toUtf8() + "\n1,900,," +
                           cell.toUtf8() + "\n";
    const QVariantMap g = guessed(im, fileUrl(write(dir, "astral.csv", csv)));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 4);
    const QVariantMap col = column(g, 2);
    const QString name = col.value(QStringLiteral("name")).toString();
    EXPECT_EQ(name, QString(30, QLatin1Char('a')));
    EXPECT_EQ(QString::fromUtf8(name.toUtf8()), name);
    ASSERT_FALSE(
        column(g, 3).value(QStringLiteral("samples")).toStringList().isEmpty());
    EXPECT_EQ(
        column(g, 3).value(QStringLiteral("samples")).toStringList().first(),
        QString(28, QLatin1Char('x')) + QStringLiteral("..."));
}

/* A time named so, of whole numbers with no unit, reads as ms; the unit the
 * caller gives wins; a file with no header is refused in core's words */
TEST(BridgeTest, ImportGuessTakesTheTimeUnitTheCallerGives)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QUrl plain =
        fileUrl(write(dir, "plain.csv", "time,rpm\n0,800\n1,900\n2,1000\n"));
    QVariantMap g = guessed(im, plain);
    ASSERT_TRUE(ok({g}));
    EXPECT_TRUE(g.value(QStringLiteral("unitKnown")).toBool());
    EXPECT_TRUE(g.value(QStringLiteral("timeNumbers")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("ms"));

    g = guessed(im, plain, QStringLiteral("s"));
    ASSERT_TRUE(ok({g}));
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("s"));

    g = guessed(im, plain, QStringLiteral("ms"));
    ASSERT_TRUE(ok({g}));
    EXPECT_TRUE(g.value(QStringLiteral("unitKnown")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("ms"));

    g = guessed(im,
                fileUrl(write(dir, "none.csv", "just,words\nmore,words\n")));
    EXPECT_FALSE(ok({g}));
    EXPECT_FALSE(g.value(QStringLiteral("words")).toString().isEmpty());
    g = guessed(im, fileUrl(dir.filePath(QStringLiteral("missing.csv"))));
    EXPECT_FALSE(ok({g}));
    EXPECT_FALSE(
        g.value(QStringLiteral("words")).toString().contains(dir.path()));
}

/* The same guess for a file the app can't open in place */
TEST(BridgeTest, ImportGuessCopiesAFileItCantReadInPlace)
{
    RecordingImport im([] { return QString(); });
    im.setCopyLocalFilesForTesting(true);
    const QVariantMap g = guessed(im, fileUrl(fixture("logger-b/log.csv")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("iso8601"));
    EXPECT_FALSE(g.value(QStringLiteral("timeNumbers")).toBool());
}

/* The edits become the map the import gets: a column left out or renamed,
 * a unit dropped, a kind changed (a text keeps no decimals), a left-out
 * column put back, the time's unit set, the time column never a signal; a
 * map that isn't JSON gives "" */
TEST(BridgeTest, ImportComposesTheMapFromTheEdits)
{
    RecordingImport im([] { return QString(); });
    const QVariantMap g = guessed(im, fileUrl(fixture("logger-a/log.csv")));
    ASSERT_TRUE(ok({g}));
    const QString base = g.value(QStringLiteral("mapText")).toString();
    const QVariantList edits{
        edit(QStringLiteral("Time [s]"), true, QStringLiteral("Time"), {},
             QStringLiteral("text")),
        edit(QStringLiteral("Engine Speed [rpm]"), true, QStringLiteral("RPM"),
             {}, QStringLiteral("number")),
        edit(QStringLiteral("Load [%]"), false, QStringLiteral("Load"),
             QStringLiteral("%"), QStringLiteral("number")),
        edit(QStringLiteral("Lambda"), true, QStringLiteral("Lambda"),
             QStringLiteral("l"), QStringLiteral("text")),
        edit(QStringLiteral("Brand new"), true, QStringLiteral("Brand new"), {},
             QStringLiteral("text"))};
    const QString composed = im.composeMap(base, edits, QStringLiteral("ms"));
    const QJsonObject map = QJsonDocument::fromJson(composed.toUtf8()).object();
    EXPECT_EQ(
        map.value(QStringLiteral("time")).toObject(),
        (QJsonObject{{QStringLiteral("column"), QStringLiteral("Time [s]")},
                     {QStringLiteral("unit"), QStringLiteral("ms")}}));
    EXPECT_EQ(map.value(QStringLiteral("delimiter")).toString(),
              QStringLiteral(";"))
        << "what the edits don't touch is the guess's";
    const QJsonArray sigs = map.value(QStringLiteral("signals")).toArray();
    ASSERT_EQ(sigs.size(), 3)
        << "an excluded column and the time column are out";
    for (const QJsonValue s : sigs) {
        EXPECT_NE(s.toObject().value(QStringLiteral("column")).toString(),
                  QStringLiteral("Time [s]"))
            << "core refuses the time column as a signal";
    }
    const QJsonObject rpm = sigs[0].toObject();
    EXPECT_EQ(rpm.value(QStringLiteral("name")).toString(),
              QStringLiteral("RPM"));
    EXPECT_FALSE(rpm.contains(QStringLiteral("unit")))
        << "an empty unit is dropped";
    EXPECT_EQ(rpm.value(QStringLiteral("decimals")).toInt(), 2)
        << "a number keeps its other keys";
    const QJsonObject lambda = sigs[1].toObject();
    EXPECT_EQ(lambda.value(QStringLiteral("kind")).toString(),
              QStringLiteral("text"));
    EXPECT_FALSE(lambda.contains(QStringLiteral("decimals")))
        << "a text has no decimals, which core refuses";
    EXPECT_EQ(lambda.value(QStringLiteral("unit")).toString(),
              QStringLiteral("l"));
    EXPECT_EQ(sigs[2].toObject().value(QStringLiteral("column")).toString(),
              QStringLiteral("Brand new"));

    EXPECT_EQ(im.composeMap(QStringLiteral("not json"), edits, {}), QString());
    EXPECT_FALSE(
        QJsonDocument::fromJson(im.composeMap(base, edits, {}).toUtf8())
            .object()
            .value(QStringLiteral("time"))
            .toObject()
            .value(QStringLiteral("unit"))
            .toString() == QLatin1String("ms"))
        << "no unit given keeps the map's";
}

/* The guess, imported as it is and with an edit, makes a recording; an
 * edit core refuses is refused in core's words and makes none */
TEST(BridgeTest, ImportStartsFromTheMapsTextAndHoldsNoMapFile)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    const QUrl log = fileUrl(fixture("logger-a/log.csv"));
    const QVariantMap g = guessed(im, log);
    ASSERT_TRUE(ok({g}));

    Imported i;
    bool done = false;
    QObject context;
    QObject::connect(&im, &RecordingImport::finished, &context,
                     [&](const QVariantMap& r) {
                         i.result = r;
                         done = true;
                     });
    ASSERT_TRUE(
        im.startWithMap(log, g.value(QStringLiteral("mapText")).toString()));
    EXPECT_FALSE(im.startWithMap(log, QString())) << "one at a time";
    ASSERT_TRUE(waitUntil([&] { return done; }));
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    EXPECT_GT(i.result.value(QStringLiteral("values")).toDouble(), 0.0);
    EXPECT_EQ(entries(root).size(), 1);

    // A name past 31 bytes, core's rule, refused with its words
    done = false;
    const QVariantList edits{
        edit(QStringLiteral("Time [s]"), false, {}, {}, QStringLiteral("text")),
        edit(QStringLiteral("Lambda"), true, QString(40, QLatin1Char('n')), {},
             QStringLiteral("number"))};
    ASSERT_TRUE(im.startWithMap(
        log, im.composeMap(g.value(QStringLiteral("mapText")).toString(), edits,
                           {})));
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_FALSE(ok(i));
    EXPECT_FALSE(words(i).isEmpty());
    EXPECT_EQ(entries(root).size(), 1) << "a refused import makes none";
}

/* Saved maps: the name made safe, kept under maps/, listed by name, never
 * overwritten */
TEST(BridgeTest, ImportSavesAMapUnderASafeNameAndListsIt)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    im.setMapsRootForTesting(dir.filePath(QStringLiteral("settings/maps")));
    EXPECT_TRUE(im.savedMaps().isEmpty());

    QVariantMap r =
        im.saveMap(QStringLiteral("  My logger: v2/x "), QStringLiteral("{}"));
    ASSERT_TRUE(ok({r}))
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("name")).toString(),
              QStringLiteral("My-logger--v2-x"));
    EXPECT_EQ(text(r.value(QStringLiteral("url")).toUrl().toLocalFile()),
              QStringLiteral("{}"));

    r = im.saveMap(QStringLiteral("My logger: v2/x"),
                   QStringLiteral("{\"other\": 1}"));
    EXPECT_FALSE(ok({r}));
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              QStringLiteral("a map named My-logger--v2-x is saved already"));
    EXPECT_EQ(text(im.savedMaps()
                       .at(0)
                       .toMap()
                       .value(QStringLiteral("url"))
                       .toUrl()
                       .toLocalFile()),
              QStringLiteral("{}"))
        << "the first is kept";
    for (const char* none : {"", "   ", "///"}) {
        r = im.saveMap(QLatin1String(none), QStringLiteral("{}"));
        EXPECT_FALSE(ok({r}));
        EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
                  QStringLiteral("give the map a name"));
    }
    ASSERT_TRUE(ok({im.saveMap(QStringLiteral("a"), QStringLiteral("{}"))}));
    const QVariantList saved = im.savedMaps();
    ASSERT_EQ(saved.size(), 2);
    EXPECT_EQ(saved.at(0).toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("a"));
    EXPECT_EQ(saved.at(1).toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("My-logger--v2-x"));
}
