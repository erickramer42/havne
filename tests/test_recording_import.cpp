// File/layer: tests/test_recording_import.cpp | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// The Recordings page's Import (app/recordingimport.{h,cpp}): a candump log
// or a CSV through its column map made a recording by core's
// ecuio_import_with, on a thread of its own, in a folder named as a
// recording is; the importer's destruction stops one under way.

#include "test_recording_import_support.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "ecuio/internal.h" /* ecuio_import_test_between_passes */

namespace {

// Two buses, can0 three frames over 0.5 s and can1 two
const QByteArray kTwoBuses = "\n"
                             "(1700000000.000000) can0 7E0#0201050000000000\n"
                             "(1700000000.010000) can1 123#11\n"
                             "(1700000000.250000) can0 7E8#03410550AAAAAAAA\n"
                             "(1700000000.300000) can1 123#22\n"
                             "(1700000000.500000) can0 7E8#03410551AAAAAAAA\n";

const QByteArray kOneBus = "(1700000000.000000) vcan0 7E8#03410550\n"
                           "(1700000001.500000) vcan0 7E8#03410551\n";

const QRegularExpression
    kStamped(QStringLiteral("^[0-9]{8}-[0-9]{6}(-[0-9]+)? (.+)$"));

/* core's between-passes seam held: the import parks after its partial folder
 * is made until release() (or the scope's end, so a failed ASSERT can't hang
 * the import's join); once released it passes. Declared after the importer
 * it holds */
class ImportHold {
public:
    ImportHold() { ecuio_import_test_between_passes(&ImportHold::hook, this); }
    ~ImportHold()
    {
        release();
        ecuio_import_test_between_passes(nullptr, nullptr);
    }
    ImportHold(const ImportHold&) = delete;
    ImportHold& operator=(const ImportHold&) = delete;
    bool reached() const { return m_reached.load(); }
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_open = true;
        }
        m_cv.notify_all();
    }

private:
    static void hook(void* user)
    {
        auto* self = static_cast<ImportHold*>(user);
        self->m_reached = true;
        std::unique_lock<std::mutex> lock(self->m_mutex);
        self->m_cv.wait_for(lock, std::chrono::seconds(60),
                            [self] { return self->m_open; });
    }
    std::atomic<bool> m_reached{false};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_open = false;
};

} // namespace

/* A worker that can't be started (the system out of threads) is a start
 * that says no, with nothing busy and nothing made, for an import and for a
 * guess alike */
TEST(BridgeTest, ImportWhoseThreadCantStartSaysSo)
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
TEST(BridgeTest, ImportCanceledEndsCanceledAndLeavesNoRecording)
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

    // Mid-way: core's seam holds the import after the second pass made its
    // partial folder, so the cancel lands there whatever the machine's speed
    {
        ImportHold hold;
        const int was = ends;
        ASSERT_TRUE(im.start(file, RecordingImport::Candump, {}, {}));
        ASSERT_TRUE(waitUntil([&] { return hold.reached(); }, 60000));
        const QStringList seen = entries(root);
        ASSERT_EQ(seen.size(), 1) << seen.join(QLatin1Char(' ')).toStdString();
        EXPECT_TRUE(seen[0].contains(QStringLiteral(".partial-")));
        EXPECT_EQ(ends, was) << "held, so not ended";
        im.cancel();
        hold.release();
        ASSERT_TRUE(waitUntil([&] { return ends > was; }, 60000));
        expectCanceled("in the second pass");
    }

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

/* A typed label is the folder's label, reduced as the recorder reduces one;
 * a CSV that carries dates is stamped with its first row's time, not the
 * import's */
TEST(BridgeTest, ImportKeepsATypedLabelAndStampsADatedCsvWithItsOwnTime)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    im.setLabel(QStringLiteral("  My  drive:/ ."));
    EXPECT_EQ(im.label(), QStringLiteral("My drive"));
    // Forty characters as core's recorder counts them: a pair that
    // straddles the end is whole or out, never cut, and characters past
    // ASCII stay as core keeps them
    const QString pair = QString::fromUcs4(U"\U0001F600");
    im.setLabel(QString(39, QLatin1Char('a')) + pair + QStringLiteral("bcd"));
    EXPECT_EQ(im.label(), QString(39, QLatin1Char('a')) + pair);
    im.setLabel(QString(40, QLatin1Char('a')) + pair);
    EXPECT_EQ(im.label(), QString(40, QLatin1Char('a')));
    im.setLabel(QStringLiteral("a") + QChar(0x2028) + QStringLiteral("b"));
    EXPECT_EQ(im.label(), QStringLiteral("a") + QChar(0x2028) + QStringLiteral("b"))
        << "core's recorder keeps what is past ASCII";
    im.setLabel(QStringLiteral("  My  drive:/ ."));
    const Imported dated =
        import(im, fileUrl(fixture("logger-b/log.csv")), RecordingImport::Csv,
               fileUrl(fixture("logger-b/map.json")));
    ASSERT_TRUE(ok(dated)) << words(dated).toStdString();
    const QString name =
        QDir(dated.result.value(QStringLiteral("folder")).toString())
            .dirName();
    const QString first =
        QDateTime::fromString(QStringLiteral("2026-11-05T22:03:22+09:00"),
                              Qt::ISODate)
            .toLocalTime()
            .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    EXPECT_EQ(name, first + QStringLiteral(" My drive"));
    EXPECT_TRUE(QFileInfo::exists(
        dated.result.value(QStringLiteral("folder")).toString() +
        QStringLiteral("/signals.csv")));

    im.setLabel(QString());
    const Imported undated = import(im, fileUrl(write(dir, "two.log", kOneBus)),
                                    RecordingImport::Candump);
    ASSERT_TRUE(ok(undated));
    EXPECT_TRUE(QDir(undated.result.value(QStringLiteral("folder")).toString())
                    .dirName()
                    .endsWith(QStringLiteral(" candump import")))
        << "an empty label keeps the format's own";
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
