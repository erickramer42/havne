// File/layer: tests/test_recording_export.cpp | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: RecordingExportTest (test_bridge)
//
// The Recordings viewer's Export (app/recordingexport.{h,cpp}): each file
// compared byte for byte with what core's ecuio_export writes for the same
// options, the sizes with core's, and what the class adds itself: the copy to
// the place picked, a provider's document, a folder copy's name.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <vector>

#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "recordingexport.h"

namespace {

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
    return QStringLiteral(ECUIO_RECORDING_FIXTURES "/") + QLatin1String(name);
}

QByteArray bytesOf(const QString& path)
{
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << path.toStdString();
    return f.readAll();
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    f.write(bytes);
}

struct Options {
    int format = RecordingExport::Csv;
    bool longLayout = false;
    quint32 rowMs = 0;
    quint32 units = ECUIO_UNITS_METRIC;
    QStringList columns;
    bool raw = false;
};

QVariantMap asMap(const QString& folder, const Options& o)
{
    return {{QStringLiteral("folder"), folder},
            {QStringLiteral("format"), o.format},
            {QStringLiteral("longLayout"), o.longLayout},
            {QStringLiteral("rowMs"), o.rowMs},
            {QStringLiteral("units"), o.units},
            {QStringLiteral("columns"), o.columns},
            {QStringLiteral("raw"), o.raw}};
}

// What core itself writes for the options, the file's bytes
QByteArray coreBytes(const QString& folder, const Options& o,
                     const QString& out)
{
    const QByteArray folderUtf8 = folder.toUtf8();
    const QByteArray outUtf8 = out.toUtf8();
    QList<QByteArray> names;
    for (const QString& c : o.columns) {
        names << c.toUtf8();
    }
    std::vector<const char*> pointers;
    for (const QByteArray& n : names) {
        pointers.push_back(n.constData());
    }
    ecuio_export_options_t c{};
    c.format = static_cast<uint32_t>(o.format);
    c.flags = o.raw ? ECUIO_EXPORT_RAW : 0u;
    c.folder_utf8 = folderUtf8.constData();
    c.out_path_utf8 = outUtf8.constData();
    if (o.format == ECUIO_EXPORT_CSV) {
        c.layout = o.longLayout ? ECUIO_CSV_LONG : ECUIO_CSV_WIDE;
        if (!o.longLayout) {
            c.row_ms = o.rowMs;
            c.units = o.units;
            c.column_count = static_cast<uint32_t>(pointers.size());
            c.columns_utf8 = pointers.empty() ? nullptr : pointers.data();
        }
    }
    ecuio_export_report_t report{};
    EXPECT_EQ(ecuio_export(&c, &report), ECUIO_OK);
    return bytesOf(out);
}

QVariantMap exported(RecordingExport& e, const QString& folder,
                     const Options& o, const QUrl& target)
{
    QVariantMap result;
    bool done = false;
    QObject context;
    QObject::connect(&e, &RecordingExport::finished, &context,
                     [&](const QVariantMap& r) {
                         result = r;
                         done = true;
                     });
    EXPECT_TRUE(e.start(asMap(folder, o), target));
    EXPECT_TRUE(e.busy());
    EXPECT_TRUE(waitUntil([&] { return done; })) << "the export ends";
    EXPECT_FALSE(e.busy());
    return result;
}

bool ok(const QVariantMap& r)
{
    return r.value(QStringLiteral("ok")).toBool();
}

QString text(const QVariantMap& r, const char* key)
{
    return r.value(QLatin1String(key)).toString();
}

QVariantList asked(std::initializer_list<int> ms)
{
    QVariantList out;
    for (const int m : ms) {
        out << m;
    }
    return out;
}

// One estimate to its end
QVariantMap sized(RecordingExport& e, const QString& folder,
                  std::initializer_list<int> ms, quint32 units = 0,
                  const QStringList& columns = {})
{
    QVariantMap result;
    bool done = false;
    QObject context;
    QObject::connect(&e, &RecordingExport::estimated, &context,
                     [&](const QVariantMap& r) {
                         result = r;
                         done = true;
                     });
    EXPECT_TRUE(
        e.estimate(folder, asked(ms), static_cast<int>(units), columns));
    EXPECT_TRUE(waitUntil([&] { return done; })) << "the estimate ends";
    EXPECT_FALSE(e.sizing());
    return result;
}

// A recording of values alone whose signal's name starts as a formula, which
// the CSV export refuses
QString formulaRecording(const QTemporaryDir& dir)
{
    const QString folder = dir.filePath(QStringLiteral("formula"));
    EXPECT_TRUE(QDir().mkpath(folder));
    writeFile(folder + QStringLiteral("/signals.csv"),
              "host_us,signal,value,unit\n100,=SUM(A1),1,rpm\n");
    return folder;
}

// A copy of a fixture under a name holding a vehicle's label
QString labeledCopy(const QTemporaryDir& dir, const char* source)
{
    const QString folder =
        dir.filePath(QStringLiteral("20261105-220322 My Weekend Car"));
    EXPECT_TRUE(QDir().mkpath(folder));
    const QDir from(fixture(source));
    for (const QString& name : from.entryList(QDir::Files)) {
        EXPECT_TRUE(QFile::copy(from.filePath(name), folder + '/' + name));
    }
    return folder;
}

} // namespace

TEST(RecordingExportTest, FileStemIsTheStampAlone)
{
    EXPECT_EQ(RecordingExport::fileStem(
                  QStringLiteral("/x/20261105-220322 My Weekend Car")),
              QStringLiteral("20261105-220322"));
    EXPECT_EQ(RecordingExport::fileStem(QStringLiteral("/x/20261105-220322-2")),
              QStringLiteral("20261105-220322-2"));
    EXPECT_EQ(RecordingExport::fileStem(QStringLiteral("/x/20261105-220322/")),
              QStringLiteral("20261105-220322"));
    EXPECT_EQ(RecordingExport::fileStem(QStringLiteral("/x/My Weekend Car")),
              QStringLiteral("recording"));
}

TEST(RecordingExportTest, HoldsValuesIsTheFoldersSignalsFile)
{
    EXPECT_TRUE(RecordingExport::holdsValues(fixture("pull-early")));
    QTemporaryDir dir;
    EXPECT_FALSE(RecordingExport::holdsValues(dir.path()));
}

TEST(RecordingExportTest, AWideCsvIsCoresFile)
{
    QTemporaryDir dir;
    RecordingExport e;
    const QString out = dir.filePath(QStringLiteral("wide.csv"));
    const QVariantMap r =
        exported(e, fixture("pull-early"), Options{}, QUrl::fromLocalFile(out));
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    EXPECT_EQ(bytesOf(out),
              coreBytes(fixture("pull-early"), Options{},
                        dir.filePath(QStringLiteral("core.csv"))));
    EXPECT_TRUE(bytesOf(out).startsWith("Time (ms),"));
    EXPECT_GT(r.value(QStringLiteral("rows")).toDouble(), 0.0);
    EXPECT_TRUE(text(r, "summary").contains(QStringLiteral(" rows of ")))
        << text(r, "summary").toStdString();
    EXPECT_TRUE(
        text(r, "summary").endsWith(QStringLiteral(", left out: 1 text value")))
        << text(r, "summary").toStdString();
    EXPECT_TRUE(
        text(r, "line").startsWith(QStringLiteral("Exported wide CSV: ")))
        << text(r, "line").toStdString();
    EXPECT_FALSE(text(r, "line").contains(dir.path())) << "no path in the log";
}

TEST(RecordingExportTest, TheIntervalUnitsAndColumnsReachCore)
{
    QTemporaryDir dir;
    RecordingExport e;
    Options o;
    o.rowMs = 250;
    o.units = ECUIO_UNITS_IMPERIAL;
    o.columns = QStringList{QStringLiteral("Coolant"), QStringLiteral("RPM")};
    const QString out = dir.filePath(QStringLiteral("grid.csv"));
    const QVariantMap r =
        exported(e, fixture("pull-early"), o, QUrl::fromLocalFile(out));
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    const QByteArray bytes = bytesOf(out);
    EXPECT_EQ(bytes, coreBytes(fixture("pull-early"), o,
                               dir.filePath(QStringLiteral("core.csv"))));
    EXPECT_TRUE(bytes.startsWith("Time (ms),Coolant "))
        << bytes.left(60).data();
    EXPECT_NE(bytes.left(40).indexOf("F)"), -1) << bytes.left(40).data();
    EXPECT_NE(bytes, coreBytes(fixture("pull-early"), Options{},
                               dir.filePath(QStringLiteral("plain.csv"))));
}

TEST(RecordingExportTest, ALongCsvTakesNoneOfTheWideOptions)
{
    QTemporaryDir dir;
    RecordingExport e;
    Options o;
    o.longLayout = true;
    o.rowMs = 250;
    o.units = ECUIO_UNITS_IMPERIAL;
    o.columns = QStringList{QStringLiteral("RPM")};
    const QString out = dir.filePath(QStringLiteral("long.csv"));
    const QVariantMap r =
        exported(e, fixture("pull-early"), o, QUrl::fromLocalFile(out));
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    EXPECT_EQ(bytesOf(out),
              coreBytes(fixture("pull-early"), o,
                        dir.filePath(QStringLiteral("core.csv"))));
    EXPECT_TRUE(
        text(r, "line").startsWith(QStringLiteral("Exported long CSV: ")));
    EXPECT_TRUE(text(r, "summary").contains(QStringLiteral(" values of ")));
}

TEST(RecordingExportTest, APcapngIsCoresFile)
{
    QTemporaryDir dir;
    RecordingExport e;
    Options o;
    o.format = RecordingExport::Pcapng;
    const QString out = dir.filePath(QStringLiteral("frames.pcapng"));
    const QVariantMap r =
        exported(e, fixture("findings"), o, QUrl::fromLocalFile(out));
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    EXPECT_EQ(bytesOf(out),
              coreBytes(fixture("findings"), o,
                        dir.filePath(QStringLiteral("core.pcapng"))));
    EXPECT_TRUE(text(r, "summary").contains(QStringLiteral(" frames")));
    EXPECT_GT(r.value(QStringLiteral("frames")).toDouble(), 0.0);
}

TEST(RecordingExportTest, RawIsAskedOfCoreAndSaid)
{
    QTemporaryDir dir;
    RecordingExport e;
    Options scrubbed;
    scrubbed.format = RecordingExport::Folder;
    Options raw = scrubbed;
    raw.raw = true;
    const QString parent = dir.filePath(QStringLiteral("out"));
    ASSERT_TRUE(QDir().mkpath(parent));
    const QVariantMap r =
        exported(e, fixture("findings"), raw, QUrl::fromLocalFile(parent));
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    EXPECT_TRUE(text(r, "summary").endsWith(QStringLiteral("not scrubbed")))
        << text(r, "summary").toStdString();
    const QVariantMap s =
        exported(e, fixture("findings"), scrubbed, QUrl::fromLocalFile(parent));
    ASSERT_TRUE(ok(s)) << text(s, "words").toStdString();
    EXPECT_FALSE(text(s, "summary").contains(QStringLiteral("not scrubbed")));
    EXPECT_NE(bytesOf(parent + '/' + text(r, "name") + "/events.log"),
              bytesOf(parent + '/' + text(s, "name") + "/events.log"));
}

TEST(RecordingExportTest, AFolderCopyIsNamedByTheStampAlone)
{
    QTemporaryDir dir;
    RecordingExport e;
    const QString source = labeledCopy(dir, "findings");
    const QString parent = dir.filePath(QStringLiteral("out"));
    ASSERT_TRUE(QDir().mkpath(parent));
    Options o;
    o.format = RecordingExport::Folder;
    const QVariantMap first =
        exported(e, source, o, QUrl::fromLocalFile(parent));
    ASSERT_TRUE(ok(first)) << text(first, "words").toStdString();
    EXPECT_EQ(text(first, "name"), QStringLiteral("20261105-220322"));
    EXPECT_TRUE(QFileInfo::exists(parent + "/20261105-220322/frames.csv"));
    EXPECT_GT(first.value(QStringLiteral("files")).toDouble(), 0.0);

    const QVariantMap second =
        exported(e, source, o, QUrl::fromLocalFile(parent));
    ASSERT_TRUE(ok(second)) << text(second, "words").toStdString();
    EXPECT_EQ(text(second, "name"), QStringLiteral("20261105-220322-2"));
    EXPECT_EQ(QDir(parent).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
              (QStringList{QStringLiteral("20261105-220322"),
                           QStringLiteral("20261105-220322-2")}));
}

TEST(RecordingExportTest, AFileReachesAProviderInChunksOverWhatItHeld)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    const QString out = dir.filePath(QStringLiteral("picked.csv"));
    writeFile(out, QByteArray(1024 * 1024 * 3, 'x')); // longer than the export
    const QVariantMap r =
        exported(e, fixture("pull-early"), Options{}, QUrl::fromLocalFile(out));
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    EXPECT_EQ(bytesOf(out),
              coreBytes(fixture("pull-early"), Options{},
                        dir.filePath(QStringLiteral("core.csv"))));
}

TEST(RecordingExportTest, AFolderCopyReachesAProviderFileByFile)
{
    QTemporaryDir dir;
    RecordingExport plain;
    RecordingExport provider;
    provider.setThroughProviderForTesting(true);
    const QString direct = dir.filePath(QStringLiteral("direct"));
    const QString picked = dir.filePath(QStringLiteral("picked"));
    ASSERT_TRUE(QDir().mkpath(direct));
    ASSERT_TRUE(QDir().mkpath(picked));
    const QString source = labeledCopy(dir, "findings");
    Options o;
    o.format = RecordingExport::Folder;
    ASSERT_TRUE(ok(exported(plain, source, o, QUrl::fromLocalFile(direct))));
    const QVariantMap r =
        exported(provider, source, o, QUrl::fromLocalFile(picked));
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    const QString copy = picked + "/20261105-220322";
    const QString whole = direct + "/20261105-220322";
    const QStringList files = QDir(whole).entryList(QDir::Files, QDir::Name);
    ASSERT_FALSE(files.isEmpty());
    EXPECT_EQ(QDir(copy).entryList(QDir::Files, QDir::Name), files);
    for (const QString& f : files) {
        EXPECT_EQ(bytesOf(copy + '/' + f), bytesOf(whole + '/' + f))
            << f.toStdString();
    }
}

TEST(RecordingExportTest, ARefusedExportLeavesAPickedDocumentGone)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    const QString picked = dir.filePath(QStringLiteral("picked.csv"));
    writeFile(picked, QByteArray());
    const QVariantMap r = exported(e, formulaRecording(dir), Options{},
                                   QUrl::fromLocalFile(picked));
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(text(r, "words").isEmpty());
    EXPECT_FALSE(text(r, "words").contains(dir.path()));
    EXPECT_TRUE(text(r, "line").startsWith(QStringLiteral("Export failed: ")));
    EXPECT_FALSE(QFileInfo::exists(picked)) << "the empty document goes";
}

/* Startup's sweep: an export's own temporary folder a day old goes, and
 * nothing else there, however near its name or however old */
TEST(RecordingExportTest, StartupRemovesOnlyStaleExportFolders)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QDir t(temp.path());
    const QStringList kept = {QStringLiteral("havne-export-Ab12C"),
                              QStringLiteral("havne-export-Ab12Cd7"),
                              QStringLiteral("havne-exporter-Ab12Cd"),
                              QStringLiteral("other")};
    for (const QString& n : kept) {
        ASSERT_TRUE(t.mkpath(n));
    }
    ASSERT_TRUE(t.mkpath(QStringLiteral("havne-export-Ab12Cd")));
    writeFile(t.filePath(QStringLiteral("havne-export-Ab12Cd/export")), "copy");
    writeFile(t.filePath(QStringLiteral("havne-export-Zz99Yy")), "a file");

    const QDateTime now = QDateTime::currentDateTimeUtc();
    EXPECT_EQ(RecordingExport::removeStaleCopies(temp.path(), now), 0);
    EXPECT_EQ(RecordingExport::removeStaleCopies(temp.path(),
                                                 now.addSecs(23 * 60 * 60)),
              0)
        << "under a day old";
    EXPECT_TRUE(t.exists(QStringLiteral("havne-export-Ab12Cd")));

    EXPECT_EQ(RecordingExport::removeStaleCopies(temp.path(), now.addDays(2)),
              1);
    EXPECT_FALSE(t.exists(QStringLiteral("havne-export-Ab12Cd")));
    for (const QString& n : kept) {
        EXPECT_TRUE(t.exists(n)) << n.toStdString();
    }
    EXPECT_TRUE(
        QFileInfo(t.filePath(QStringLiteral("havne-export-Zz99Yy"))).isFile())
        << "a file of that name stays";
}

/* Core's export written, the copy to the provider refused: the picked
 * document goes, as after core's refusal */
TEST(RecordingExportTest, ACopyAProviderRefusesLeavesThePickedDocumentGone)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    e.setFailCopyForTesting(true);
    const QString picked = dir.filePath(QStringLiteral("picked.csv"));
    writeFile(picked, QByteArray());
    const QVariantMap r = exported(e, fixture("pull-early"), Options{},
                                   QUrl::fromLocalFile(picked));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(text(r, "words"), QStringLiteral("Couldn't write the file"));
    EXPECT_FALSE(QFileInfo::exists(picked)) << "the empty document goes";
}

/* A folder copy whose first file a provider refuses leaves no folder of
 * its own behind */
TEST(RecordingExportTest, AFolderCopyAProviderRefusesLeavesNoFolder)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    e.setFailCopyForTesting(true);
    const QString picked = dir.filePath(QStringLiteral("picked"));
    ASSERT_TRUE(QDir().mkpath(picked));
    Options o;
    o.format = RecordingExport::Folder;
    const QVariantMap r = exported(e, labeledCopy(dir, "findings"), o,
                                   QUrl::fromLocalFile(picked));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(text(r, "words"), QStringLiteral("Couldn't write the folder"));
    EXPECT_TRUE(QDir(picked)
                    .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                    .isEmpty())
        << "the copy's folder goes";
}

TEST(RecordingExportTest, ARefusedExportLeavesALocalFileAsItWas)
{
    QTemporaryDir dir;
    RecordingExport e;
    const QString there = dir.filePath(QStringLiteral("there.csv"));
    writeFile(there, "kept");
    const QVariantMap r = exported(e, formulaRecording(dir), Options{},
                                   QUrl::fromLocalFile(there));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(bytesOf(there), QByteArray("kept"));
}

TEST(RecordingExportTest, AFileThatCantBeWrittenSaysSoInWords)
{
    QTemporaryDir dir;
    RecordingExport e;
    const QVariantMap r = exported(
        e, fixture("pull-early"), Options{},
        QUrl::fromLocalFile(dir.filePath(QStringLiteral("no-such/out.csv"))));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(text(r, "words"), QStringLiteral("Couldn't write the file"));
}

TEST(RecordingExportTest, OneExportAtATimeAndAKnownFormat)
{
    QTemporaryDir dir;
    RecordingExport e;
    const QUrl out = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.csv")));
    Options bad;
    bad.format = 9;
    EXPECT_FALSE(e.start(asMap(fixture("pull-early"), bad), out));
    EXPECT_FALSE(e.start(asMap(QString(), Options{}), out));
    EXPECT_FALSE(e.start(asMap(fixture("pull-early"), Options{}), QUrl()));
    EXPECT_FALSE(e.busy());

    bool done = false;
    QObject context;
    QObject::connect(&e, &RecordingExport::finished, &context,
                     [&](const QVariantMap&) { done = true; });
    ASSERT_TRUE(e.start(asMap(fixture("pull-early"), Options{}), out));
    EXPECT_FALSE(e.start(asMap(fixture("pull-early"), Options{}), out));
    EXPECT_TRUE(waitUntil([&] { return done; }));
}

TEST(RecordingExportTest, TheSizesAreTheFilesOwn)
{
    QTemporaryDir dir;
    RecordingExport e;
    const QVariantMap r = sized(e, fixture("pull-early"), {0, 250});
    ASSERT_TRUE(ok(r)) << text(r, "words").toStdString();
    const QVariantList sizes = r.value(QStringLiteral("sizes")).toList();
    ASSERT_EQ(sizes.size(), 2);
    EXPECT_GT(r.value(QStringLiteral("columns")).toInt(), 0);
    int i = 0;
    for (const int ms : {0, 250}) {
        const QVariantMap s = sizes[i++].toMap();
        EXPECT_EQ(s.value(QStringLiteral("rowMs")).toInt(), ms);
        Options o;
        o.rowMs = static_cast<quint32>(ms);
        const QString out = dir.filePath(QStringLiteral("w%1.csv").arg(ms));
        const QVariantMap written =
            exported(e, fixture("pull-early"), o, QUrl::fromLocalFile(out));
        EXPECT_EQ(s.value(QStringLiteral("bytes")).toDouble(),
                  static_cast<double>(QFileInfo(out).size()));
        EXPECT_EQ(s.value(QStringLiteral("rows")).toDouble(),
                  written.value(QStringLiteral("rows")).toDouble());
    }
}

TEST(RecordingExportTest, TheSizesTakeTheUnitsAndColumns)
{
    RecordingExport e;
    const QVariantMap all = sized(e, fixture("pull-early"), {0});
    const QVariantMap one = sized(e, fixture("pull-early"), {0}, 0,
                                  QStringList{QStringLiteral("RPM")});
    EXPECT_GT(all.value(QStringLiteral("columns")).toInt(), 1);
    EXPECT_EQ(one.value(QStringLiteral("columns")).toInt(), 1);
    const auto bytes = [](const QVariantMap& r) {
        return r.value(QStringLiteral("sizes"))
            .toList()
            .first()
            .toMap()
            .value(QStringLiteral("bytes"))
            .toDouble();
    };
    const QVariantMap imperial =
        sized(e, fixture("pull-early"), {0}, ECUIO_UNITS_IMPERIAL);
    EXPECT_NE(bytes(imperial), bytes(all));
}

TEST(RecordingExportTest, AFolderItCantSizeSaysSoInWords)
{
    QTemporaryDir dir;
    RecordingExport e;
    const QVariantMap r = sized(e, dir.path(), {0});
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(text(r, "words").isEmpty());
    EXPECT_FALSE(text(r, "words").contains(dir.path()));
}

TEST(RecordingExportTest, BadEstimateRequestsAreRefused)
{
    RecordingExport e;
    EXPECT_FALSE(e.estimate(QString(), asked({0}), 0, {}));
    EXPECT_FALSE(e.estimate(fixture("pull-early"), {}, 0, {}));
    EXPECT_FALSE(e.estimate(fixture("pull-early"),
                            QVariantList(65, QVariant(100)), 0, {}));
    EXPECT_FALSE(e.sizing());
}

TEST(RecordingExportTest, TheNewestEstimateWins)
{
    RecordingExport e;
    QList<QVariantMap> answers;
    QObject context;
    QObject::connect(&e, &RecordingExport::estimated, &context,
                     [&](const QVariantMap& r) { answers << r; });
    // No event runs between the calls, so the first is still running
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({10}), 0, {}));
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({20}), 0, {}));
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({30}), 0, {}));
    EXPECT_TRUE(e.sizing());
    EXPECT_TRUE(waitUntil([&] { return !e.sizing(); }));
    ASSERT_EQ(answers.size(), 1);
    EXPECT_EQ(answers[0]
                  .value(QStringLiteral("sizes"))
                  .toList()
                  .first()
                  .toMap()
                  .value(QStringLiteral("rowMs"))
                  .toInt(),
              30);
}

TEST(RecordingExportTest, ADroppedEstimateSaysNothing)
{
    RecordingExport e;
    int answers = 0;
    QObject context;
    QObject::connect(&e, &RecordingExport::estimated, &context,
                     [&](const QVariantMap&) { ++answers; });
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({10}), 0, {}));
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({20}), 0, {}));
    e.dropEstimate();
    EXPECT_TRUE(waitUntil([&] { return !e.sizing(); }));
    EXPECT_EQ(answers, 0);

    const QVariantMap again = sized(e, fixture("pull-early"), {0});
    EXPECT_TRUE(ok(again)) << "it works after one is dropped";
}

TEST(RecordingExportTest, AWorkerOutOfMemorySaysSoAndFreesTheNext)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setFailWorkerForTesting(true);
    const QUrl out = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.csv")));
    const QVariantMap r = exported(e, fixture("pull-early"), Options{}, out);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(text(r, "words"), QStringLiteral("out of memory"));
    EXPECT_EQ(text(r, "folder"), fixture("pull-early"));
    EXPECT_TRUE(text(r, "line").startsWith(QStringLiteral("Export failed: ")));
    const QVariantMap s = sized(e, fixture("pull-early"), {0});
    EXPECT_FALSE(ok(s));
    EXPECT_EQ(text(s, "words"), QStringLiteral("out of memory"));

    e.setFailWorkerForTesting(false);
    EXPECT_TRUE(ok(exported(e, fixture("pull-early"), Options{}, out)));
    EXPECT_TRUE(ok(sized(e, fixture("pull-early"), {0})));
}

TEST(RecordingExportTest, DestroyedWithAnExportRunningItWaits)
{
    QTemporaryDir dir;
    const QString out = dir.filePath(QStringLiteral("a.csv"));
    {
        RecordingExport e;
        ASSERT_TRUE(e.start(asMap(fixture("pull-early"), Options{}),
                            QUrl::fromLocalFile(out)));
        ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({0}), 0, {}));
    }
    EXPECT_TRUE(QFileInfo::exists(out));
}
