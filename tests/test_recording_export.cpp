// File/layer: tests/test_recording_export.cpp | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: RecordingExportTest (test_bridge)
//
// The Recordings viewer's Export (app/recordingexport.{h,cpp}): each file
// compared byte for byte with what core's ecuio_export writes for the same
// options, the sizes with core's, and what the class adds itself: the copy to
// the place picked, a provider's document, a folder copy's name, and its
// stops: a cancel, a sizing superseded or dropped, the object's end.

#include "test_recording_export_support.h"

namespace {

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
    EXPECT_TRUE(bytesOf(out).startsWith("\xEF\xBB\xBF" "Time (ms),"));
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
    EXPECT_TRUE(bytes.startsWith("\xEF\xBB\xBF" "Time (ms),Coolant "))
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

/* A document the user chose to replace holds their bytes: a refused export,
 * a refused start and a thread that can't start leave it as it was */
TEST(RecordingExportTest, ARefusedExportLeavesAReplacedDocumentAlone)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    const QString picked = dir.filePath(QStringLiteral("picked.csv"));
    const QByteArray theirs("the user's earlier export\n");
    writeFile(picked, theirs);

    const QVariantMap r = exported(e, formulaRecording(dir), Options{},
                                   QUrl::fromLocalFile(picked));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(bytesOf(picked), theirs) << "a refused export";

    EXPECT_FALSE(
        e.start(asMap(QString(), Options{}), QUrl::fromLocalFile(picked)));
    EXPECT_EQ(bytesOf(picked), theirs) << "an empty folder";

    Options odd;
    odd.format = 99;
    EXPECT_FALSE(e.start(asMap(fixture("pull-early"), odd),
                         QUrl::fromLocalFile(picked)));
    EXPECT_EQ(bytesOf(picked), theirs) << "a bad format";

    e.setFailThreadForTesting(true);
    EXPECT_FALSE(e.start(asMap(fixture("pull-early"), Options{}),
                         QUrl::fromLocalFile(picked)));
    EXPECT_EQ(bytesOf(picked), theirs) << "a thread that can't start";
    e.setFailThreadForTesting(false);
}

/* A save pick's verdict decides (healthreport.h): an empty document the user
 * chose to replace stays through a refused export and a refused start, the
 * refusal takes the verdict (a later refusal with none goes by the read), and
 * one the pick made goes */
TEST(RecordingExportTest, ARefusedExportFollowsTheSavePicksVerdict)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    const QString source = formulaRecording(dir);
    const QString picked = dir.filePath(QStringLiteral("picked.csv"));
    const QUrl url = QUrl::fromLocalFile(picked);
    writeFile(picked, QByteArray());

    notePick(pathOfUrl(url), PickMade::Replaced);
    EXPECT_FALSE(ok(exported(e, source, Options{}, url)));
    EXPECT_TRUE(QFileInfo::exists(picked)) << "a refused export";

    notePick(pathOfUrl(url), PickMade::Replaced);
    EXPECT_FALSE(e.start(asMap(QString(), Options{}), url));
    EXPECT_TRUE(QFileInfo::exists(picked)) << "a refused start";

    EXPECT_FALSE(e.start(asMap(QString(), Options{}), url));
    EXPECT_FALSE(QFileInfo::exists(picked))
        << "the earlier refusal took the verdict: the empty document goes";

    writeFile(picked, QByteArray());
    notePick(pathOfUrl(url), PickMade::Made);
    EXPECT_FALSE(ok(exported(e, source, Options{}, url)));
    EXPECT_FALSE(QFileInfo::exists(picked)) << "the pick's own goes";
}

/* A copy that fails after it wrote some bytes: a document empty when the
 * export began goes, one that held bytes survives (cut by the copy's own
 * truncation, which a temporary file first would avoid) */
TEST(RecordingExportTest, AFailureAfterBytesWereWrittenJudgesTheDocumentAtEntry)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    e.setFailCopyAfterBytesForTesting(true);

    const QString fresh = dir.filePath(QStringLiteral("fresh.csv"));
    writeFile(fresh, QByteArray());
    EXPECT_FALSE(ok(exported(e, fixture("pull-early"), Options{},
                             QUrl::fromLocalFile(fresh))));
    EXPECT_FALSE(QFileInfo::exists(fresh)) << "an empty one goes, part written";

    const QString theirs = dir.filePath(QStringLiteral("theirs.csv"));
    writeFile(theirs, QByteArray("the user's earlier export\n"));
    EXPECT_FALSE(ok(exported(e, fixture("pull-early"), Options{},
                             QUrl::fromLocalFile(theirs))));
    EXPECT_TRUE(QFileInfo::exists(theirs)) << "one that held bytes stays";
}

/* A start that refuses (a start under way, no folder, a format it doesn't
 * know) or whose thread can't start leaves no picked document, as a failed
 * export does; a folder target is never removed */
TEST(RecordingExportTest, ARefusedStartLeavesAPickedDocumentGone)
{
    QTemporaryDir dir;
    RecordingExport e;
    e.setThroughProviderForTesting(true);
    const QString picked = dir.filePath(QStringLiteral("picked.csv"));
    const auto pick = [&] { writeFile(picked, QByteArray()); };

    pick();
    EXPECT_FALSE(
        e.start(asMap(QString(), Options{}), QUrl::fromLocalFile(picked)))
        << "an empty folder";
    EXPECT_FALSE(QFileInfo::exists(picked)) << "no folder";

    pick();
    Options odd;
    odd.format = 99;
    EXPECT_FALSE(e.start(asMap(fixture("pull-early"), odd),
                         QUrl::fromLocalFile(picked)));
    EXPECT_FALSE(QFileInfo::exists(picked)) << "a bad format";

    pick();
    e.setFailThreadForTesting(true);
    EXPECT_FALSE(e.start(asMap(fixture("pull-early"), Options{}),
                         QUrl::fromLocalFile(picked)));
    EXPECT_FALSE(e.busy());
    EXPECT_FALSE(QFileInfo::exists(picked)) << "a thread that can't start";
    e.setFailThreadForTesting(false);

    const QString folder = dir.filePath(QStringLiteral("target"));
    ASSERT_TRUE(QDir().mkpath(folder));
    Options copy;
    copy.format = RecordingExport::Folder;
    EXPECT_FALSE(e.start(asMap(QString(), copy), QUrl::fromLocalFile(folder)));
    EXPECT_TRUE(QFileInfo(folder).isDir()) << "a folder target stays";

    // A start under way (busy until its result is taken) refuses another,
    // which leaves its own document
    Options o;
    pick();
    const QString second = dir.filePath(QStringLiteral("second.csv"));
    writeFile(second, QByteArray());
    ASSERT_TRUE(
        e.start(asMap(fixture("pull-early"), o), QUrl::fromLocalFile(picked)));
    EXPECT_FALSE(
        e.start(asMap(fixture("pull-early"), o), QUrl::fromLocalFile(second)));
    EXPECT_FALSE(QFileInfo::exists(second)) << "the second pick";
}

/* Startup's sweep: the temporary folder of an export, an import or a health
 * report a day old goes, and nothing else there, however near its name or
 * however old */
TEST(RecordingExportTest, StartupRemovesOnlyStaleCopies)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QDir t(temp.path());
    const QStringList kept = {QStringLiteral("havne-export-Ab12C"),
                              QStringLiteral("havne-export-Ab12Cd7"),
                              QStringLiteral("havne-exporter-Ab12Cd"),
                              QStringLiteral("havne-imports-Ab12Cd"),
                              QStringLiteral("havne-other-Ab12Cd"),
                              QStringLiteral("other")};
    for (const QString& n : kept) {
        ASSERT_TRUE(t.mkpath(n));
    }
    const QStringList stale = {QStringLiteral("havne-export-Ab12Cd"),
                               QStringLiteral("havne-import-Ab12Cd"),
                               QStringLiteral("havne-health-Ab12Cd")};
    for (const QString& n : stale) {
        ASSERT_TRUE(t.mkpath(n));
        writeFile(t.filePath(n + QStringLiteral("/copy")), "copy");
    }
    writeFile(t.filePath(QStringLiteral("havne-export-Zz99Yy")), "a file");

    const QDateTime now = QDateTime::currentDateTimeUtc();
    EXPECT_EQ(RecordingExport::removeStaleCopies(temp.path(), now), 0);
    EXPECT_EQ(RecordingExport::removeStaleCopies(temp.path(),
                                                 now.addSecs(23 * 60 * 60)),
              0)
        << "under a day old";
    for (const QString& n : stale) {
        EXPECT_TRUE(t.exists(n)) << n.toStdString();
    }

    EXPECT_EQ(RecordingExport::removeStaleCopies(temp.path(), now.addDays(2)),
              3);
    for (const QString& n : stale) {
        EXPECT_FALSE(t.exists(n)) << n.toStdString();
    }
    for (const QString& n : kept) {
        EXPECT_TRUE(t.exists(n)) << n.toStdString();
    }
    EXPECT_TRUE(
        QFileInfo(t.filePath(QStringLiteral("havne-export-Zz99Yy"))).isFile())
        << "a file of that name stays";
}

// The summary counts a scrubbed export's masks and the frames of the
// messages it left out for a VIN-shaped token (made up here)
TEST(RecordingExportTest, TheSummaryCountsTheVinShapedMasksAndMessages)
{
    QTemporaryDir dir;
    const QString folder = dir.filePath(QStringLiteral("glued"));
    ASSERT_TRUE(QDir().mkpath(folder));
    writeFile(folder + QStringLiteral("/signals.csv"),
              "host_us,signal,value,unit\n100,PN_ZZZFAKE11M0000001x,1,rpm\n");
    const QByteArray glued = "aZZZFAKE11M0000001";
    writeFile(folder + QStringLiteral("/frames.csv"),
              "host_us,timestamp_us,id,flags,data\n100,100,0x7E8,0x" +
                  QByteArray::number(ECUIO_FRAME_FLAG_REASSEMBLED, 16) +
                  ",62 D8 01 " + glued.toHex(' ').toUpper() + "\n");
    RecordingExport e;
    Options csv;
    csv.longLayout = true;
    const QVariantMap c = exported(e, folder, csv,
                                   QUrl::fromLocalFile(dir.filePath(
                                       QStringLiteral("long.csv"))));
    ASSERT_TRUE(ok(c)) << text(c, "words").toStdString();
    EXPECT_TRUE(text(c, "summary").endsWith(
        QStringLiteral(", left out: 1 VIN-shaped token masked")))
        << text(c, "summary").toStdString();
    Options pcapng;
    pcapng.format = RecordingExport::Pcapng;
    const QVariantMap p = exported(e, folder, pcapng,
                                   QUrl::fromLocalFile(dir.filePath(
                                       QStringLiteral("frames.pcapng"))));
    ASSERT_TRUE(ok(p)) << text(p, "words").toStdString();
    EXPECT_TRUE(text(p, "summary").endsWith(
        QStringLiteral("left out: 1 frame of a VIN-shaped message")))
        << text(p, "summary").toStdString();
}
