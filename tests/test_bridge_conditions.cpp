// File/layer: tests/test_bridge_conditions.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Cases for the saved conditions, their model, the bridge's attach and tick, and the tone.

#include "test_bridge_conditions_support.h"

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
