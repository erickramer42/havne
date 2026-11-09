// File/layer: app/phonewindow.h | Qt6 Qml, app layer, C++17
// Testing: QML: QmlTest.driveplatform (test_qml); BENCH for the Android and
//          iOS calls
//
// What the drive view (ADR-0059) asks of the phone itself: the external
// power reading, the screen kept on and the orientation held. Exposed to
// QML as the `PhoneWindow` singleton. On a desktop each call only records
// its state, so a page and a test can use it anywhere.

#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration>

class PhoneWindow : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool keepScreenOn READ keepScreenOn NOTIFY keepScreenOnChanged)
    Q_PROPERTY(QString orientation READ orientation NOTIFY orientationChanged)
    // Whether lockOrientation does anything: Android only (ADR-0059 section 8)
    Q_PROPERTY(
        bool orientationLockAvailable READ orientationLockAvailable CONSTANT)

public:
    explicit PhoneWindow(QObject* parent = nullptr);

    bool keepScreenOn() const { return m_keepScreenOn; }
    QString orientation() const { return m_orientation; }
    bool orientationLockAvailable() const;

    // Whether the phone has external power: plugged in, not "charging"
    // (ADR-0059 section 6; a phone can be plugged in and not charging)
    Q_INVOKABLE bool externalPower() const;
    Q_INVOKABLE void setKeepScreenOn(bool on);
    // "follow", "upright" or "side"; anything else is "follow"
    Q_INVOKABLE void lockOrientation(const QString& mode);

signals:
    void keepScreenOnChanged();
    void orientationChanged();

private:
    void applyKeepScreenOn(bool on);
    void applyOrientation(const QString& mode);

    bool m_keepScreenOn = false;
    QString m_orientation = QStringLiteral("follow");
};
