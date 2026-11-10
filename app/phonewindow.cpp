// File/layer: app/phonewindow.cpp | Qt6 Qml, app layer, C++17
// Testing: QML: QmlTest.driveplatform (test_qml); BENCH for the Android calls

#include "phonewindow.h"

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

PhoneWindow::PhoneWindow(QObject* parent) : QObject(parent) {}

void PhoneWindow::setKeepScreenOn(bool on)
{
    if (on == m_keepScreenOn) {
        return;
    }
    m_keepScreenOn = on;
    applyKeepScreenOn(on);
    emit keepScreenOnChanged();
}

void PhoneWindow::lockOrientation(const QString& mode)
{
    const QString shown =
        (mode == QLatin1String("upright") || mode == QLatin1String("side"))
            ? mode
            : QStringLiteral("follow");
    const bool changed = shown != m_orientation;
    m_orientation = shown;
    // Applied every time: a recreated activity has lost the last request
    applyOrientation(shown);
    if (changed) {
        emit orientationChanged();
    }
}

#ifdef Q_OS_ANDROID
namespace {
// android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
constexpr jint kFlagKeepScreenOn = 0x80;
// android.content.pm.ActivityInfo.SCREEN_ORIENTATION_*
constexpr jint kOrientationUnspecified = -1;
constexpr jint kOrientationSensorLandscape = 6;
constexpr jint kOrientationSensorPortrait = 7;
} // namespace

bool PhoneWindow::orientationLockAvailable() const
{
    return true;
}

bool PhoneWindow::externalPower() const
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject action = QJniObject::fromString(
        QStringLiteral("android.intent.action.BATTERY_CHANGED"));
    const QJniObject filter("android/content/IntentFilter",
                            "(Ljava/lang/String;)V", action.object<jstring>());
    // A null receiver reads the sticky intent and registers nothing
    const QJniObject intent = context.callObjectMethod(
        "registerReceiver",
        "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)"
        "Landroid/content/Intent;",
        static_cast<jobject>(nullptr), filter.object());
    int plugged = 0;
    if (intent.isValid()) {
        const QJniObject key =
            QJniObject::fromString(QStringLiteral("plugged"));
        plugged = intent.callMethod<jint>(
            "getIntExtra", "(Ljava/lang/String;I)I", key.object<jstring>(),
            static_cast<jint>(0));
    }
    QJniEnvironment().checkAndClearExceptions();
    return plugged > 0;
}

void PhoneWindow::applyKeepScreenOn(bool on)
{
    using QNativeInterface::QAndroidApplication;
    QAndroidApplication::runOnAndroidMainThread([on]() {
        const QJniObject activity = QAndroidApplication::context();
        const QJniObject window =
            activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
        if (window.isValid()) {
            window.callMethod<void>(on ? "addFlags" : "clearFlags", "(I)V",
                                    kFlagKeepScreenOn);
        }
        QJniEnvironment().checkAndClearExceptions();
    });
}

void PhoneWindow::applyOrientation(const QString& mode)
{
    using QNativeInterface::QAndroidApplication;
    const jint value =
        mode == QLatin1String("upright") ? kOrientationSensorPortrait
        : mode == QLatin1String("side")  ? kOrientationSensorLandscape
                                         : kOrientationUnspecified;
    QAndroidApplication::runOnAndroidMainThread([value]() {
        const QJniObject activity = QAndroidApplication::context();
        activity.callMethod<void>("setRequestedOrientation", "(I)V", value);
        QJniEnvironment().checkAndClearExceptions();
    });
}
#elif !defined(Q_OS_IOS)
bool PhoneWindow::orientationLockAvailable() const
{
    return false;
}
bool PhoneWindow::externalPower() const
{
    return false;
}
void PhoneWindow::applyKeepScreenOn(bool on)
{
    Q_UNUSED(on);
}
void PhoneWindow::applyOrientation(const QString& mode)
{
    Q_UNUSED(mode);
}
#endif
