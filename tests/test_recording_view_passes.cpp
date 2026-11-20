// File/layer: tests/test_recording_view_passes.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "test_recording_view_support.h"

namespace {

// The column means answers that arrive, with their request numbers, and
// whether a statistics answer came, for the replacement cases
struct MeansWatch {
    QObject context; // its connections end with it
    QList<int> requests;
    std::vector<QList<double>> got;
    bool statsDone = false;
    explicit MeansWatch(RecordingView& v)
    {
        QObject::connect(&v, &RecordingView::columnMeansReady, &context,
                         [this](int request, const QVariantList& m) {
                             requests << request;
                             for (const QVariant& one : m) {
                                 got.push_back(
                                     one.toMap()
                                         .value(QStringLiteral("means"))
                                         .value<QList<double>>());
                             }
                         });
        QObject::connect(
            &v, &RecordingView::statsReady, &context,
            [this](int, const QVariantList&) { statsDone = true; });
    }
};

} // namespace

// A newer means ask that arrives while the older pass has given way to a
// statistics ask, before it resumes, ends it at its next column: only the
// newer ask is answered, over its own range
TEST(RecordingViewTest, ANewerMeansAskReplacesAPassThatGaveWay)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir, 120, 100);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 120000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QVariantList ids{0};
    const std::vector<QList<double>> alone =
        askedMeans(v, ids, 1.0e6, 2.0e6, 50);
    MeansWatch w(v);
    // Held in the turn that takes the pass and again in the one that takes
    // the statistics it gave way to: the newer ask lands in the second hold
    const int hold = 200;
    v.setAsksHoldForTesting(hold);
    ASSERT_GT(v.columnMeans(ids, 0, 6.0e6, 65536), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    ASSERT_GT(v.stats(ids, 2.9e6, 3.1e6), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 1; }));
    ASSERT_EQ(v.meansYieldsForTesting(), 1);
    const int stopped = v.meansStoppedForTesting();
    v.setAsksHoldForTesting(0);
    const int newer = v.columnMeans(ids, 1.0e6, 2.0e6, 50);
    ASSERT_GT(newer, 0);
    ASSERT_TRUE(waitFor([&] { return !w.requests.isEmpty(); }, 120000));
    QThread::msleep(300);
    QCoreApplication::processEvents();
    EXPECT_TRUE(w.statsDone);
    ASSERT_EQ(w.requests.size(), 1) << "the older pass answered too";
    EXPECT_EQ(w.requests[0], newer);
    ASSERT_EQ(w.got.size(), 1u);
    EXPECT_EQ(w.got[0], alone[0]);
    EXPECT_EQ(v.meansStoppedForTesting(), stopped + 1)
        << "the older pass read on to its end";
    EXPECT_EQ(v.readsStoppedForTesting(), 0) << "no close ended it";
}

// A newer means ask that arrives while the task waits to take the
// statistics ask a pass gave way to replaces the pass in that take: the
// older pass never resumes, and only the newer ask is answered
TEST(RecordingViewTest, ANewerMeansAskTakenWithTheAskAPassGaveWayToReplacesIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir, 120, 100);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 120000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QVariantList ids{0};
    const std::vector<QList<double>> alone =
        askedMeans(v, ids, 1.0e6, 2.0e6, 50);
    MeansWatch w(v);
    v.setYieldHoldForTesting(300);
    v.setAsksHoldForTesting(100);
    ASSERT_GT(v.columnMeans(ids, 0, 6.0e6, 65536), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    v.setAsksHoldForTesting(0);
    ASSERT_GT(v.stats(ids, 2.9e6, 3.1e6), 0);
    // The pass has given way, and the task waits before its next take
    ASSERT_TRUE(waitFor([&] { return v.meansYieldsForTesting() > 0; }));
    const int stopped = v.meansStoppedForTesting();
    const int newer = v.columnMeans(ids, 1.0e6, 2.0e6, 50);
    ASSERT_GT(newer, 0);
    ASSERT_TRUE(waitFor([&] { return !w.requests.isEmpty(); }, 120000));
    QThread::msleep(300);
    QCoreApplication::processEvents();
    EXPECT_TRUE(w.statsDone);
    ASSERT_EQ(w.requests.size(), 1) << "the older pass answered too";
    EXPECT_EQ(w.requests[0], newer);
    ASSERT_EQ(w.got.size(), 1u);
    EXPECT_EQ(w.got[0], alone[0]);
    EXPECT_EQ(v.meansYieldsForTesting(), 1);
    EXPECT_EQ(v.meansStoppedForTesting(), stopped + 1)
        << "the replaced pass went uncounted";
    EXPECT_EQ(v.readsStoppedForTesting(), 0) << "no close ended it";
}

// A newer stretches ask ends an older pass under way at its next second: it
// has no answer and doesn't keep the pool busy for a whole pass
TEST(RecordingViewTest, ANewerStretchesAskStalesTheOlderPass)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir, 120, 100);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 120000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    QList<int> requests;
    QList<int> counts;
    QObject::connect(&v, &RecordingView::stretchesReady,
                     [&](int request, const QVariantList& list, bool) {
                         requests << request;
                         counts << list.size();
                     });
    // Held at the start, so the first pass meets the second ask before it
    // reads
    v.setStretchesHoldForTesting(200);
    const int older = v.stretches(true, false);
    const int newer = v.stretches(true, false);
    ASSERT_GT(older, 0);
    ASSERT_GT(newer, older);
    ASSERT_TRUE(waitFor([&] { return !requests.isEmpty(); }, 120000));
    EXPECT_EQ(v.stretchesStaledForTesting(), 1)
        << "the older pass read on to its end";
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0], newer);
    EXPECT_GE(counts[0], 20);
}

// A stretches ask whose pass can't start leaves the pass under way alone:
// it reads on and is answered
TEST(RecordingViewTest, AStretchesAskThatCantStartLeavesTheOlderPassAlone)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir, 120, 100);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 120000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    QList<int> requests;
    QList<int> counts;
    QObject::connect(&v, &RecordingView::stretchesReady,
                     [&](int request, const QVariantList& list, bool) {
                         requests << request;
                         counts << list.size();
                     });
    v.setStretchesHoldForTesting(200);
    const int older = v.stretches(true, false);
    ASSERT_GT(older, 0);
    v.setFailThreadForTesting(true);
    EXPECT_EQ(v.stretches(true, false), 0);
    v.setFailThreadForTesting(false);
    ASSERT_TRUE(waitFor([&] { return !requests.isEmpty(); }, 120000))
        << "the pass under way was ended";
    EXPECT_EQ(v.stretchesStaledForTesting(), 0);
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0], older);
    EXPECT_GE(counts[0], 20);
}

// A close that lands while a means pass that gave way is in hand, before it
// resumes, ends it there: no answer comes, and the open's stop counts it
TEST(RecordingViewTest, ACloseEndsAMeansPassThatGaveWay)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir, 120, 100);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 120000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QVariantList ids{0, 1, 2};
    int answers = 0;
    QObject::connect(&v, &RecordingView::statsReady,
                     [&](int, const QVariantList&) { ++answers; });
    QObject::connect(&v, &RecordingView::columnMeansReady,
                     [&](int, const QVariantList&) { ++answers; });
    const int yieldHold = 300;
    v.setYieldHoldForTesting(yieldHold);
    v.setAsksHoldForTesting(100);
    ASSERT_GT(v.columnMeans(ids, 0, 6.0e6, 65536), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    v.setAsksHoldForTesting(0);
    ASSERT_GT(v.stats(ids, 2.9e6, 3.1e6), 0);
    // The pass has given way, and the task waits before its next take
    ASSERT_TRUE(waitFor([&] { return v.meansYieldsForTesting() > 0; }));
    EXPECT_EQ(v.readsStoppedForTesting(), 0) << "nothing stopped yet";
    v.close();
    ASSERT_TRUE(waitFor([&] { return v.readsStoppedForTesting() > 0; },
                        yieldHold + 5000))
        << "the pass read on after the close";
    QThreadPool::globalInstance()->waitForDone(10000);
    QThread::msleep(50);
    QCoreApplication::processEvents();
    EXPECT_EQ(answers, 0);
    EXPECT_EQ(v.readsStoppedForTesting(), 1)
        << "the pass alone: the statistics ask went with the close";
}

// A report thread the system can't start leaves the bytes of a document the
// user chose to replace where they were: only the picker's empty one goes
TEST(RecordingViewTest, AReportWhoseThreadCantStartKeepsTheUsersDocument)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("the user's earlier report");
    }
    v.setFailThreadForTesting(true);
    EXPECT_FALSE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    QFile f(picked);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "the user's document went";
    EXPECT_EQ(f.readAll(), QByteArray("the user's earlier report"));
}

// A CSV import holds values alone, and Save report stays offered on it:
// core's report takes a folder with no frames.csv, which has no ECU and
// says so, so the save ends and succeeds rather than failing on frames
TEST(RecordingViewTest, AnImportedCsvsReportEndsWithNoEcu)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = importedCsv(dir);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    ASSERT_FALSE(v.hasFrames());

    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    const QUrl url =
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/report.json"));
    ASSERT_TRUE(v.saveHealthReport(url, 1));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(v.reporting());
    const QVariantMap r = saved[0];
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("ecus")).toInt(), 0);
    EXPECT_EQ(r.value(QStringLiteral("line")).toString().toStdString(),
              "Health report saved: 0 ECUs, 0 trouble codes, 0 key values (0 "
              "identification items left out)");

    // The report says why it has no ECU, in the viewer's words, and is
    // the whole import's: its raw profile ranks nothing, so no key values
    QFile f(url.toLocalFile());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    EXPECT_TRUE(doc.value(QStringLiteral("ecus")).toArray().isEmpty());
    EXPECT_EQ(doc.value(QStringLiteral("ecus_note")).toString(),
              QStringLiteral("the recording has no frames"));
    EXPECT_TRUE(doc.value(QStringLiteral("key_values")).toArray().isEmpty());
    const QJsonObject source = doc.value(QStringLiteral("source")).toObject();
    EXPECT_EQ(source.value(QStringLiteral("kind")).toString(),
              QStringLiteral("recording"));
    EXPECT_FALSE(source.value(QStringLiteral("frames")).toBool(true));
    EXPECT_EQ(source.value(QStringLiteral("from_s")).toDouble(-1), 0.0);
    EXPECT_EQ(source.value(QStringLiteral("to_s")).toDouble(-1), 1.75);
}

// The conditions fixture's cond lines as the viewer's entries: each ON
// span between its ON and OFF lines, and the stale stretch while OFF to
// the recording's end, by start, with names and levels from the
// recording's own conditions.json
TEST(RecordingViewTest, AConditionsOnAndOffLinesPairIntoSpans)
{
    RecordingView v;
    v.open(fixture("conditions"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(v.info().value(QStringLiteral("conditionEvents")).toDouble(),
              7.0);
    EXPECT_FALSE(v.info().value(QStringLiteral("conditionsCut")).toBool());
    EXPECT_EQ(v.info().value(QStringLiteral("conditionEventsLost")).toDouble(),
              0.0);
    EXPECT_EQ(v.info().value(QStringLiteral("conditionSpanCount")).toDouble(),
              3.0);
    EXPECT_FALSE(v.info().value(QStringLiteral("conditionSpansCut")).toBool());

    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), 3);
    const QVariantMap& coolant = spans[0];
    EXPECT_EQ(coolant.value(QStringLiteral("key")).toString(),
              QStringLiteral("coolant_high"));
    EXPECT_EQ(coolant.value(QStringLiteral("name")).toString(),
              QStringLiteral("Coolant high"));
    EXPECT_EQ(coolant.value(QStringLiteral("level")).toInt(),
              static_cast<int>(ECUIO_ALERT_WARNING));
    EXPECT_TRUE(coolant.value(QStringLiteral("on")).toBool());
    EXPECT_EQ(coolant.value(QStringLiteral("fromMs")).toDouble(), 7251.0);
    EXPECT_EQ(coolant.value(QStringLiteral("toMs")).toDouble(), 17501.0);
    EXPECT_FALSE(coolant.value(QStringLiteral("heldStart")).toBool());
    EXPECT_FALSE(coolant.value(QStringLiteral("openEnd")).toBool());
    EXPECT_TRUE(staleOf(coolant).isEmpty());

    // A condition with mark and no alert has no level
    const QVariantMap& rpm = spans[1];
    EXPECT_EQ(rpm.value(QStringLiteral("key")).toString(),
              QStringLiteral("rpm_high"));
    EXPECT_EQ(rpm.value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine speed high"));
    EXPECT_EQ(rpm.value(QStringLiteral("level")).toInt(),
              static_cast<int>(ECUIO_ALERT_NONE));
    EXPECT_TRUE(rpm.value(QStringLiteral("on")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("fromMs")).toDouble(), 12876.0);
    EXPECT_EQ(rpm.value(QStringLiteral("toMs")).toDouble(), 13376.0);
    EXPECT_FALSE(rpm.value(QStringLiteral("openEnd")).toBool());

    // OFF, then stale to the end: no new data to judge it by
    const QVariantMap& stale = spans[2];
    EXPECT_EQ(stale.value(QStringLiteral("key")).toString(),
              QStringLiteral("rpm_high"));
    EXPECT_FALSE(stale.value(QStringLiteral("on")).toBool());
    EXPECT_EQ(stale.value(QStringLiteral("fromMs")).toDouble(), 26876.0);
    EXPECT_EQ(stale.value(QStringLiteral("toMs")).toDouble(), v.durationMs());
    EXPECT_TRUE(stale.value(QStringLiteral("openEnd")).toBool());
    EXPECT_EQ(staleOf(stale),
              (QList<std::pair<double, double>>{{26876.0, v.durationMs()}}));

    // Closed, the entries go with the rest
    v.close();
    EXPECT_TRUE(v.conditionSpans().isEmpty());
}
