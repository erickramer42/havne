// File/layer: tests/test_bridge_signals.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest: the signal model (windows, charts, traces, plot choices, units, text and enum rows) (test_bridge)
//
// Cases for the signal model.

#include <cmath>
#include <gtest/gtest.h>

#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

TEST(BridgeTest, AWindowShowsItsNewestAndChartsItsEnvelope)
{
    /* Three frames' values inside one coalesce window, as core's worker
     * hands them to onValues, at 1, 2 and 3 ms of session time: 20, then
     * a peak of 30, then 10. The row shows the newest, numeric and text
     * alike, and the window's highest and lowest beside it; the chart
     * gets those at their own times, the peak included (ADR-0005's
     * appendix). The mock's own
     * frames (0x0C0) match neither def. */
    static ecuio_signal_def_t defs[2] = {};
    defs[0] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].message_id = 0x123u;
    defs[0].bit_length = 8;
    defs[0].scale = 1.0;
    strncpy(defs[0].name, "Byte", sizeof(defs[0].name) - 1);
    defs[1] = {};
    defs[1].id = 2;
    defs[1].kind = ECUIO_VALUE_HEX;
    defs[1].message_id = 0x123u;
    defs[1].byte_length = 1;
    defs[1].scale = 1.0;
    strncpy(defs[1].name, "Byte as hex", sizeof(defs[1].name) - 1);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-newest", &set, nullptr), ECUIO_OK);

    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://newest?profile=test-newest").isEmpty());
    spinLoop(150);   // a tick or more, so the window below starts clean
    const int batches = bridge.signalBatchCount();
    uint64_t at = 0;
    for (const char* hex : {"14", "1E", "0A"}) {
        ecuio_value_t values[2] = {};
        values[0].signal_id = 1;
        values[0].kind = ECUIO_VALUE_UINT;
        values[0].value = static_cast<double>(std::strtol(hex, nullptr, 16));
        values[1].signal_id = 2;
        values[1].kind = ECUIO_VALUE_HEX;
        values[1].text = hex;
        values[1].text_len = 2;
        ecuio_value_batch_t batch = {};
        batch.timestamp_us = at += 1000;
        batch.arbitration_id = 0x123;
        batch.count = 2;
        batch.values = values;
        bridge.dispatchValuesForTesting(batch);
    }
    ASSERT_TRUE(spinUntil([&]() { return bridge.signalBatchCount() > batches; }, 2000));

    SignalListModel* model = bridge.signalModel();
    ASSERT_EQ(model->rowCount(), 2);
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::ValueRole).toDouble(), 10.0)
        << "the newest";
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::WindowHighRole).toDouble(), 30.0)
        << "the Dashboard's peak hold sees the window's highest";
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::WindowLowRole).toDouble(), 10.0);
    EXPECT_EQ(model->roleNames().value(SignalListModel::WindowHighRole), "windowHigh");
    EXPECT_EQ(model->roleNames().value(SignalListModel::WindowLowRole), "windowLow");
    EXPECT_EQ(model->data(model->index(1, 0), SignalListModel::TextRole).toString(),
              QStringLiteral("0A"));
    const QVariantList chart = model->chartData();
    ASSERT_EQ(chart.size(), 1);
    QList<double> samples;
    for (const QVariant& v : chart.at(0).toMap().value("samples").toList())
        samples << v.toDouble();
    EXPECT_EQ(samples, (QList<double>{2, 30, 3, 10}))
        << "the peak at 2 ms and the low, which is the newest, at 3 ms";
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(ecuio_profile_unregister("test-newest"), ECUIO_OK);
}

// ---------------------------------------------------------------------------
// Strip chart: per-signal rolling history ring in SignalListModel
// ---------------------------------------------------------------------------

TEST(BridgeTest, HistoryRingEvictsOldestAtCapacity)
{
    /* Deterministic: injected timestamps + values equal to the timestamp,
       so retention/ordering asserts are exact arithmetic, not timing. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    // Push 10 MORE one-point batches than the ring holds.
    std::vector<std::pair<uint16_t, double>> batch;
    for (qint64 t = 0; t < SignalListModel::HistoryCapacity + 10; ++t) {
        batch.clear();
        batch.emplace_back(uint16_t(1001), double(t));
        batch.emplace_back(uint16_t(2001), double(t) - 40.0);
        model.updateValues(batch, t);
    }

    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 2);   // both signals subscribed with history

    for (int s = 0; s < 2; ++s) {
        const QVariantMap m = data.at(s).toMap();
        const QVariantList samples = m.value("samples").toList();
        // Exactly capacity survives: ring size is a hard bound, no leaks
        ASSERT_EQ(samples.size(), SignalListModel::HistoryCapacity * 2)
            << "wrong sample count for " << m.value("name").toString().toStdString();

        // Oldest retained = push #10; newest = the last push.
        // Exact ints: injected timestamps make wrap position deterministic.
        const double newest = SignalListModel::HistoryCapacity + 9;
        EXPECT_EQ(samples.at(0).toDouble(), 10.0);
        EXPECT_EQ(samples.at(samples.size() - 2).toDouble(), newest);

        // Values track their timestamps (order preserved through the wrap)
        const double offset = (m.value("id").toInt() == 1001) ? 0.0 : -40.0;
        EXPECT_NEAR(samples.at(1).toDouble(), 10.0 + offset, 0.01);
        EXPECT_NEAR(samples.at(samples.size() - 1).toDouble(), newest + offset, 0.01);

        // Monotonic timestamps oldest->newest (wrap-around correctness)
        double prev = -1.0;
        for (int i = 0; i < samples.size(); i += 2) {
            EXPECT_GE(samples.at(i).toDouble(), prev);
            prev = samples.at(i).toDouble();
        }
    }
}

/* A window's points (ADR-0005's appendix): its lowest, highest and
 * newest value in time order, each once; the row's age counts from the
 * value's arrival, whatever its sample time */
TEST(BridgeTest, AWindowChartsItsLowestHighestAndNewestInTimeOrder)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto window = [](double last, qint64 lastMs, double min, qint64 minMs,
                           double max, qint64 maxMs) {
        SignalListModel::WindowValue w;
        w.id = 1001;
        w.last = last;
        w.lastMs = lastMs;
        w.min = min;
        w.minMs = minMs;
        w.max = max;
        w.maxMs = maxMs;
        return w;
    };
    const auto points = [&model]() {
        QList<double> out;
        for (const QVariant& v : model.chartData().at(0).toMap().value("samples").toList())
            out << v.toDouble();
        return out;
    };
    model.updateWindow({window(25, 30, 5, 10, 50, 20)});
    EXPECT_EQ(points(), (QList<double>{10, 5, 20, 50, 30, 25})) << "three apart";
    model.updateWindow({window(7, 40, 7, 40, 7, 40)});
    EXPECT_EQ(points().size(), 8) << "one value: one point";
    model.updateWindow({window(1, 60, 1, 60, 9, 50)});
    EXPECT_EQ(points().mid(8), (QList<double>{50, 9, 60, 1}))
        << "the high first; the low is the newest";
    EXPECT_EQ(model.data(model.index(0, 0), SignalListModel::ValueRole).toDouble(), 1.0);

    /* Sample times far past the model's clock, as a replay's can run */
    model.updateWindow({window(3, 1000000000, 3, 1000000000, 3, 1000000000)});
    const int age = model.data(model.index(0, 0), SignalListModel::AgeMsRole).toInt();
    EXPECT_GE(age, 0);
    EXPECT_LT(age, 5000) << "from arrival, not sample time";
}

TEST(BridgeTest, ChartDataFiltersUnsubscribedAndEmptyBatchesDoNotEmit)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    int emissions = 0;
    QObject::connect(&model, &SignalListModel::valuesUpdated,
                     [&emissions]() { ++emissions; });

    std::vector<std::pair<uint16_t, double>> batch;
    for (int t = 0; t < 5; ++t) {
        batch.clear();
        batch.emplace_back(uint16_t(1001), double(t));
        batch.emplace_back(uint16_t(2001), double(t));
        model.updateValues(batch, t);
    }
    ASSERT_EQ(emissions, 5);   // one per non-empty batch, per contract

    // Empty batch = silent: no dataChanged, no valuesUpdated
    model.updateValues({}, 999);
    EXPECT_EQ(emissions, 5);

    // Unknown IDs = silent too (defensive: staging should never send them)
    batch.clear();
    batch.emplace_back(uint16_t(9999), 1.0);
    model.updateValues(batch, 1000);
    EXPECT_EQ(emissions, 5);
    EXPECT_EQ(model.chartData().size(), 2);

    // Unsubscribe signal 0: excluded from chartData
    model.toggleSubscribed(0);
    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data.at(0).toMap().value("id").toInt(), 2001);
    // `row` is the model row, not the position in chartData(), so the
    // trace keeps its color when a signal before it is unsubscribed
    EXPECT_EQ(data.at(0).toMap().value("row").toInt(), 1);
    EXPECT_EQ(data.at(0).toMap().value("samples").toList().size(), 10); // still 5 samples

    // Resubscribed signal keeps its accumulated history (non-destructive)
    model.toggleSubscribed(0);
    EXPECT_EQ(model.chartData().size(), 2);
    EXPECT_EQ(model.chartData().at(0).toMap().value("samples").toList().size(), 10);
}

TEST(BridgeTest, UnsubscribedSignalKeepsUpdating)
{
    /* A subscription chooses what the chart plots, nothing else. An
     * unsubscribed row still takes values, so the Dashboard's gauge for
     * it stays live instead of dimming as stale while its frames arrive,
     * and its history keeps growing, so the trace is whole when it's
     * plotted again. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    model.toggleSubscribed(0);   // row 0 is id 1001

    bool row0Notified = false;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&row0Notified](const QModelIndex& topLeft, const QModelIndex& bottomRight,
                        const QList<int>& roles) {
            if (roles.contains(SignalListModel::ValueRole)
                && topLeft.row() <= 0 && bottomRight.row() >= 0) {
                row0Notified = true;
            }
        });

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(1001), 3000.0);
    batch.emplace_back(uint16_t(2001), 7.0);
    model.updateValues(batch, 10);

    const QModelIndex row0 = model.index(0, 0);
    EXPECT_TRUE(row0Notified);
    EXPECT_FALSE(model.data(row0, SignalListModel::SubscribedRole).toBool());
    EXPECT_DOUBLE_EQ(model.data(row0, SignalListModel::ValueRole).toDouble(), 3000.0);
    EXPECT_TRUE(model.data(row0, SignalListModel::HasValueRole).toBool());

    // Only the subscribed row is plotted
    const QVariantList plotted = model.chartData();
    ASSERT_EQ(plotted.size(), 1);
    EXPECT_EQ(plotted.at(0).toMap().value("id").toInt(), 2001);

    // Plotted again: the sample taken while unsubscribed is in its trace
    model.toggleSubscribed(0);
    const QVariantList both = model.chartData();
    ASSERT_EQ(both.size(), 2);
    EXPECT_EQ(both.at(0).toMap().value("id").toInt(), 1001);
    EXPECT_EQ(both.at(0).toMap().value("samples").toList().size(), 2);   // one [ts, val]
}

/* A text row (a TEXT or HEX def) carries its latest text, counts as
 * having a value once one arrived, and is never plotted: not
 * subscribed, not toggled, not in chartData. */
TEST(BridgeTest, TextRowsShowTheirTextAndNeverPlot)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[1].kind = ECUIO_VALUE_HEX;   /* row 1, id 2001, a text row */
    model.setSignalSet(&set);

    const QModelIndex row0 = model.index(0, 0);
    const QModelIndex row1 = model.index(1, 0);
    EXPECT_FALSE(model.data(row0, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model.data(row1, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model.data(row0, SignalListModel::SubscribedRole).toBool());
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.subscribedCount(), 1);
    /* A text row's tick asks it (the default pick and a log take it) and
     * never plots: the count and the chart leave it out */
    int ticks = 0;
    QObject::connect(&model, &SignalListModel::ticksChanged,
                     [&ticks]() { ++ticks; });
    model.toggleSubscribed(1);
    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.subscribedCount(), 1);
    EXPECT_EQ(model.tickedCount(), 2) << "a log counts the ticked text row";
    const QString textName =
        model.data(row1, SignalListModel::NameRole).toString();
    const QString textKey =
        model.data(row1, SignalListModel::KeyRole).toString();
    ASSERT_FALSE(textKey.isEmpty());
    EXPECT_EQ(model.plottedKeys(), QStringList{textKey});
    EXPECT_EQ(model.tickedIds(), std::vector<uint16_t>{2001});
    EXPECT_EQ(model.plottedIds(), (std::vector<uint16_t>{1001, 2001}));
    EXPECT_FALSE(model.data(row1, SignalListModel::HasValueRole).toBool());
    EXPECT_EQ(model.data(row1, SignalListModel::TextRole).toString(), QString());

    QList<int> notified;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&notified](const QModelIndex& topLeft, const QModelIndex&, const QList<int>& roles) {
            if (topLeft.row() == 1) notified = roles;
        });
    model.updateTexts({{uint16_t(2001), QStringLiteral("62 F1 90")}}, 10);
    EXPECT_EQ(model.data(row1, SignalListModel::TextRole).toString(), QStringLiteral("62 F1 90"));
    EXPECT_TRUE(model.data(row1, SignalListModel::HasValueRole).toBool());
    EXPECT_NE(model.data(row1, SignalListModel::AgeMsRole).toInt(), -1);   /* arrived */
    EXPECT_TRUE(notified.contains(SignalListModel::TextRole));
    EXPECT_TRUE(notified.contains(SignalListModel::HasValueRole));
    EXPECT_TRUE(notified.contains(SignalListModel::AgeMsRole));
    EXPECT_TRUE(model.chartData().isEmpty()) << "a text row was plotted";

    /* A numeric row ignores a text batch */
    model.updateTexts({{uint16_t(1001), QStringLiteral("no")}}, 11);
    EXPECT_FALSE(model.data(row0, SignalListModel::HasValueRole).toBool());
    EXPECT_EQ(model.data(row0, SignalListModel::TextRole).toString(), QString());

    /* A vehicle's choices tick it again; an untick is never kept for it,
     * its default being unticked */
    model.toggleSubscribed(1);
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_TRUE(model.tickedIds().empty());
    model.setPlotChoices({textName}, {});
    EXPECT_EQ(ticks, 3);
    EXPECT_TRUE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    model.setPlotChoices({}, {textName});
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_TRUE(model.unplottedKeys().isEmpty());
    // A row in both lists is unplotted, a text row never: it stays ticked
    model.setPlotChoices({textName}, {textName});
    EXPECT_TRUE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.tickedCount(), 2);
}

/* An enum row (ADR-0051) shows its label for the newest raw value, and
 * a raw value with no label its number; the label is notified with the
 * value, and a row of another kind has none */
TEST(BridgeTest, AnEnumRowShowsItsLabel)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    static const ecuio_enum_entry_t kGears[] = {
        {0, "Park"}, {3, "Drive"}, {7, "Low"}};
    defs[1].kind = ECUIO_VALUE_ENUM; /* row 1, id 2001 */
    defs[1].table_first = 1;         /* Drive and Low */
    defs[1].table_count = 2;
    set.enums = kGears;
    set.enum_count = 3;
    model.setSignalSet(&set);

    const QModelIndex row0 = model.index(0, 0);
    const QModelIndex row1 = model.index(1, 0);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString())
        << "a label before any value";
    QList<int> notified;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&notified](const QModelIndex& topLeft, const QModelIndex&,
                                 const QList<int>& roles) {
                         if (topLeft.row() <= 1)
                             notified = roles;
                     });
    model.updateValues({{uint16_t(2001), 3.0}, {uint16_t(1001), 3.0}}, 10);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QStringLiteral("Drive"));
    EXPECT_TRUE(notified.contains(SignalListModel::LabelRole));
    EXPECT_EQ(model.data(row0, SignalListModel::LabelRole).toString(),
              QString())
        << "a number row has no label";
    model.updateValues({{uint16_t(2001), 7.0}}, 11);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QStringLiteral("Low"));
    /* Park is the set's, but not the def's: past its table_first */
    model.updateValues({{uint16_t(2001), 0.0}}, 12);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString());
    model.updateValues({{uint16_t(2001), 5.0}}, 13);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString())
        << "a raw value with no label shows its number";
    EXPECT_DOUBLE_EQ(model.data(row1, SignalListModel::ValueRole).toDouble(),
                     5.0);
}

/* A text def takes the bridge's whole path like a number: decoded
 * from the frame (here two bytes of the mock's telemetry as hex),
 * staged, shown on its row, and written to signals.csv. */
TEST(BridgeTest, TextSignalsReachTheRowAndTheRecording)
{
    static ecuio_signal_def_t defs[2] = {};
    defs[0] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_HEX;
    defs[0].message_id = 0x0C0u;   /* the mock's telemetry frame */
    defs[0].byte_length = 2;
    defs[0].scale = 1.0;
    strncpy(defs[0].name, "Raw bytes", sizeof(defs[0].name) - 1);
    defs[1] = {};
    defs[1].id = 2;
    defs[1].kind = ECUIO_VALUE_UINT;
    defs[1].message_id = 0x0C0u;
    defs[1].bit_length = 8;
    defs[1].scale = 1.0;
    strncpy(defs[1].name, "First byte", sizeof(defs[1].name) - 1);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-text", &set, nullptr), ECUIO_OK);

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://text?profile=test-text").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.signalBatchCount() > 1; }, 5000));

    SignalListModel* model = bridge.signalModel();
    ASSERT_EQ(model->rowCount(), 2);
    const QModelIndex raw = model->index(0, 0);
    const QModelIndex first = model->index(1, 0);
    EXPECT_TRUE(model->data(raw, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model->data(raw, SignalListModel::HasValueRole).toBool());
    const QString text = model->data(raw, SignalListModel::TextRole).toString();
    const auto hexPair = [](const QString& s) {
        if (s.size() != 5 || s[2] != QLatin1Char(' ')) return false;
        for (int i : {0, 1, 3, 4}) {
            const QChar c = s[i];
            if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                  (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) return false;
        }
        return true;
    };
    EXPECT_TRUE(hexPair(text)) << text.toStdString();
    EXPECT_TRUE(model->data(first, SignalListModel::HasValueRole).toBool());
    EXPECT_FALSE(model->data(first, SignalListModel::IsTextRole).toBool());
    EXPECT_EQ(model->subscribedCount(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray signalsCsv =
        readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/signals.csv"));
    // "Raw bytes" is kept under its key: two bytes of text from byte 0
    bool recorded = false;
    for (const QByteArray& line : signalsCsv.split('\n')) {
        const QList<QByteArray> cols = line.split(',');
        if (cols.size() == 4 && cols[1] == "0C0::0:t2" &&
            hexPair(QString::fromUtf8(cols[2]))) {
            recorded = true;
            break;
        }
    }
    EXPECT_TRUE(recorded) << "no hex text row in signals.csv:\n" << signalsCsv.toStdString();
    EXPECT_EQ(ecuio_profile_unregister("test-text"), ECUIO_OK);
}

/* LiveView's delegate and SignalHub's Entry take these roles as
 * required properties, by NAME. A missing name isn't a build error: the
 * delegate fails at creation and its row never shows. */
TEST(BridgeTest, SignalRoleNamesMatchQmlBindings)
{
    SignalListModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    EXPECT_EQ(roles.value(SignalListModel::NameRole), QByteArray("name"));
    EXPECT_EQ(roles.value(SignalListModel::UnitRole), QByteArray("unit"));
    EXPECT_EQ(roles.value(SignalListModel::ValueRole), QByteArray("value"));
    EXPECT_EQ(roles.value(SignalListModel::SubscribedRole), QByteArray("subscribed"));
    EXPECT_EQ(roles.value(SignalListModel::HasValueRole), QByteArray("hasValue"));
    EXPECT_EQ(roles.value(SignalListModel::AgeMsRole), QByteArray("ageMs"));
    EXPECT_EQ(roles.value(SignalListModel::DecimalsRole), QByteArray("decimals"));
    EXPECT_EQ(roles.value(SignalListModel::TextRole), QByteArray("text"));
    EXPECT_EQ(roles.value(SignalListModel::IsTextRole), QByteArray("isText"));
    EXPECT_EQ(roles.value(SignalListModel::RefusedRole), QByteArray("refused"));
    EXPECT_EQ(roles.value(SignalListModel::RefusedCodeRole),
              QByteArray("refusedCode"));
    EXPECT_EQ(roles.value(SignalListModel::RefRole), QByteArray("ref"));
    EXPECT_EQ(roles.value(SignalListModel::RangeLowRole),
              QByteArray("rangeLow"));
    EXPECT_EQ(roles.value(SignalListModel::RangeHighRole),
              QByteArray("rangeHigh"));
    EXPECT_EQ(roles.value(SignalListModel::IsEnumRole), QByteArray("isEnum"));
    EXPECT_EQ(roles.value(SignalListModel::HiddenWhyRole),
              QByteArray("hiddenWhy"));

    // A row answers only its roles, and only rows that exist answer
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    EXPECT_EQ(model.data(model.index(1, 0), SignalListModel::IdRole).toInt(),
              2001);
    EXPECT_FALSE(model.data(model.index(0, 0), Qt::DisplayRole).isValid());
    EXPECT_FALSE(
        model.data(model.index(2, 0), SignalListModel::NameRole).isValid());
}

TEST(BridgeTest, DecimalsComeFromTheDefAndReachTheChart)
{
    /* Display precision has one source, the def's decimal_places:
     * LiveView and the Dashboard format with DecimalsRole, the strip
     * chart's value labels with chartData's "decimals". */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[0].decimal_places = 0;
    defs[1].decimal_places = 2;
    model.setSignalSet(&set);

    EXPECT_EQ(model.data(model.index(0, 0), SignalListModel::DecimalsRole).toInt(), 0);
    EXPECT_EQ(model.data(model.index(1, 0), SignalListModel::DecimalsRole).toInt(), 2);
    EXPECT_FALSE(model.data(model.index(1, 0), SignalListModel::HasValueRole).toBool());

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(2001), 21.5);
    model.updateValues(batch, 0);
    EXPECT_TRUE(model.data(model.index(1, 0), SignalListModel::HasValueRole).toBool());

    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data.at(0).toMap().value("decimals").toInt(), 2);
}

TEST(BridgeTest, UnitSystemConvertsRowsAndChartAndKeepsHistory)
{
    /* The model keeps decoded values; imperial converts value, unit,
     * decimals and the chart's whole history on the way out, and metric
     * gives back exactly what was decoded. A unit with no imperial form
     * passes through. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    std::memset(defs[1].unit, 0, sizeof defs[1].unit);
    std::memcpy(defs[1].unit, "\xC2\xB0" "C", 3);
    defs[1].decimal_places = 1;
    model.setSignalSet(&set);
    int redraws = 0;
    QObject::connect(&model, &SignalListModel::valuesUpdated, [&redraws]() { ++redraws; });

    for (int i = 0; i < 3; ++i) {
        std::vector<std::pair<uint16_t, double>> batch;
        batch.emplace_back(uint16_t(1001), 100.0 + i);
        batch.emplace_back(uint16_t(2001), 90.0 + i);
        model.updateValues(batch, i * 100);
    }
    const int before = redraws;
    const QModelIndex speed = model.index(0, 0);
    const QModelIndex temp = model.index(1, 0);

    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(redraws, before + 1) << "the chart must redraw in the new units";
    EXPECT_NEAR(model.data(speed, SignalListModel::ValueRole).toDouble(), 102.0 * 0.621371192237334, 1e-9);
    EXPECT_EQ(model.data(speed, SignalListModel::UnitRole).toString(), QStringLiteral("mph"));
    EXPECT_NEAR(model.data(temp, SignalListModel::ValueRole).toDouble(), 92.0 * 1.8 + 32.0, 1e-9);
    EXPECT_EQ(model.data(temp, SignalListModel::UnitRole).toString(), QString::fromUtf8("\xC2\xB0" "F"));
    EXPECT_EQ(model.data(temp, SignalListModel::DecimalsRole).toInt(), 1);

    const QVariantList chart = model.chartData();
    ASSERT_EQ(chart.size(), 2);
    for (const QVariant& v : chart) {
        const QVariantMap m = v.toMap();
        const QVariantList samples = m.value("samples").toList();
        ASSERT_EQ(samples.size(), 6) << "history lost on a unit change";
        if (m.value("id").toInt() == 1001) {
            EXPECT_EQ(m.value("unit").toString(), QStringLiteral("mph"));
            EXPECT_NEAR(samples.at(1).toDouble(), 100.0 * 0.621371192237334, 1e-9);
        } else {
            EXPECT_NEAR(samples.at(5).toDouble(), 92.0 * 1.8 + 32.0, 1e-9);
        }
    }

    model.setUnitSystem(ECUIO_UNITS_METRIC);
    EXPECT_EQ(model.data(speed, SignalListModel::ValueRole).toDouble(), 102.0);
    EXPECT_EQ(model.data(speed, SignalListModel::UnitRole).toString(), QStringLiteral("km/h"));
    EXPECT_EQ(model.chartData().at(1).toMap().value("samples").toList().at(1).toDouble(), 90.0);
}

TEST(BridgeTest, UnitSystemShowsImperialAndRecordsDecodedUnits)
{
    /* The property takes metric or imperial only. The Dashboard's
     * helpers convert through core. A recording keeps what the ECU sent,
     * whatever the screen shows. */
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    EXPECT_EQ(bridge.unitSystem(), 0);
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::unitSystemChanged, [&changes]() { ++changes; });
    bridge.setUnitSystem(7);
    EXPECT_EQ(bridge.unitSystem(), 0);
    bridge.setUnitSystem(1);
    bridge.setUnitSystem(1);
    EXPECT_EQ(bridge.unitSystem(), 1);
    EXPECT_EQ(changes, 1);
    EXPECT_NEAR(bridge.displayValue(QStringLiteral("km/h"), 160.0), 99.4193907579734, 1e-9);
    EXPECT_EQ(bridge.displayUnit(QStringLiteral("km/h")), QStringLiteral("mph"));
    EXPECT_EQ(bridge.displayValue(QStringLiteral("%"), 42.0), 42.0);

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://units").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.signalBatchCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    bool sawMph = false;
    for (int r = 0; r < model->rowCount(); ++r) {
        sawMph = sawMph || model->data(model->index(r, 0), SignalListModel::UnitRole).toString() ==
                                QStringLiteral("mph");
    }
    EXPECT_TRUE(sawMph) << "no row shows mph under imperial";
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray signalsCsv =
        readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/signals.csv"));
    // Vehicle Speed, kept under its key: 0x0C0's fourth byte
    EXPECT_TRUE(signalsCsv.contains(",0C0::3.0:8,"))
        << signalsCsv.left(400).toStdString();
    EXPECT_TRUE(signalsCsv.contains(",km/h")) << "the recording should keep km/h";
    EXPECT_FALSE(signalsCsv.contains("mph")) << "the recording followed the display";
}

TEST(BridgeTest, StripChartDataPopulatedByLiveStream)
{
    /* End-to-end through the coalescer: real mock frames -> decode ->
       staging -> updateValues -> ring. Poll to the condition instead
       of a fixed 750ms window: the CONTRACT is history accumulates,
       not that 7.5 windows elapse. Monotonicity assertions are timing-
       independent once the data exists. */
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://strips").isEmpty());

    // CONTRACT: every subscribed signal has at least 4 samples
    auto chart_ready = [&bridge]() {
        const QVariantList data = bridge.signalModel()->chartData();
        if (data.isEmpty()) {
            return false;
        }
        for (const QVariant& v : data) {
            if (v.toMap().value("samples").toList().size() < 4) {
                return false;
            }
        }
        return true;
    };
    ASSERT_TRUE(spinUntil(chart_ready, 10000))
        << "strip chart history did not accumulate in 10s";

    const QVariantList data = bridge.signalModel()->chartData();
    ASSERT_GT(data.size(), 0);

    for (const QVariant& v : data) {
        const QVariantMap m = v.toMap();
        const QVariantList samples = m.value("samples").toList();

        // Sample count floor satisfied by the poll; upper bound sanity
        // (poll exits promptly, so a healthy stream sits near 4-8)
        EXPECT_GE(samples.size(), 4)
            << "no history for " << m.value("name").toString().toStdString();

        // Timestamps monotonic non-decreasing across the window
        double prev = -1.0;
        for (int i = 0; i < samples.size(); i += 2) {
            EXPECT_GE(samples.at(i).toDouble(), prev);
            prev = samples.at(i).toDouble();
        }
    }

    bridge.disconnectSession();
}

// ---------------------------------------------------------------------------
// ADR-0013: the bridge renders the SESSION's profile, not a hardcoded
// set
// ---------------------------------------------------------------------------

TEST(BridgeTest, RawProfileLeavesSignalModelEmpty)
{
    /* A raw session after a mock session must CLEAR the signal model.
     * Failure shape guarded: a setSignalSet that early-returns on NULL
     * without clearing leaves a raw session displaying the PREVIOUS
     * session's rows with no live updates: stale data posing as live.
     * Frames still flow to the Log page; decode alone is silenced. */
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://profile-then-raw").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.signalModel()->rowCount() > 0; }, 5000));
    EXPECT_GT(bridge.signalModel()->rowCount(), 0);

    ASSERT_TRUE(disconnectAndWait(bridge));

    ASSERT_TRUE(bridge.connectSession("mock://profile-raw-stage2?profile=raw")
                    .isEmpty());
    /* Frames keep arriving (mock streams regardless of profile):
     * the Log page stays live while LiveView stays empty. */
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    EXPECT_EQ(bridge.signalModel()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, AgeMsRoleEmittedAndRefreshesWhileStalled)
{
    /* Freshness contract: (a) updateValues emits dataChanged carrying
     * AgeMsRole; (b) refreshAges emits over all rows for AgeMsRole
     * ONLY: the stalled-row dimming path; (c) ageMs is -1 before the
     * first sample and a sane clock delta after. ageMs's wall-clock
     * basis is nondeterministic by nature, so assert ranges, not
     * exact values. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    // -1 before any sample
    EXPECT_EQ(model.data(model.index(0, 0),
                         SignalListModel::AgeMsRole).toInt(), -1);

    int ageEmissions = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&ageEmissions](const QModelIndex&, const QModelIndex&,
                        const QList<int>& roles) {
            if (roles.contains(SignalListModel::AgeMsRole)) ++ageEmissions;
        });

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(1001), 1500.0);
    model.updateValues(batch, 0);   // deterministic stamp of 0
    EXPECT_GE(ageEmissions, 1);

    const int age = model.data(model.index(0, 0),
                               SignalListModel::AgeMsRole).toInt();
    EXPECT_GE(age, 0);
    EXPECT_LT(age, 5000);   // clock basis sanity, not timing

    const int before = ageEmissions;
    model.refreshAges();   // the stalled-row path
    EXPECT_EQ(ageEmissions, before + 1);

    SignalListModel empty;
    empty.refreshAges();   // no rows: silent, no crash
}

/* ADR-0027 section 1: a vehicle's plotted signals are the rows ticked
 * past the default and unticked from it, read and set by name. The
 * mock's set ranks none, so every numeric row is its default. */
/* The chart's window (LiveViewPage's 10 to 120 s): chartData keeps the
 * samples of the last windowMs before the newest plotted one, and with
 * columns, each slice's lowest and highest in time order and the newest
 * sample, never more than two a slice plus it. Slices are counted from
 * time 0, so the window's first may be partial */
TEST(BridgeTest, ChartDataKeepsTheWindowAndEachSlicesExtremes)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    /* 1001 rises and falls every second; 2001 tracks the time */
    for (qint64 t = 0; t < 20000; t += 10) {
        const double wave = static_cast<double>((t / 10) % 100 < 50 ? (t / 10) % 50
                                                                    : 50 - (t / 10) % 50);
        model.updateValues({{uint16_t(1001), wave}, {uint16_t(2001), double(t)}}, t);
    }
    const auto samples = [](const QVariantList& data, int s) {
        return data.at(s).toMap().value("samples").toList();
    };

    const QVariantList all = model.chartData();
    ASSERT_EQ(all.size(), 2);
    EXPECT_EQ(samples(all, 1).size(), 2 * 2000) << "no window: every sample";

    const QVariantList win = model.chartData(5000);
    const QVariantList w = samples(win, 1);
    EXPECT_EQ(w.at(0).toDouble(), 19990.0 - 5000.0) << "from the window's start";
    EXPECT_EQ(w.at(w.size() - 2).toDouble(), 19990.0) << "to the newest";
    EXPECT_EQ(w.size(), 2 * 501);

    const int columns = 10;
    const QVariantList cut = model.chartData(5000, columns);
    for (int s = 0; s < 2; ++s) {
        const QVariantList c = samples(cut, s);
        EXPECT_LE(c.size(), 2 * (2 * (columns + 1) + 1)) << s;
        for (int i = 2; i < c.size(); i += 2) {
            EXPECT_LT(c.at(i - 2).toDouble(), c.at(i).toDouble()) << "in time order";
        }
        EXPECT_EQ(c.at(c.size() - 2).toDouble(), 19990.0) << "the newest kept";
    }
    /* Each slice of the wave keeps its own lowest and highest, as the
     * whole window's samples show them */
    const QVariantList full = samples(model.chartData(5000), 0);
    const QVariantList wave = samples(cut, 0);
    const double slice = 5000.0 / columns;
    std::map<int, std::pair<double, double>> want, got;
    const auto note = [&](std::map<int, std::pair<double, double>>& m,
                          const QVariantList& v) {
        for (int i = 0; i < v.size(); i += 2) {
            const int k =
                static_cast<int>(std::floor(v.at(i).toDouble() / slice));
            const double x = v.at(i + 1).toDouble();
            auto it = m.find(k);
            if (it == m.end()) {
                m[k] = {x, x};
            } else {
                it->second.first = std::min(it->second.first, x);
                it->second.second = std::max(it->second.second, x);
            }
        }
    };
    note(want, full);
    note(got, wave);
    EXPECT_EQ(got, want) << "each slice's lowest and highest";
}

/* A window that slides keeps its points: a sample stays in its slice
 * however far the window has moved, so between two updates every slice
 * away from the window's ends keeps the same lowest and highest, and a
 * noisy trace scrolls instead of shimmering */
TEST(BridgeTest, ChartDataKeepsEachSlicesPointsAsTheWindowSlides)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    /* A noisy trace: a value that jumps sample to sample */
    const auto noise = [](qint64 t) {
        return static_cast<double>((t * 7919) % 97);
    };
    qint64 t = 0;
    const auto feed = [&](qint64 until) {
        for (; t < until; t += 10)
            model.updateValues({{uint16_t(1001), noise(t)}}, t);
    };
    const qint64 windowMs = 10000;
    const int columns = 300; /* a phone's chart: 33.3 ms a slice */
    const auto points = [&]() {
        std::map<double, double> out;
        const QVariantList v = model.chartData(windowMs, columns)
                                   .at(0)
                                   .toMap()
                                   .value("samples")
                                   .toList();
        for (int i = 0; i < v.size(); i += 2)
            out[v.at(i).toDouble()] = v.at(i + 1).toDouble();
        return out;
    };
    feed(20000);
    const std::map<double, double> before = points();
    feed(20130); /* one 100 ms update and a bit: not a slice's width */
    const std::map<double, double> after = points();

    /* The stretch both windows hold whole, a slice clear of either end */
    const double slice = static_cast<double>(windowMs) / columns;
    const double lo = 20120.0 - windowMs + slice;
    const double hi = 19990.0 - slice;
    const auto inside = [&](const std::map<double, double>& m) {
        std::map<double, double> out;
        for (const auto& p : m)
            if (p.first >= lo && p.first <= hi)
                out.insert(p);
        return out;
    };
    ASSERT_GT(inside(before).size(), 100u);
    EXPECT_EQ(inside(after), inside(before))
        << "the same points, slice for slice";
}

/* The Dashboard's trace (ADR-0057's note "a trace reads the model's
 * history"): one row's samples sliced as chartData slices them, for any row
 * whether Live plots it or not, over the window before its own newest sample */
TEST(BridgeTest, ATraceIsTheChartsSlicingForAnyRow)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    EXPECT_TRUE(model.trace(1001, 5000, 10).isEmpty()) << "no history yet";
    for (qint64 t = 0; t < 20000; t += 10) {
        const double wave = static_cast<double>(
            (t / 10) % 100 < 50 ? (t / 10) % 50 : 50 - (t / 10) % 50);
        model.updateValues(
            {{uint16_t(1001), wave}, {uint16_t(2001), double(t)}}, t);
    }
    const auto chartOf = [&model](int s, qint64 windowMs, int columns) {
        QList<double> out;
        for (const QVariant& v : model.chartData(windowMs, columns)
                                     .at(s)
                                     .toMap()
                                     .value("samples")
                                     .toList())
            out << v.toDouble();
        return out;
    };
    const std::pair<qint64, int> shapes[] = {
        {0, 0}, {5000, 0}, {5000, 10}, {5000, 300}, {120000, 40}};
    for (const auto& shape : shapes) {
        EXPECT_EQ(model.trace(1001, shape.first, shape.second),
                  chartOf(0, shape.first, shape.second))
            << shape.first << " ms, " << shape.second << " columns";
        EXPECT_EQ(model.trace(2001, shape.first, shape.second),
                  chartOf(1, shape.first, shape.second))
            << shape.first << " ms, " << shape.second << " columns";
    }

    const QList<double> plotted = model.trace(1001, 5000, 10);
    model.toggleSubscribed(0);
    ASSERT_EQ(model.chartData().size(), 1) << "Live no longer plots it";
    EXPECT_EQ(model.trace(1001, 5000, 10), plotted)
        << "the Dashboard still traces it";

    model.updateValues({{uint16_t(2001), 1.0}}, 30000);
    const QList<double> own = model.trace(1001, 5000, 0);
    ASSERT_FALSE(own.isEmpty());
    EXPECT_EQ(own.first(), 19990.0 - 5000.0)
        << "the window ends at the row's own newest";

    EXPECT_TRUE(model.trace(4242, 5000, 10).isEmpty()) << "no such def";
    EXPECT_TRUE(model.trace(-1, 5000, 10).isEmpty());
    EXPECT_TRUE(model.trace(1001 + 65536, 5000, 10).isEmpty())
        << "an id past 16 bits is no def's";
}

TEST(BridgeTest, ATextRowHasNoTrace)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[1].kind = ECUIO_VALUE_HEX; /* id 2001, a text row */
    model.setSignalSet(&set);
    model.updateTexts({{uint16_t(2001), QStringLiteral("62 F1 90")}}, 10);
    model.updateValues({{uint16_t(1001), 5.0}}, 10);
    EXPECT_TRUE(model.trace(2001, 0, 0).isEmpty());
    EXPECT_EQ(model.trace(1001, 0, 0), (QList<double>{10, 5}));
}

/* A one-window spike (a fuel cut's lambda) keeps its height and its place
 * in every read while it is in the window, however the window has moved */
TEST(BridgeTest, ATraceKeepsAOneWindowSpikeInPlaceAsItScrolls)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto window = [](qint64 endMs, double high) {
        SignalListModel::WindowValue w;
        w.id = 1001;
        w.last = 1.0;
        w.lastMs = endMs;
        w.min = 0.98;
        w.minMs = endMs - 70;
        w.max = high;
        w.maxMs = endMs - 40;
        return w;
    };
    const qint64 windowMs = 30000;
    const int columns = 100; /* slices of 300 ms, three windows in each */
    const qint64 spikeAt = 5100 - 40;
    int reads = 0;
    for (qint64 end = 100; end <= windowMs; end += 100) {
        model.updateWindow({window(end, end == 5100 ? 1.99 : 1.0)});
        if (end < 5100)
            continue;
        const QList<double> t = model.trace(1001, windowMs, columns);
        bool found = false;
        for (int i = 0; i + 1 < t.size(); i += 2)
            found =
                found || (t.at(i) == double(spikeAt) && t.at(i + 1) == 1.99);
        ASSERT_TRUE(found) << "the spike at its own time and height, read at "
                           << end;
        ++reads;
    }
    EXPECT_GT(reads, 200);
}

TEST(BridgeTest, ATraceIsInTheUnitSystem)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    model.setSignalSet(&set);
    model.updateValues({{uint16_t(1001), 100.0}}, 0);
    model.updateValues({{uint16_t(1001), 110.0}}, 100);
    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const QList<double> mph = model.trace(1001, 0, 0);
    ASSERT_EQ(mph.size(), 4);
    EXPECT_NEAR(mph.at(1), 100.0 * 0.621371192237334, 1e-9);
    EXPECT_NEAR(mph.at(3), 110.0 * 0.621371192237334, 1e-9);
    model.setUnitSystem(ECUIO_UNITS_METRIC);
    EXPECT_EQ(model.trace(1001, 0, 0), (QList<double>{0, 100, 100, 110}));
}

/* Two minutes of windows, each with its lowest, highest and newest apart
 * (three points for each window), fit the ring: the longest trace is whole */
TEST(BridgeTest, TheRingHoldsTheLongestTracesWindow)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const qint64 longestMs = 120000;
    for (qint64 end = 0; end <= longestMs; end += 100) {
        SignalListModel::WindowValue w;
        w.id = 1001;
        w.last = static_cast<double>(end % 7);
        w.lastMs = end;
        w.min = -1.0;
        w.minMs = end - 60;
        w.max = 50.0;
        w.maxMs = end - 30;
        model.updateWindow({w});
    }
    const QList<double> t = model.trace(1001, longestMs, 0);
    ASSERT_FALSE(t.isEmpty());
    EXPECT_LE(t.first(), 100.0)
        << "the trace reaches back to its window's start";
    EXPECT_EQ(t.at(t.size() - 2), double(longestMs));
}

/* A trace breaks only where the ring stalls past the stale time, never on
 * its slices' spacing: a phone tile's 120 s over 40 columns (3 s slices)
 * draws a signal every 100 ms as one piece, and a lone sample between two
 * stalls keeps its value */
TEST(BridgeTest, ATraceBreaksOnlyWhereTheRingStalls)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto breaks = [](const QList<double>& t) {
        int n = 0;
        for (int i = 1; i < t.size(); i += 2) {
            n += std::isnan(t.at(i)) ? 1 : 0;
        }
        return n;
    };
    qint64 t = 0;
    for (; t < 120000; t += 100) {
        model.updateValues({{uint16_t(1001), static_cast<double>(t % 900)}}, t);
    }
    const QList<double> narrow = model.trace(1001, 120000, 40);
    ASSERT_GT(narrow.size(), 40);
    EXPECT_EQ(breaks(narrow), 0) << "slices 3 s wide are no stall";

    const qint64 stall = SessionBridge::valueStaleMs() + 500;
    t += stall;
    model.updateValues({{uint16_t(1001), 1.99}}, t);
    t += stall;
    for (const qint64 end = t + 2000; t < end; t += 100) {
        model.updateValues({{uint16_t(1001), 1.0}}, t);
    }
    const QList<double> after = model.trace(1001, 120000, 40);
    EXPECT_EQ(breaks(after), 2) << "one break for each stall";
    bool lone = false;
    for (int i = 3; i + 2 < after.size(); i += 2) {
        lone = lone || (std::isnan(after.at(i - 2)) && after.at(i) == 1.99 &&
                        std::isnan(after.at(i + 2)));
    }
    EXPECT_TRUE(lone) << "the lone sample between the stalls, at its value";
    EXPECT_EQ(breaks(model.trace(1001, 120000, 0)), 2) << "unsliced too";
}

/* A value that isn't a number is a point of its own: the slice it falls in
 * still shows its lowest and highest number */
TEST(BridgeTest, AValueThatIsntANumberHidesNoSlicesExtremes)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    model.updateValues(
        {{uint16_t(1001), std::numeric_limits<double>::quiet_NaN()}}, 1000);
    model.updateValues({{uint16_t(1001), 1.0}}, 1100);
    model.updateValues({{uint16_t(1001), 9.0}}, 1200);
    model.updateValues({{uint16_t(1001), 5.0}}, 1300);
    model.updateValues({{uint16_t(1001), 4.0}}, 1400);
    /* one slice of 5 s holds them all */
    const QList<double> t = model.trace(1001, 5000, 1);
    QList<double> numbers;
    int breaks = 0;
    for (int i = 0; i + 1 < t.size(); i += 2) {
        if (std::isnan(t.at(i + 1))) {
            ++breaks;
            EXPECT_TRUE(numbers.isEmpty())
                << "the break comes first, at its time";
        } else {
            numbers << t.at(i) << t.at(i + 1);
        }
    }
    EXPECT_GE(breaks, 1);
    EXPECT_EQ(numbers, (QList<double>{1100, 1, 1200, 9, 1400, 4}))
        << "the slice's low, its high and the newest";
}

TEST(BridgeTest, UnplottedKeysReadAndSetTheChartsRows)
{
    SessionBridge bridge;
    SignalListModel* m = bridge.signalModel();
    int countNotified = 0;
    QObject::connect(m, &SignalListModel::subscribedCountChanged,
                     [&countNotified]() { ++countNotified; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    EXPECT_GE(countNotified, 1) << "a new set's count is notified";
    ASSERT_GE(m->rowCount(), 4);
    const auto key = [m](int row) {
        return m->data(m->index(row), SignalListModel::KeyRole).toString();
    };
    const auto plotted = [m](int row) {
        return m->data(m->index(row), SignalListModel::SubscribedRole).toBool();
    };
    const int all = m->subscribedCount();
    ASSERT_FALSE(key(0).isEmpty()) << "the session's rows have keys";
    EXPECT_TRUE(m->unplottedKeys().isEmpty())
        << "every numeric row starts plotted";
    EXPECT_TRUE(m->plottedKeys().isEmpty()) << "and none past its default";

    m->setPlotChoices({key(1)},
                      {key(0), key(2), key(1), QStringLiteral("Not a signal")});
    EXPECT_EQ(m->unplottedKeys(), (QStringList{key(0), key(1), key(2),
                                               QStringLiteral("Not a signal")}))
        << "a row in both lists is unplotted, and a text no row answers "
           "is carried after the rows";
    EXPECT_TRUE(m->plottedKeys().isEmpty());
    EXPECT_EQ(m->subscribedCount(), all - 3);
    EXPECT_FALSE(plotted(0));
    EXPECT_TRUE(plotted(3));

    m->setPlotChoices({}, {});
    EXPECT_TRUE(m->unplottedKeys().isEmpty());
    EXPECT_EQ(m->subscribedCount(), all);

    m->toggleSubscribed(1);
    EXPECT_EQ(m->unplottedKeys(), QStringList{key(1)})
        << "a click is read back";
    m->toggleSubscribed(1);
    EXPECT_TRUE(m->unplottedKeys().isEmpty())
        << "a click back to the default clears the choice";
    EXPECT_TRUE(m->plottedKeys().isEmpty()) << "rather than turning it over";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* The names of the rows the chart plots, in set order */
QStringList plottedRows(const SignalListModel& model)
{
    QStringList names;
    for (int r = 0; r < model.rowCount(); ++r) {
        const QModelIndex i = model.index(r);
        if (model.data(i, SignalListModel::SubscribedRole).toBool()
                && !model.data(i, SignalListModel::HiddenRole).toBool()) {
            names << model.data(i, SignalListModel::NameRole).toString();
        }
    }
    return names;
}

/* The test car's first two ranges with PID 0x5C clear, as its
 * bitmaps read (docs/bench-sessions/2026-10-31-elm327-vehicle.md) */
ecuio_channel_support_t carWithoutOilTemp()
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    return car;
}

} // namespace

/* ADR-0039 section 8: a ranked set plots the DefaultPlotted shown rows
 * of the lowest plot_rank; a row Live hides gives its place to the
 * next, and a rescan that shows it takes the place back */
TEST(BridgeTest, ARankedSetPlotsTheTopRankedRowsItShows)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    ASSERT_EQ(SignalListModel::DefaultPlotted, 6);
    const QStringList topSix = {
        QStringLiteral("Coolant Temp"),    QStringLiteral("RPM"),
        QStringLiteral("Vehicle Speed"),   QStringLiteral("Intake Air Temp"),
        QStringLiteral("MAF"),             QStringLiteral("Engine Oil Temp")};
    EXPECT_EQ(plottedRows(model), topSix) << "ranks 1 to 6, in set order";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    const ecuio_channel_support_t car = carWithoutOilTemp();
    model.applyUnsupported(unsupportedIds(&car, 1));
    QStringList withLoad = topSix;
    withLoad.removeOne(QStringLiteral("Engine Oil Temp"));
    withLoad.prepend(QStringLiteral("Engine Load"));
    EXPECT_EQ(plottedRows(model), withLoad) << "rank 7 takes the hidden row's place";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&rescan, 1));
    EXPECT_EQ(plottedRows(model), topSix) << "and gives it back";

    /* A car with fewer ranked rows than that plots all it has */
    ecuio_channel_support_t sparse{};
    sparse.flags = ECUIO_SUPPORT_PROBED;
    sparse.rx_id = 0x7E8;
    sparse.generation = 3;
    sparse.ranges = 0x7; /* 0x01-0x60: only RPM and speed */
    sparse.pid_bitmap[(0x0C - 1) / 8] |= 0x80u >> ((0x0C - 1) % 8);
    sparse.pid_bitmap[(0x0D - 1) / 8] |= 0x80u >> ((0x0D - 1) % 8);
    model.applyUnsupported(unsupportedIds(&sparse, 1));
    EXPECT_EQ(plottedRows(model),
              (QStringList{QStringLiteral("RPM"), QStringLiteral("Vehicle Speed")}));
}

/* A refused row never has a value to draw: it gives its place among the
 * default plotted rows to the next, as a hidden one does, and takes it
 * back once it isn't refused; a tick still plots it. In a set that ranks
 * none, it is no default either */
TEST(BridgeTest, ARefusedRowGivesItsDefaultPlotToTheNext)
{
    SignalListModel model;
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    model.setSignalSet(set);
    const QStringList topSix = plottedRows(model);
    int oilRow = -1;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (std::strcmp(set->defs[i].name, "Engine Oil Temp") == 0) {
            oilRow = static_cast<int>(i);
        }
    }
    ASSERT_GE(oilRow, 0);
    ASSERT_TRUE(topSix.contains(QStringLiteral("Engine Oil Temp")));
    const SignalListModel::Refusal oil{
        static_cast<uint16_t>(set->defs[oilRow].id),
        QStringLiteral("Refused: subFunctionNotSupported (7F 01 12)"),
        QStringLiteral("7F 01 12")};

    model.applyRefused({oil});
    QStringList withLoad = topSix;
    withLoad.removeOne(QStringLiteral("Engine Oil Temp"));
    withLoad.prepend(QStringLiteral("Engine Load"));
    EXPECT_EQ(plottedRows(model), withLoad)
        << "rank 7 takes the refused row's place";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);
    model.applyRefused({});
    EXPECT_EQ(plottedRows(model), topSix) << "and gives it back";

    model.applyRefused({oil});
    model.toggleSubscribed(oilRow);
    EXPECT_TRUE(plottedRows(model).contains(QStringLiteral("Engine Oil Temp")))
        << "a tick plots it all the same";

    SignalListModel plain;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t two;
    buildTestSignalSet(defs, two);
    plain.setSignalSet(&two);
    ASSERT_EQ(plottedRows(plain).size(), 2)
        << "an unranked set plots every row";
    plain.applyRefused(
        {{static_cast<uint16_t>(defs[1].id),
          QStringLiteral("Refused: requestOutOfRange (7F 22 31)"),
          QStringLiteral("7F 22 31")}});
    EXPECT_EQ(plottedRows(plain), QStringList{QStringLiteral("Engine_RPM")});
    EXPECT_EQ(plain.subscribedCount(), 1);
}

/* The user's ticks and unticks outlive the defaults moving, and a click
 * back to the default clears one */
TEST(BridgeTest, APlotChoiceOutlivesTheDefaultsMoving)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    const int rpm = obd2RowOf(ECUIO_OBD2_SIG_RPM_ID);
    const int load = obd2RowOf(ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    const int pedal = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x49, 0));
    ASSERT_GE(rpm, 0);
    ASSERT_GE(load, 0);
    ASSERT_GE(pedal, 0);
    const auto plotted = [&model](int row) {
        return model.data(model.index(row), SignalListModel::SubscribedRole)
            .toBool();
    };

    model.toggleSubscribed(rpm);
    model.toggleSubscribed(load);
    EXPECT_FALSE(plotted(rpm));
    EXPECT_TRUE(plotted(load));
    const auto key = [&model](int row) {
        return model.data(model.index(row), SignalListModel::KeyRole)
            .toString();
    };
    EXPECT_EQ(model.unplottedKeys(), QStringList{key(rpm)});
    EXPECT_EQ(model.plottedKeys(), QStringList{key(load)});
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    /* Load's default turns on and RPM's stays on: neither choice moves */
    const ecuio_channel_support_t car = carWithoutOilTemp();
    model.applyUnsupported(unsupportedIds(&car, 1));
    EXPECT_FALSE(plotted(rpm)) << "an untick holds against its default";
    EXPECT_TRUE(plotted(load));
    EXPECT_FALSE(plotted(pedal)) << "an untick leaves its place empty";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted - 1);

    model.toggleSubscribed(rpm);
    EXPECT_TRUE(model.unplottedKeys().isEmpty()) << "back to its default";
    EXPECT_EQ(model.plottedKeys(), QStringList{key(load)})
        << "with no choice of its own";

    /* A vehicle's lists, applied whole: every other row to its default */
    model.setPlotChoices({QStringLiteral("Accelerator Pedal D")}, {});
    EXPECT_TRUE(plotted(pedal));
    EXPECT_TRUE(plotted(load)) << "its default now, with no choice";
    EXPECT_TRUE(model.unplottedKeys().isEmpty());
    EXPECT_EQ(model.plottedKeys(), QStringList{key(pedal)});

    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_TRUE(model.plottedKeys().isEmpty())
        << "a new set starts with no choices";
}

/* The chart keeps a peak (ADR-0005's appendix): the vehicle fixture's
 * engine start peaks at 1,261 rpm, and replayed at 100x under obd2,
 * ten seconds of the recording to a window, the chart reaches it. Each
 * sample sits at its frame's time in the recording, in order */
TEST(BridgeTest, TheChartKeepsTheFixturesPeak)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&speed=100")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    double peak = 0.0;
    bool ordered = true;
    qint64 lastMs = -1;
    QObject::connect(model, &SignalListModel::valuesUpdated, model, [&]() {
        for (const QVariant& s : model->chartData()) {
            const QVariantMap m = s.toMap();
            if (m.value("name").toString() != QStringLiteral("RPM"))
                continue;
            const QVariantList samples = m.value("samples").toList();
            qint64 prev = -1;
            for (qsizetype i = 0; i + 1 < samples.size(); i += 2) {
                const qint64 t = samples.at(i).toLongLong();
                ordered = ordered && t >= prev && t <= 297446;
                prev = t;
                peak = std::max(peak, samples.at(i + 1).toDouble());
            }
            lastMs = prev;
        }
    });
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() + bridge.droppedFrames() >= 7074; },
        20000));
    spinLoop(300);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_NEAR(peak, 1261.25, 0.01) << "the recording's peak, the sheet's 1,261";
    EXPECT_TRUE(ordered) << "times in order, inside the recording";
    EXPECT_GT(lastMs, 290000) << "the last sample near the recording's end";
}
