// File/layer: app/windowtheme.h | Qt6 Gui, app layer, C++17
// Testing: Manual smoke; BENCH (the phone run sheet); CI android-build
//          (compiles it)
//
// Colors the platform's own window from Theme's bg, surface and text, so a
// night theme never sits under a bright title bar or a white strip. The
// class builds on every platform; only its apply step differs: Android
// darkens its window, Windows colors the title bar, elsewhere it does
// nothing.

#pragma once

#include <QObject>

class QWindow;

// Colors the window now and again whenever the theme changes bg, surface
// or text. Owned by the window.
class WindowTheme : public QObject
{
    Q_OBJECT
public:
    WindowTheme(QObject* theme, QWindow* window);

public slots:
    void apply();

private:
    QObject* m_theme;
    QWindow* m_window;
};
