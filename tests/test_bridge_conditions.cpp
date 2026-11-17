// File/layer: tests/test_bridge_conditions.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest: the conditions (ADR-0063), their rows and the alert tone (test_bridge)
//
// Cases for the saved conditions, their model, the bridge's attach and tick, and the tone.

#include <cmath>
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <vector>

#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include "new_counter.h" // operator new counted per thread: the conditions callback
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's seams: a failed write, the attach's handover
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

// ---------------------------------------------------------------------------
// The conditions (ADR-0063 section 9): the app's saved set
// (app/conditionstore.h), its rows (app/conditionmodel.h) and the bridge's
// attach, callback and tick. The mock's first 15 s are its KeyOn (0 rpm),
// so a condition true there fires at the first frame and stays ON.
// ---------------------------------------------------------------------------
namespace {

// One condition, so a set told from mockConditions by its count; of two
// terms, on two units
QByteArray otherConditions()
{
    return QByteArray(R"({"havne_conditions":1,"conditions":[)"
                      R"({"key":"second","name":"Second set","when":{"all":[)"
                      R"({"signal":["id:1001"],"unit":"RPM","below":300},)"
                      R"({"signal":["id:3001"],"unit":"km/h","below":250}]},)"
                      R"("alert":{"level":"warning"}}]})");
}

// The model's rows, each a map by role name
QVariantList conditionRows(const ConditionModel* model)
{
    QVariantList rows;
    const QHash<int, QByteArray> roles = model->roleNames();
    for (int r = 0; r < model->rowCount(); ++r) {
        QVariantMap row;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            row.insert(QString::fromLatin1(it.value()),
                       model->data(model->index(r), it.key()));
        }
        rows << row;
    }
    return rows;
}

QVariantMap conditionRow(const ConditionModel* model, int r)
{
    return conditionRows(model).value(r).toMap();
}

// Whether row r reads live and in that state, within 5 s
bool rowReads(const SessionBridge& bridge, int r, const char* state)
{
    return spinUntil(
        [&]() {
            const QVariantMap row = conditionRow(bridge.conditionModel(), r);
            return row.value(QStringLiteral("live")).toBool() &&
                   row.value(QStringLiteral("state")).toString() ==
                       QLatin1String(state);
        },
        5000);
}

// An event as core's worker gives it
ecuio_condition_event_t conditionEvent(uint32_t condition, uint32_t generation,
                                       uint32_t flags, double value,
                                       uint64_t at_us = 1000000,
                                       uint32_t term = 0)
{
    ecuio_condition_event_t e{};
    e.timestamp_us = at_us;
    e.term = term;
    e.condition = condition;
    e.state = (flags & ECUIO_CONDITION_FIRED) != 0 ? ECUIO_CONDITION_ON
                                                   : ECUIO_CONDITION_OFF;
    e.flags = flags;
    e.value = value;
    e.generation = generation;
    return e;
}

// The lines of log starting with prefix
QStringList linesStarting(const QStringList& log, const QString& prefix)
{
    QStringList lines;
    for (const QString& line : log) {
        if (line.startsWith(prefix))
            lines << line;
    }
    return lines;
}

} // namespace

// A file is read by core before it's kept: one core refuses, or one too
// long, leaves the kept file as it was; the save replaces it whole, with
// nothing left beside it
TEST(BridgeTest, TheStoreRefusesAFileCoreRefuses)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    ConditionStore store;
    QString words;
    EXPECT_TRUE(store.load(&words)) << "no file is no set";
    EXPECT_EQ(store.set(), nullptr);

    QVariantMap saved = store.save(mockConditions());
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(saved.value(QStringLiteral("conditions")).toInt(), 3);
    EXPECT_EQ(saved.value(QStringLiteral("name")).toString(),
              QStringLiteral("Bridge test"));
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_EQ(QDir(QFileInfo(ConditionStore::path()).absolutePath())
                  .entryList(QDir::Files),
              QStringList{QStringLiteral("conditions.json")})
        << "the save leaves only conditions.json";

    const QByteArray refused =
        R"({"havne_conditions":1,"conditions":[{"key":"a","name":"A",)"
        R"("when":{"signal":["id:1001"],"unit":"RPM","abov":3}}]})";
    saved = store.save(refused);
    EXPECT_FALSE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(saved.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("abov")))
        << "core's words: "
        << saved.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_EQ(ecuio_condition_set_count(store.set()), 3u)
        << "the kept set stays";

    QByteArray tooLong = mockConditions();
    tooLong.append(QByteArray(ECUIO_CONDITION_FILE_MAX_BYTES, ' '));
    saved = store.save(tooLong);
    EXPECT_FALSE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("words")).toString(),
              QStringLiteral("a condition file is at most 256 KiB"));
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());

    ConditionStore again;
    EXPECT_TRUE(again.load(&words)) << words.toStdString();
    EXPECT_EQ(again.bytes(), mockConditions());
    EXPECT_EQ(ecuio_condition_set_count(again.set()), 3u);

    // A file core refuses, put there by hand: read as no set, with words
    {
        QFile f(ConditionStore::path());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(refused);
    }
    ConditionStore damaged;
    EXPECT_FALSE(damaged.load(&words));
    EXPECT_TRUE(words.contains(QStringLiteral("abov"))) << words.toStdString();
    EXPECT_EQ(damaged.set(), nullptr);
    EXPECT_TRUE(damaged.bytes().isEmpty());
    SessionBridge bridge;
    EXPECT_TRUE(bridge.conditionsProblem().startsWith(
        QStringLiteral("The saved conditions weren't read: ")))
        << bridge.conditionsProblem().toStdString();
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 0);
}

// The Alerts page's Import: a picked file goes through saveConditions, so
// one core refuses, or one that can't be read, leaves the saved file as it
// was; a leading BOM is skipped, and no refusal names the file's path
TEST(BridgeTest, AnImportedConditionFileReplacesTheSavedOneOrLeavesIt)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir picked;
    ASSERT_TRUE(picked.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QVariantMap kept = bridge.importConditions(writeScratch(
        picked.path(), "other.json", QByteArray("\xEF\xBB\xBF") + otherConditions()));
    ASSERT_TRUE(kept.value(QStringLiteral("ok")).toBool())
        << kept.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(kept.value(QStringLiteral("conditions")).toInt(), 1);
    EXPECT_EQ(kept.value(QStringLiteral("pending")).toString(), QString());
    EXPECT_EQ(readFile(ConditionStore::path()), otherConditions())
        << "the file replaces the saved one, its BOM left out";
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(otherConditions()));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);

    const auto refused = [&](const QUrl& url, const QString& words) {
        log.clear();
        const QVariantMap r = bridge.importConditions(url);
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool()) << words.toStdString();
        const QString said = r.value(QStringLiteral("words")).toString();
        EXPECT_TRUE(said.contains(words)) << said.toStdString();
        EXPECT_FALSE(said.contains(picked.path())) << said.toStdString();
        EXPECT_EQ(readFile(ConditionStore::path()), otherConditions())
            << "the saved file stays: " << words.toStdString();
        EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
        ASSERT_EQ(log.size(), 1) << words.toStdString();
        EXPECT_EQ(log.first(), QStringLiteral("Alerts not imported: ") + said);
    };
    refused(writeScratch(picked.path(), "refused.json",
                         R"({"havne_conditions":1,"conditions":[{"key":"a","name":"A",)"
                         R"("when":{"signal":["id:1001"],"unit":"RPM","abov":3}}]})"),
            QStringLiteral("abov"));
    refused(QUrl::fromLocalFile(picked.path() + QStringLiteral("/none.json")),
            QStringLiteral("Couldn't read the file"));
    refused(writeScratch(picked.path(), "latin1.json", QByteArray("{\xE9}")),
            QStringLiteral("The file isn't UTF-8 text"));
    QByteArray tooLong = otherConditions();
    tooLong.append(QByteArray(ECUIO_CONDITION_FILE_MAX_BYTES, ' '));
    refused(writeScratch(picked.path(), "long.json", tooLong),
            QStringLiteral("The file is over 256 KiB, a condition file's most"));
}

// Export writes the saved file's bytes where the user chose, and says why
// when there are none or the target takes no file; no line names a path
TEST(BridgeTest, AnExportedConditionFileIsTheSavedOne)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QUrl target = QUrl::fromLocalFile(out.path() + QStringLiteral("/c.json"));
    QVariantMap r = bridge.exportConditions(target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              QStringLiteral("There are no saved alerts to export"));
    EXPECT_FALSE(QFile::exists(target.toLocalFile()));

    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    log.clear();
    r = bridge.exportConditions(target);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(readFile(target.toLocalFile()), mockConditions());
    ASSERT_EQ(log.size(), 1);
    EXPECT_EQ(log.first(), QStringLiteral("Alerts exported to a file (%1 bytes)")
                               .arg(mockConditions().size()));

    ASSERT_TRUE(QDir(out.path()).mkdir(QStringLiteral("folder")));
    log.clear();
    r = bridge.exportConditions(QUrl::fromLocalFile(out.path() + QStringLiteral("/folder")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              QStringLiteral("Couldn't write the file"));
    EXPECT_TRUE(QFileInfo(out.path() + QStringLiteral("/folder")).isDir());
    ASSERT_EQ(log.size(), 1);
    EXPECT_FALSE(log.first().contains(out.path())) << log.first().toStdString();
}

// A saved file core refused at the start is exported as it is, so it can
// be mended elsewhere
TEST(BridgeTest, AnExportOfAFileCoreRefusedIsTheFileAsItIs)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QByteArray typo =
        R"({"havne_conditions":1,"conditions":[{"key":"x"}]})";
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).absolutePath()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(typo);
    }
    SessionBridge bridge;
    ASSERT_FALSE(bridge.conditionsProblem().isEmpty());
    const QUrl target = QUrl::fromLocalFile(out.path() + QStringLiteral("/c.json"));
    const QVariantMap r = bridge.exportConditions(target);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(readFile(target.toLocalFile()), typo);

    // Read again as at the start: a file core takes clears the problem
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(mockConditions());
    }
    bridge.reloadSavedConditionsForTesting();
    EXPECT_EQ(bridge.conditionsProblem(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
}

// A saved file past the bound is read as no set, but it is the user's
// copy: Export says it wasn't read rather than that nothing is saved, and
// the first save keeps it beside the new file as conditions.json.bad-1
TEST(BridgeTest,
     AnOverBoundSavedFileIsKeptBesideTheFirstSaveAndExportSaysItWasntRead)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    QByteArray big = mockConditions();
    big.append(QByteArray(ECUIO_CONDITION_FILE_MAX_BYTES, ' '));
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).absolutePath()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(big);
    }
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_FALSE(bridge.conditionsProblem().isEmpty());
    const QUrl target =
        QUrl::fromLocalFile(out.path() + QStringLiteral("/c.json"));
    const QVariantMap r = bridge.exportConditions(target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              bridge.conditionsProblem())
        << "not \"There are no saved alerts to export\"";

    const QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(mockConditions()));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_EQ(readFile(ConditionStore::path() + QStringLiteral(".bad-1")), big)
        << "the old file is kept, byte for byte";
    EXPECT_EQ(saved.value(QStringLiteral("aside")).toString(),
              QStringLiteral("conditions.json.bad-1"));
    EXPECT_TRUE(log.contains(
        QStringLiteral("The conditions file that didn't load is kept as "
                       "conditions.json.bad-1")))
        << log.join(u'\n').toStdString();

    // A second save has nothing unreadable to keep
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_FALSE(
        QFile::exists(ConditionStore::path() + QStringLiteral(".bad-2")));
}

// The file that didn't load may be gone by the time the user saves (moved
// or deleted by hand): the save has nothing to keep aside and goes through,
// where a refusal would block every save until a restart
TEST(BridgeTest, AnUnreadableSavedFileRemovedByHandDoesNotBlockTheNextSave)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QByteArray big = mockConditions();
    big.append(QByteArray(ECUIO_CONDITION_FILE_MAX_BYTES, ' '));
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).absolutePath()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(big);
    }
    SessionBridge bridge;
    ASSERT_FALSE(bridge.conditionsProblem().isEmpty());
    ASSERT_TRUE(QFile::remove(ConditionStore::path()));

    const QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(mockConditions()));
    EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(saved.value(QStringLiteral("aside")).toString(), QString());
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_FALSE(
        QFile::exists(ConditionStore::path() + QStringLiteral(".bad-1")));
}

// A level typed in the display unit back in the def's: the inverse of
// displayValue in either unit system, and a unit no system converts as typed
TEST(BridgeTest, ADecodedValueIsTheDisplayValuesInverse)
{
    SessionBridge bridge;
    const QString degC = QString::fromUtf8("\xC2\xB0" "C");
    bridge.setUnitSystem(0);
    EXPECT_EQ(bridge.decodedValue(degC, 90.0), 90.0);
    EXPECT_EQ(bridge.decodedValue(QStringLiteral("kPa"), 101.5), 101.5);
    bridge.setUnitSystem(1);
    EXPECT_NEAR(bridge.decodedValue(degC, 212.0), 100.0, 1e-9);
    EXPECT_NEAR(bridge.decodedValue(degC, -40.0), -40.0, 1e-9);
    EXPECT_NEAR(bridge.decodedValue(QStringLiteral("kPa"), 14.5037737730209), 100.0, 1e-6);
    EXPECT_NEAR(bridge.decodedValue(QStringLiteral("km/h"), 62.1371192237334), 100.0, 1e-6);
    for (const double v : {-40.0, 0.0, 0.5, 98.25, 1200.0}) {
        for (const char* unit : {"\xC2\xB0" "C", "kPa", "bar", "km/h", "g/s", "L/h"}) {
            const QString u = QString::fromUtf8(unit);
            EXPECT_NEAR(bridge.decodedValue(u, bridge.displayValue(u, v)), v, 1e-9 * (1 + std::fabs(v)))
                << unit << " at " << v;
        }
    }
    EXPECT_EQ(bridge.decodedValue(QStringLiteral("RPM"), 3000.0), 3000.0);
    EXPECT_EQ(bridge.decodedValue(QString(), 7.0), 7.0);
}

// The rows come from the file, offline too; connected, the tick gives
// each its state, its value in the display unit and the defs its terms
// bind, and an alert ON names its level for its signal
TEST(BridgeTest, AConditionsEventsReachTheModelOnTheTick)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(mockConditions()));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(), QString());
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(mockConditions()));
    const ConditionModel* model = bridge.conditionModel();
    ASSERT_EQ(model->rowCount(), 3);
    QVariantMap row = conditionRow(model, 0);
    EXPECT_EQ(row.value(QStringLiteral("key")).toString(),
              QStringLiteral("stopped"));
    EXPECT_FALSE(row.value(QStringLiteral("live")).toBool())
        << "no session judges it";
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(), QVariantList{-1})
        << "bound to no session's signals";

    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://conditions")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(rowReads(bridge, 1, "on"));
    row = conditionRow(model, 0);
    EXPECT_EQ(row.value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine stopped"));
    EXPECT_EQ(row.value(QStringLiteral("level")).toString(),
              QStringLiteral("warning"));
    EXPECT_EQ(row.value(QStringLiteral("uses")).toStringList(),
              QStringList{QStringLiteral("alert")});
    EXPECT_TRUE(row.value(QStringLiteral("banner")).toBool());
    EXPECT_FALSE(row.value(QStringLiteral("sound")).toBool());
    EXPECT_TRUE(row.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(row.value(QStringLiteral("stale")).toBool());
    EXPECT_FALSE(row.value(QStringLiteral("unavailable")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("why")).toString(), QString());
    EXPECT_EQ(row.value(QStringLiteral("fired")).toLongLong(), 1);
    EXPECT_EQ(row.value(QStringLiteral("value")).toDouble(), 0.0);
    EXPECT_EQ(row.value(QStringLiteral("unit")).toString(),
              QStringLiteral("RPM"));
    EXPECT_GE(row.value(QStringLiteral("sinceMs")).toLongLong(), 0);
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(),
              QVariantList{ECUIO_MOCK_SIG_RPM_ID});

    row = conditionRow(model, 1);
    EXPECT_EQ(row.value(QStringLiteral("level")).toString(),
              QStringLiteral("danger"));
    EXPECT_TRUE(row.value(QStringLiteral("sound")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("unit")).toString(),
              QStringLiteral("°C"));
    const double celsius = row.value(QStringLiteral("value")).toDouble();
    EXPECT_TRUE(std::isfinite(celsius));
    bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    row = conditionRow(model, 1);
    EXPECT_EQ(row.value(QStringLiteral("unit")).toString(),
              QStringLiteral("°F"));
    EXPECT_GT(row.value(QStringLiteral("value")).toDouble(), celsius)
        << "a warm engine's deg F is above its deg C";
    bridge.setUnitSystem(ECUIO_UNITS_METRIC);

    row = conditionRow(model, 2);
    EXPECT_EQ(row.value(QStringLiteral("state")).toString(),
              QStringLiteral("off"));
    EXPECT_EQ(row.value(QStringLiteral("level")).toString(), QString());
    EXPECT_EQ(row.value(QStringLiteral("uses")).toStringList(),
              QStringList{QStringLiteral("mark")});

    const QVariantMap levels{{QString::number(ECUIO_MOCK_SIG_RPM_ID), 1},
                             {QString::number(ECUIO_MOCK_SIG_COOLANT_ID), 2}};
    EXPECT_EQ(bridge.alertLevels(), levels)
        << "a mark-only condition names no level";
    EXPECT_EQ(bridge.alertLevelOf(ECUIO_MOCK_SIG_COOLANT_ID), 2);
    EXPECT_EQ(bridge.alertLevelOf(ECUIO_MOCK_SIG_SPEED_ID), 0);

    const int connected = static_cast<int>(
        log.indexOf(QStringLiteral("Connected: mock://conditions")));
    const int judging = static_cast<int>(
        log.indexOf(QStringLiteral("Alerts: judging 3 conditions")));
    EXPECT_GT(judging, connected) << "after the session's lines";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM")))
        << log.join(u'\n').toStdString();

    ASSERT_TRUE(disconnectAndWait(bridge));
    row = conditionRow(model, 0);
    EXPECT_FALSE(row.value(QStringLiteral("live")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("state")).toString(),
              QStringLiteral("off"));
    EXPECT_TRUE(bridge.alertLevels().isEmpty());
    EXPECT_EQ(model->rowCount(), 3) << "the saved set's rows offline";
}

// The tick applies a row's value term, which tells the model's handlers:
// one that saves the conditions attaches again, reassigning the slots the
// tick reads. The tick finishes with the slots it had, so the firing of
// the set before still goes out under that set's name
TEST(BridgeTest, AHandlerThatSavesConditionsMidTickLeavesTheTicksFiringsIntact)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    int handled = 0;
    QObject::connect(
        bridge.conditionModel(), &QAbstractItemModel::dataChanged,
        [&bridge, &handled](const QModelIndex&, const QModelIndex&,
                            const QList<int>& roles) {
            if (!roles.contains(ConditionModel::UnitRole) || handled++ > 0) {
                return;
            }
            bridge.saveConditions(QString::fromUtf8(mockConditions()));
        });
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://conditions")).isEmpty());
    ASSERT_TRUE(spinUntil(
        [&log]() {
            for (const QString& line : log) {
                if (line.startsWith(QStringLiteral("Alert: Second set"))) {
                    return true;
                }
            }
            return false;
        },
        5000, 10))
        << log.join(u'\n').toStdString();
    EXPECT_GE(handled, 1)
        << "a value term changed, so the handler ran in the tick";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A handler that attaches again mid-tick leaves the rest of the tick's
// (row, term) pairs unapplied: they were the set before's rows, and the
// set attached has rows of its own at those numbers
TEST(BridgeTest, AHandlerThatSavesConditionsMidTickLeavesTheNewSetsRowsAlone)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray cond =
        R"({"key":"KEY","name":"NAME","when":{"all":[)"
        R"({"signal":["id:1001"],"unit":"RPM","below":300},)"
        R"({"signal":["id:3001"],"unit":"km/h","below":250}]},)"
        R"("alert":{"level":"warning"}})";
    const auto setOf = [&cond](const char* a, const char* b) {
        QByteArray one = cond;
        QByteArray two = cond;
        one.replace("KEY", a).replace("NAME", a);
        two.replace("KEY", b).replace("NAME", b);
        return QByteArray(R"({"havne_conditions":1,"conditions":[)") + one +
               "," + two + "]}";
    };
    SessionBridge bridge;
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(setOf("aa", "bb")))
                    .value(QStringLiteral("ok"))
                    .toBool());
    std::vector<int> seen; // the rows whose unit changed, in order
    int mark = -1;
    bool saved = false;
    QObject::connect(
        bridge.conditionModel(), &QAbstractItemModel::dataChanged,
        [&](const QModelIndex& at, const QModelIndex&,
            const QList<int>& roles) {
            if (!roles.contains(ConditionModel::UnitRole)) {
                return;
            }
            seen.push_back(at.row());
            if (!saved) {
                saved = true;
                bridge.saveConditions(QString::fromUtf8(setOf("cc", "dd")));
                // Queued: it runs once the tick that is calling us has returned
                QTimer::singleShot(
                    0, [&]() { mark = static_cast<int>(seen.size()); });
            }
        });
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://conditions")).isEmpty());
    ASSERT_TRUE(spinUntil([&mark]() { return mark >= 0; }, 5000, 10));
    EXPECT_EQ(mark, 1) << "the tick's other pair was left alone: "
                       << seen.size();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A re-attach begins a new generation: a firing of the set before is a
// line under that set's name and moves no row, one of the set attached is
// the tick's, and one of a set older than that is passed by
TEST(BridgeTest, AnEventOfTheSetBeforeIsALineOfItsOwnSetAndMovesNoRow)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://generations")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);

    // A firing of the set before, still in the window: written, not lost
    const ecuio_condition_event_t staged =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 777);
    bridge.dispatchConditionsForTesting(&staged, 1);
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 2u)
        << "no recorder: at once";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 777 RPM")))
        << "the window's firing before the re-attach: "
        << log.join(u'\n').toStdString();
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts saved: 1 condition")));
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));

    const ecuio_condition_event_t old =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 4321);
    bridge.dispatchConditionsForTesting(&old, 1);
    const ecuio_condition_event_t now =
        conditionEvent(0, 2, ECUIO_CONDITION_FIRED, 1234, 1000000, 1);
    bridge.dispatchConditionsForTesting(&now, 1);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Second set, 1234 km/h"));
        },
        5000))
        << "in the unit of the term that settled it: "
        << log.join(u'\n').toStdString();
    EXPECT_EQ(conditionRow(bridge.conditionModel(), 0)
                  .value(QStringLiteral("unit"))
                  .toString(),
              QStringLiteral("km/h"));
    EXPECT_TRUE(
        linesStarting(log, QStringLiteral("Alert: Second set, 4321")).isEmpty())
        << "the set before's event";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 4321 RPM")))
        << "written as its own set's condition: "
        << log.join(u'\n').toStdString();
    EXPECT_LT(log.indexOf(QStringLiteral("Alert: Engine stopped, 4321 RPM")),
              log.indexOf(QStringLiteral("Alert: Second set, 1234 km/h")))
        << "at one time, the set before's first";
    EXPECT_TRUE(
        linesStarting(log, QStringLiteral("Alert: Second set fired")).isEmpty())
        << "the two weren't counted together";

    // An index the set attached doesn't hold
    const ecuio_condition_event_t past =
        conditionEvent(5, 2, ECUIO_CONDITION_FIRED, 99);
    bridge.dispatchConditionsForTesting(&past, 1);
    // And one the set before doesn't hold
    const ecuio_condition_event_t pastBefore =
        conditionEvent(5, 1, ECUIO_CONDITION_FIRED, 81234);
    bridge.dispatchConditionsForTesting(&pastBefore, 1);
    spinLoop(300);
    EXPECT_TRUE(
        linesStarting(log, QStringLiteral("Alert: Second set, 99 ")).isEmpty());
    EXPECT_TRUE(log.filter(QStringLiteral(", 81234")).isEmpty())
        << log.join(u'\n').toStdString();
    EXPECT_EQ(log.filter(QStringLiteral(", 4321 ")).size(), 1)
        << "written once, whatever the ticks since";

    // A third set: the first is no longer the set before
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 3u);
    const ecuio_condition_event_t oldest =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 555);
    bridge.dispatchConditionsForTesting(&oldest, 1);
    const ecuio_condition_event_t second =
        conditionEvent(0, 2, ECUIO_CONDITION_FIRED, 666, 2000000, 1);
    bridge.dispatchConditionsForTesting(&second, 1);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Second set, 666 km/h"));
        },
        5000))
        << "the set before, by its own rows: " << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.filter(QStringLiteral(", 555")).isEmpty())
        << "two sets back: " << log.join(u'\n').toStdString();

    // One the disconnect finds in the window, with no tick since
    const ecuio_condition_event_t last =
        conditionEvent(0, 2, ECUIO_CONDITION_FIRED, 888, 3000000, 1);
    bridge.dispatchConditionsForTesting(&last, 1);
    bridge.disconnectSession();
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Second set, 888 km/h")))
        << "written by the disconnect's flush: "
        << log.join(u'\n').toStdString();
    ASSERT_TRUE(
        spinUntil([&bridge]() { return !bridge.disconnecting(); }, 5000, 10));
}

// A handler that saves from a line a save's flush writes, before that
// save's attach: the save it interrupts attaches nothing more, since the
// handler's set, the store's, is the one judged
TEST(BridgeTest, AHandlerThatSavesFromAnAttachsFlushedLineAttachesOnce)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(
        &bridge, &SessionBridge::logMessage, [&](const QString& line) {
            log << line;
            if (line == QStringLiteral("Alert: Engine stopped, 777 RPM")) {
                bridge.saveConditions(QString::fromUtf8(mockConditions()));
            }
        });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://nested")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 1u);

    const ecuio_condition_event_t staged =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 777);
    bridge.dispatchConditionsForTesting(&staged, 1);
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 2u)
        << "the handler's attach alone";
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3)
        << "the handler's set, saved last";
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(mockConditions()));
    EXPECT_EQ(linesStarting(log, QStringLiteral("Alerts: judging 1 condition"))
                  .size(),
              0)
        << log.join(u'\n').toStdString();
    EXPECT_EQ(linesStarting(log, QStringLiteral("Alerts: judging 3 conditions"))
                  .size(),
              2)
        << "the connect's and the handler's: " << log.join(u'\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A save writes the window's firings before its attach, and a handler of
// those lines runs between the two: a firing the worker gives then, after
// the flush and before core has the new set, is still a line
TEST(BridgeTest, AFiringAfterAnAttachsFlushIsStillALine)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    const ecuio_condition_event_t late[] = {
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 260, 3000000),
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 12, 2000000),
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 13, 2500000),
    };
    QObject::connect(
        &bridge, &SessionBridge::logMessage, [&](const QString& line) {
            log << line;
            if (line == QStringLiteral("Alert: Engine stopped, 777 RPM")) {
                bridge.dispatchConditionsForTesting(late, 3);
            }
        });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://flushed")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 1u);

    const ecuio_condition_event_t staged =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 777);
    bridge.dispatchConditionsForTesting(&staged, 1);
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 2u);
    ASSERT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 777 RPM")))
        << "the save's flush, whose handler gave the late firings";
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Condition: Moving, 260 km/h"));
        },
        5000))
        << log.join(u'\n').toStdString();
    const qsizetype twice =
        log.indexOf(QStringLiteral("Alert: Engine stopped fired 2 times"));
    EXPECT_GE(twice, 0) << "counted as the set before counts: "
                        << log.join(u'\n').toStdString();
    EXPECT_LT(twice, log.indexOf(QStringLiteral("Condition: Moving, 260 km/h")))
        << "in the order they fired";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

// The session's worker parked in a frame of the mock's telemetry: it has
// taken the set it judges the frame by and has not judged it yet
// (tests/worker_hold.h's hold, on one ID and from one frame to the next).
// Declared before the bridge, with a Release after it
struct TelemetryHold {
    std::mutex m;
    std::condition_variable cv;
    bool armed = false;
    int parks = 0;  // the frames parked so far
    int passed = 0; // the parked frames let go

    static void onFrame(const ecuio_frame_t* frame, void* user)
    {
        auto* h = static_cast<TelemetryHold*>(user);
        if (frame->arbitration_id != ECUIO_MOCK_TELEMETRY_ARB_ID) {
            return;
        }
        std::unique_lock<std::mutex> lock(h->m);
        if (!h->armed) {
            return;
        }
        const int mine = ++h->parks;
        h->cv.notify_all();
        h->cv.wait(lock, [h, mine] { return !h->armed || h->passed >= mine; });
    }
    // True once the worker is parked in a telemetry frame, within ms: its
    // next, or with one parked already the one after it
    bool hold(int ms)
    {
        std::unique_lock<std::mutex> lock(m);
        armed = true;
        passed = parks;
        const int want = parks + 1;
        cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::milliseconds(ms),
                         [this, want] { return parks >= want; })) {
            armed = false;
            cv.notify_all();
            return false;
        }
        return true;
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(m);
        armed = false;
        cv.notify_all();
    }
    struct Release {
        TelemetryHold& h;
        ~Release() { h.release(); }
    };
};

// One alert, true through the mock's KeyOn: it fires at the first
// telemetry frame judged by it
QByteArray stoppedConditions()
{
    return QByteArray(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"stopped","name":"Engine stopped",)"
        R"("when":{"signal":["id:1001"],"unit":"RPM","below":300},)"
        R"("alert":{"level":"danger","sound":true}}]})");
}

// A session whose worker is parked in the frame that fires
// stoppedConditions (generation 1), which it has taken and not judged
struct HeldFiring {
    SettingsDirScope dir;
    TelemetryHold hold;
    QStringList log;
    SessionBridge bridge;
    TelemetryHold::Release unhold{hold};

    bool start()
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
        if (!dir.valid() ||
            !bridge.connectSession(QStringLiteral("mock://held")).isEmpty() ||
            ecuio_register_frame_callback(bridge.sessionForTesting(),
                                          &TelemetryHold::onFrame,
                                          &hold) != ECUIO_OK ||
            // Parked with no set: the attach is whole before the next frame
            !hold.hold(5000) ||
            !bridge.saveConditions(QString::fromUtf8(stoppedConditions()))
                 .value(QStringLiteral("ok"))
                 .toBool() ||
            bridge.conditionsGenerationForTesting() != 1u) {
            return false;
        }
        // The next telemetry frame: the first the set judges, taken with it
        return hold.hold(5000);
    }
    bool fired() const
    {
        return log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM"));
    }
    uint64_t danger() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_DANGER);
    }
};

// ecuio_session_test_after_conditions_handover for a scope
struct AfterHandover {
    AfterHandover(void (*hook)(void*), void* user)
    {
        ecuio_session_test_after_conditions_handover(hook, user);
    }
    ~AfterHandover()
    {
        ecuio_session_test_after_conditions_handover(nullptr, nullptr);
    }
};

} // namespace

// The worker's dispatch under way at a re-attach took the set before: it
// judges its frame by that set and gives the firing once core's call has
// returned. The firing is that set's line and tone
TEST(BridgeTest, AFiringTheWorkerGivesAfterAReattachIsStillALineAndATone)
{
    HeldFiring run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), 2u);
    spinLoop(300);
    ASSERT_FALSE(run.fired()) << "the worker is held: nothing judged yet";
    EXPECT_EQ(run.danger(), 0u);

    run.hold.release();
    EXPECT_TRUE(spinUntil([&run]() { return run.fired(); }, 5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "the set before's alert sounds";
    EXPECT_TRUE(spinUntil(
        [&run]() {
            return !linesStarting(run.log, QStringLiteral("Alert: Second set"))
                        .isEmpty();
        },
        5000))
        << "the set attached is judged from the next frame: "
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(
        linesStarting(run.log, QStringLiteral("Alert: Engine stopped")).size(),
        1);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// The same firing given while the attach is inside core's call, after the
// window's flush and before the call returns
TEST(BridgeTest, AFiringTheWorkerGivesDuringCoresAttachIsStillALine)
{
    // Before the session: the worker calls with it until the close
    struct Given {
        std::mutex m;
        std::condition_variable cv;
        bool given = false; // the bridge has the set before's firing
        bool inTime = false;
        TelemetryHold* hold = nullptr;
    } given;
    HeldFiring run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    given.hold = &run.hold;
    // Registered after the bridge's, so called once the bridge has the event
    const auto onConditions = [](const ecuio_condition_event_t* events,
                                 uint32_t count, void* user) {
        auto* g = static_cast<Given*>(user);
        for (uint32_t i = 0; i < count; ++i) {
            if (events[i].generation == 1 &&
                (events[i].flags & ECUIO_CONDITION_FIRED) != 0) {
                std::lock_guard<std::mutex> lock(g->m);
                g->given = true;
                g->cv.notify_all();
            }
        }
    };
    ASSERT_EQ(ecuio_register_conditions_callback(run.bridge.sessionForTesting(),
                                                 +onConditions, &given),
              ECUIO_OK);
    {
        // Inside core's call: the worker goes on and gives the firing
        AfterHandover inside(
            [](void* user) {
                auto* g = static_cast<Given*>(user);
                g->hold->release();
                std::unique_lock<std::mutex> lock(g->m);
                g->inTime = g->cv.wait_for(lock, std::chrono::seconds(5),
                                           [g] { return g->given; });
            },
            &given);
        ASSERT_TRUE(
            run.bridge.saveConditions(QString::fromUtf8(otherConditions()))
                .value(QStringLiteral("ok"))
                .toBool());
    }
    ASSERT_TRUE(given.inTime) << "the firing came before core's call returned";
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), 2u);
    ASSERT_FALSE(run.fired()) << "no tick since";
    EXPECT_TRUE(spinUntil([&run]() { return run.fired(); }, 5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// Each firing is a line of the activity log, and a note in the
// recording; several of one condition's in one window are one line, and
// a change that isn't a firing is none
TEST(BridgeTest, EachFiringIsAnActivityLine)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://firings")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(rowReads(bridge, 1, "on"));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM"));
        },
        5000));
    EXPECT_EQ(
        linesStarting(log, QStringLiteral("Alert: Engine stopped")).size(), 1)
        << "one firing, one line";
    EXPECT_EQ(
        linesStarting(log, QStringLiteral("Alert: Coolant cold, ")).size(), 1);

    const uint32_t g = bridge.conditionsGenerationForTesting();
    std::vector<ecuio_condition_event_t> window;
    for (int i = 0; i < 1000; ++i) {
        window.push_back(conditionEvent(0, g, ECUIO_CONDITION_FIRED, i));
        window.push_back(conditionEvent(0, g, 0, i)); // back OFF
    }
    window.push_back(conditionEvent(2, g, ECUIO_CONDITION_FIRED, 260));
    window.push_back(
        conditionEvent(1, g, ECUIO_CONDITION_STALE, 20)); // no firing
    bridge.dispatchConditionsForTesting(window.data(),
                                        static_cast<uint32_t>(window.size()));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(
                QStringLiteral("Alert: Engine stopped fired 1000 times"));
        },
        5000))
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Condition: Moving, 260 km/h")))
        << "a condition with no alert";
    EXPECT_EQ(linesStarting(log, QStringLiteral("Alert: Coolant cold")).size(),
              1)
        << "a STALE change isn't a firing";

    // In the order they fired, whatever their order in the set; a
    // condition's place is its window's first firing
    const ecuio_condition_event_t first[] = {
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 1, 2000000),
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 2, 3000000),
    };
    bridge.dispatchConditionsForTesting(first, 2);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Engine stopped, 2 RPM"));
        },
        5000));
    EXPECT_LT(log.indexOf(QStringLiteral("Condition: Moving, 1 km/h")),
              log.indexOf(QStringLiteral("Alert: Engine stopped, 2 RPM")));
    const ecuio_condition_event_t second[] = {
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 11, 4000000),
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 12, 5000000),
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 13, 6000000),
    };
    bridge.dispatchConditionsForTesting(second, 3);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Condition: Moving, 12 km/h"));
        },
        5000));
    EXPECT_LT(
        log.indexOf(QStringLiteral("Alert: Engine stopped fired 2 times")),
        log.indexOf(QStringLiteral("Condition: Moving, 12 km/h")));

    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QLatin1Char('/') + folders.first();
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains("Alert: Engine stopped, 0 RPM"))
        << "noted in the recording";
    EXPECT_TRUE(events.contains("Alert: Engine stopped fired 1000 times"));
    EXPECT_EQ(readFile(folder + QStringLiteral("/conditions.json")),
              mockConditions())
        << "the set the session judged";
}

// A recording holds its set (ADR-0063 section 6): an edit saved while one
// runs waits for its end, then is judged; a disconnect ends the wait too
TEST(BridgeTest, AnEditDuringARecordingWaitsForItsEnd)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://edit")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(bridge.startLog());

    QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(otherConditions()));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(),
              QStringLiteral("applies when this recording ends"));
    EXPECT_EQ(bridge.conditionsPending(),
              QStringLiteral("applies when this recording ends"));
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u)
        << "core would refuse it";
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3) << "the set judged";
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(otherConditions()))
        << "saved";
    EXPECT_TRUE(
        log.contains(QStringLiteral("Alerts saved: 1 condition; the change "
                                    "applies when this recording ends")));

    bridge.stopLog();
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 2u);
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    EXPECT_EQ(readFile(root.path() + QLatin1Char('/') + folders.first() +
                       QStringLiteral("/conditions.json")),
              mockConditions())
        << "the recording kept its set";

    // A disconnect ends a recording's wait: the saved set, offline
    ASSERT_TRUE(bridge.startLog());
    saved = bridge.saveConditions(QString::fromUtf8(mockConditions()));
    EXPECT_FALSE(saved.value(QStringLiteral("pending")).toString().isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
    EXPECT_FALSE(conditionRow(bridge.conditionModel(), 0)
                     .value(QStringLiteral("live"))
                     .toBool());
}

// A replay is judged by its recording's own conditions.json while it
// matches the header's hash (ADR-0063 decision 12); an edit then waits for
// the replay's end, and a copy changed since is passed by for the saved set
TEST(BridgeTest, AReplayJudgesByItsRecordingsCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://recorded")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 10; }, 5000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);

    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantList rows = bridge.recentRecordings(1);
    ASSERT_EQ(rows.size(), 1);
    const QString uri =
        rows.first().toMap().value(QStringLiteral("uri")).toString();
    const QString folder =
        rows.first().toMap().value(QStringLiteral("folder")).toString();

    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_TRUE(bridge.conditionsFromRecording());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Alerts: judging the recording's own 3 conditions")))
        << log.join(u'\n').toStdString();
    ASSERT_EQ(bridge.conditionModel()->rowCount(), 3);
    EXPECT_EQ(conditionRow(bridge.conditionModel(), 1)
                  .value(QStringLiteral("key"))
                  .toString(),
              QStringLiteral("cold"));
    ASSERT_TRUE(rowReads(bridge, 0, "on")) << "the replay fires what fired";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM")));

    const QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(otherConditions()));
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(),
              QStringLiteral("applies when this replay is disconnected"));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.conditionsFromRecording());
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1) << "the saved set's rows";

    // A copy changed since the recording: the saved set, and a line
    {
        QFile copy(folder + QStringLiteral("/conditions.json"));
        ASSERT_TRUE(copy.open(QIODevice::Append));
        copy.write(" ");
    }
    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_FALSE(bridge.conditionsFromRecording());
    const qsizetype unmatched = log.indexOf(QStringLiteral(
        "Replay: the recording's conditions.json doesn't match its hash; "
        "judging the saved conditions"));
    EXPECT_GE(unmatched, 0) << log.join(u'\n').toStdString();
    const QStringList connected =
        linesStarting(log, QStringLiteral("Connected: "));
    ASSERT_EQ(connected.size(), 1);
    EXPECT_GT(unmatched, log.indexOf(connected.first()))
        << "with the connect's lines";
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));

    // With no saved set, the line says none is judged
    {
        SettingsDirScope none;
        ASSERT_TRUE(none.valid());
        SessionBridge fresh;
        fresh.setRecordingsRootForTesting(root.path());
        QStringList lines;
        QObject::connect(&fresh, &SessionBridge::logMessage,
                         [&lines](const QString& line) { lines << line; });
        ASSERT_TRUE(fresh.connectSession(uri).isEmpty());
        EXPECT_TRUE(lines.contains(QStringLiteral(
            "Replay: the recording's conditions.json doesn't match its "
            "hash; judging none")))
            << lines.join(u'\n').toStdString();
        EXPECT_TRUE(linesStarting(lines, QStringLiteral("Alerts")).isEmpty());
        EXPECT_EQ(fresh.conditionModel()->rowCount(), 0);
        ASSERT_TRUE(disconnectAndWait(fresh));
    }

    // A copy that matches but isn't judged: the saved set in its place
    {
        QFile copy(folder + QStringLiteral("/conditions.json"));
        ASSERT_TRUE(copy.open(QIODevice::ReadWrite));
        copy.resize(copy.size() - 1);
    }
    bridge.failConditionsAttachesForTesting(1);
    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_FALSE(bridge.conditionsFromRecording());
    EXPECT_FALSE(
        linesStarting(log, QStringLiteral("Replay: the recording's "
                                          "conditions.json isn't judged ("))
            .isEmpty())
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// The level a signal's alerts give it is the highest ON among them,
// whatever their order; one OFF, or a condition with no alert, gives none
TEST(BridgeTest, TheHighestAlertOnASignalIsItsLevel)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"d","name":"D","when":{"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"("alert":{"level":"danger"}},)"
        R"({"key":"w","name":"W","when":{"all":[)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"({"signal":["id:4001"],"unit":"%","above":1}]},"alert":{"level":"warning"}},)"
        R"({"key":"off","name":"Off","when":{"signal":["id:3001"],"unit":"km/h","above":1},)"
        R"("alert":{"level":"danger"}},)"
        R"({"key":"m","name":"M","when":{"signal":["id:6001"],"unit":"%","above":1},)"
        R"("mark":true}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    EXPECT_TRUE(model.alertLevels().empty()) << "nothing judged yet";
    ecuio_condition_state_t states[4] = {};
    for (ecuio_condition_state_t& s : states) {
        s.state = ECUIO_CONDITION_ON;
    }
    states[2].state = ECUIO_CONDITION_OFF;
    model.applyStates(states, 4);
    const std::map<uint32_t, int> levels{
        {ECUIO_MOCK_SIG_RPM_ID, ECUIO_ALERT_DANGER},
        {ECUIO_MOCK_SIG_THROTTLE_ID, ECUIO_ALERT_WARNING}};
    EXPECT_EQ(model.alertLevels(), levels);
}

// A row gone stale takes its time from the term it went stale on (the
// event's, through applyTerm) plus the wall offset, judged once while that
// term reads nothing new, and reads -1 again once fresh or unjudged
TEST(BridgeTest, AStaleRowsTimeIsTakenOnceAndDroppedWhenFresh)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"s","name":"S","when":{"all":[)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"({"signal":["id:3001"],"unit":"km/h","above":1}]},)"
        R"("alert":{"level":"warning","banner":true}}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    const auto since = [&model]() {
        return model.data(model.index(0), ConditionModel::StaleSinceMsRole)
            .toLongLong();
    };
    int notified = 0;
    QObject::connect(
        &model, &QAbstractItemModel::dataChanged,
        [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            if (roles.contains(ConditionModel::StaleSinceMsRole)) {
                ++notified;
            }
        });
    std::map<uint32_t, qint64> readAt{{ECUIO_MOCK_SIG_RPM_ID, 200},
                                      {ECUIO_MOCK_SIG_SPEED_ID, 500}};
    int calls = 0;
    const auto read = [&](uint32_t id) {
        ++calls;
        return readAt[id];
    };
    constexpr qint64 offset = 1000000;
    ecuio_condition_state_t s{};
    s.state = ECUIO_CONDITION_ON;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.applyTerm(0, 1);
    EXPECT_EQ(notified, 0) << "the states leave the time to the judge";
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), offset + 500)
        << "the speed it went stale on, though the RPM read longer ago";
    EXPECT_EQ(notified, 1);
    calls = 0;
    model.judgeStaleSince(read, offset + 5);
    EXPECT_EQ(calls, 1) << "only the judged def is asked again";
    EXPECT_EQ(since(), offset + 500) << "taken once while it reads nothing";
    EXPECT_EQ(notified, 1) << "nothing moved, nothing notified";

    s.flags = 0;
    model.applyStates(&s, 1);
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), -1) << "fresh again";
    EXPECT_EQ(notified, 2);
    readAt[ECUIO_MOCK_SIG_SPEED_ID] = 900;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), offset + 900) << "a new stretch, its stale term's time";

    model.clearStates();
    EXPECT_EQ(since(), -1) << "unjudged";
    readAt[ECUIO_MOCK_SIG_SPEED_ID] = -1;
    model.applyStates(&s, 1);
    const int unjudged = notified;
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), -1) << "its stale term has no value: no time to name";
    EXPECT_EQ(notified, unjudged) << "-1 still: nothing notified";
    readAt[ECUIO_MOCK_SIG_SPEED_ID] = 950;
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), offset + 200)
        << "it read since: the input read longest ago";
}

// Inputs read at different rates: a fast one stale beside a slow one still
// fresh, whose last read is older. The time is the stale input's, and the
// slow one reading on moves nothing
TEST(BridgeTest, AStaleRowsFirstTimeIsTheInputThatAgedOut)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"s","name":"S","when":{"all":[)"
        R"({"signal":["id:11001"],"unit":"kPa","above":1},)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1,"max_age":0.5}]},)"
        R"("alert":{"level":"warning","banner":true}}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    const auto since = [&model]() {
        return model.data(model.index(0), ConditionModel::StaleSinceMsRole)
            .toLongLong();
    };
    // At 10000: the RPM (every 100 ms) stopped 550 ms ago, the manifold
    // pressure (once a second) read 900 ms ago
    std::map<uint32_t, qint64> readAt{{ECUIO_MOCK_SIG_RPM_ID, 9450},
                                      {ECUIO_MOCK_SIG_MAP_ID, 9100}};
    const auto read = [&](uint32_t id) { return readAt[id]; };
    ecuio_condition_state_t s{};
    s.state = ECUIO_CONDITION_OFF;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.applyTerm(0, 1);
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 9450) << "the RPM aged out, not the slower pressure";
    readAt[ECUIO_MOCK_SIG_MAP_ID] = 10100;
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 9450) << "the pressure reading on moves nothing";
}

// Core gives no event while a row stays stale: with both inputs of an all
// gone quiet, the first back leaves the row stale, and the time it names
// moves to the input still quiet
TEST(BridgeTest, AStaleRowsTimeFollowsTheInputStillQuiet)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"s","name":"S","when":{"all":[)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"({"signal":["id:3001"],"unit":"km/h","above":1}]},)"
        R"("alert":{"level":"warning","banner":true}}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    const auto since = [&model]() {
        return model.data(model.index(0), ConditionModel::StaleSinceMsRole)
            .toLongLong();
    };
    std::map<uint32_t, qint64> readAt{{ECUIO_MOCK_SIG_RPM_ID, 1000},
                                      {ECUIO_MOCK_SIG_SPEED_ID, 4000}};
    const auto read = [&](uint32_t id) { return readAt[id]; };
    ecuio_condition_state_t s{};
    s.state = ECUIO_CONDITION_OFF;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 1000) << "the RPM went quiet first";
    readAt[ECUIO_MOCK_SIG_RPM_ID] = 9000;
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 4000) << "the RPM is back; the speed is still quiet";
    readAt[ECUIO_MOCK_SIG_RPM_ID] = 9100;
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 4000) << "the RPM reading on moves nothing";
}

// Through a replay whose RPM stops for two seconds while the speed goes on:
// the condition goes stale on its RPM term and names the wall time the RPM
// row last took a value (the playback's, for a replay), then -1 once the
// RPM comes back
TEST(BridgeTest, AStaleConditionKeepsItsLastReadTime)
{
    SettingsDirScope settings;
    ASSERT_TRUE(settings.valid());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray text = "host_us,timestamp_us,id,flags,data\n";
    for (int ms = 0; ms <= 4000; ms += 50) {
        const QByteArray t = QByteArray::number(qint64{ms} * 1000);
        text += t + ',' + t + ",0x7E8,0x0,41 0D 20\n";
        if (ms < 1000 || ms >= 3000) {
            text += t + ',' + t + ",0x7E8,0x0,41 0C 0F A0\n";
        }
    }
    QFile f(dir.filePath(QStringLiteral("frames.csv")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(text), text.size());
    f.close();

    SessionBridge bridge;
    const QString conditions =
        QStringLiteral(
            R"({"havne_conditions":1,"conditions":[{"key":"rpm","name":"Running",)"
            R"("when":{"all":[{"signal":["id:%1"],"unit":"km/h","above":-1},)"
            R"({"signal":["id:%2"],"unit":"RPM","above":500,"max_age":0.5}]},)"
            R"("alert":{"level":"warning","banner":true}}]})")
            .arg(ECUIO_OBD2_SIG_SPEED_ID)
            .arg(ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_TRUE(
        bridge.saveConditions(conditions).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bridge.signalModel()->lastReadMs(ECUIO_OBD2_SIG_RPM_ID), -1)
        << "no row before a session";
    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    ASSERT_TRUE(
        bridge
            .connectSession(
                QStringLiteral("replay://%1?profile=obd2").arg(dir.path()))
            .isEmpty());
    const ConditionModel* model = bridge.conditionModel();
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_EQ(conditionRow(model, 0).value(QStringLiteral("staleSinceMs")),
              QVariant(qint64{-1}))
        << "fresh";

    ASSERT_TRUE(spinUntil(
        [model]() {
            return conditionRow(model, 0)
                .value(QStringLiteral("stale"))
                .toBool();
        },
        5000));
    const qint64 seen = QDateTime::currentMSecsSinceEpoch();
    const qint64 since = conditionRow(model, 0)
                             .value(QStringLiteral("staleSinceMs"))
                             .toLongLong();
    const qint64 rpmRead =
        bridge.signalModel()->lastReadMs(ECUIO_OBD2_SIG_RPM_ID);
    EXPECT_LE(std::abs(since - rpmRead), 20)
        << "the RPM row's last value, not the speed's";
    EXPECT_GE(since, before + 800) << "the RPM ran for a second";
    EXPECT_LE(since, seen - 250) << "its age bound passed before it was stale";

    ASSERT_TRUE(spinUntil(
        [model]() {
            return !conditionRow(model, 0)
                        .value(QStringLiteral("stale"))
                        .toBool();
        },
        5000));
    EXPECT_EQ(conditionRow(model, 0).value(QStringLiteral("staleSinceMs")),
              QVariant(qint64{-1}))
        << "fresh again once the RPM is back";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A term on a signal the session reports it doesn't get reads
// unavailable, in the app's words; so does one bound to a def in
// another unit
TEST(BridgeTest, AConditionOnASignalTheMockDisclaimsReadsNotReceived)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    ASSERT_TRUE(
        bridge
            .saveConditions(QStringLiteral(
                R"({"havne_conditions":1,"conditions":[)"
                R"({"key":"fuel","name":"Fuel pressure",)"
                R"("when":{"signal":["obd2.pid_0a"],"unit":"kPa","above":1}},)"
                R"({"key":"unit","name":"Engine speed in Hz",)"
                R"("when":{"signal":["obd2.rpm"],"unit":"Hz","above":1}}]})"))
            .value(QStringLiteral("ok"))
            .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://disclaims?profile=obd2"))
            .isEmpty());
    const ConditionModel* model = bridge.conditionModel();
    ASSERT_TRUE(spinUntil(
        [&]() {
            return conditionRow(model, 0)
                .value(QStringLiteral("unavailable"))
                .toBool();
        },
        5000));
    QVariantMap row = conditionRow(model, 0);
    EXPECT_EQ(row.value(QStringLiteral("why")).toString(),
              QStringLiteral("the ECU doesn't send its signal"));
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(),
              QVariantList{ECUIO_OBD2_PID_SIG_ID(0x0A, 0)});
    row = conditionRow(model, 1);
    EXPECT_TRUE(row.value(QStringLiteral("unavailable")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("why")).toString(),
              QStringLiteral("its unit isn't its signal's"));
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(),
              QVariantList{ECUIO_OBD2_SIG_RPM_ID})
        << "a term bound to a def it can't judge still names it";
    EXPECT_EQ(ConditionModel::whyText(ECUIO_CONDITION_WHY_NONE), QString());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// The worker's callback writes a slot made at the attach: nothing is
// allocated per event, however many come (counted on this thread through
// the same call core's worker makes)
TEST(BridgeTest, TheBridgeAllocatesNothingPerConditionEvent)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://news")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    const uint32_t g = bridge.conditionsGenerationForTesting();
    ecuio_condition_event_t events[3] = {
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 1),
        conditionEvent(1, g, 0, 2),
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 3),
    };
    bridge.dispatchConditionsForTesting(events, 3);
    const unsigned long before = new_counter::on_this_thread();
    for (int i = 0; i < 10000; ++i) {
        bridge.dispatchConditionsForTesting(events, 3);
    }
    EXPECT_EQ(new_counter::on_this_thread() - before, 0ul);
    // Each landed: no tick ran between them, and the mock never moves
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(
                QStringLiteral("Condition: Moving fired 10001 times"));
        },
        5000))
        << log.join(u'\n').toStdString();

    // The same events once their set is the set before
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), g + 1);
    bridge.dispatchConditionsForTesting(events, 3);
    const unsigned long late = new_counter::on_this_thread();
    for (int i = 0; i < 10000; ++i) {
        bridge.dispatchConditionsForTesting(events, 3);
    }
    EXPECT_EQ(new_counter::on_this_thread() - late, 0ul);
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.filter(
                          QStringLiteral("Condition: Moving fired 10001 times"))
                       .size() == 2;
        },
        5000))
        << log.join(u'\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

// Three alerts on an engine speed the mock never reaches, so only the
// events a case dispatches fire: two that sound, at each level, and one
// that doesn't
QByteArray toneConditions()
{
    return QByteArray(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"danger","sound":true}},)"
        R"({"key":"warm","name":"Warm","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":19000},)"
        R"("alert":{"level":"warning","sound":true}},)"
        R"({"key":"quiet","name":"Quiet","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":18000},)"
        R"("alert":{"level":"danger","banner":true}}]})");
}

// A session of mock://demo judging toneConditions, its log kept
struct ToneRun {
    SettingsDirScope dir;
    SessionBridge bridge;
    QStringList log;
    uint32_t generation = 0;

    bool start()
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
        if (!dir.valid() ||
            !bridge.saveConditions(QString::fromUtf8(toneConditions()))
                 .value(QStringLiteral("ok"))
                 .toBool() ||
            !bridge.connectSession(QStringLiteral("mock://demo")).isEmpty() ||
            !rowReads(bridge, 0, "off")) {
            return false;
        }
        generation = bridge.conditionsGenerationForTesting();
        return true;
    }
    // Each condition fired in one window, then the tick that writes it
    bool fire(std::initializer_list<uint32_t> conditions)
    {
        std::vector<ecuio_condition_event_t> events;
        for (uint32_t c : conditions) {
            events.push_back(
                conditionEvent(c, generation, ECUIO_CONDITION_FIRED, 20001));
        }
        const qsizetype before =
            linesStarting(log, QStringLiteral("Alert: ")).size();
        bridge.dispatchConditionsForTesting(
            events.data(), static_cast<uint32_t>(events.size()));
        const auto want = before + static_cast<qsizetype>(events.size());
        return spinUntil(
            [&]() {
                return linesStarting(log, QStringLiteral("Alert: ")).size() >=
                       want;
            },
            5000);
    }
    uint64_t danger() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_DANGER);
    }
    uint64_t warning() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_WARNING);
    }
};

} // namespace

// A firing whose alert sounds plays one tone; another of the same
// condition within 5 s plays none, and one after it plays again
TEST(BridgeTest, ASoundingFiringPlaysOnceInFiveSeconds)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 0u) << "nothing fired yet";

    ASSERT_TRUE(run.fire({0})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u);
    EXPECT_EQ(run.warning(), 0u);

    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 1u) << "a second firing at once";
    // 3 s added leaves the ticks' own time 2 s of margin
    run.bridge.addToneTimeForTesting(3000000);
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 1u) << "within 5 s of the tone";

    run.bridge.addToneTimeForTesting(2000000);
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 2u) << "5 s after the tone";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// Each condition keeps its own 5 s, and a level its own tone. Firings in
// one tick are one tone, the loudest, and each of them waits its 5 s
TEST(BridgeTest, EachConditionSoundsItsOwnLevel)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0}));
    ASSERT_TRUE(run.fire({1}));
    EXPECT_EQ(run.danger(), 1u);
    EXPECT_EQ(run.warning(), 1u) << "the second condition's own tone";

    run.bridge.addToneTimeForTesting(5000000);
    ASSERT_TRUE(run.fire({1, 0}));
    EXPECT_EQ(run.danger(), 2u) << "one tick: the danger tone";
    EXPECT_EQ(run.warning(), 1u) << "one tick: one tone";
    ASSERT_TRUE(run.fire({1}));
    EXPECT_EQ(run.warning(), 1u) << "the warning sounded with the danger";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// An alert without "sound" plays nothing when it fires
TEST(BridgeTest, AnAlertWithoutSoundPlaysNone)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({2})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 0u);
    EXPECT_EQ(run.warning(), 0u);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// Many firings of a sounding alert in one window are one tone, and the
// worker's events allocate nothing; the tick's lines are 7a's
TEST(BridgeTest, AWindowsFiringsAreOneToneAndItsEventsAllocateNothing)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    const ecuio_condition_event_t event =
        conditionEvent(0, run.generation, ECUIO_CONDITION_FIRED, 20001);
    run.bridge.dispatchConditionsForTesting(&event, 1);
    const unsigned long before = new_counter::on_this_thread();
    for (int i = 0; i < 9999; ++i) {
        run.bridge.dispatchConditionsForTesting(&event, 1);
    }
    EXPECT_EQ(new_counter::on_this_thread() - before, 0ul);
    EXPECT_TRUE(spinUntil(
        [&run]() {
            return run.log.contains(
                QStringLiteral("Alert: Hot fired 10000 times"));
        },
        5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// An edit saved mid-session keeps the 5 s of each condition whose key it
// keeps, wherever the condition now sits, and its new ones sound
TEST(BridgeTest, AnEditKeepsEachConditionsFiveSecondsByItsKey)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0}));
    ASSERT_EQ(run.danger(), 1u);

    // A condition before Hot and one past the old set's end, both new
    QByteArray grown = toneConditions();
    grown.replace(
        R"("conditions":[)",
        R"("conditions":[{"key":"first","name":"First","when":{"signal":)"
        R"(["id:1001"],"unit":"RPM","above":21000},"alert":)"
        R"({"level":"warning","sound":true}},)");
    grown.replace(
        R"(]})", R"(,{"key":"last","name":"Last","when":{"signal":["id:1001"],)"
                 R"("unit":"RPM","above":22000},"alert":)"
                 R"({"level":"danger","sound":true}}]})");
    const QVariantMap saved =
        run.bridge.saveConditions(QString::fromUtf8(grown));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(run.bridge.conditionModel()->rowCount(), 5);
    run.generation = run.bridge.conditionsGenerationForTesting();

    ASSERT_TRUE(run.fire({1})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "Hot, now second, within its 5 s";
    ASSERT_TRUE(run.fire({4}));
    EXPECT_EQ(run.danger(), 2u) << "the new last condition sounds";
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.warning(), 1u) << "the new first condition sounds";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// A firing of the set before that arrives after an edit sounds by its
// key's 5 s: the row that keeps the key shares it, and a key the edit
// dropped keeps its own
TEST(BridgeTest, ALateFiringOfTheSetBeforeKeepsItsKeysFiveSeconds)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0, 1}));
    ASSERT_EQ(run.danger(), 1u) << "one tick: the danger tone";
    ASSERT_EQ(run.warning(), 0u);
    const uint32_t before = run.generation;

    // Hot alone, behind a new condition: Warm and Quiet are dropped
    const QByteArray edited(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"first","name":"First","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":21000},)"
        R"("alert":{"level":"warning","banner":true}},)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"danger","sound":true}}]})");
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(edited))
                    .value(QStringLiteral("ok"))
                    .toBool());
    run.generation = run.bridge.conditionsGenerationForTesting();
    ASSERT_EQ(run.generation, before + 1);
    // A late firing of the set before's condition c, then its line
    const auto late = [&run, before](uint32_t c, const char* line) {
        const qsizetype had =
            linesStarting(run.log, QLatin1String(line)).size();
        const ecuio_condition_event_t event =
            conditionEvent(c, before, ECUIO_CONDITION_FIRED, 20001);
        run.bridge.dispatchConditionsForTesting(&event, 1);
        return spinUntil(
            [&]() {
                return linesStarting(run.log, QLatin1String(line)).size() > had;
            },
            5000);
    };

    ASSERT_TRUE(late(0, "Alert: Hot, 20001 RPM"))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "Hot, within the 5 s of its tone";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 0u)
        << "a dropped key, within the 5 s it began before the edit";
    ASSERT_TRUE(late(2, "Alert: Quiet, 20001 RPM"));
    EXPECT_EQ(run.danger(), 1u) << "an alert with no sound";

    run.bridge.addToneTimeForTesting(5000000);
    ASSERT_TRUE(late(0, "Alert: Hot, 20001 RPM"));
    EXPECT_EQ(run.danger(), 2u) << "5 s after its tone";
    ASSERT_TRUE(run.fire({1})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 2u)
        << "Hot's row in the set attached shares the late firing's 5 s";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 1u) << "the dropped key, 5 s on";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 1u) << "and within the 5 s that began";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// A late firing of a key the edit keeps sounds as the edit has it: at its
// new level, not at all with its sound turned off, and with one turned on
TEST(BridgeTest, ALateFiringOfAKeptKeySoundsAsTheSetAttachedHasIt)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    const uint32_t before = run.generation;
    // Hot a warning, Warm without its sound, Quiet with one
    const QByteArray edited(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"warning","sound":true}},)"
        R"({"key":"warm","name":"Warm","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":19000},)"
        R"("alert":{"level":"warning","banner":true}},)"
        R"({"key":"quiet","name":"Quiet","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":18000},)"
        R"("alert":{"level":"danger","sound":true}}]})");
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(edited))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), before + 1);
    const auto late = [&run, before](uint32_t c, const char* line) {
        const ecuio_condition_event_t event =
            conditionEvent(c, before, ECUIO_CONDITION_FIRED, 20001);
        run.bridge.dispatchConditionsForTesting(&event, 1);
        return spinUntil(
            [&]() { return run.log.contains(QLatin1String(line)); }, 5000);
    };

    ASSERT_TRUE(late(0, "Alert: Hot, 20001 RPM"))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.warning(), 1u) << "the level the edit gave it";
    EXPECT_EQ(run.danger(), 0u) << "not the level it had";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 1u) << "its sound was turned off";
    ASSERT_TRUE(late(2, "Alert: Quiet, 20001 RPM"));
    EXPECT_EQ(run.danger(), 1u) << "its sound was turned on";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// An attach core refuses leaves every tone's 5 s as it was: the attached
// set's and the set before's
TEST(BridgeTest, ARefusedAttachKeepsEachTonesFiveSeconds)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0, 1}));
    ASSERT_EQ(run.danger(), 1u);
    const uint32_t before = run.generation;
    // Hot alone: Warm's 5 s is the set before's from here
    const QByteArray edited(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"danger","sound":true}}]})");
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(edited))
                    .value(QStringLiteral("ok"))
                    .toBool());
    run.generation = run.bridge.conditionsGenerationForTesting();
    ASSERT_EQ(run.generation, before + 1);

    run.bridge.failConditionsAttachesForTesting(1);
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(toneConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), run.generation)
        << "refused";

    ASSERT_TRUE(run.fire({0})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "Hot, within the 5 s of its tone";
    const ecuio_condition_event_t warm =
        conditionEvent(1, before, ECUIO_CONDITION_FIRED, 20001);
    run.bridge.dispatchConditionsForTesting(&warm, 1);
    // Its second line: the first is the firing before the edit
    ASSERT_TRUE(spinUntil(
        [&run]() {
            return linesStarting(run.log,
                                 QStringLiteral("Alert: Warm, 20001 RPM"))
                       .size() == 2;
        },
        5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.warning(), 0u) << "the set before's Warm, within its 5 s";

    run.bridge.addToneTimeForTesting(5000000);
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 2u) << "5 s on, each sounds again";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

namespace {

// A tone maker standing in for Android's: each call recorded in a state
// the case keeps after the player is gone
struct FakeTones : AlertTone::Backend {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>>
            starts;
        int makes = 0;
        int releases = 0;
        std::vector<std::chrono::steady_clock::time_point> makeTimes;
        std::vector<std::chrono::steady_clock::time_point> releaseTimes;
        int failMakes = 0;    // the next n makes fail
        int failStarts = 0;   // the next n starts fail
        int blockFirstMs = 0; // the first start takes this long
    };
    std::shared_ptr<State> s = std::make_shared<State>();

    bool make() override
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->makes;
        s->makeTimes.push_back(std::chrono::steady_clock::now());
        s->changed.notify_all();
        if (s->failMakes > 0) {
            --s->failMakes;
            return false;
        }
        return true;
    }
    bool start(uint32_t level) override
    {
        int block = 0;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            std::swap(block, s->blockFirstMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(block));
        std::lock_guard<std::mutex> lock(s->mutex);
        s->changed.notify_all();
        if (s->failStarts > 0) {
            --s->failStarts;
            return false;
        }
        s->starts.emplace_back(level, std::chrono::steady_clock::now());
        return true;
    }
    void release() override
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->releases;
        s->releaseTimes.push_back(std::chrono::steady_clock::now());
        s->changed.notify_all();
    }
};

// Waits up to ms for pred over the state, under its lock
template <typename Pred> bool toneState(FakeTones::State& s, int ms, Pred pred)
{
    std::unique_lock<std::mutex> lock(s.mutex);
    return s.changed.wait_for(lock, std::chrono::milliseconds(ms),
                              [&]() { return pred(s); });
}

} // namespace

// A tone plays out before the next starts, and the asks meanwhile are one
// tone, the loudest: a warning then a danger is a beep, then two beeps
TEST(BridgeTest, ATonePlaysOutAndAsksMeanwhileMergeIntoTheLoudest)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    {
        AlertTone tone(std::move(fake), {1000, 3000, 5000});
        tone.play(ECUIO_ALERT_WARNING);
        ASSERT_TRUE(toneState(*s, 2000, [](FakeTones::State& t) {
            return t.starts.size() == 1;
        }));
        tone.play(ECUIO_ALERT_WARNING);
        tone.play(ECUIO_ALERT_DANGER);
        tone.play(ECUIO_ALERT_WARNING);
        ASSERT_TRUE(toneState(*s, 4000, [](FakeTones::State& t) {
            return t.starts.size() == 2;
        }));
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            EXPECT_EQ(s->starts[0].first, ECUIO_ALERT_WARNING);
            EXPECT_EQ(s->starts[1].first, ECUIO_ALERT_DANGER);
            EXPECT_GE(s->starts[1].second - s->starts[0].second,
                      std::chrono::milliseconds(1000))
                << "the first tone played out";
            EXPECT_EQ(s->makes, 1);
        }
        // Nothing was left waiting: no third tone after the second's room
        EXPECT_FALSE(toneState(*s, 1300, [](FakeTones::State& t) {
            return t.starts.size() > 2;
        })) << "the three asks were one tone";
    }
    EXPECT_EQ(s->releases, 1) << "released as the player ends";
}

// A generator that can't be made, or fails, is made again on a later ask
// once the retry interval has passed, and not before
TEST(BridgeTest, AFailedToneGeneratorIsMadeAgainAfterAPause)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    s->failMakes = 1;
    AlertTone tone(std::move(fake), {50, 5000, 400});
    // Asks every 50 ms until the backend sees n makes
    const auto askUntilMade = [&](uint32_t level, int n) {
        for (int i = 0; i < 100; ++i) {
            tone.play(level);
            if (toneState(*s, 50,
                          [n](FakeTones::State& t) { return t.makes >= n; })) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(askUntilMade(ECUIO_ALERT_WARNING, 1));
    ASSERT_TRUE(askUntilMade(ECUIO_ALERT_WARNING, 2));
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 1; }));
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        EXPECT_GE(s->makeTimes[1] - s->makeTimes[0],
                  std::chrono::milliseconds(400))
            << "made again within the interval, though asked meanwhile";
        s->failStarts = 1;
    }

    // A start that fails releases the generator; a later ask makes another
    ASSERT_TRUE(askUntilMade(ECUIO_ALERT_DANGER, 3));
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 2; }));
    std::lock_guard<std::mutex> lock(s->mutex);
    ASSERT_EQ(s->releaseTimes.size(), 1u);
    EXPECT_GE(s->makeTimes[2] - s->releaseTimes[0],
              std::chrono::milliseconds(400))
        << "made again within the interval after the failed start";
    EXPECT_EQ(s->starts[1].first, ECUIO_ALERT_DANGER);
}

// An ask still waiting past the stale bound, behind a start that blocked,
// plays nothing; a fresh one plays
TEST(BridgeTest, AToneAskedLongBeforeItsTurnPlaysNone)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    s->blockFirstMs = 700;
    AlertTone tone(std::move(fake), {50, 300, 5000});
    tone.play(ECUIO_ALERT_WARNING);
    ASSERT_TRUE(
        toneState(*s, 2000, [](FakeTones::State& t) { return t.makes == 1; }));
    tone.play(ECUIO_ALERT_DANGER); // waits behind the blocked start
    ASSERT_TRUE(
        spinUntil([&tone]() { return tone.staleForTesting() == 1; }, 5000))
        << "the late danger wasn't dropped";
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ASSERT_EQ(s->starts.size(), 1u) << "the late danger played";
        EXPECT_EQ(s->starts[0].first, ECUIO_ALERT_WARNING);
    }
    tone.play(ECUIO_ALERT_DANGER);
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 2; }));
}

// An ask of a level already waiting brings its time up to date: behind a
// start that blocked, the newer danger plays though the older is stale
TEST(BridgeTest, AFreshAskBehindABlockedStartPlaysThoughOneOfItsLevelWaited)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    s->blockFirstMs = 1500;
    AlertTone tone(std::move(fake), {50, 1000, 5000});
    tone.play(ECUIO_ALERT_WARNING);
    ASSERT_TRUE(
        toneState(*s, 2000, [](FakeTones::State& t) { return t.makes == 1; }));
    tone.play(ECUIO_ALERT_DANGER);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    tone.play(ECUIO_ALERT_DANGER); // the older is about to be stale
    ASSERT_TRUE(toneState(*s, 5000, [](FakeTones::State& t) {
        return t.starts.size() == 2;
    })) << "the fresh danger was dropped with the stale one";
    std::lock_guard<std::mutex> lock(s->mutex);
    EXPECT_EQ(s->starts[1].first, ECUIO_ALERT_DANGER);
}

// play hands a tone to a running player with no allocation, and only the
// two levels are tones
TEST(BridgeTest, PlayAllocatesNothingAndOnlyTheTwoLevelsAreTones)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    AlertTone tone(std::move(fake), {1, 1000, 5000});
    tone.play(ECUIO_ALERT_WARNING);
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 1; }));
    const unsigned long before = new_counter::on_this_thread();
    for (int i = 0; i < 10000; ++i) {
        tone.play(i % 2 == 0 ? ECUIO_ALERT_DANGER : ECUIO_ALERT_WARNING);
    }
    EXPECT_EQ(new_counter::on_this_thread() - before, 0ul);
    tone.play(ECUIO_ALERT_NONE);
    tone.play(7);
    EXPECT_EQ(tone.playedForTesting(ECUIO_ALERT_DANGER), 5000u)
        << "a level past danger plays nothing";
    EXPECT_EQ(tone.playedForTesting(ECUIO_ALERT_WARNING), 5001u);
    EXPECT_EQ(tone.playedForTesting(ECUIO_ALERT_NONE), 0u);
}

// An attach core refuses leaves the slots as they were: the set still
// judged keeps its events
TEST(BridgeTest, ARefusedAttachKeepsTheSlotsOfTheSetJudged)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://refused")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 1u);

    // A firing the worker gives while the attach is being refused
    const ecuio_condition_event_t during =
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 5);
    bridge.failConditionsAttachesForTesting(1, [&bridge, &during]() {
        bridge.dispatchConditionsForTesting(&during, 1);
    });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_FALSE(
        linesStarting(log, QStringLiteral("Alerts not judged: ")).isEmpty())
        << log.join(u'\n').toStdString();
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3) << "the set judged";
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(QStringLiteral("Condition: Moving, 5 km/h"));
        },
        5000))
        << "kept through the refusal: " << log.join(u'\n').toStdString();

    // The third slot of generation 1: kept at its size and generation
    const ecuio_condition_event_t third =
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 7);
    bridge.dispatchConditionsForTesting(&third, 1);
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(QStringLiteral("Condition: Moving, 7 km/h"));
        },
        5000))
        << log.join(u'\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A refused attach leaves the set before's slots and rows too: its firing
// during the refused call or after it is still its line, and the set
// judged keeps its own
TEST(BridgeTest, ARefusedAttachKeepsTheSlotsOfTheSetBefore)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://refused")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 2u);
    // The mock's own firing of the second set first: one line, not a count
    ASSERT_TRUE(spinUntil(
        [&log]() {
            return !linesStarting(log, QStringLiteral("Alert: Second set"))
                        .isEmpty();
        },
        5000));

    // The set before's firing, given while the attach is being refused
    const ecuio_condition_event_t during =
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 6, 500000);
    bridge.failConditionsAttachesForTesting(1, [&bridge, &during]() {
        bridge.dispatchConditionsForTesting(&during, 1);
    });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(toneConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 2u) << "refused";
    const ecuio_condition_event_t events[] = {
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 7, 1000000),
        conditionEvent(0, 2, ECUIO_CONDITION_FIRED, 8, 2000000, 1),
    };
    bridge.dispatchConditionsForTesting(events, 2);
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(QStringLiteral("Alert: Second set, 8 km/h"));
        },
        5000))
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Condition: Moving fired 2 times")))
        << "the set before the one judged, during the refusal and after: "
        << log.join(u'\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A saved file core refuses at the start is kept for its editor: its text
// is the conditions' text until a save that core takes replaces it
TEST(BridgeTest, AFileCoreRefusedAtTheStartKeepsItsText)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray typo =
        R"({"havne_conditions":1,"conditions":[{"key":"x"}]})";
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).absolutePath()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(typo);
    }
    SessionBridge bridge;
    EXPECT_TRUE(bridge.conditionsProblem().startsWith(
        QStringLiteral("The saved conditions weren't read: ")))
        << bridge.conditionsProblem().toStdString();
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(typo));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 0);
    EXPECT_EQ(readFile(ConditionStore::path()), typo)
        << "the file left as it was";

    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_EQ(bridge.conditionsProblem(), QString());
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(mockConditions()));
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
}
