#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickStyle>

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
