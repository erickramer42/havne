// File/layer: app/phonewindow_ios.mm | Qt6 Qml and UIKit, app layer, Objective-C++
// Testing: BENCH, not run (no iOS device)
//
// PhoneWindow's iOS half. The orientation lock isn't offered (ADR-0059
// section 8), so applyOrientation does nothing; lockOrientation keeps the
// state.

#include "phonewindow.h"

#import <UIKit/UIKit.h>

bool PhoneWindow::orientationLockAvailable() const
{
    return false;
}

bool PhoneWindow::externalPower() const
{
    UIDevice* device = UIDevice.currentDevice;
    device.batteryMonitoringEnabled = YES;
    const UIDeviceBatteryState state = device.batteryState;
    return state == UIDeviceBatteryStateCharging ||
           state == UIDeviceBatteryStateFull;
}

void PhoneWindow::applyKeepScreenOn(bool on)
{
    UIApplication.sharedApplication.idleTimerDisabled = on ? YES : NO;
}

void PhoneWindow::applyOrientation(const QString& mode)
{
    Q_UNUSED(mode);
}
