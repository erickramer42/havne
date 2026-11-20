// File/layer: tests/test_bridge_signalfiles.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Cases for signal files, plugins, refs and ranges, layout files and the pickers.

#include "test_bridge_signalfiles_support.h"

// ---------------------------------------------------------------------------
// The vehicles' signal files (ADR-0051), kept as plugins (ADR-0064): each
// case's store in a HAVNE_SETTINGS_DIR of its own.
// ---------------------------------------------------------------------------

/* A format 2 file (ADR-0062) is kept and used as any: its chain counts
 * among its signals, its profile registers, and a format 1 file using a
 * call or a format 3 file is refused in the loader's words */
TEST(BridgeTest, AFormat2SignalFileImportsAndIsUsedAsAny)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const auto keep = [&bridge](const char* format, const char* expr) {
        const QString text =
            QStringLiteral(
                R"({"havne_signals":%1,"name":"Chained","source":{"name":"t"},)"
                R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
                R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
                R"("field":{"byte":0,"bits":8}},)"
                R"({"key":"b","name":"B","kind":"computed","expr":"%2"},)"
                R"({"key":"c","name":"C","kind":"computed","expr":"b * 2"}]})")
                .arg(QLatin1String(format), QLatin1String(expr));
        return bridge.keepSignalText(text, QString(), QString(), QString());
    };
    QVariantMap r = keep("2", "max(a, 1)");
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("counts")).toList().value(0).toInt(), 3);
    const QByteArray profile =
        r.value(QStringLiteral("profiles")).toStringList().value(0).toUtf8();
    EXPECT_TRUE(registered(profile.constData())) << profile.constData();

    r = keep("1", "max(a, 1)");
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("\"havne_signals\": 2")))
        << r.value(QStringLiteral("words")).toString().toStdString();

    r = keep("3", "a");
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("format 3")))
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(bridge.plugins().size(), 1);
}

// ---- ADR-0057's refs and ranges: the model's rows and profileSignals ----

namespace {

// The rows of the bridge's signal model, each a map of its role names
QVariantList modelRows(SessionBridge& bridge)
{
    SignalListModel* model = bridge.signalModel();
    const QHash<int, QByteArray> roles = model->roleNames();
    QVariantList rows;
    for (int i = 0; i < model->rowCount(); ++i) {
        QVariantMap row;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            row.insert(QString::fromUtf8(it.value()),
                       model->data(model->index(i, 0), it.key()));
        }
        rows.append(row);
    }
    return rows;
}

// The row named name, an empty map if none
QVariantMap rowNamed(const QVariantList& rows, const QString& name)
{
    for (const QVariant& r : rows) {
        if (r.toMap().value(QStringLiteral("name")).toString() == name) {
            return r.toMap();
        }
    }
    return {};
}

QStringList refsOf(const QVariantList& rows)
{
    QStringList refs;
    for (const QVariant& r : rows) {
        refs << r.toMap().value(QStringLiteral("ref")).toString();
    }
    return refs;
}

// Keeps bytes as a plugin of the user's own; its variant's profile
QString useFile(SessionBridge& bridge, const QByteArray& bytes)
{
    const QVariantMap r = bridge.keepSignalText(
        QString::fromUtf8(bytes), QString(), QString(), QString());
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    return r.value(QStringLiteral("profiles")).toStringList().value(0);
}

// A signal file extending obd2: an 8-bit temperature at -40 and one at
// -50 (whose low end differs between the unit systems), an enum, a
// text, and a computed signal over the temperature
QByteArray rangesFile()
{
    return QByteArray(
        R"({"havne_signals":1,"name":"Ranges","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"temp","name":"Temp","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8},"offset":-40,"unit":"\u00b0C"},)"
        R"({"key":"gear","name":"Gear","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"kind":"enum","values":{"0":"Park"}},)"
        R"({"key":"part","name":"Part","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0},"kind":"text"},)"
        R"({"key":"cold","name":"Cold","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":8},"offset":-50,"unit":"\u00b0C"},)"
        R"({"key":"twice","name":"Twice","kind":"computed","expr":"temp * 2"}]})");
}

} // namespace

/* The mock's and obd2's rows are theirs by core's def ID; a row's ref
 * is the same in the model and in profileSignals */
TEST(BridgeTest, ABuiltInSetsRowsAreReferencedByTheirIds)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    const QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QVariant& r : rows) {
        const QVariantMap m = r.toMap();
        EXPECT_EQ(
            m.value(QStringLiteral("ref")).toString(),
            QStringLiteral("id:%1").arg(m.value(QStringLiteral("id")).toInt()))
            << m.value(QStringLiteral("name")).toString().toStdString();
    }
    EXPECT_EQ(
        rowNamed(rows, QStringLiteral("RPM")).value(QStringLiteral("ref")),
        QStringLiteral("id:1001"));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("mock://demo"))),
              refsOf(rows));
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QString obd2 =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_TRUE(bridge.connectSession(obd2).isEmpty());
    const QVariantList obd2Rows = modelRows(bridge);
    EXPECT_TRUE(refsOf(obd2Rows).contains(QStringLiteral("id:7003")));
    for (const QString& ref : refsOf(obd2Rows)) {
        EXPECT_TRUE(ref.startsWith(QStringLiteral("id:"))) << ref.toStdString();
    }
    EXPECT_EQ(refsOf(bridge.profileSignals(obd2)), refsOf(obd2Rows));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("elm327://COM9"))),
              refsOf(obd2Rows))
        << "an adapter's default, obd2";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* uds-generic's rows and a set registered with no signal file (the
 * mock's, copied) have no ref, though they carry IDs */
TEST(BridgeTest, ASetWithNoFileHasNoRefs)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://demo?profile=uds-generic&did=1E1C:u16"))
                    .isEmpty());
    QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));

    // The registry keeps the set's pointer until it's unregistered
    static std::vector<ecuio_signal_def_t> defs;
    static ecuio_signal_set_t copy{};
    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    defs.assign(mock->defs, mock->defs + mock->count);
    copy = *mock;
    copy.defs = defs.data();
    ASSERT_EQ(ecuio_profile_register("copied-mock", &copy, nullptr), ECUIO_OK);
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=copied-mock"))
            .isEmpty());
    rows = modelRows(bridge);
    ASSERT_EQ(rows.size(), static_cast<int>(mock->count));
    EXPECT_EQ(rowNamed(rows, QStringLiteral("RPM"))
                  .value(QStringLiteral("id"))
                  .toInt(),
              1001);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    const QVariantList copied = bridge.profileSignals(
        QStringLiteral("mock://demo?profile=copied-mock"));
    ASSERT_EQ(copied.size(), static_cast<int>(mock->count));
    for (const QString& ref : refsOf(copied)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(ecuio_profile_unregister("copied-mock"), ECUIO_OK);
}

/* The Dashboard starts a dial from the file's gauge: profileSignals' row
 * carries its range and levels in the decoded unit, and {} for a signal
 * with none or a level the file leaves out */
TEST(BridgeTest, AProfileRowCarriesTheFilesGauge)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QByteArray gauged = signalFile(QStringLiteral("Gauged"));
    gauged.replace(
        R"("field":{"byte":0,"bits":8}})",
        R"("field":{"byte":0,"bits":8},)"
        R"("gauge":{"min":10,"max":16,"warn_low":11.5,"warn_high":15.25}})");
    const QString profile = useFile(bridge, gauged);
    ASSERT_FALSE(profile.isEmpty());
    const QVariantList rows =
        bridge.profileSignals(QStringLiteral("mock://demo?profile=") + profile);
    const QVariantMap gauge = rowNamed(rows, QStringLiteral("A"))
                                  .value(QStringLiteral("gauge"))
                                  .toMap();
    EXPECT_EQ(gauge.value(QStringLiteral("min")).toDouble(), 10.0);
    EXPECT_EQ(gauge.value(QStringLiteral("max")).toDouble(), 16.0);
    EXPECT_EQ(gauge.value(QStringLiteral("warnLow")).toDouble(), 11.5);
    EXPECT_EQ(gauge.value(QStringLiteral("warnHigh")).toDouble(), 15.25);

    QByteArray lowOnly =
        signalFile(QStringLiteral("LowOnly"), QStringLiteral("D101"));
    lowOnly.replace(R"("field":{"byte":0,"bits":8}})",
                    R"("field":{"byte":0,"bits":8},)"
                    R"("gauge":{"min":0,"max":100,"warn_low":20}})");
    const QString lowProfile = useFile(bridge, lowOnly);
    ASSERT_FALSE(lowProfile.isEmpty());
    const QVariantMap low =
        rowNamed(bridge.profileSignals(QStringLiteral("mock://demo?profile=") +
                                       lowProfile),
                 QStringLiteral("A"))
            .value(QStringLiteral("gauge"))
            .toMap();
    EXPECT_TRUE(low.contains(QStringLiteral("warnLow")));
    EXPECT_FALSE(low.contains(QStringLiteral("warnHigh")));

    const QVariantList plain =
        bridge.profileSignals(QStringLiteral("mock://demo"));
    ASSERT_FALSE(plain.isEmpty());
    for (const QVariant& row : plain) {
        EXPECT_TRUE(
            row.toMap().value(QStringLiteral("gauge")).toMap().isEmpty());
    }
}

/* A signal file's own defs are theirs by key, its extends base's by
 * the base's ID; a file extending nothing has keys alone */
TEST(BridgeTest, ASignalFilesRowsAreReferencedByTheirKeys)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QByteArray extending = signalFile(QStringLiteral("Extending"));
    extending.replace(R"("source":{"name":"t"},)",
                      R"("source":{"name":"t"},"extends":"obd2",)");
    const QString profile = useFile(bridge, extending);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QVariantList rows = modelRows(bridge);
    EXPECT_EQ(rowNamed(rows, QStringLiteral("A")).value(QStringLiteral("ref")),
              QStringLiteral("key:a"));
    const QStringList refs = refsOf(rows);
    EXPECT_TRUE(refs.contains(QStringLiteral("id:7003"))) << "obd2's RPM";
    EXPECT_EQ(refs.filter(QStringLiteral("key:")).size(), 1);
    EXPECT_FALSE(refs.contains(QString()));
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)), refs);

    ASSERT_TRUE(disconnectAndWait(bridge));

    // The plugin removed: its profile is gone from the registry, so
    // profileSignals gives []
    ASSERT_EQ(removeEveryPlugin(bridge), 1);
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty());

    const QString alone = useFile(bridge, signalFile(QStringLiteral("Alone")));
    ASSERT_FALSE(alone.isEmpty());
    EXPECT_EQ(refsOf(bridge.profileSignals(
                  QStringLiteral("mock://demo?profile=") + alone)),
              QStringList{QStringLiteral("key:a")});
}

/* The mock answers Service 01 from its drive cycle and says what it
 * doesn't send: under a signal file that extends obd2, the PIDs its ECU
 * lacks (the file's own among them) and what is computed from one are
 * hidden as not sent by the ECU (ADR-0039 section 6), so a Dashboard cell
 * reads "not sent by this ECU", never "waiting for data" for good; obd2's
 * engine speed and Boost, the file's DID and what is computed from it
 * have values. The ECU's line is logged once. */
TEST(BridgeTest, TheMockSendsTheDemoCarsPidsAndHidesTheRest)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray file(
        R"({"havne_signals":1,"name":"Disclaimed","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"did","name":"Own DID","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8}},)"
        R"({"key":"pid","name":"Own PID","read":{"service":"01","number":"06"},)"
        R"("field":{"byte":0,"bits":8}},)"
        R"({"key":"of_did","name":"Of DID","kind":"computed","expr":"did * 2"},)"
        R"({"key":"of_pid","name":"Of PID","kind":"computed","expr":"pid * 2"}]})");
    const QString profile = useFile(bridge, file);
    ASSERT_FALSE(profile.isEmpty());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=") + profile)
            .isEmpty());
    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil([&] { return model->hiddenCount() > 0; }, 3000));
    ASSERT_TRUE(spinUntil(
        [&] {
            return rowNamed(modelRows(bridge), QStringLiteral("Of DID"))
                .value(QStringLiteral("hasValue"))
                .toBool();
        },
        3000));

    const QVariantList rows = modelRows(bridge);
    const auto hidden = [&rows](const char* name) {
        const QVariantMap row = rowNamed(rows, QString::fromLatin1(name));
        EXPECT_FALSE(row.isEmpty()) << name;
        return row.value(QStringLiteral("hidden")).toBool();
    };
    const auto hasValue = [&rows](const char* name) {
        return rowNamed(rows, QString::fromLatin1(name))
            .value(QStringLiteral("hasValue"))
            .toBool();
    };
    EXPECT_FALSE(hidden("Own DID"));
    EXPECT_TRUE(hasValue("Own DID"));
    EXPECT_FALSE(hidden("Of DID"));
    EXPECT_TRUE(hidden("Own PID"));
    EXPECT_FALSE(hasValue("Own PID"));
    EXPECT_TRUE(hidden("Of PID")) << "computed from a PID the ECU lacks";
    EXPECT_FALSE(hasValue("Of PID"));
    EXPECT_FALSE(hidden("RPM")) << "obd2's, which the demo car sends";
    EXPECT_FALSE(hidden("Boost"))
        << "obd2's computed signal, from MAP and baro";
    ASSERT_TRUE(spinUntil(
        [&] {
            const QVariantList now = modelRows(bridge);
            return rowNamed(now, QStringLiteral("RPM"))
                       .value(QStringLiteral("hasValue"))
                       .toBool() &&
                   rowNamed(now, QStringLiteral("Boost"))
                       .value(QStringLiteral("hasValue"))
                       .toBool();
        },
        3000));

    // obd2's rows the ECU doesn't send are hidden, the rest shown in Live:
    // each field of a PID the demo car doesn't answer (sim_ecu.h's drive
    // PIDs and its diagnostics' readiness and counts since a clear)
    static const uint8_t kDemoPids[] = {
        0x01, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1F, 0x21,
        0x24, 0x30, 0x31, 0x33, 0x41, 0x42, 0x44, 0x46, 0x4D, 0x4E, 0x5C};
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    int notSent = 0;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue; /* Boost: MAP and baro are sent */
        }
        const auto pid = static_cast<uint8_t>(d.mux_key);
        notSent += std::find(std::begin(kDemoPids), std::end(kDemoPids), pid) ==
                           std::end(kDemoPids)
                       ? 1
                       : 0;
    }
    uint32_t unsupported = 0;
    ASSERT_EQ(ecuio_session_get_unsupported_signals(bridge.sessionForTesting(),
                                                    nullptr, 0, &unsupported),
              ECUIO_OK);
    int byId = 0;
    int byIdHidden = 0;
    for (const QVariant& r : rows) {
        const QVariantMap row = r.toMap();
        if (row.value(QStringLiteral("ref"))
                .toString()
                .startsWith(QStringLiteral("id:"))) {
            ++byId;
            byIdHidden += row.value(QStringLiteral("hidden")).toBool() ? 1 : 0;
        }
    }
    ASSERT_GT(byId, 100);
    EXPECT_EQ(byIdHidden, notSent);
    EXPECT_EQ(model->hiddenCount(), byIdHidden + 2) << "Own PID and Of PID";
    EXPECT_EQ(model->hiddenCount(), static_cast<int>(unsupported));
    EXPECT_EQ(bridge.liveSignalModel()->rowCount(),
              model->rowCount() - model->hiddenCount());

    spinLoop(300);
    const QStringList ecu = log.filter(QStringLiteral("ECU 0x7E8: supports "));
    ASSERT_EQ(ecu.size(), 1) << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(
        ecu.first().startsWith(QStringLiteral("ECU 0x7E8: supports 22 PIDs")))
        << ecu.first().toStdString();
    EXPECT_TRUE(
        ecu.first().contains(QStringLiteral("(not in its supported PIDs)")))
        << ecu.first().toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));

    // The mock's own profile: nothing hidden, no ECU line
    log.clear();
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    spinLoop(400);
    EXPECT_EQ(model->hiddenCount(), 0);
    EXPECT_TRUE(log.filter(QStringLiteral("ECU 0x7E8: supports ")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A plugin variant's rows have refs, its file being the plugin
 * store's, in profileSignals and in a session's model alike */
TEST(BridgeTest, APluginVariantsRowsHaveRefs)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_FALSE(bytes.isEmpty());
    ASSERT_TRUE(bridge
                    .importPlugin(
                        writeScratch(from.path(), "p.havne-plugin.json", bytes))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString uri =
        QStringLiteral("mock://demo?profile=plugin.example-sim.base");
    const QVariantList rows = bridge.profileSignals(uri);
    ASSERT_GT(rows.size(), 0);
    const QStringList refs = refsOf(rows);
    EXPECT_FALSE(refs.contains(QString())) << refs.join(' ').toStdString();
    EXPECT_FALSE(refs.filter(QStringLiteral("key:")).isEmpty());
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_EQ(refsOf(modelRows(bridge)), refs);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A def's range follows the unit system in the model and in
 * profileSignals; an enum's is scaled as the decoder scales it, and a
 * text or computed def has none (NaN) */
TEST(BridgeTest, ARowsRangeIsShownInTheUnitSystem)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const QString profile = useFile(bridge, rangesFile());
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());

    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        const bool metric = units == 0;
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
            ASSERT_FALSE(temp.isEmpty());
            EXPECT_EQ(temp.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("°C") : QStringLiteral("°F"));
            EXPECT_NEAR(temp.value(QStringLiteral("rangeLow")).toDouble(),
                        -40.0, 1e-9);
            EXPECT_NEAR(temp.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 215.0 : 419.0, 1e-9);
            EXPECT_FALSE(temp.value(QStringLiteral("isEnum")).toBool());
            const QVariantMap cold = rowNamed(rows, QStringLiteral("Cold"));
            ASSERT_FALSE(cold.isEmpty());
            EXPECT_NEAR(cold.value(QStringLiteral("rangeLow")).toDouble(),
                        metric ? -50.0 : -58.0, 1e-9);
            EXPECT_NEAR(cold.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 205.0 : 401.0, 1e-9);

            const QVariantMap gear = rowNamed(rows, QStringLiteral("Gear"));
            EXPECT_TRUE(gear.value(QStringLiteral("isEnum")).toBool());
            EXPECT_EQ(gear.value(QStringLiteral("rangeLow")).toDouble(), 0.0);
            EXPECT_EQ(gear.value(QStringLiteral("rangeHigh")).toDouble(),
                      255.0);

            for (const char* none : {"Part", "Twice"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(none));
                ASSERT_FALSE(row.isEmpty()) << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << none;
                EXPECT_FALSE(row.value(QStringLiteral("isEnum")).toBool())
                    << none;
            }
            const QVariantMap speed =
                rowNamed(rows, QStringLiteral("Vehicle Speed"));
            ASSERT_FALSE(speed.isEmpty()) << "obd2's, the base";
            EXPECT_EQ(speed.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("km/h") : QStringLiteral("mph"));
            EXPECT_NEAR(speed.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 255.0 : 255.0 / 1.609344, 1e-3);
        }

        // profileSignals also gives the def's own unit and range, the
        // same in every unit system
        const QVariantList rows = bridge.profileSignals(uri);
        const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
        EXPECT_EQ(temp.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("°C"));
        EXPECT_NEAR(temp.value(QStringLiteral("decodedLow")).toDouble(), -40.0,
                    1e-9);
        EXPECT_NEAR(temp.value(QStringLiteral("decodedHigh")).toDouble(), 215.0,
                    1e-9);
        const QVariantMap speed =
            rowNamed(rows, QStringLiteral("Vehicle Speed"));
        EXPECT_EQ(speed.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("km/h"));
        EXPECT_NEAR(speed.value(QStringLiteral("decodedHigh")).toDouble(),
                    255.0, 1e-9);
        const QVariantMap part = rowNamed(rows, QStringLiteral("Part"));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedLow")).toDouble()));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedHigh")).toDouble()));
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A range whose end isn't a finite number (a 32-bit field at scale
 * 1e300), whose ends are one number (a byte at offset 1e300, past the
 * double's precision), whose end converts past a double (deg C near
 * DBL_MAX, times 1.8 in deg F), or whose ends meet once converted (a bit
 * at 1.75 km/h and one ulp more, one number in mph) is no range, NaN, in
 * the model and in profileSignals */
TEST(BridgeTest, ARangeOfAnEndPastADoubleIsNoRange)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const QByteArray bytes(
        R"({"havne_signals":1,"name":"Huge","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"huge","name":"Huge","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":32},"scale":1e300},)"
        R"({"key":"flat","name":"Flat","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"offset":1e300},)"
        R"({"key":"edge","name":"Edge","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0,"bits":32},"scale":3.49e298,"unit":"\u00b0C"},)"
        R"({"key":"meet","name":"Meet","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":1,"order":"le"},"offset":1.75,)"
        R"("scale":2.220446049250313e-16,"unit":"km/h"}]})");
    const QString profile = useFile(bridge, bytes);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            for (const char* name : {"Huge", "Flat"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(name));
                ASSERT_FALSE(row.isEmpty()) << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << name;
            }
            // About 1.499e308 in deg C; deg F's 2.698e308 is inf
            const QVariantMap edge = rowNamed(rows, QStringLiteral("Edge"));
            ASSERT_FALSE(edge.isEmpty());
            const double lo = edge.value(QStringLiteral("rangeLow")).toDouble();
            const double hi =
                edge.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(lo, 0.0);
                EXPECT_GT(hi, 1.4e308);
                EXPECT_TRUE(std::isfinite(hi));
            } else {
                EXPECT_TRUE(std::isnan(lo)) << lo;
                EXPECT_TRUE(std::isnan(hi)) << hi;
            }
            // 1.75 and 1.75 + 2^-52 km/h are both 1.0873995864153345 mph
            const QVariantMap meet = rowNamed(rows, QStringLiteral("Meet"));
            ASSERT_FALSE(meet.isEmpty());
            const double mlo =
                meet.value(QStringLiteral("rangeLow")).toDouble();
            const double mhi =
                meet.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(mlo, 1.75);
                EXPECT_EQ(mhi, std::nextafter(1.75, 2.0));
            } else {
                EXPECT_TRUE(std::isnan(mlo)) << mlo;
                EXPECT_TRUE(std::isnan(mhi)) << mhi;
            }
        }
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* profileSignals gives the rows of the profile a URI selects, and []
 * for one with no rows to bind or none registered */
TEST(BridgeTest, ProfileSignalsGivesTheProfilesRows)
{
    SessionBridge bridge;
    const QVariantList mock =
        bridge.profileSignals(QStringLiteral("mock://demo"));
    ASSERT_EQ(mock.size(), static_cast<int>(ecuio_mock_signal_set()->count));
    const QVariantMap rpm = rowNamed(mock, QStringLiteral("RPM"));
    EXPECT_EQ(rpm.value(QStringLiteral("id")).toInt(), 1001);
    EXPECT_EQ(rpm.value(QStringLiteral("ref")).toString(),
              QStringLiteral("id:1001"));
    EXPECT_EQ(rpm.value(QStringLiteral("unit")).toString(),
              QStringLiteral("RPM"));
    EXPECT_FALSE(rpm.value(QStringLiteral("isText")).toBool());
    EXPECT_FALSE(rpm.value(QStringLiteral("isEnum")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("group")).toInt(),
              static_cast<int>(ECUIO_SIGNAL_GROUP_ENGINE));
    EXPECT_EQ(rpm.value(QStringLiteral("decimals")).toInt(), 0);
    EXPECT_NEAR(rpm.value(QStringLiteral("rangeHigh")).toDouble(), 65535 * 0.25,
                1e-9);

    for (const char* none :
         {"mock://demo?profile=uds-generic", "mock://demo?profile=no-such",
          "mock://demo?profile=raw", "replay://somewhere", "bogus://x",
          "mock://demo?profile="}) {
        EXPECT_TRUE(bridge.profileSignals(QString::fromLatin1(none)).isEmpty())
            << none;
    }
}

/* A replay's own signals.json is registered when it connects, so its
 * profile has no rows before and the file's after */
TEST(BridgeTest, ProfileSignalsOfAReplaysOwnFileFollowTheConnect)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QString folder = recordWithSignalFile(
        bridge, root.path(), signalFile(QStringLiteral("Recorded")));
    ASSERT_FALSE(folder.isEmpty());
    const QString own =
        QStringLiteral("rec.") +
        sha256Of(readFile(folder + QStringLiteral("/signals.json"))).left(16);
    EXPECT_EQ(removeEveryPlugin(bridge), 1);
    const QString uri = QStringLiteral("replay://") +
                        QDir::fromNativeSeparators(folder) +
                        QStringLiteral("?profile=") + own;
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty()) << "before the connect";
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(own))
        << bridge.currentUri().toStdString();
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)),
              QStringList{QStringLiteral("key:a")});
    EXPECT_EQ(refsOf(modelRows(bridge)), QStringList{QStringLiteral("key:a")});
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* profileOfUri is core's choice of profile: each URI core takes gives
 * the profile its session reports, and one core refuses for its scheme
 * or its query gives "" */
TEST(BridgeTest, ProfileOfUriIsTheProfileCoreChooses)
{
    const QString fixture =
        QStringLiteral("replay://%1/2026-10-25-engine-start")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    const QStringList taken = {
        QStringLiteral("mock://demo"),
        QStringLiteral("mock://demo?profile=obd2"),
        QStringLiteral("mock://demo?profile=raw&profile=mock"),
        QStringLiteral("mock://demo?profile=uds-generic&did=1E1C:u16"),
        fixture,
        fixture + QStringLiteral("?profile=obd2&speed=max"),
    };
    for (const QString& uri : taken) {
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
        EXPECT_EQ(SessionBridge::profileOfUri(uri).toStdString(),
                  std::string(info.profile))
            << uri.toStdString();
        ecuio_session_destroy(s);
    }

    const QString tooLong =
        QStringLiteral("mock://demo?profile=") +
        QString(ECUIO_PROFILE_NAME_MAX + 1, QLatin1Char('x'));
    const QStringList refused = {
        QStringLiteral("Mock://demo"),
        QStringLiteral("bogus://x"),
        QStringLiteral("mock://demo?profile"),
        QStringLiteral("mock://demo?profile="),
        QStringLiteral("mock://demo?&profile=obd2"),
        tooLong,
    };
    for (const QString& uri : refused) {
        ecuio_session_t* s = nullptr;
        EXPECT_NE(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_destroy(s);
        EXPECT_EQ(SessionBridge::profileOfUri(uri), QString())
            << uri.toStdString();
    }

    // The longest name core takes, and the transports this test can't open
    const QString longest(ECUIO_PROFILE_NAME_MAX, QLatin1Char('x'));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("mock://demo?profile=") + longest),
              longest);
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("elm327://COM9")),
              QStringLiteral("obd2"));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("elm327://COM9?profile=mock")),
              QStringLiteral("mock"));
#ifdef _WIN32
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QStringLiteral("obd2"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QString());
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QStringLiteral("raw"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QString());
#endif
}
