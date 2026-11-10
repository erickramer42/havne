// File/layer: app/windowtheme.cpp | Qt6 Gui, app layer, C++17
// Testing: BRIDGE: BridgeTest.WindowTheme* (test_bridge, the wiring and the
//          count of applies); manual smoke and BENCH for the platform step

#include "windowtheme.h"

#include <QColor>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QtLogging>
#include <QWindow>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#ifdef Q_OS_ANDROID
// Android's own window, behind Qt's, follows the phone's day/night theme
// (QtActivity's DayNight): light on a light phone, so after a rotation the
// uncovered screen showed white and three-button navigation sat on a light
// scrim (docs/bench-sessions/2026-10-28-android-3.md). It is made dark like
// the app (the theme's bg), no scrim, the navigation buttons drawn light.
static void darkenAndroidWindow(const QColor& bg)
{
    using QNativeInterface::QAndroidApplication;
    QAndroidApplication::runOnAndroidMainThread([bg]() {
        const QJniObject activity = QAndroidApplication::context();
        const QJniObject window =
            activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
        if (!window.isValid()) {
            return;
        }
        const QJniObject background("android/graphics/drawable/ColorDrawable",
                                    "(I)V", static_cast<jint>(bg.rgba()));
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
        // A refused call leaves a pending Java exception; the next JNI call
        // would abort on it
        QJniEnvironment().checkAndClearExceptions();
    });
}
#endif

#ifdef Q_OS_WIN
// The title bar in the header's color (Theme.surface) with the app's text
// color, so the two read as one bar. Windows 11 takes both; Windows 10
// refuses them and keeps the dark title bar the color scheme gives it
static void colorTitleBar(QWindow* window, const QColor& surface,
                          const QColor& textColor)
{
    // WId is the window's HWND held as an integer: the cast is Qt's own
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    const COLORREF caption =
        RGB(surface.red(), surface.green(), surface.blue());
    const COLORREF text =
        RGB(textColor.red(), textColor.green(), textColor.blue());
    (void)DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption,
                                sizeof caption);
    (void)DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof text);
}
#endif

WindowTheme::WindowTheme(QObject* theme, QWindow* window)
    : QObject(window), m_theme(theme), m_window(window)
{
    const QMetaObject* themeMeta = theme->metaObject();
    const QMetaMethod slot =
        staticMetaObject.method(staticMetaObject.indexOfSlot("apply()"));
    for (const char* name : {"bg", "surface", "text"}) {
        const QMetaProperty property =
            themeMeta->property(themeMeta->indexOfProperty(name));
        if (property.isValid() && property.hasNotifySignal()) {
            connect(theme, property.notifySignal(), this, slot);
        } else {
            qWarning("WindowTheme: the theme has no notifying property '%s': "
                     "the window will not follow it",
                     name);
        }
    }
    apply();
}

void WindowTheme::apply()
{
#ifdef Q_OS_ANDROID
    darkenAndroidWindow(m_theme->property("bg").value<QColor>());
#endif
#ifdef Q_OS_WIN
    if (m_window != nullptr) {
        colorTitleBar(m_window, m_theme->property("surface").value<QColor>(),
                      m_theme->property("text").value<QColor>());
    }
#endif
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_WIN)
    (void)m_theme;
    (void)m_window;
#endif
    emit applied();
}
