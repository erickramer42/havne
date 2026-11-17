// File/layer: tests/test_bridge_live.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest: Live (hidden, refused and unasked rows, the filter, the gauges' pick) (test_bridge)
//
// Cases for what Live shows and hides.

#include <gtest/gtest.h>

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <cstring>
#include <initializer_list>
#include <utility>
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

/* A set with no rows (raw, or none) ticks nothing: Start log goes by it */
TEST(BridgeTest, AnEmptySetLeavesNoRowTicked)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    ASSERT_GT(model.tickedCount(), 0);
    int changes = 0;
    QObject::connect(&model, &SignalListModel::tickedCountChanged,
                     [&changes]() { ++changes; });
    model.setSignalSet(nullptr);
    EXPECT_EQ(model.tickedCount(), 0);
    EXPECT_EQ(changes, 1);

    model.setSignalSet(&set);
    ASSERT_GT(model.tickedCount(), 0);
    ecuio_signal_set_t none = set;
    none.count = 0;
    model.setSignalSet(&none);
    EXPECT_EQ(model.tickedCount(), 0);
}

/* The rows the focus leaves unasked (ecuio_session_get_unasked_signals)
 * say so, and Live counts the ones it shows; a new set asks every row */
TEST(BridgeTest, AnUnaskedRowSaysSoAndLiveCountsTheOnesItShows)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    int counted = 0;
    QObject::connect(&model, &SignalListModel::unaskedCountChanged,
                     [&counted]() { ++counted; });
    const auto rowOf = [&model](uint16_t id) {
        for (int r = 0; r < model.rowCount(); ++r) {
            if (model.data(model.index(r), SignalListModel::IdRole).toUInt() ==
                id) {
                return model.index(r);
            }
        }
        return QModelIndex();
    };
    const QModelIndex throttle = rowOf(ECUIO_OBD2_SIG_THROTTLE_ID);
    const QModelIndex fuel = rowOf(ECUIO_OBD2_SIG_FUEL_LEVEL_ID);
    const QModelIndex rpm = rowOf(ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_TRUE(throttle.isValid() && fuel.isValid() && rpm.isValid());
    EXPECT_FALSE(model.data(throttle, SignalListModel::UnaskedRole).toBool());

    model.applyUnasked(
        {ECUIO_OBD2_SIG_THROTTLE_ID, ECUIO_OBD2_SIG_FUEL_LEVEL_ID});
    EXPECT_TRUE(model.data(throttle, SignalListModel::UnaskedRole).toBool());
    EXPECT_TRUE(model.data(fuel, SignalListModel::UnaskedRole).toBool());
    EXPECT_FALSE(model.data(rpm, SignalListModel::UnaskedRole).toBool());
    EXPECT_EQ(model.unaskedCount(), 2);
    EXPECT_EQ(counted, 1);

    /* A hidden row isn't Live's to count */
    model.applyUnsupported({ECUIO_OBD2_SIG_FUEL_LEVEL_ID});
    EXPECT_EQ(model.unaskedCount(), 1);

    /* Each call judges afresh */
    model.applyUnasked({ECUIO_OBD2_SIG_RPM_ID});
    EXPECT_FALSE(model.data(throttle, SignalListModel::UnaskedRole).toBool());
    EXPECT_TRUE(model.data(rpm, SignalListModel::UnaskedRole).toBool());
    EXPECT_EQ(model.unaskedCount(), 1);

    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.unaskedCount(), 0);
    EXPECT_FALSE(
        model.data(rowOf(ECUIO_OBD2_SIG_RPM_ID), SignalListModel::UnaskedRole)
            .toBool());
}

/* The gauges the default pick adds: each cell's first ref the set has,
 * found as SignalHub finds it (a key def that took a base def's place
 * answers to the base's id: too), each once */
TEST(BridgeTest, AGaugesPickIsItsCellsFirstRefTheSetHas)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const auto cells = [](std::initializer_list<QStringList> lists) {
        QVariantList out;
        for (const QStringList& refs : lists) {
            out.append(refs);
        }
        return out;
    };
    EXPECT_EQ(SessionBridge::gaugeIds(
                  obd2, nullptr,
                  cells({{QStringLiteral("key:none"), QStringLiteral("id:7006"),
                          QStringLiteral("id:7003")},
                         {},
                         {QStringLiteral("id:99")},
                         {QStringLiteral("id:7003")},
                         {QStringLiteral("id:7006")}})),
              (std::vector<uint16_t>{7006, 7003}));
    EXPECT_TRUE(SessionBridge::gaugeIds(nullptr, nullptr,
                                        cells({{QStringLiteral("id:7003")}}))
                    .empty());

    /* A file's RPM replaces obd2's and keeps its id; its own signal has
     * a key alone */
    const QByteArray text =
        R"({"havne_signals":1,"name":"Pick","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"rpm","name":"RPM","read":{"service":"22","number":"1E1C"},)"
        R"("field":{"byte":0,"bits":16}},)"
        R"({"key":"own","name":"Own","read":{"service":"22","number":"1E1D"},)"
        R"("field":{"byte":0,"bits":8}}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.constData(),
                                     static_cast<size_t>(text.size()), &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    const ecuio_signal_def_t* own = ecuio_signal_file_find(file, "own");
    ASSERT_NE(own, nullptr);
    EXPECT_EQ(SessionBridge::gaugeIds(set, file,
                                      cells({{QStringLiteral("id:7003")},
                                             {QStringLiteral("key:own")}})),
              (std::vector<uint16_t>{ECUIO_OBD2_SIG_RPM_ID, own->id}));
    EXPECT_EQ(SessionBridge::gaugeIds(set, file,
                                      cells({{QStringLiteral("key:rpm")}})),
              std::vector<uint16_t>{ECUIO_OBD2_SIG_RPM_ID});
    EXPECT_TRUE(SessionBridge::gaugeIds(
                    set, file, cells({{QStringLiteral("id:%1").arg(own->id)}}))
                    .empty())
        << "a file's own signal answers to its key alone";
    ecuio_signal_file_free(file);
}

/* A session that asks nothing (the mock, under a profile that ranks its
 * signals too) takes no default pick: no line, no row unasked, and its
 * recording names no pick */
TEST(BridgeTest, ASessionThatAsksNothingTakesNoPick)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setPickGauges({QVariant(QStringList{QStringLiteral("id:7006")})});
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=obd2"))
            .isEmpty());
    bridge.signalModel()->toggleSubscribed(0);
    spinLoop(400);
    EXPECT_TRUE(
        log.filter(QRegularExpression(QStringLiteral("^Asking "))).isEmpty())
        << log.join('\n').toStdString();
    EXPECT_EQ(bridge.signalModel()->unaskedCount(), 0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray events =
        readFile(root.path() + QLatin1Char('/') + folders[0] +
                 QStringLiteral("/events.log"));
    EXPECT_FALSE(events.contains("\npick  ")) << events.toStdString();
}

/* Live's count of the refusals past a report's list: each ECU's, one
 * request said as one */
TEST(BridgeTest, TheRefusalsLiveCantNameAreCounted)
{
    EXPECT_EQ(SessionBridge::refusedUnnamedText({}), QString());
    EXPECT_EQ(SessionBridge::refusedUnnamedText({{0x7E8, 3}}),
              QStringLiteral("ECU 0x7E8 refused 3 more requests"));
    EXPECT_EQ(SessionBridge::refusedUnnamedText({{0x7E8, 1}, {0x7E9, 2}}),
              QStringLiteral("ECU 0x7E8 refused 1 more request; "
                             "ECU 0x7E9 refused 2 more requests"));
}

/* ADR-0051 section 3: a refused signal's words name the code, with the
 * reply in frames.csv's form; a code with no name gives the reply alone */
TEST(BridgeTest, ARefusalNamesItsCode)
{
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x31),
              QStringLiteral("Refused: requestOutOfRange (7F 22 31)"));
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x33),
              QStringLiteral("Refused: securityAccessDenied (7F 22 33)"));
    EXPECT_EQ(SessionBridge::refusalText(0x01, 0x12),
              QStringLiteral("Refused: subFunctionNotSupported (7F 01 12)"));
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x99),
              QStringLiteral("Refused: 7F 22 99"));
    EXPECT_EQ(SessionBridge::refusalCode(0x22, 0x31),
              QStringLiteral("7F 22 31"));
}

/* The model takes each refusal by id, judges every row afresh on each
 * call, and a new set refuses none */
TEST(BridgeTest, ARefusedRowSaysSoUntilItIsntRefused)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto refused = [&model](int row) {
        return model.data(model.index(row, 0), SignalListModel::RefusedRole)
            .toString();
    };
    QList<int> changed;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&changed](const QModelIndex& first, const QModelIndex&,
                                const QList<int>& roles) {
                         if (roles.contains(SignalListModel::RefusedRole)) {
                             changed << first.row();
                         }
                     });
    const uint16_t second = static_cast<uint16_t>(defs[1].id);
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()},
         {9999, QStringLiteral("x"), QString()}});
    EXPECT_EQ(refused(0), QString());
    EXPECT_EQ(refused(1), QStringLiteral("Refused: 7F 22 31"));
    EXPECT_EQ(changed, QList<int>{1}) << "only the row that changed";
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()}});
    EXPECT_EQ(changed, QList<int>{1}) << "the same words change nothing";
    model.applyRefused({});
    EXPECT_EQ(refused(1), QString());
    // The reply's bytes come with the words, for a row too narrow for them
    const auto code = [&model](int row) {
        return model.data(model.index(row, 0), SignalListModel::RefusedCodeRole)
            .toString();
    };
    model.applyRefused({{second, QStringLiteral("Refused: 7F 22 31"),
                         QStringLiteral("7F 22 31")}});
    EXPECT_EQ(code(1), QStringLiteral("7F 22 31"));
    EXPECT_EQ(code(0), QString());
    model.setSignalSet(&set);
    EXPECT_EQ(code(1), QString()) << "a new set's rows carry no code";
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()}});
    model.setSignalSet(&set);
    EXPECT_EQ(refused(1), QString()) << "a new set refuses none";
}

/* The "ECU 0x…:" line's wording, from hand-built reports: the first
 * vehicle's three bitmaps (docs/bench-sessions/2026-10-22-vehicle.md:
 * 44 data PIDs to 0x60, whose bit says a fourth exists), a chain
 * answered to its end, one unanswered, and requests dropped for each
 * reason, past the eight a report lists. */
/* Finding a signal among many (ADR-0050): every word in the name or the
 * unit, ignoring case; plotted rows only; the chosen groups, a code
 * core doesn't name counted as Other; the choices in code order */
TEST(BridgeTest, SignalFilterMatchesWordsPlottedRowsAndGroups)
{
    SignalFilter f;
    int changes = 0;
    QObject::connect(&f, &SignalFilter::changed, [&changes]() { ++changes; });
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, false));

    f.setText(QStringLiteral("  COOL   temp "));
    EXPECT_TRUE(f.active());
    EXPECT_TRUE(f.matches(QStringLiteral("Coolant Temp"), QStringLiteral("°C"),
                          1, false));
    EXPECT_FALSE(f.matches(QStringLiteral("Engine Oil Temp"),
                           QStringLiteral("°C"), 1, false))
        << "every word";
    f.setText(QStringLiteral("temp °c"));
    EXPECT_TRUE(f.matches(QStringLiteral("Engine Oil Temp"),
                          QStringLiteral("°C"), 1, false))
        << "a word in the unit";
    f.setText(QStringLiteral("psi"));
    EXPECT_TRUE(f.matches(QStringLiteral("Fuel Pressure"),
                          QStringLiteral("psi"), 3, false));
    EXPECT_FALSE(f.matches(QStringLiteral("Fuel Pressure"),
                           QStringLiteral("kPa"), 3, false));
    f.setText(QString());
    EXPECT_FALSE(f.active());

    f.setPlottedOnly(true);
    EXPECT_FALSE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, false));
    EXPECT_TRUE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, true));
    f.setPlottedOnly(false);

    f.toggleGroup(ECUIO_SIGNAL_GROUP_ENGINE);
    EXPECT_TRUE(f.chosen(ECUIO_SIGNAL_GROUP_ENGINE));
    EXPECT_TRUE(f.matches(QStringLiteral("RPM"), QString(),
                          ECUIO_SIGNAL_GROUP_ENGINE, false));
    EXPECT_FALSE(f.matches(QStringLiteral("MAF"), QString(),
                           ECUIO_SIGNAL_GROUP_AIR, false));
    f.toggleGroup(ECUIO_SIGNAL_GROUP_AIR);
    EXPECT_TRUE(f.matches(QStringLiteral("MAF"), QString(),
                          ECUIO_SIGNAL_GROUP_AIR, false))
        << "either chosen group";
    f.toggleGroup(200);
    EXPECT_TRUE(f.chosen(ECUIO_SIGNAL_GROUP_OTHER))
        << "an unknown code is Other's";
    EXPECT_TRUE(f.matches(QStringLiteral("DID 1E1C"), QString(), 0, false));
    EXPECT_TRUE(
        f.matches(QStringLiteral("Next year's"), QString(), 250, false));
    EXPECT_EQ(f.groups(), (QVariantList{0, 1, 2}));
    f.toggleGroup(ECUIO_SIGNAL_GROUP_ENGINE);
    EXPECT_FALSE(f.chosen(ECUIO_SIGNAL_GROUP_ENGINE))
        << "a second tap lets it go";

    const QVariantList choices = f.groupChoices(QVariantList{3, 1, 1, 200, 0});
    ASSERT_EQ(choices.size(), 3);
    EXPECT_EQ(choices[0].toMap().value(QStringLiteral("code")).toInt(), 0);
    EXPECT_EQ(choices[0].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Other"));
    EXPECT_EQ(choices[1].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine"));
    EXPECT_EQ(choices[2].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Fuel"));

    f.clear();
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(f.groups().isEmpty());
    EXPECT_EQ(changes, 11);
}

/* Live's list through its filter (ADR-0050): the mock's rows by word,
 * group and plotted; a tick under "Plotted only" re-filters; the groups
 * and the count before the filter leave out the rows core hides */
TEST(BridgeTest, LiveListFollowsItsFilter)
{
    SignalListModel model;
    model.setSignalSet(ecuio_mock_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    ASSERT_EQ(live.rowCount(), rows);
    EXPECT_EQ(live.shownCount(), rows);
    EXPECT_EQ(live.groups(), (QVariantList{1, 2, 4, 6, 7}))
        << "Engine, Air, Oxygen sensors, Vehicle, Electrical";
    const auto names = [&live]() {
        QStringList n;
        for (int r = 0; r < live.rowCount(); ++r) {
            n << live.data(live.index(r, 0), SignalListModel::NameRole)
                     .toString();
        }
        return n;
    };

    live.filter()->setText(QStringLiteral("temp"));
    EXPECT_EQ(names(), (QStringList{QStringLiteral("Coolant Temp"),
                                    QStringLiteral("Intake Air Temp"),
                                    QStringLiteral("Engine Oil Temp")}));
    live.filter()->toggleGroup(ECUIO_SIGNAL_GROUP_AIR);
    EXPECT_EQ(names(), QStringList{QStringLiteral("Intake Air Temp")});
    live.filter()->clear();
    EXPECT_EQ(live.rowCount(), rows);

    live.filter()->setPlottedOnly(true);
    EXPECT_EQ(live.rowCount(), rows) << "the mock plots every row";
    model.toggleSubscribed(0); /* RPM */
    EXPECT_EQ(live.rowCount(), rows - 1);
    EXPECT_FALSE(names().contains(QStringLiteral("RPM")));
    EXPECT_EQ(live.shownCount(), rows) << "the count before the filter";
    live.filter()->clear();

    /* obd2's Diagnostics rows hidden by core: no choice, not counted */
    model.setSignalSet(ecuio_obd2_signal_set());
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> diag;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].group == ECUIO_SIGNAL_GROUP_DIAGNOSTICS) {
            diag.push_back(set->defs[i].id);
        }
    }
    ASSERT_FALSE(diag.empty());
    EXPECT_TRUE(live.groups().contains(QVariant(8)));
    model.applyAbsent(diag);
    EXPECT_FALSE(live.groups().contains(QVariant(8)));
    EXPECT_EQ(live.shownCount(),
              model.rowCount() - static_cast<int>(diag.size()));
}

/* Live leaves out the rows core judges unsupported (ADR-0039 section 6;
 * the rule is PidSupportTest's): on the first vehicle's bitmaps (00, 20
 * and 40 answered), 0x5C among them; a PID past an answered range, and a
 * row no report names, stay. Each list is applied afresh, so a rescan
 * that finds a PID shows its row again. */
TEST(BridgeTest, LiveHidesTheRowsTheirEcuMarksUnsupported)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    ASSERT_GT(rows, 0);
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);
    int hiddenSignals = 0;
    QObject::connect(&model, &SignalListModel::hiddenCountChanged,
                     [&hiddenSignals]() { ++hiddenSignals; });

    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    model.applyUnsupported(unsupportedIds(&car, 1));

    /* The rule, from the bitmaps' bytes: PID p is supported when bit
     * 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8 is set */
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    int expectHidden = 0;
    int pastTheRanges = 0;
    int row5C = -1;
    int row0C = -1;
    for (int r = 0; r < rows; ++r) {
        const unsigned pid =
            (set->defs[r].flags & ECUIO_DEF_FLAG_COMPUTED) != 0
                ? 0x100u /* a computed def has no PID: past every range */
                : set->defs[r].mux_key & 0xFFu;
        const bool clear = pid <= 0x60 && (bitmaps[(pid - 1) / 8] &
                                           (0x80u >> ((pid - 1) % 8))) == 0;
        const QModelIndex i = model.index(r);
        EXPECT_EQ(model.data(i, SignalListModel::HiddenRole).toBool(), clear)
            << "PID " << std::hex << pid;
        EXPECT_EQ(model.data(i, SignalListModel::RowRole).toInt(), r);
        expectHidden += clear ? 1 : 0;
        pastTheRanges += pid > 0x60 ? 1 : 0;
        if (pid == 0x5C)
            row5C = r;
        if (pid == 0x0C)
            row0C = r;
    }
    ASSERT_GE(row5C, 0);
    ASSERT_GE(row0C, 0);
    EXPECT_TRUE(
        model.data(model.index(row5C), SignalListModel::HiddenRole).toBool());
    EXPECT_GT(expectHidden, 1);
    EXPECT_GT(pastTheRanges, 0);
    EXPECT_EQ(model.hiddenCount(), expectHidden);
    EXPECT_EQ(hiddenSignals, 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted)
        << "a ranked set plots its top ranked rows";

    /* Live's rows are the shown ones in order, each naming its model row */
    ASSERT_EQ(live.rowCount(), rows - expectHidden);
    int last = -1;
    for (int r = 0; r < live.rowCount(); ++r) {
        const QModelIndex i = live.index(r, 0);
        const int source = live.data(i, SignalListModel::RowRole).toInt();
        EXPECT_GT(source, last);
        last = source;
        EXPECT_FALSE(live.data(i, SignalListModel::HiddenRole).toBool());
        EXPECT_EQ(live.data(i, SignalListModel::NameRole),
                  model.data(model.index(source), SignalListModel::NameRole));
    }

    /* A hidden row is never charted, and its plot box counts nothing */
    model.updateValues(
        {{set->defs[row5C].id, 80.0}, {set->defs[row0C].id, 2000.0}}, 100);
    QList<int> charted;
    for (const QVariant& v : model.chartData()) {
        charted << v.toMap().value(QStringLiteral("row")).toInt();
    }
    EXPECT_TRUE(charted.contains(row0C));
    EXPECT_FALSE(charted.contains(row5C));
    model.toggleSubscribed(row5C);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);
    model.toggleSubscribed(row5C);

    /* A rescan that finds 0x5C shows it again */
    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&rescan, 1));
    EXPECT_FALSE(
        model.data(model.index(row5C), SignalListModel::HiddenRole).toBool());
    EXPECT_EQ(model.hiddenCount(), expectHidden - 1);
    EXPECT_EQ(live.rowCount(), rows - expectHidden + 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    /* Another ECU's report, or one with nothing learned, hides nothing */
    ecuio_channel_support_t other = car;
    other.rx_id = 0x7E9;
    ecuio_channel_support_t nothing = car;
    nothing.generation = 0;
    const ecuio_channel_support_t neither[2] = {other, nothing};
    model.applyUnsupported(unsupportedIds(neither, 2));
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);

    /* A new set shows every row */
    model.applyUnsupported(unsupportedIds(&car, 1));
    ASSERT_GT(model.hiddenCount(), 0);
    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);
}

/* ADR-0039 section 6: a row core reports absent (its field past every reply)
 * is hidden beside the ones the bitmaps rule out, and shown again when
 * core no longer reports it; a PID the bitmaps list keeps its other
 * rows. A new set shows all. */
TEST(BridgeTest, LiveHidesTheFieldsCoreReportsAbsent)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    const int b1 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 0));
    const int b3 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ASSERT_GE(b1, 0);
    ASSERT_GE(b3, 0);
    const auto hidden = [&model](int row) {
        return model.data(model.index(row), SignalListModel::HiddenRole)
            .toBool();
    };

    model.applyAbsent({ECUIO_OBD2_PID_SIG_ID(0x55, 1)});
    EXPECT_TRUE(hidden(b3));
    EXPECT_FALSE(hidden(b1));
    EXPECT_EQ(model.hiddenCount(), 1);
    EXPECT_EQ(live.rowCount(), rows - 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted)
        << "an unranked row's hiding moves no default";

    /* The bitmaps hide theirs beside it; neither undoes the other */
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x1; /* 0x01-0x20: all clear but 0x0C */
    car.pid_bitmap[(0x0C - 1) / 8] = 0x80u >> ((0x0C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&car, 1));
    int unsupported = 0;
    for (int r = 0; r < rows; ++r) {
        const ecuio_signal_def_t& d = ecuio_obd2_signal_set()->defs[r];
        /* Boost, the set's computed def, reads MAP (0B), which this car
         * lacks: it is unsupported with it */
        const unsigned pid = (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0
                                 ? 0x0Bu
                                 : d.mux_key & 0xFFu;
        unsupported += (pid <= 0x20 && pid != 0x0C) ? 1 : 0;
    }
    ASSERT_GT(unsupported, 0);
    EXPECT_TRUE(hidden(obd2RowOf(ECUIO_OBD2_SIG_BOOST_ID)));
    EXPECT_TRUE(hidden(b3)) << "still absent after the support's judgment";
    EXPECT_EQ(model.hiddenCount(), unsupported + 1);

    model.applyAbsent({});
    EXPECT_FALSE(hidden(b3)) << "core no longer reports it";
    EXPECT_EQ(model.hiddenCount(), unsupported);

    model.applyAbsent({ECUIO_OBD2_PID_SIG_ID(0x55, 1)});
    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.hiddenCount(), 0) << "a new set shows every row";
}

/* A hidden row says why: its ECU's supported-PID list leaves it out
 * ("unsupported", first when both hold) or its replies come without the
 * field ("absent"); a shown row says nothing. A reason that changes under
 * a row that stays hidden is notified, since Live's list shows it. */
TEST(BridgeTest, AHiddenRowSaysWhy)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    const int b3 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    const int coolant = obd2RowOf(ECUIO_OBD2_SIG_COOLANT_TEMP_ID);
    const int rpm = obd2RowOf(ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_GE(b3, 0);
    ASSERT_GE(coolant, 0);
    ASSERT_GE(rpm, 0);
    const auto why = [&model](int row) {
        return model.data(model.index(row), SignalListModel::HiddenWhyRole)
            .toString();
    };
    EXPECT_EQ(why(b3), QString()) << "a shown row says nothing";

    model.applyAbsent({ECUIO_OBD2_PID_SIG_ID(0x55, 1),
                       ECUIO_OBD2_SIG_COOLANT_TEMP_ID});
    EXPECT_EQ(why(b3), QStringLiteral("absent"));
    EXPECT_EQ(why(coolant), QStringLiteral("absent"));

    /* A car that supports every PID in 0x01-0x20 but 0x05: only the coolant
     * row's reason changes (absent, then unsupported, first), and it stays
     * hidden, so what is notified is that one row and nothing wider */
    std::vector<std::pair<int, int>> spans;
    QObject::connect(&model, &SignalListModel::dataChanged, &model,
                     [&](const QModelIndex& from, const QModelIndex& to,
                         const QList<int>& roles) {
                         if (roles.contains(SignalListModel::HiddenWhyRole)) {
                             spans.emplace_back(from.row(), to.row());
                         }
                     });
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x1; /* 0x01-0x20 */
    std::memset(car.pid_bitmap, 0xFF, 4);
    car.pid_bitmap[(0x05 - 1) / 8] &= static_cast<uint8_t>(~(0x80u >> ((0x05 - 1) % 8)));
    model.applyUnsupported(unsupportedIds(&car, 1));
    EXPECT_EQ(why(coolant), QStringLiteral("unsupported"))
        << "the bitmaps' reason comes first";
    EXPECT_EQ(why(b3), QStringLiteral("absent")) << "the bitmaps say nothing of 0x55";
    EXPECT_EQ(why(rpm), QString()) << "a PID the bitmaps list is shown";
    ASSERT_EQ(spans.size(), 1u) << "one judgment, one notification";
    EXPECT_EQ(spans[0], std::make_pair(coolant, coolant))
        << "the range is the one row whose reason changed";

    model.applyAbsent({});
    EXPECT_EQ(why(b3), QString());
    EXPECT_EQ(why(coolant), QStringLiteral("unsupported"));
}

/* The bridge reads core's absent signals on its tick: a replay of the
 * test car's one-byte PID 55 replies hides bank 3's row, and only it */
TEST(BridgeTest, LiveHidesAFieldTheRepliesComeWithout)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray text = "host_us,timestamp_us,id,flags,data\n";
    for (int i = 0; i < 5; ++i) {
        const QByteArray t = QByteArray::number(10000 * i);
        text += t + ',' + t + ",0x7E8,0x0,41 55 80\n";
    }
    QFile f(dir.filePath(QStringLiteral("frames.csv")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(text), text.size());
    f.close();

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("replay://%1?profile=obd2&speed=max")
                                .arg(dir.path()))
            .isEmpty())
        << log.join('\n').toStdString();
    SignalListModel* model = bridge.signalModel();
    const int b1 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 0));
    const int b3 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ASSERT_TRUE(
        spinUntil([model]() { return model->hiddenCount() > 0; }, 5000));
    spinLoop(200);
    EXPECT_EQ(model->hiddenCount(), 1);
    EXPECT_TRUE(
        model->data(model->index(b3), SignalListModel::HiddenRole).toBool());
    EXPECT_FALSE(
        model->data(model->index(b1), SignalListModel::HiddenRole).toBool());
    EXPECT_TRUE(
        model->data(model->index(b1), SignalListModel::HasValueRole).toBool());
    ASSERT_TRUE(disconnectAndWait(bridge));
}
