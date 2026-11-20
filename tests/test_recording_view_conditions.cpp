// File/layer: tests/test_recording_view_conditions.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "test_recording_view_support.h"

namespace {

// One cond line at host_us, as the recorder writes it
QByteArray condLine(const char* key, bool on, const char* flags,
                    uint64_t host_us)
{
    const double s = static_cast<double>(host_us) / 1e6;
    const auto ms = static_cast<int>(host_us / 1000);
    return QStringLiteral("09:%1:%2.%3  +%4  cond  %5,%6,%7,0,0,%8,%9\n")
        .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'))
        .arg(s, 0, 'f', 3)
        .arg(QLatin1String(key), QLatin1String(on ? "on" : "off"),
             QLatin1String(flags))
        .arg(host_us)
        .arg(host_us)
        .toUtf8();
}

// A copy of the conditions fixture in a folder of dir, its cond lines
// replaced by cond (kept when cond is null) and conditions.json as
// setEdit leaves it
QString conditionsCopy(const QTemporaryDir& dir, const QByteArray* cond,
                       const std::function<void(QByteArray&)>& setEdit = {})
{
    const QString folder = dir.filePath(QStringLiteral("conditions"));
    QDir().mkpath(folder);
    for (const char* file : {"events.log", "signals.csv", "frames.csv",
                             "markers.csv", "conditions.json"}) {
        QFile in(fixture("conditions") + QLatin1Char('/') +
                 QLatin1String(file));
        EXPECT_TRUE(in.open(QIODevice::ReadOnly));
        QByteArray bytes = in.readAll();
        if (qstrcmp(file, "events.log") == 0 && cond != nullptr) {
            QByteArray out;
            bool placed = false;
            for (const QByteArray& line : bytes.split('\n')) {
                if (line.contains("  cond  ")) {
                    if (!placed) {
                        out += *cond;
                        placed = true;
                    }
                    continue;
                }
                out += line + '\n';
            }
            out.chop(1); // the split's last piece had no line end
            bytes = out;
        }
        if (qstrcmp(file, "conditions.json") == 0 && setEdit) {
            setEdit(bytes);
        }
        QFile to(folder + QLatin1Char('/') + QLatin1String(file));
        EXPECT_TRUE(to.open(QIODevice::WriteOnly));
        to.write(bytes);
    }
    return folder;
}

} // namespace

// A held ON line (the state the recording began in) draws from the start,
// a stale stretch inside an ON span is that span's, and one still ON at
// the end runs to it
TEST(RecordingViewTest, AHeldStartAndAnOpenEndRunToTheEdges)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray cond = condLine("coolant_high", true, "held", 500000) +
                            condLine("coolant_high", true, "stale", 3000000) +
                            condLine("coolant_high", true, "", 5000000) +
                            condLine("coolant_high", false, "", 9000000) +
                            condLine("rpm_high", false, "", 10000000) +
                            condLine("rpm_high", true, "fired", 20000000);
    RecordingView v;
    v.open(conditionsCopy(dir, &cond));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();

    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), 2);
    const QVariantMap& held = spans[0];
    EXPECT_EQ(held.value(QStringLiteral("key")).toString(),
              QStringLiteral("coolant_high"));
    EXPECT_TRUE(held.value(QStringLiteral("heldStart")).toBool());
    EXPECT_EQ(held.value(QStringLiteral("fromMs")).toDouble(), 0.0);
    EXPECT_EQ(held.value(QStringLiteral("toMs")).toDouble(), 9000.0);
    EXPECT_FALSE(held.value(QStringLiteral("openEnd")).toBool());
    EXPECT_EQ(staleOf(held),
              (QList<std::pair<double, double>>{{3000.0, 5000.0}}));

    const QVariantMap& open = spans[1];
    EXPECT_EQ(open.value(QStringLiteral("key")).toString(),
              QStringLiteral("rpm_high"));
    EXPECT_FALSE(open.value(QStringLiteral("heldStart")).toBool());
    EXPECT_EQ(open.value(QStringLiteral("fromMs")).toDouble(), 20000.0);
    EXPECT_EQ(open.value(QStringLiteral("toMs")).toDouble(), v.durationMs());
    EXPECT_TRUE(open.value(QStringLiteral("openEnd")).toBool());
}

// The events a recorder lost are counted for the panel's line, beside the
// span their loss leaves open: conditions-lost is the conditions fixture
// less three events, coolant_high's turn OFF among them, and its drop line
TEST(RecordingViewTest, TheConditionEventsARecorderLostAreCounted)
{
    RecordingView v;
    v.open(fixture("conditions-lost"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(v.info().value(QStringLiteral("conditionEvents")).toDouble(),
              4.0);
    EXPECT_EQ(v.info().value(QStringLiteral("conditionEventsLost")).toDouble(),
              3.0);
    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_FALSE(spans.isEmpty());
    EXPECT_EQ(spans[0].value(QStringLiteral("key")).toString(),
              QStringLiteral("coolant_high"));
    EXPECT_TRUE(spans[0].value(QStringLiteral("openEnd")).toBool())
        << "its turn OFF was among the lost";

    // Another recording's count replaces it
    v.open(fixture("conditions"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(v.info().value(QStringLiteral("conditionEventsLost")).toDouble(),
              0.0);
}

// A conditions.json that isn't the one the header names can't say the
// levels the session judged by: the keys stand for the names, with no
// level, and the open goes on with no error
TEST(RecordingViewTest, AConditionsFileThatDoesntMatchShowsTheKeys)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RecordingView v;
    v.open(conditionsCopy(dir, nullptr, [](QByteArray& set) {
        set.replace("\"warning\"", "\"danger\"");
    }));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_TRUE(v.error().isEmpty());

    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), 3);
    for (const QVariantMap& s : spans) {
        EXPECT_EQ(s.value(QStringLiteral("name")).toString(),
                  s.value(QStringLiteral("key")).toString());
        EXPECT_EQ(s.value(QStringLiteral("level")).toInt(),
                  static_cast<int>(ECUIO_ALERT_NONE));
    }
}

// A recording with no conditions has no entries
TEST(RecordingViewTest, ARecordingWithoutConditionsHasNoSpans)
{
    RecordingView v;
    v.open(fixture("findings"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_TRUE(v.conditionSpans().isEmpty());
    EXPECT_EQ(v.info().value(QStringLiteral("conditionEvents")).toDouble(),
              0.0);
    EXPECT_EQ(v.info().value(QStringLiteral("conditionSpanCount")).toDouble(),
              0.0);
}

// Past ViewerConditionSpans entries the earliest are kept, and every one
// is counted
TEST(RecordingViewTest, TheEarliestConditionSpansAreKeptPastTheBound)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const int n = RecordingView::ViewerConditionSpans + 5;
    QByteArray cond;
    for (int i = 0; i < n; ++i) {
        const auto at = static_cast<uint64_t>(i) * 4000u + 1000u;
        cond += condLine("rpm_high", true, "fired", at);
        cond += condLine("rpm_high", false, "", at + 2000u);
    }
    RecordingView v;
    v.open(conditionsCopy(dir, &cond));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();

    EXPECT_EQ(v.info().value(QStringLiteral("conditionSpanCount")).toDouble(),
              static_cast<double>(n));
    EXPECT_TRUE(v.info().value(QStringLiteral("conditionSpansCut")).toBool());
    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), RecordingView::ViewerConditionSpans);
    EXPECT_EQ(spans.first().value(QStringLiteral("fromMs")).toDouble(), 1.0);
    EXPECT_EQ(spans.last().value(QStringLiteral("fromMs")).toDouble(),
              (RecordingView::ViewerConditionSpans - 1) * 4.0 + 1.0);
}

// STALE across a change of state ends the stretch at the change: ON and
// stale to OFF and stale ends the span's part there and starts an OFF
// entry; OFF and stale to ON ends that entry where the span starts; a
// span stale at the end has its part to the end
TEST(RecordingViewTest, AStaleStretchEndsAtAChangeOfState)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray cond = condLine("coolant_high", true, "fired", 2000000) +
                            condLine("coolant_high", true, "stale", 3000000) +
                            condLine("coolant_high", false, "stale", 5000000) +
                            condLine("coolant_high", true, "fired", 8000000) +
                            condLine("coolant_high", true, "stale", 10000000);
    RecordingView v;
    v.open(conditionsCopy(dir, &cond));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();

    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), 3);
    EXPECT_TRUE(spans[0].value(QStringLiteral("on")).toBool());
    EXPECT_EQ(spans[0].value(QStringLiteral("fromMs")).toDouble(), 2000.0);
    EXPECT_EQ(spans[0].value(QStringLiteral("toMs")).toDouble(), 5000.0);
    EXPECT_EQ(staleOf(spans[0]),
              (QList<std::pair<double, double>>{{3000.0, 5000.0}}));
    EXPECT_FALSE(spans[1].value(QStringLiteral("on")).toBool());
    EXPECT_EQ(spans[1].value(QStringLiteral("fromMs")).toDouble(), 5000.0);
    EXPECT_EQ(spans[1].value(QStringLiteral("toMs")).toDouble(), 8000.0);
    EXPECT_FALSE(spans[1].value(QStringLiteral("openEnd")).toBool());
    EXPECT_TRUE(spans[2].value(QStringLiteral("on")).toBool());
    EXPECT_EQ(spans[2].value(QStringLiteral("fromMs")).toDouble(), 8000.0);
    EXPECT_EQ(spans[2].value(QStringLiteral("toMs")).toDouble(), v.durationMs());
    EXPECT_TRUE(spans[2].value(QStringLiteral("openEnd")).toBool());
    EXPECT_EQ(staleOf(spans[2]),
              (QList<std::pair<double, double>>{{10000.0, v.durationMs()}}));
    EXPECT_FALSE(v.info().value(QStringLiteral("conditionPartsCut")).toBool());
}

namespace {

// One ON span from 1 ms with n stale parts, each 1 ms long every 4 ms from
// 2 ms, ending at last_us
QByteArray flappingSpan(int n, uint64_t last_us)
{
    QByteArray cond = condLine("coolant_high", true, "fired", 1000);
    for (int i = 0; i < n; ++i) {
        const auto at = static_cast<uint64_t>(i) * 4000u + 2000u;
        cond += condLine("coolant_high", true, "stale", at);
        cond += condLine("coolant_high", true, "", at + 1000u);
    }
    cond += condLine("coolant_high", false, "", last_us);
    return cond;
}

} // namespace

// The kept spans' stale parts share ViewerConditionSpans: past it a span's
// next part runs to the span's end, drawn as more stale than there was
TEST(RecordingViewTest, StalePartsPastTheBoundRunToTheSpansEnd)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const int bound = RecordingView::ViewerConditionSpans;
    const QByteArray cond = flappingSpan(bound + 4, 20000000);
    RecordingView v;
    v.open(conditionsCopy(dir, &cond));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();

    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), 1);
    const QList<std::pair<double, double>> parts = staleOf(spans[0]);
    ASSERT_EQ(parts.size(), bound + 1);
    EXPECT_EQ(parts[bound - 1],
              std::make_pair((bound - 1) * 4.0 + 2.0, (bound - 1) * 4.0 + 3.0));
    // The first part past the bound, to the span's end
    EXPECT_EQ(parts[bound], std::make_pair(bound * 4.0 + 2.0, 20000.0));
    EXPECT_TRUE(v.info().value(QStringLiteral("conditionPartsCut")).toBool());
    EXPECT_EQ(v.info().value(QStringLiteral("conditionBound")).toInt(), bound);
}

// After core's cut (more cond lines than it keeps) what came after the last
// line kept isn't known: an entry still open ends at that line, not at the
// recording's end
TEST(RecordingViewTest, ACutRecordingsOpenEntriesEndAtTheLastLine)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray cond = condLine("coolant_high", true, "fired", 7251000) +
                            condLine("rpm_high", false, "", 20000000);
    RecordingView v;
    v.setConditionsCutForTesting(true);
    v.open(conditionsCopy(dir, &cond));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();

    EXPECT_TRUE(v.info().value(QStringLiteral("conditionsCut")).toBool());
    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), 1);
    EXPECT_EQ(spans[0].value(QStringLiteral("fromMs")).toDouble(), 7251.0);
    EXPECT_EQ(spans[0].value(QStringLiteral("toMs")).toDouble(), 20000.0);
    EXPECT_FALSE(spans[0].value(QStringLiteral("openEnd")).toBool());

    // The same lines uncut run to the end
    v.setConditionsCutForTesting(false);
    v.open(conditionsCopy(dir, &cond));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_FALSE(v.info().value(QStringLiteral("conditionsCut")).toBool());
    ASSERT_EQ(spansOf(v).size(), 1);
    EXPECT_TRUE(spansOf(v)[0].value(QStringLiteral("openEnd")).toBool());
}

// A close or another open while the pairing runs keeps none of its entries:
// the closed view has none, and the next recording shows its own alone
TEST(RecordingViewTest, AnOpenEndedWhilePairingShowsNoneOfItsSpans)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray cond = flappingSpan(20000, 90000000);
    const QString folder = conditionsCopy(dir, &cond);
    RecordingView v;
    v.open(folder);
    v.close();
    EXPECT_EQ(v.state(), RecordingView::Closed);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    EXPECT_TRUE(v.conditionSpans().isEmpty());
    EXPECT_TRUE(v.info().isEmpty());

    v.open(folder);
    v.open(fixture("conditions"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    const QList<QVariantMap> spans = spansOf(v);
    ASSERT_EQ(spans.size(), 3);
    EXPECT_EQ(spans[0].value(QStringLiteral("fromMs")).toDouble(), 7251.0);
    EXPECT_EQ(v.info().value(QStringLiteral("conditionEvents")).toDouble(),
              7.0);
}
