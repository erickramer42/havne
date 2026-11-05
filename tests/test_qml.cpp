// File/layer: tests/test_qml.cpp | Qt Quick Test, QML tier
// Testing: runs tests/qml/tst_*.qml against the app's App module
//          and a real bridge, the one its Bridge singleton serves,
//          and a relay whose adapter opens at once (Relay); clicks
//          with stamps of their own (Harness.clickStamped).
//          ctest runs each file in its own process (QmlTest.<name>).

#include <QCoreApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QtQuickTest>

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "adapterrelay.h"
#include "bluetoothdevices.h"
#include "bridgesingleton.h"
#include "sessionbridge.h"
#include "ecuio/profile.h"
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

public:
    QString replayFixtures() const { return QStringLiteral(ECUIO_REPLAY_FIXTURES); }
    QString recordingFixtures() const
    {
        return QStringLiteral(ECUIO_RECORDING_FIXTURES);
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
    // as a profile, and returns its name ("" if refused): the Dashboard's
    // gauges must find their signals by ID alone. The registry keeps the
    // set's pointer, so it lives as long as the process.
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

    Q_INVOKABLE void clearFakeKnobs()
    {
#ifdef _WIN32
        for (const QString& name : std::as_const(m_armed)) {
            SetEnvironmentVariableA(name.toUtf8().constData(), nullptr);
        }
#endif
        m_armed.clear();
    }

private:
    QStringList m_armed;
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
        // Main.qml stores its zoom with Settings; here that goes to a
        // scratch ini file, never the user's Havne settings
        QCoreApplication::setOrganizationName(QStringLiteral("Havne"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
        QQuickStyle::setStyle(QStringLiteral("Basic"));   // as main() sets it

        m_bridge = std::make_unique<SessionBridge>();
        // Settings lists the recordings: here an empty scratch folder,
        // never the user's Documents
        m_bridge->setRecordingsRootForTesting(m_recordingsDir.path());
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
