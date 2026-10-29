// File/layer: app/main.cpp | Qt6 Gui/Qml/Quick, app entry point, C++17
// Testing: manual smoke (the built app launched; QTTUNE_SETTINGS_DIR
//          through the run-qttune skill's launch; the title bar in the
//          header's color on Windows 11);
//          BENCH, the phone run sheet (the system bars' color, Android's
//          window behind Qt's dark); CI's android-build compiles the
//          Android part;
//          QmlTest drives the same QML module and Bridge singleton
//          without this file

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickStyle>
#include <QSettings>
#include <QStyleHints>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif
#ifdef Q_OS_WIN
#include <QWindow>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#ifdef Q_OS_ANDROID
#include "androidsession.h"
#endif
#include "bluetoothdevices.h"
#include "bridgesingleton.h"
#include "qttunebridge.h"
#include "ecuio/version.h"

// The QtTune module is a static library (app/CMakeLists.txt); this pulls
// in its plugin, which registers the module's types
Q_IMPORT_QML_PLUGIN(QtTunePlugin)

#ifdef Q_OS_ANDROID
// Android's own window, behind Qt's, follows the phone's day/night theme
// (QtActivity's DayNight): light on a light phone. After a rotation, the
// screen Qt's window didn't reach showed white, and three-button
// navigation sat on a light scrim (docs/bench-sessions/2026-10-28-android-3.md).
// Dark like the app instead, no scrim, the navigation buttons drawn light.
static void darkenAndroidWindow()
{
    using QNativeInterface::QAndroidApplication;
    QAndroidApplication::runOnAndroidMainThread([]() {
        const QJniObject activity = QAndroidApplication::context();
        const QJniObject window =
            activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
        if (!window.isValid())
            return;
        const QJniObject background("android/graphics/drawable/ColorDrawable",
                                    "(I)V",
                                    static_cast<jint>(0xFF14141Cu)); // Theme.bg
        window.callMethod<void>("setBackgroundDrawable",
                                "(Landroid/graphics/drawable/Drawable;)V",
                                background.object());
        if (QAndroidApplication::sdkVersion() >= 29) {
            window.callMethod<void>("setNavigationBarContrastEnforced", "(Z)V",
                                    static_cast<jboolean>(false));
        }
        if (QAndroidApplication::sdkVersion() >= 30) {
            const QJniObject insets = window.callObjectMethod(
                "getInsetsController",
                "()Landroid/view/WindowInsetsController;");
            if (insets.isValid()) {
                // APPEARANCE_LIGHT_NAVIGATION_BARS (0x10) off: light buttons
                insets.callMethod<void>("setSystemBarsAppearance", "(II)V",
                                        static_cast<jint>(0),
                                        static_cast<jint>(0x10));
            }
        }
    });
}
#endif

#ifdef Q_OS_WIN
// The title bar in the header's color (Theme.surface) with the app's text
// color, so the two read as one bar. Windows 11 takes both; Windows 10
// refuses them and keeps the dark title bar the color scheme gives it
static void colorTitleBar(QWindow *window)
{
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    const COLORREF caption = RGB(0x1e, 0x1e, 0x2e); // Theme.surface
    const COLORREF text = RGB(0xe8, 0xe8, 0xf0);    // Theme.text
    (void)DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof caption);
    (void)DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof text);
}
#endif

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName("QtTune");
    app.setApplicationName("QtTune");
    app.setApplicationVersion(QStringLiteral(ECUIO_VERSION_STRING));

    // The UI is dark only. Saying so lets the platform match it: a dark
    // title bar on Windows, and Android's system bars drawn for a dark
    // app rather than over a white strip
    // (docs/bench-sessions/2026-10-28-android-2.md)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);

    // QTTUNE_SETTINGS_DIR keeps this instance's preferences in an ini file
    // there instead of the user's (the registry on Windows), so a scripted
    // run neither reads nor changes them: the URI, the Record switch, zoom
    const QString settingsDir = qEnvironmentVariable("QTTUNE_SETTINGS_DIR");
    if (!settingsDir.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    }

    QQuickStyle::setStyle("Basic");

    // Declared before the bridge, so it outlives the bridge's sessions:
    // the Bluetooth links' streams unregister when it goes (ADR-0028)
    BluetoothDevices bluetooth;
    BluetoothDevices::instance = &bluetooth;

    // Declared before the engine, so it outlives everything QML holds.
    QtTuneBridge bridge;
    BridgeSingleton::instance = &bridge;
    bluetooth.attachTo(&bridge);
#ifdef Q_OS_ANDROID
    // The foreground service while an adapter session is up (ADR-0029 §2)
    AndroidSession androidSession(&bridge);
#endif

    QQmlApplicationEngine engine;
    engine.loadFromModule("QtTune", "Main");

    if (engine.rootObjects().isEmpty()) {
        qCritical("=== QML FAILED TO LOAD ===");
        return -1;
    }
#ifdef Q_OS_ANDROID
    darkenAndroidWindow();
#endif
#ifdef Q_OS_WIN
    if (auto *window = qobject_cast<QWindow *>(engine.rootObjects().constFirst()))
        colorTitleBar(window);
#endif

    return app.exec();
}
