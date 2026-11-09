// File/layer: tests/fake_theme.h | test-only, Qt6 Core and Gui, C++17
// Testing: the stand-in for Theme in BridgeTest.WindowTheme* (test_bridge)
//
// The three properties WindowTheme follows (bg, surface, text), each with
// its own notify signal as a QML singleton's properties have.

#pragma once

#include <QColor>
#include <QObject>

class FakeTheme : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QColor bg READ bg WRITE setBg NOTIFY bgChanged)
    Q_PROPERTY(
        QColor surface READ surface WRITE setSurface NOTIFY surfaceChanged)
    Q_PROPERTY(QColor text READ text WRITE setText NOTIFY textChanged)

public:
    QColor bg() const { return m_bg; }
    QColor surface() const { return m_surface; }
    QColor text() const { return m_text; }
    void setBg(const QColor& c)
    {
        m_bg = c;
        emit bgChanged();
    }
    void setSurface(const QColor& c)
    {
        m_surface = c;
        emit surfaceChanged();
    }
    void setText(const QColor& c)
    {
        m_text = c;
        emit textChanged();
    }

signals:
    void bgChanged();
    void surfaceChanged();
    void textChanged();

private:
    QColor m_bg{Qt::black};
    QColor m_surface{Qt::black};
    QColor m_text{Qt::white};
};

// A theme whose "surface" is not a notifying property
class FakeThemeWithoutSurface : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QColor bg MEMBER m_bg NOTIFY bgChanged)
    Q_PROPERTY(QColor text MEMBER m_text NOTIFY textChanged)

public:
    void setBg(const QColor& c)
    {
        m_bg = c;
        emit bgChanged();
    }

signals:
    void bgChanged();
    void textChanged();

private:
    QColor m_bg{Qt::black};
    QColor m_text{Qt::white};
};
