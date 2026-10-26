// File/layer: tests/test_qml.cpp | Qt Quick Test, QML tier
// Testing: runs tests/qml/tst_*.qml against the app's QtTune module
//          and a real bridge, the one its Bridge singleton serves.
//          ctest runs each file in its own process (QmlTest.<name>).

#include <QCoreApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickStyle>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QtQuickTest>

#include <memory>
#include <utility>

#include "bridgesingleton.h"
#include "qttunebridge.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

Q_IMPORT_QML_PLUGIN(QtTunePlugin)

namespace {

// Test-only helpers, as the QtTuneTest module's Harness singleton
class Harness : public QObject
{
    Q_OBJECT
    // The fake vendor DLL for a j2534:// URI; empty where there is none
    // (off Windows)
    Q_PROPERTY(QString fakeDllPath READ fakeDllPath CONSTANT)

public:
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
        // scratch ini file, never the user's QtTune settings
        QCoreApplication::setOrganizationName(QStringLiteral("QtTune"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
        QQuickStyle::setStyle(QStringLiteral("Basic"));   // as main() sets it

        m_bridge = std::make_unique<QtTuneBridge>();
        BridgeSingleton::instance = m_bridge.get();
        qmlRegisterSingletonInstance("QtTuneTest", 1, 0, "Harness", &m_harness);
    }

    void cleanupTestCase()
    {
        m_harness.clearFakeKnobs();
        m_bridge.reset();   // ends a session a test left up, joining its threads
        BridgeSingleton::instance = nullptr;
    }

private:
    QTemporaryDir m_settingsDir;
    Harness m_harness;
    std::unique_ptr<QtTuneBridge> m_bridge;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(qttune, Setup)

#include "test_qml.moc"
