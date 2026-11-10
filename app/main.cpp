// File/layer: app/main.cpp | Qt6 Gui/Qml/Quick, app entry point, C++17
// Testing: Manual smoke; BENCH (the phone run sheet); CI android-build
//          (compiles it)

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickStyle>
#include <QSettings>
#include <QStyleHints>
#include <QWindow>

#ifdef Q_OS_ANDROID
#include "androidsession.h"
#endif
#ifdef APP_BLUETOOTH
#include "btrelay.h"
#endif
#include "adapterrelay.h"
#include "bluetoothdevices.h"
#include "bridgesingleton.h"
#include "carryover.h"
#include "recordingexport.h"
#include "sessionbridge.h"
#if defined(Q_OS_ANDROID) || defined(Q_OS_WIN)
#include "windowtheme.h"
#endif
#include "ecuio/version.h"

// The App module is a static library (app/CMakeLists.txt); this pulls
// in its plugin, which registers the module's types
Q_IMPORT_QML_PLUGIN(AppPlugin)

#ifdef Q_OS_ANDROID
// No drawing while the app isn't active. With the event loops kept running
// in the background (ADR-0029 section 1), Qt 6.12's Android plugin, handling the
// screen going off, destroys the window's surface and then still delivers
// an expose that draws; the draw waits for Android to make a new surface,
// which it doesn't with the screen off, and the thread that draws stops
// until the screen comes back (docs/bench-sessions/2026-10-28-android-10.md).
// With the basic render loop every draw starts from one of these two
// events on the GUI thread; Qt sends a new expose when the app is active
// again, so the page redraws then
class NoDrawWhileInactive : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if ((event->type() == QEvent::Expose ||
             event->type() == QEvent::UpdateRequest) &&
            QGuiApplication::applicationState() != Qt::ApplicationActive) {
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
};
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_ANDROID
    // Qt Quick's basic render loop: every draw on the GUI thread, started
    // by an event NoDrawWhileInactive sees. The threaded loop's render
    // thread starts frames of its own, and one that started as the screen
    // went off waited there, the GUI thread waiting on it
    if (!qEnvironmentVariableIsSet("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", "basic");
    }
#endif
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Havne");
    app.setApplicationName("Havne");
    app.setApplicationVersion(QStringLiteral(ECUIO_VERSION_STRING));

    // The UI is dark only. Saying so lets the platform match it: a dark
    // title bar on Windows, and Android's system bars drawn for a dark
    // app rather than over a white strip
    // (docs/bench-sessions/2026-10-28-android-2.md)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);

    // The text face is Barlow (bundled; Theme loads the same files for its
    // family names). It is the application's font so every Text, which
    // takes no family from a window, uses it too; if the Regular file
    // fails to load the platform's font stays, as in Theme.fontFamily
    bool barlow = false;
    for (const char *file : {"Barlow-Regular", "Barlow-Medium", "Barlow-SemiBold", "Barlow-Bold"}) {
        const QString path = QStringLiteral(":/qt/qml/App/fonts/%1.ttf").arg(QLatin1String(file));
        const bool loaded = QFontDatabase::addApplicationFont(path) >= 0;
        if (QLatin1String(file) == QLatin1String("Barlow-Regular")) {
            barlow = loaded;
        }
    }
    if (barlow) {
        QFont font = QGuiApplication::font();
        font.setFamily(QStringLiteral("Barlow"));
        QGuiApplication::setFont(font);
    }

    // HAVNE_SETTINGS_DIR keeps this instance's preferences in an ini file
    // there instead of the user's (the registry on Windows), so a scripted
    // run neither reads nor changes them: the URI, the Record switch, zoom
    const QString settingsDir = qEnvironmentVariable("HAVNE_SETTINGS_DIR");
    if (!settingsDir.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    }
    // The earlier product name's preferences, copied once into an empty
    // store before anything reads it (ADR-0001's appendix of 2026-10-28)
    const int carriedOver =
        carryOverSettings(earlierProductName(), earlierProductName(),
                          app.organizationName(), app.applicationName());

    QQuickStyle::setStyle("Basic");

    // An export killed mid-way leaves its temporary copy behind
    RecordingExport::removeStaleCopies(QDir::tempPath(),
                                       QDateTime::currentDateTimeUtc());

    // Declared before the bridge, so it outlives the bridge's sessions:
    // the Bluetooth links' streams unregister when it goes (ADR-0028)
    BluetoothDevices bluetooth;
    BluetoothDevices::instance = &bluetooth;

    // Declared before the engine, so it outlives everything QML holds.
    SessionBridge bridge;
    BridgeSingleton::instance = &bridge;
    bluetooth.attachTo(&bridge);
    // A paired adapter relayed to a computer over USB (ADR-0047): the
    // phone's build alone has a factory, so elsewhere it can't start
    AdapterRelay relay;
    AdapterRelay::instance = &relay;
#ifdef APP_BLUETOOTH
    relay.setFactory([](const QString& address) {
        return std::unique_ptr<RelayAdapter>(new BtRelayAdapter(address));
    });
#endif
#ifdef Q_OS_ANDROID
    // The foreground service while an adapter session is up or a relay
    // runs (ADR-0029 section 2)
    AndroidSession androidSession(&bridge, &relay);
#endif

    QQmlApplicationEngine engine;
    engine.loadFromModule("App", "Main");

    if (engine.rootObjects().isEmpty()) {
        qCritical("=== QML FAILED TO LOAD ===");
        return -1;
    }
    if (carriedOver > 0) {
        bridge.logEvent(
            QStringLiteral("Preferences carried over from %1: %2 settings")
                .arg(earlierProductName())
                .arg(carriedOver));
    }
#if defined(Q_OS_ANDROID) || defined(Q_OS_WIN)
    if (auto *window =
            qobject_cast<QWindow *>(engine.rootObjects().constFirst())) {
        if (auto* theme = engine.singletonInstance<QObject*>("App", "Theme")) {
            new WindowTheme(theme, window);
        }
#ifdef Q_OS_ANDROID
        window->installEventFilter(new NoDrawWhileInactive(window));
#endif
    }
#endif

    return app.exec();
}
