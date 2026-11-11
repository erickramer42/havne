// File/layer: app/windowtheme.h | Qt6 Gui, app layer, C++17
// Testing: BRIDGE: BridgeTest.WindowTheme* (test_bridge, the wiring, the
//          count of applies and the light rule); manual smoke and BENCH for
//          the platform step
//
// Colors the platform's own window from Theme's bg, surface and text, so a
// night theme never sits under a bright title bar or a white strip, and a
// light look never under a dark one. The class builds on every platform;
// main() makes one only on Windows and Android, so the color scheme follows
// the look there and stays Dark, as main() sets it, elsewhere. Its apply
// step: the scheme follows the look's lightness, Android colors its window
// and its bars' icons, Windows colors the title bar.

#pragma once

#include <QObject>

class QColor;
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

    // Whether a page of this background is a light look's: its system bars
    // then draw dark icons and the color scheme is Light. HSL lightness
    // over a half, so every palette sits well clear of the line
    static bool lightBars(const QColor& bg);

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
