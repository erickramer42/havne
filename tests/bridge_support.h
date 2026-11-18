// File/layer: tests/bridge_support.h | test-only, GTest + QtCore, SessionBridge
// Testing: the helpers more than one test_bridge file uses (test_bridge)
//
// What the BridgeTest files share: waiting on the GUI thread, files and
// folders, the fake J2534 DLL's control page, and the signal store's scratch
// folders. Each is inline, so every file that includes this keeps its own
// copy and an unused one costs nothing.

#pragma once

#include <cmath>
#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "pid_support.h" // core/src: core's judgment, fed to the model

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "fake_control.h"   // the fake J2534 DLL's control page
#endif

/* The obd2 set's signals core judges unsupported against reports: what
 * ecuio_session_get_unsupported_signals hands the bridge */
inline std::vector<uint16_t> unsupportedIds(const ecuio_channel_support_t* reports,
                                     uint32_t n)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> ids(set->count);
    ids.resize(ecuio_unsupported_signals(*set, reports, n, ids.data(),
                                         static_cast<uint32_t>(ids.size())));
    return ids;
}

/* Spin the GUI event loop for `ms`, letting queued invocations drain.
 * Without this, worker-posted lambdas never execute. */
inline void spinLoop(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/* Poll-until-predicate with deadline, event-loop flavored. The mock's
 * 50ms cadence is a LOCAL expectation, not a CI-runner guarantee:
 * a starved macOS VM can deliver 2 frames in 200+ ms of wall clock.
 * Polling keeps the asserted CONTRACT (the
 * condition eventually holds on a healthy stream) and drops the
 * coincidence (any particular wall-clock duration). The event loop
 * must keep turning while polling: worker lambdas drain through it. */
template <typename Predicate>
bool spinUntil(Predicate pred, int timeout_ms, int step_ms = 50)
{
    QElapsedTimer clock;
    clock.start();
    while (!pred()) {
        if (clock.elapsed() >= timeout_ms) {
            return false;
        }
        spinLoop(step_ms);
    }
    return true;
}

/* Disconnect is async: connected drops at once and the teardown ends on
 * a worker thread. A test that connects again waits for it, as the UI
 * does (Connect stays disabled while disconnecting). */
inline bool disconnectAndWait(SessionBridge& bridge, int timeout_ms = 5000)
{
    bridge.disconnectSession();
    return spinUntil([&bridge]() { return !bridge.disconnecting(); }, timeout_ms, 10);
}

inline QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/* Lines without their line ends (the recorder writes text mode, CRLF
 * on Windows). */
inline QList<QByteArray> fileLines(const QString& path)
{
    QList<QByteArray> lines;
    for (QByteArray line : readFile(path).split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty()) lines << line;
    }
    return lines;
}

inline QStringList recordingFolders(const QString& root)
{
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

/* Restores an environment variable as it was, set or not. */
struct EnvRestore {
    const char* name;
    bool had;
    QByteArray value;
    explicit EnvRestore(const char* n)
        : name(n), had(qEnvironmentVariableIsSet(n)), value(qgetenv(n)) {}
    ~EnvRestore() { had ? qputenv(name, value) : qunsetenv(name); }
    EnvRestore(const EnvRestore&) = delete;
    EnvRestore& operator=(const EnvRestore&) = delete;
};

/* The fake ECU's diagnostics as the activity log words them (ADR-0037):
 * the fake J2534 DLL's and the mock's alike (core/src/mock_ecu.h) */
inline const char* const kFakeEcuDiagnosticsLine =
    "ECU 0x7E8 diagnostics: MIL on; stored P0301 P0420 U0100; pending P0171 "
    "C0035 B1001; permanent P0420; readiness 8 of 8 complete; freeze frame "
    "for P0301";

/* Live's plot: the rows with these ids ticked and every other numeric
 * row unticked, as a vehicle's choices set them */
inline void plotOnly(SignalListModel* model, const std::vector<uint16_t>& ids)
{
    QStringList plotted;
    QStringList unplotted;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r);
        if (model->data(i, SignalListModel::IsTextRole).toBool()) {
            continue;
        }
        const auto id = static_cast<uint16_t>(
            model->data(i, SignalListModel::IdRole).toUInt());
        const QString name =
            model->data(i, SignalListModel::NameRole).toString();
        (std::find(ids.begin(), ids.end(), id) != ids.end() ? plotted
                                                            : unplotted)
            << name;
    }
    model->setPlotChoices(plotted, unplotted);
}

/* The names of the model's rows with these ids, in the model's order */
inline QStringList namesOf(SignalListModel* model, const std::vector<uint16_t>& ids)
{
    QStringList names;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r);
        const auto id = static_cast<uint16_t>(
            model->data(i, SignalListModel::IdRole).toUInt());
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
            names << model->data(i, SignalListModel::NameRole).toString();
        }
    }
    return names;
}

/* The keys a recording of the bridge's connected session keeps the model's
 * rows with these ids under, in the model's order: core's keys for the
 * session's set, which has no signal file */
inline QStringList keysOf(SessionBridge& bridge, SignalListModel* model,
                          const std::vector<uint16_t>& ids)
{
    QStringList keys;
    const ecuio_signal_set_t* set =
        ecuio_session_signal_set(bridge.sessionForTesting());
    EXPECT_NE(set, nullptr) << "keysOf needs a connected session";
    if (set == nullptr) {
        return keys;
    }
    std::vector<ecuio_signal_key_t> all(set->count);
    uint32_t count = 0;
    EXPECT_EQ(
        ecuio_signal_set_keys(set, nullptr, all.data(), set->count, &count),
        ECUIO_OK);
    for (int r = 0; r < model->rowCount(); ++r) {
        const auto id = static_cast<uint16_t>(
            model->data(model->index(r), SignalListModel::IdRole).toUInt());
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (set->defs[i].id == id) {
                keys << QString::fromUtf8(all[i].text);
            }
        }
    }
    EXPECT_EQ(keys.size(), static_cast<qsizetype>(ids.size()));
    return keys;
}

/* Minimal two-signal set, built test-side. Plain assignments only:
   designated initializers are C++20, and MSVC accepts them where GCC
   rejects them. */
inline void buildTestSignalSet(ecuio_signal_def_t (&defs)[2], ecuio_signal_set_t& set)
{
    ecuio_signal_def_t& rpm = defs[0];
    rpm = {};
    rpm.id = 1001;
    strncpy(rpm.name, "Engine_RPM", sizeof(rpm.name) - 1);
    rpm.kind = ECUIO_VALUE_UINT;
    strncpy(rpm.unit, "RPM", sizeof(rpm.unit) - 1);

    ecuio_signal_def_t& temp = defs[1];
    temp = {};
    temp.id = 2001;
    strncpy(temp.name, "Coolant_Temp", sizeof(temp.name) - 1);
    temp.kind = ECUIO_VALUE_INT;
    strncpy(temp.unit, "degC", sizeof(temp.unit) - 1);

    set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
}

#ifdef _WIN32

/* The fake's control page; created before any session opens, so a
 * surrogate child takes its name at spawn. */
inline fake_control::Page* fakePage()
{
    return fake_control::create_for_this_process();
}

/* Sets one of the fake's knobs for one test, on its page (which the
 * fake in the child reads on every call), and clears it on exit,
 * so a failing ASSERT can't leak it into the next case. */
class FakeEnvArm
{
public:
    FakeEnvArm(const char* name, const char* value) : m_name(name)
    {
        EXPECT_TRUE(fake_control::set(fakePage(), name, value)) << name;
    }
    ~FakeEnvArm() { fake_control::set(fakePage(), m_name, nullptr); }
    FakeEnvArm(const FakeEnvArm&) = delete;
    FakeEnvArm& operator=(const FakeEnvArm&) = delete;

private:
    const char* m_name;
};

/* What the fake publishes on its page, reset for one test. */
struct FakeHooks
{
    bool load()
    {
        fake_control::Page* page = fakePage();
        if (page == nullptr) return false;
        fake_control::reset(page);
        return true;
    }
    ~FakeHooks()
    {
        if (fakePage() != nullptr) fake_control::reset(fakePage());
    }
    unsigned long disconnect_count() const
    {
        return fake_control::disconnects(fakePage());
    }
};

/* A passive raw session: no requests, so the vendor calls under test
 * are the open, the engine ECU's filter and the teardown. */
inline QString fakeUri()
{
    return QStringLiteral("j2534://%1?profile=raw")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
}

#endif  // _WIN32

/* The obd2 set's row for signal id, or -1 */
inline int obd2RowOf(uint16_t id)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

// HAVNE_SETTINGS_DIR at a scratch folder for one case, put back after
class SettingsDirScope
{
public:
    SettingsDirScope() : m_before(qgetenv("HAVNE_SETTINGS_DIR"))
    {
        qputenv("HAVNE_SETTINGS_DIR", m_dir.path().toUtf8());
    }
    ~SettingsDirScope() { qputenv("HAVNE_SETTINGS_DIR", m_before); }
    SettingsDirScope(const SettingsDirScope&) = delete;
    SettingsDirScope& operator=(const SettingsDirScope&) = delete;

    bool valid() const { return m_dir.isValid(); }
    QString store() const { return m_dir.path() + QStringLiteral("/signals"); }

private:
    QTemporaryDir m_dir;
    QByteArray m_before;
};

// A signal file of one DID on the engine ECU, named name
inline QByteArray signalFile(const QString& name,
                      const QString& did = QStringLiteral("D100"))
{
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},)"
               R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
               R"({"key":"a","name":"A","read":{"service":"22","number":"%2"},)"
               R"("field":{"byte":0,"bits":8}}]})")
        .arg(name, did)
        .toUtf8();
}

inline QString sha256Of(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

inline QUrl writeScratch(const QString& folder, const QString& name,
                  const QByteArray& bytes)
{
    QDir().mkpath(folder);
    QFile f(folder + QLatin1Char('/') + name);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    return QUrl::fromLocalFile(f.fileName());
}

inline bool registered(const char* profile)
{
    const ecuio_signal_set_t* set = nullptr;
    return ecuio_profile_lookup(profile, &set) == ECUIO_OK;
}

inline QStringList storeFiles(const QString& store)
{
    return QDir(store).entryList(QDir::Files, QDir::Name);
}

/* The value Live shows for the signal named name, NaN with no row or no
 * value yet */
inline double liveValue(SessionBridge& bridge, const QString& name)
{
    SignalListModel* model = bridge.signalModel();
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r, 0);
        if (model->data(i, SignalListModel::NameRole).toString() == name) {
            // ValueRole reads 0 before any value: HasValueRole says whether
            // one came
            if (!model->data(i, SignalListModel::HasValueRole).toBool()) {
                return std::nan("");
            }
            return model->data(i, SignalListModel::ValueRole).toDouble();
        }
    }
    return std::nan("");
}

/* A mock session decoded by `bytes` (a file named "Recorded") kept as a
 * plugin of the user's own, profile plugin.recorded.base, recorded under
 * root until signal A has a value; the recording's folder, whose
 * signals.json is the variant's written file */
inline QString recordWithSignalFile(SessionBridge& bridge, const QString& root,
                             const QByteArray& bytes)
{
    const QVariantMap kept = bridge.keepSignalText(
        QString::fromUtf8(bytes), QString(), QString(), QString());
    EXPECT_EQ(kept.value(QStringLiteral("profiles")).toStringList(),
              QStringList{QStringLiteral("plugin.recorded.base")})
        << kept.value(QStringLiteral("words")).toString().toStdString();
    bridge.setRecordingsRootForTesting(root);
    bridge.setRecording(true);
    EXPECT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://rec?profile=plugin.recorded.base"))
                    .isEmpty());
    EXPECT_TRUE(spinUntil(
        [&bridge]() {
            return !std::isnan(liveValue(bridge, QStringLiteral("A")));
        },
        5000));
    EXPECT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);
    const QStringList folders = recordingFolders(root);
    EXPECT_EQ(folders.size(), 1);
    return folders.isEmpty() ? QString()
                             : root + QLatin1Char('/') + folders.first();
}

/* Every plugin the store keeps removed, as Remove does: how many went */
inline int removeEveryPlugin(SessionBridge& bridge)
{
    int gone = 0;
    for (const QVariant& p : bridge.plugins()) {
        gone += bridge
                        .removePlugin(
                            p.toMap().value(QStringLiteral("sha256")).toString())
                        .value(QStringLiteral("ok"))
                        .toBool()
                    ? 1
                    : 0;
    }
    return gone;
}

/* The app's Replay of folder, at max speed: connected, and signal A
 * given a value, or the connect's error */
inline QString replayFolder(SessionBridge& bridge, const QString& folder,
                     bool expectValue = true)
{
    const QString uri =
        SessionBridge::replayUriFor(
            folder, QStringLiteral("mock://rec?profile=plugin.recorded.base")) +
        QStringLiteral("&speed=max");
    const QString err = bridge.connectSession(uri);
    if (err.isEmpty() && expectValue) {
        EXPECT_TRUE(spinUntil(
            [&bridge]() {
                return !std::isnan(liveValue(bridge, QStringLiteral("A")));
            },
            5000))
            << "signal A decoded in the replay";
    }
    return err;
}

// Three conditions on the mock's set: an alert ON from the first frame,
// a danger alert ON with it, and a mark-only one that stays OFF
inline QByteArray mockConditions()
{
    return QByteArray(
        R"({"havne_conditions":1,"name":"Bridge test","conditions":[)"
        R"({"key":"stopped","name":"Engine stopped",)"
        R"("when":{"signal":["id:1001"],"unit":"RPM","below":300},)"
        R"("alert":{"level":"warning","banner":true}},)"
        R"({"key":"cold","name":"Coolant cold",)"
        R"("when":{"signal":["id:2001"],"unit":")"
        "\xC2\xB0"
        R"(C","above":-100},)"
        R"("alert":{"level":"danger","sound":true}},)"
        R"({"key":"fast","name":"Moving",)"
        R"("when":{"signal":["id:3001"],"unit":"km/h","above":250},)"
        R"("mark":true}]})");
}
