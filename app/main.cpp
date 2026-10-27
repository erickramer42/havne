// File/layer: app/main.cpp | Qt6 Gui/Qml/Quick, app entry point, C++17
// Testing: manual smoke (the built app launched; QTTUNE_SETTINGS_DIR
//          through the run-qttune skill's launch); QmlTest drives the
//          same QML module and Bridge singleton without this file

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickStyle>
#include <QSettings>

#include "bridgesingleton.h"
#include "qttunebridge.h"
#include "ecuio/version.h"

// The QtTune module is a static library (app/CMakeLists.txt); this pulls
// in its plugin, which registers the module's types
Q_IMPORT_QML_PLUGIN(QtTunePlugin)

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName("QtTune");
    app.setApplicationName("QtTune");
    app.setApplicationVersion(QStringLiteral(ECUIO_VERSION_STRING));

    // QTTUNE_SETTINGS_DIR keeps this instance's preferences in an ini file
    // there instead of the user's (the registry on Windows), so a scripted
    // run neither reads nor changes them: the URI, the Record switch, zoom
    const QString settingsDir = qEnvironmentVariable("QTTUNE_SETTINGS_DIR");
    if (!settingsDir.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    }

    QQuickStyle::setStyle("Basic");

    // Declared before the engine, so it outlives everything QML holds.
    QtTuneBridge bridge;
    BridgeSingleton::instance = &bridge;

    QQmlApplicationEngine engine;
    engine.loadFromModule("QtTune", "Main");

    if (engine.rootObjects().isEmpty()) {
        qCritical("=== QML FAILED TO LOAD ===");
        return -1;
    }

    return app.exec();
}
