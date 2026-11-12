// File/layer: tests/test_qml.cpp | Qt Quick Test, QML tier
// Testing: QML: QmlTest.<name>, one per tests/qml/tst_*.qml (test_qml)

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QStyleHints>
#include <QKeyEvent>
#include <QLocale>
#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QWindow>
#include <QtQuickTest>

#include "packed_plugin.h"
#include "pluginstore.h"
#include "recordingexport.h"
#include "recordingimport.h"
#include "recordingview.h"
#include "signalstore.h"
#include "windowtheme.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "adapterrelay.h"
#include "bluetoothdevices.h"
#include "bridgesingleton.h"
#include "sessionbridge.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

Q_IMPORT_QML_PLUGIN(AppPlugin)

QT_BEGIN_NAMESPACE
// QtGui's hook for a test's mouse event with a timestamp of its own, the
// one QTest's clicks go through (qtestmouse.h)
Q_GUI_EXPORT void qt_handleMouseEvent(QWindow* window, const QPointF& local,
                                      const QPointF& global,
                                      Qt::MouseButtons state,
                                      Qt::MouseButton button, QEvent::Type type,
                                      Qt::KeyboardModifiers mods,
                                      int timestamp);
QT_END_NAMESPACE

namespace {

// Test-only helpers, as the AppTest module's Harness singleton
class Harness : public QObject
{
    Q_OBJECT
    // The fake vendor DLL for a j2534:// URI; empty where there is none
    // (off Windows)
    Q_PROPERTY(QString fakeDllPath READ fakeDllPath CONSTANT)
    // The directory of the scrubbed recordings replay:// plays
    Q_PROPERTY(QString replayFixtures READ replayFixtures CONSTANT)
    // The directory of the recordings the Recordings page opens (ADR-0049)
    Q_PROPERTY(QString recordingFixtures READ recordingFixtures CONSTANT)
    // The synthetic OBDb set (tests/fixtures/obdb), as a picker's URL
    Q_PROPERTY(QUrl obdbFixture READ obdbFixture CONSTANT)

public:
    QString replayFixtures() const { return QStringLiteral(ECUIO_REPLAY_FIXTURES); }
    QString recordingFixtures() const
    {
        return QStringLiteral(ECUIO_RECORDING_FIXTURES);
    }

    QUrl obdbFixture() const
    {
        return QUrl::fromLocalFile(QStringLiteral(ECUIO_OBDB_FIXTURE));
    }

    QString fakeDllPath() const
    {
#ifdef FAKE_J2534_DLL_PATH
        return QStringLiteral(FAKE_J2534_DLL_PATH);
#else
        return {};
#endif
    }

    // Arms one of the fake's knobs (ECUIO_FAKE_*) in the OS environment,
    // which the surrogate child inherits at the next connect's spawn.
    // clearFakeKnobs() removes every knob armed.
    Q_INVOKABLE void armFakeKnob(const QString& name, const QString& value)
    {
#ifdef _WIN32
        SetEnvironmentVariableA(name.toUtf8().constData(), value.toUtf8().constData());
        if (!m_armed.contains(name)) m_armed << name;
#else
        Q_UNUSED(name)
        Q_UNUSED(value)
#endif
    }

    // Registers the mock's set with every def renamed ("Renamed RPM", ...)
    // as a profile, and returns its name ("" if refused): a set registered
    // without a signal file, whose rows have no ADR-0057 refs, so no
    // Dashboard gauge binds them though they carry the mock's IDs. The
    // registry keeps the set's pointer, so it lives as long as the
    // process.
    Q_INVOKABLE QString registerRenamedMock()
    {
        static std::vector<ecuio_signal_def_t> defs;
        static ecuio_signal_set_t set{};
        if (defs.empty()) {
            const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
            defs.assign(mock->defs, mock->defs + mock->count);
            for (ecuio_signal_def_t& d : defs) {
                /* Room for the whole text, then cut to the field */
                char name[sizeof d.name + 8];
                std::snprintf(name, sizeof name, "Renamed %s", d.name);
                std::memcpy(d.name, name, sizeof d.name - 1);
                d.name[sizeof d.name - 1] = '\0';
            }
            set = *mock;
            set.defs = defs.data();
        }
        return ecuio_profile_register("renamed-mock", &set, nullptr) == ECUIO_OK
                   ? QStringLiteral("renamed-mock")
                   : QString();
    }

    // A left click on item at (x, y), its press stamped stampMs and its
    // release a millisecond later, delivered at once. Qt Quick Test's own
    // clicks add the double-click interval to their stamps after each
    // release; with these, two clicks stamped close can arrive a slow
    // frame apart, as a phone's taps do behind one
    Q_INVOKABLE void clickStamped(QQuickItem* item, qreal x, qreal y,
                                  int stampMs)
    {
        QQuickWindow* window = item ? item->window() : nullptr;
        if (!window)
            return;
        const QPointF at = item->mapToScene(QPointF(x, y));
        const QPointF global = window->mapToGlobal(at);
        qt_handleMouseEvent(window, at, global, Qt::LeftButton, Qt::LeftButton,
                            QEvent::MouseButtonPress, Qt::NoModifier, stampMs);
        qt_handleMouseEvent(window, at, global, Qt::NoButton, Qt::LeftButton,
                            QEvent::MouseButtonRelease, Qt::NoModifier,
                            stampMs + 1);
    }

    // The system's Back key, into a window as Android sends it (a keyClick
    // goes to the test's window); true when something in QML took it
    Q_INVOKABLE bool pressBack(QObject* window, bool autoRepeat = false)
    {
        auto* target = qobject_cast<QWindow*>(window);
        if (!target)
            return false;
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier,
                        QString(), autoRepeat);
        QCoreApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Back, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &release);
        return press.isAccepted();
    }

    // An Export panel's exporter (its "exporter" child) holds each worker it
    // starts at core's first ask up to ms, as a long export would; 0 ends it
    Q_INVOKABLE void holdExports(QObject* exporter, int ms)
    {
        if (auto* e = qobject_cast<RecordingExport*>(exporter)) {
            e->setHoldForTesting(ms > 0
                                     ? RecordingExport::HoldForTesting::FirstAsk
                                     : RecordingExport::HoldForTesting::None,
                                 ms);
        }
    }
    // The workers that exporter has held so far; -1 for anything else
    Q_INVOKABLE int heldExports(QObject* exporter) const
    {
        const auto* e = qobject_cast<RecordingExport*>(exporter);
        return e != nullptr ? e->heldForTesting() : -1;
    }

    // The importer (Bridge.recordingImport) can't start its worker thread
    // while on, as a system out of threads
    Q_INVOKABLE void failImportThreads(QObject* importer, bool on)
    {
        if (auto* i = qobject_cast<RecordingImport*>(importer)) {
            i->setFailThreadForTesting(on);
        }
    }

    // A page's RecordingView (its "recordingView" or "compareView" child)
    // can't start its asks' task while on: every ask is refused with 0.
    // False for anything else
    Q_INVOKABLE bool refuseViewAsks(QObject* view, bool on)
    {
        auto* v = qobject_cast<RecordingView*>(view);
        if (v != nullptr) {
            v->setFailThreadForTesting(on);
        }
        return v != nullptr;
    }
    // The view's asks' task runs out of memory at each column means ask
    // while on. False for anything else
    Q_INVOKABLE bool loseViewMeans(QObject* view, bool on)
    {
        auto* v = qobject_cast<RecordingView*>(view);
        if (v != nullptr) {
            v->setLoseMeansForTesting(on);
        }
        return v != nullptr;
    }

    // The importer's next imports wait this long, or until a cancel,
    // before core's call (0: no wait)
    Q_INVOKABLE void holdImports(QObject* importer, int ms)
    {
        if (auto* i = qobject_cast<RecordingImport*>(importer)) {
            i->setHoldForTesting(ms);
        }
    }

    // The color scheme (Qt::ColorScheme as an int: 1 Light, 2 Dark) a
    // WindowTheme sets on a real window that follows theme, as main() makes
    // one; the platform's own scheme is put back after
    Q_INVOKABLE int colorSchemeFollowing(QObject* theme)
    {
        QQuickWindow window;
        const WindowTheme follower(theme, &window);
        const int scheme =
            static_cast<int>(QGuiApplication::styleHints()->colorScheme());
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
        return scheme;
    }

    // The running recording's next write fails, as on a full disk
    Q_INVOKABLE void failRecordingWrites()
    {
        ecuio_recorder_test_fail_writes(
            BridgeSingleton::instance->recorderForTesting());
    }

    // A file of text in a scratch folder of the harness's own, as a URL a
    // file dialog would give; an empty URL when it can't be written
    Q_INVOKABLE QUrl writeScratchFile(const QString& name, const QString& text)
    {
        QFile f(m_scratch.path() + QLatin1Char('/') + name);
        if (!m_scratch.isValid() || !f.open(QIODevice::WriteOnly)) {
            return {};
        }
        f.write(text.toUtf8());
        return QUrl::fromLocalFile(f.fileName());
    }

    // A recording folder of the harness's own holding frames.csv alone (and
    // the events.log that names its format): rows frames, the nth at
    // (n + 1) * stepUs microseconds, the folder's path, "" when it can't be
    // written
    Q_INVOKABLE QString writeFramesRecording(const QString& name, int rows,
                                             int stepUs)
    {
        const QString folder = m_scratch.path() + QLatin1Char('/') + name;
        if (!m_scratch.isValid() || !QDir().mkpath(folder)) {
            return {};
        }
        QFile events(folder + QStringLiteral("/events.log"));
        if (!events.open(QIODevice::WriteOnly)) {
            return {};
        }
        events.write(
            "ecuio session recording, format 2 (ADR-0049)\n"
            "started  2026-11-07 10:00:00.000 (local time)\n"
            "utc  2026-11-07T16:00:00.000Z\n"
            "core  0.5.0\n"
            "uri  elm327://COM3?profile=obd2\n"
            "profile  obd2\n"
            "frame flags  complete\n"
            "caller  havne-cli 0.5.0\n"
            "\n"
            "10:00:01.000  +1.000  end  havne-cli: seconds: 3 frames, 0 "
            "values, 0 records dropped, 0 notes dropped\n");
        QFile frames(folder + QStringLiteral("/frames.csv"));
        if (!frames.open(QIODevice::WriteOnly)) {
            return {};
        }
        QByteArray csv = "host_us,timestamp_us,id,flags,data\n";
        for (int n = 0; n < rows; ++n) {
            const qint64 host = static_cast<qint64>(n + 1) * stepUs;
            csv += QByteArray::number(host) + ',' +
                   QByteArray::number(host + 7) +
                   ",0x7E8,0x0,04 41 0C 0C 90 00 00 00\n";
        }
        frames.write(csv);
        return folder;
    }

    // A file's text, as a Save wrote it: "" when it can't be read
    Q_INVOKABLE QString readScratchFile(const QUrl& url) const
    {
        QFile f(url.toLocalFile());
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                           : QString();
    }

    // A made-up logger's CSV (tests/fixtures/import/csv/<logger>) imported
    // by its column map into the recordings folder the page lists, as
    // havne-cli import csv makes one (ADR-0054's appendix "the CSV import
    // as built"), named name: the folder, "" when the import is refused
    Q_INVOKABLE QString importCsvFixture(const QString& logger,
                                         const QString& name)
    {
        const QString base =
            QStringLiteral(ECUIO_IMPORT_FIXTURES "/csv/") + logger;
        QFile mapFile(base + QStringLiteral("/map.json"));
        if (!mapFile.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QByteArray map = mapFile.readAll();
        const QString folder = m_recordingsRoot + QLatin1Char('/') + name;
        const QByteArray in = (base + QStringLiteral("/log.csv")).toUtf8();
        const QByteArray out = folder.toUtf8();
        ecuio_import_options_t o{};
        o.format = ECUIO_IMPORT_CSV;
        o.in_path_utf8 = in.constData();
        o.out_folder_utf8 = out.constData();
        o.column_map_utf8 = map.constData();
        ecuio_import_report_t r{};
        return ecuio_import(&o, &r) == ECUIO_OK ? folder : QString();
    }

    // The signal store's copies (SignalStore::root, the test's settings
    // folder), by file name
    Q_INVOKABLE QStringList storedSignalFiles() const
    {
        return QDir(SignalStore::root()).entryList(QDir::Files, QDir::Name);
    }

    // The repository's example plugin, packed as a phone imports it
    // (tests/packed_plugin.h), written as a scratch file named name: its
    // URL, empty when it can't be; version replaces its own when given
    Q_INVOKABLE QUrl writeExamplePlugin(const QString& name,
                                        const QString& version = QString())
    {
        const QByteArray bytes = packed_plugin::example(version.toUtf8());
        return bytes.isEmpty()
                   ? QUrl()
                   : writeScratchFile(name, QString::fromUtf8(bytes));
    }

    // The plugin store's copies, by file name
    Q_INVOKABLE QStringList storedPlugins() const
    {
        return QDir(PluginStore::root()).entryList(QDir::Files, QDir::Name);
    }

    Q_INVOKABLE void clearFakeKnobs()
    {
#ifdef _WIN32
        for (const QString& name : std::as_const(m_armed)) {
            SetEnvironmentVariableA(name.toUtf8().constData(), nullptr);
        }
#endif
        m_armed.clear();
    }

    // The folder the bridge lists recordings from (Setup's scratch one)
    void setRecordingsRoot(const QString& root) { m_recordingsRoot = root; }

private:
    QStringList m_armed;
    QTemporaryDir m_scratch;
    QString m_recordingsRoot;
};

// The relay's adapter as main() makes it on the phone, opening at once:
// with it the relay is available, and Settings shows its box
class OpenAdapter : public RelayAdapter
{
public:
    void open() override { opened(); }
    void write(const QByteArray&) override {}
    void close() override {}
};

class Setup : public QObject
{
    Q_OBJECT

public:
    Setup()
    {
        // Constructed before the test run starts. With no console (ctest,
        // a pipe), Qt on Windows sends warnings and the test log to the
        // debugger instead, and a failure would print nothing.
        if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING")) {
            qputenv("QT_FORCE_STDERR_LOGGING", "1");
        }
    }

public slots:
    void applicationAvailable()
    {
        // HAVNE_TEST_LOCALE (for example "C") runs a file under that locale, as CI
        // does; the readouts group digits with the default locale
        if (qEnvironmentVariableIsSet("HAVNE_TEST_LOCALE")) {
            QLocale::setDefault(QLocale(qEnvironmentVariable("HAVNE_TEST_LOCALE")));
        }
        // Main.qml stores its zoom with Settings; here that goes to a
        // scratch ini file, never the user's Havne settings
        QCoreApplication::setOrganizationName(QStringLiteral("Havne"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
        // The vehicles' signal files there too (SignalStore::root), as
        // HAVNE_SETTINGS_DIR puts a second instance's
        qputenv("HAVNE_SETTINGS_DIR", m_settingsDir.path().toUtf8());
        QQuickStyle::setStyle(QStringLiteral("Basic"));   // as main() sets it

        m_bridge = std::make_unique<SessionBridge>();
        // Settings lists the recordings: here an empty scratch folder,
        // never the user's Documents
        m_bridge->setRecordingsRootForTesting(m_recordingsDir.path());
        m_harness.setRecordingsRoot(m_recordingsDir.path());
        BridgeSingleton::instance = m_bridge.get();
        // As main() wires it: a bt: URI is refused before core is asked
        m_bluetooth = std::make_unique<BluetoothDevices>();
        BluetoothDevices::instance = m_bluetooth.get();
        m_bluetooth->attachTo(m_bridge.get());
        m_relay = std::make_unique<AdapterRelay>();
        m_relay->setFactory([](const QString&) {
            return std::unique_ptr<RelayAdapter>(new OpenAdapter);
        });
        AdapterRelay::instance = m_relay.get();
        qmlRegisterSingletonInstance("AppTest", 1, 0, "Harness", &m_harness);
    }

    void cleanupTestCase()
    {
        m_harness.clearFakeKnobs();
        m_bridge.reset();   // ends a session a test left up, joining its threads
        BridgeSingleton::instance = nullptr;
        m_bluetooth.reset(); // after the bridge: its sessions are closed
        BluetoothDevices::instance = nullptr;
        m_relay.reset();
        AdapterRelay::instance = nullptr;
    }

private:
    QTemporaryDir m_settingsDir;
    QTemporaryDir m_recordingsDir;
    Harness m_harness;
    std::unique_ptr<BluetoothDevices> m_bluetooth;
    std::unique_ptr<SessionBridge> m_bridge;
    std::unique_ptr<AdapterRelay> m_relay;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(app, Setup)

#include "test_qml.moc"
