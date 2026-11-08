// File/layer: app/windowtheme.h | Qt6 Gui, app layer, C++17
// Testing: BRIDGE: BridgeTest.WindowTheme* (test_bridge, the wiring and the
//          count of applies); manual smoke and BENCH for the platform step
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
// or text. Owned by the window. A theme that lacks one of the three
// properties, or does not notify its changes, is logged as a warning: the
// window would silently stop following the theme. With no window (tests),
// the platform step is skipped.
class WindowTheme : public QObject
{
    Q_OBJECT
public:
    WindowTheme(QObject* theme, QWindow* window);

public slots:
    void apply();

signals:
    // Emitted at the end of every apply(), whether or not the platform has
    // a step for it
    void applied();

private:
    QObject* m_theme;
    QWindow* m_window;
};
