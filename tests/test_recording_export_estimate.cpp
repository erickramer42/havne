// File/layer: tests/test_recording_export_estimate.cpp | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: RecordingExportTest (test_bridge)

#include "test_recording_export_support.h"

namespace {

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

} // namespace

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

/* The panel's own case: one estimate out, the format switched. Nothing
 * waits behind it, so only the drop's generation keeps its answer from
 * being said */
TEST(RecordingExportTest, ADroppedEstimateWithNoneWaitingSaysNothing)
{
    RecordingExport e;
    int answers = 0;
    QObject context;
    QObject::connect(&e, &RecordingExport::estimated, &context,
                     [&](const QVariantMap&) { ++answers; });
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({10}), 0, {}));
    e.dropEstimate();
    EXPECT_TRUE(waitUntil([&] { return !e.sizing(); }));
    EXPECT_EQ(answers, 0);
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

// Longer than any wait a stop reaching the workers takes
constexpr int kHoldMs = 20000;

using Hold = RecordingExport::HoldForTesting;

/* Destroyed with an export and a sizing held at core's first ask, as long
 * ones would be: the destructor's stop ends both long before the hold
 * would, and nothing is written */
TEST(RecordingExportTest, DestroyedMidExportItStopsBothWorkers)
{
    QTemporaryDir dir;
    const QString out = dir.filePath(QStringLiteral("a.csv"));
    QElapsedTimer t;
    {
        RecordingExport e;
        e.setHoldForTesting(Hold::FirstAsk, kHoldMs);
        ASSERT_TRUE(e.start(asMap(fixture("pull-early"), Options{}),
                            QUrl::fromLocalFile(out)));
        ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({0}), 0, {}));
        ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 2; }))
            << e.heldForTesting() << " of 2 held";
        t.start();
    }
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    EXPECT_FALSE(QFileInfo::exists(out));
    EXPECT_EQ(
        QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
        QStringList());
}

/* Destroyed at core's first ask of a local folder copy, which core makes
 * in place with no copy of the app's after it: core's own stop leaves no
 * copy */
TEST(RecordingExportTest, DestroyedAsALocalFolderCopyBeginsLeavesNoCopy)
{
    QTemporaryDir dir;
    QElapsedTimer t;
    {
        RecordingExport e;
        e.setHoldForTesting(Hold::FirstAsk, kHoldMs);
        Options o;
        o.format = RecordingExport::Folder;
        ASSERT_TRUE(e.start(asMap(fixture("pull-early"), o),
                            QUrl::fromLocalFile(dir.path())));
        ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 1; }));
        t.start();
    }
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    EXPECT_EQ(
        QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
        QStringList());
}

/* Destroyed after core's export, before a provider's copy: the copy
 * stops at its first chunk and the document the picker made (empty) goes */
TEST(RecordingExportTest, DestroyedBeforeAProvidersCopyItCopiesNothing)
{
    QTemporaryDir dir;
    const QString out = dir.filePath(QStringLiteral("a.csv"));
    writeFile(out, QByteArray());
    QElapsedTimer t;
    {
        RecordingExport e;
        e.setThroughProviderForTesting(true);
        e.setHoldForTesting(Hold::BeforeCopy, kHoldMs);
        ASSERT_TRUE(e.start(asMap(fixture("pull-early"), Options{}),
                            QUrl::fromLocalFile(out)));
        ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 1; }));
        t.start();
    }
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    EXPECT_FALSE(QFileInfo::exists(out));
}

/* The same for a folder copy through a provider: no copy is left */
TEST(RecordingExportTest, DestroyedBeforeAProvidersFolderCopyLeavesNoCopy)
{
    QTemporaryDir dir;
    QElapsedTimer t;
    {
        RecordingExport e;
        e.setThroughProviderForTesting(true);
        e.setHoldForTesting(Hold::BeforeCopy, kHoldMs);
        Options o;
        o.format = RecordingExport::Folder;
        ASSERT_TRUE(e.start(asMap(fixture("pull-early"), o),
                            QUrl::fromLocalFile(dir.path())));
        ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 1; }));
        t.start();
    }
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    EXPECT_EQ(
        QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
        QStringList());
}

// Every finished() an export says
struct Finished {
    QList<QVariantMap> results;
    QObject context;
    explicit Finished(RecordingExport& e)
    {
        QObject::connect(&e, &RecordingExport::finished, &context,
                         [this](const QVariantMap& r) { results << r; });
    }
};

/* Canceled at core's first ask: it ends well under the hold, says it was
 * canceled and not that it failed, writes nothing, and the next export runs
 * whole. A cancel with none running is nothing */
TEST(RecordingExportTest, ACanceledExportSaysSoAndWritesNothing)
{
    QTemporaryDir dir;
    const QString out = dir.filePath(QStringLiteral("a.csv"));
    RecordingExport e;
    Finished done(e);
    e.cancel();
    e.setHoldForTesting(Hold::FirstAsk, kHoldMs);
    ASSERT_TRUE(e.start(asMap(fixture("pull-early"), Options{}),
                        QUrl::fromLocalFile(out)));
    ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 1; }));
    QElapsedTimer t;
    t.start();
    e.cancel();
    ASSERT_TRUE(
        waitUntil([&] { return !done.results.isEmpty(); }, kHoldMs / 2));
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    ASSERT_EQ(done.results.size(), 1);
    const QVariantMap r = done.results[0];
    EXPECT_FALSE(ok(r));
    EXPECT_TRUE(r.value(QStringLiteral("canceled")).toBool());
    EXPECT_EQ(text(r, "words").toStdString(), "The export was canceled");
    EXPECT_EQ(text(r, "line").toStdString(), "Export canceled");
    EXPECT_EQ(text(r, "folder"), fixture("pull-early"));
    EXPECT_FALSE(e.busy());
    EXPECT_EQ(
        QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
        QStringList());

    e.setHoldForTesting(Hold::None, 0);
    const QVariantMap again =
        exported(e, fixture("pull-early"), Options{}, QUrl::fromLocalFile(out));
    EXPECT_TRUE(ok(again)) << text(again, "words").toStdString();
    EXPECT_FALSE(again.value(QStringLiteral("canceled")).toBool());
    EXPECT_FALSE(bytesOf(out).isEmpty());
}

/* Canceled after core's export, before a provider's copy: the copy stops
 * at its first chunk and the document the picker made (empty) goes */
TEST(RecordingExportTest, ACancelBeforeAProvidersCopyCopiesNothing)
{
    QTemporaryDir dir;
    const QString out = dir.filePath(QStringLiteral("a.csv"));
    writeFile(out, QByteArray());
    RecordingExport e;
    Finished done(e);
    e.setThroughProviderForTesting(true);
    e.setHoldForTesting(Hold::BeforeCopy, kHoldMs);
    ASSERT_TRUE(e.start(asMap(fixture("pull-early"), Options{}),
                        QUrl::fromLocalFile(out)));
    ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 1; }));
    QElapsedTimer t;
    t.start();
    e.cancel();
    ASSERT_TRUE(
        waitUntil([&] { return !done.results.isEmpty(); }, kHoldMs / 2));
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    EXPECT_TRUE(done.results[0].value(QStringLiteral("canceled")).toBool());
    EXPECT_FALSE(QFileInfo::exists(out));
}

/* A newer sizing stops the one running at core's next ask, and is the only
 * one answered, whole */
TEST(RecordingExportTest, ANewerSizingStopsTheOneRunning)
{
    RecordingExport e;
    QList<QVariantMap> answers;
    QObject context;
    QObject::connect(&e, &RecordingExport::estimated, &context,
                     [&](const QVariantMap& r) { answers << r; });
    e.setHoldForTesting(Hold::FirstAsk, kHoldMs);
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({10}), 0, {}));
    ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 1; }));
    e.setHoldForTesting(Hold::None, 0); // the newer one is held nowhere
    QElapsedTimer t;
    t.start();
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({20}), 0, {}));
    ASSERT_TRUE(waitUntil([&] { return !answers.isEmpty(); }, kHoldMs / 2));
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    ASSERT_TRUE(waitUntil([&e] { return !e.sizing(); }));
    ASSERT_EQ(answers.size(), 1);
    ASSERT_TRUE(ok(answers[0])) << text(answers[0], "words").toStdString();
    EXPECT_EQ(answers[0]
                  .value(QStringLiteral("sizes"))
                  .toList()
                  .first()
                  .toMap()
                  .value(QStringLiteral("rowMs"))
                  .toInt(),
              20);
}

/* A sizing dropped while it runs stops at core's next ask and says
 * nothing; the next one runs whole */
TEST(RecordingExportTest, ADroppedSizingStopsAtTheNextAsk)
{
    RecordingExport e;
    int answers = 0;
    QObject context;
    QObject::connect(&e, &RecordingExport::estimated, &context,
                     [&](const QVariantMap&) { ++answers; });
    e.setHoldForTesting(Hold::FirstAsk, kHoldMs);
    ASSERT_TRUE(e.estimate(fixture("pull-early"), asked({10}), 0, {}));
    ASSERT_TRUE(waitUntil([&e] { return e.heldForTesting() == 1; }));
    QElapsedTimer t;
    t.start();
    e.dropEstimate();
    ASSERT_TRUE(waitUntil([&e] { return !e.sizing(); }, kHoldMs / 2));
    EXPECT_LT(t.elapsed(), kHoldMs / 4);
    EXPECT_EQ(answers, 0);

    e.setHoldForTesting(Hold::None, 0);
    const QVariantMap again = sized(e, fixture("pull-early"), {0});
    EXPECT_TRUE(ok(again)) << text(again, "words").toStdString();
}
