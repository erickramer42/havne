// File/layer: tests/qml/tst_driveview_platform.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.driveview_platform (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

DriveViewKit {
    id: testCase
    name: "DriveView"

    Component { id: otherWindow; Window { width: 100; height: 100; visible: true } }

    Component { id: fieldComponent; TextInput { width: 100; height: 30 } }

    function fronted(s) {
        s.main.requestActivate()
        tryVerify(() => s.main.active, 3000, "the window is in front")
    }

    // ---- Remembered once connected (section 7)

    function test_aConnectEntersWithDashShowing() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        compare(s.dash.drivePart.driving, false, "never opens in the view")
        connect("mock://demo")
        tryVerify(() => s.dash.drivePart.driving, 3000, "it comes back with the session")
    }

    function test_aConnectFromAnotherPageStaysThere() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        s.main.gotoPage(2)
        connect("mock://demo")
        wait(50)
        compare(s.main.currentPage, 2)
        compare(s.dash.drivePart.driving, false)
    }

    function test_aReplayNeverEntersByItself() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        verify(s.dash.drivePart.isReplay("REPLAY://x"), "whatever its case")
        verify(!s.dash.drivePart.isReplay("mock://demo"))
        connect(replayUri())
        wait(50)
        compare(s.dash.drivePart.driving, false)
    }

    function test_aConnectLeavesEditModeAlone() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        s.dash.startEditing()
        connect("mock://demo")
        wait(50)
        compare(s.dash.drivePart.driving, false, "edit mode is never interrupted")
        s.dash.stopEditing()
    }

    function test_aSettingLeftOffDoesntEnter() {
        const s = phoneMain()
        connect("mock://demo")
        wait(50)
        compare(s.dash.drivePart.driving, false)
    }

    // ---- The hint (section 3)

    function test_theHintShowsOnTheFirstThreeEntries() {
        dashStore.setValue("driveHints", 0)
        dashStore.sync()
        const s = driven()
        const hint = find(s.dash, "driveHint")
        for (let i = 1; i <= 3; ++i) {
            s.dash.drivePart.enterDrive()
            verify(hint.visible, "entry " + i)
            s.dash.drivePart.leaveDrive(true)
        }
        s.dash.drivePart.enterDrive()
        verify(!hint.visible, "not on the fourth")
    }

    function test_theHintLastsTheDelay_data() {
        return [{ tag: "two seconds", ms: 2000, want: 2000 }, { tag: "never", ms: 0, want: 4000 }]
    }

    function test_theHintLastsTheDelay(data) {
        dashStore.setValue("driveHints", 0)
        dashStore.sync()
        uiStore.setValue("driveHideMs", data.ms)
        uiStore.sync()
        const s = driven()
        s.dash.drivePart.enterDrive()
        compare(find(s.dash, "driveHintTimer").interval, data.want)
    }

    function test_theHintGoesAtItsTimeOrATap() {
        dashStore.setValue("driveHints", 0)
        dashStore.sync()
        const s = driven()
        s.dash.drivePart.enterDrive()
        const hint = find(s.dash, "driveHint")
        find(s.dash, "driveHintTimer").triggered()
        verify(!hint.visible, "its time")
        s.dash.drivePart.leaveDrive(true)
        dashStore.setValue("driveHints", 0)
        s.dash.drivePart.enterDrive()
        s.dash.drivePart.driveTap()
        verify(!hint.visible, "a tap")
    }

    // ---- The platform (sections 5, 6 and 8)

    function test_theOrientationIsLockedInTheViewAlone() {
        uiStore.setValue("driveOrientation", "side")
        uiStore.sync()
        const s = driven()
        compare(PhoneWindow.orientation, "follow", "before")
        s.dash.drivePart.enterDrive()
        compare(PhoneWindow.orientation, "side", "held while the view is on")
        s.dash.drivePart.leaveDrive(true)
        compare(PhoneWindow.orientation, "follow", "released on leaving")
    }

    function test_theScreenStaysOnWithDashShown() {
        const s = driven()
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000, "a session and Dash, on battery")
        s.main.gotoPage(2)
        tryCompare(PhoneWindow, "keepScreenOn", false, 2000, "another page on battery sleeps")
        s.main.gotoPage(s.main.dashboardPage)
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000)
    }

    // A tablet has the view but not the phone form's rule
    function test_aTabletStaysAwakeInTheView() {
        const s = driven()
        s.main.width = 1000
        s.main.height = 900
        verify(waitForPolish(s.main), "laid out")
        compare(s.main.phoneForm, false)
        tryCompare(PhoneWindow, "keepScreenOn", false, 2000, "not the phone form")
        enterOn(s)
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000, "the view holds it")
        s.dash.drivePart.leaveDrive(true)
        tryCompare(PhoneWindow, "keepScreenOn", false, 2000)
    }

    function test_theKeepAwakeFormIsThePhonesAlone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main)
        main.gotoPage(main.dashboardPage)
        connect("mock://demo")
        wait(50)
        compare(PhoneWindow.keepScreenOn, false, "a desktop's Main never asks, even on Dash")
    }

    function test_theScreenSleepsWithoutASession() {
        const s = phoneMain()
        s.main.gotoPage(s.main.dashboardPage)
        wait(50)
        compare(PhoneWindow.keepScreenOn, false, "Dash alone doesn't hold it")
        connect("mock://demo")
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000)
        Bridge.disconnectSession()
        tryCompare(PhoneWindow, "keepScreenOn", false, 5000, "the session's end lets it go")
    }

    function test_theInsetsBoundTheView_data() {
        return [{ tag: "gone", bottom: 0 }, { tag: "kept", bottom: 24 }]
    }

    function test_theInsetsBoundTheView(data) {
        const s = phoneMain({ testInsets: { left: 0, top: 34, right: 0, bottom: data.bottom } })
        connect("mock://demo")
        enterOn(s)
        const line = find(s.dash, "driveStatusLine")
        compare(line.mapToItem(s.main.contentItem, 0, 0).y, 34, "the status line sits under the cutout")
        const pages = find(s.dash, "dashPages")
        compare(pages.mapToItem(s.main.contentItem, 0, pages.height).y,
                s.main.contentItem.height - data.bottom, "the gauges reach the bottom inset")
    }

    function test_aRotationKeepsTheView() {
        const s = driven()
        enterOn(s)
        s.main.width = 832
        s.main.height = 384
        verify(waitForPolish(s.main), "laid out")
        verify(s.dash.drivePart.driving, "the view stays")
        tryVerify(() => find(s.dash, "dashPage_0").columns === 12, 2000, "twelve columns on its side")
        compare(find(s.dash, "driveStatusLine").y, 0, "the line stays at the top")
    }

    // ---- Android's Back

    function test_backFromTheLogReturnsToTheTabItCameFrom() {
        const s = phoneMain()
        const pill = find(s.main.contentItem, "linkPill")
        s.main.gotoPage(3)
        mouseClick(pill)
        compare(s.main.currentPage, s.main.logPage)
        compare(back(s), true)
        compare(s.main.currentPage, 3, "Codes, where it came from")
        compare(back(s), false, "and nothing further back")
    }

    function test_otherAddressBackReturnsToDash() {
        const s = phoneMain()
        const settings = find(s.main.contentItem, "settingsPage")
        compare(s.main.currentPage, s.main.dashboardPage)
        s.dash.otherAddressRequested()
        compare(s.main.currentPage, s.main.settingsPage)
        compare(settings.pageKey, "connection")
        compare(back(s), true)
        compare(s.main.currentPage, s.main.dashboardPage, "back where it came from")
        compare(settings.pageKey, "", "and Settings' list is what it opens to next")
    }

    function test_otherAddressAfterTheListIsBackInTheList() {
        const s = phoneMain()
        const settings = find(s.main.contentItem, "settingsPage")
        s.dash.otherAddressRequested()
        settings.pageKey = ""
        compare(settings.backLeaves, false, "the list ends it")
        settings.open("connection")
        compare(back(s), true)
        compare(s.main.currentPage, s.main.settingsPage, "Back closes Connection as usual")
    }

    function test_aSettingsSubpageClosesAfterALeaveAndReturn() {
        const s = phoneMain()
        const settings = find(s.main.contentItem, "settingsPage")
        s.main.gotoPage(s.main.settingsPage)
        settings.open("display")
        s.main.gotoPage(2)
        s.main.gotoPage(s.main.settingsPage)
        compare(back(s), true, "taken")
        compare(settings.pageKey, "")
        compare(back(s), false, "at the list: left to the system")
    }

    function test_aHeldBackIsOneBack() {
        const s = driven()
        enterOn(s)
        compare(Harness.pressBack(s.main), true)
        verify(s.dash.drivePart.barsUp, "the press reveals")
        compare(Harness.pressBack(s.main, true), true, "the repeat is taken, or the system would act on it")
        verify(s.dash.drivePart.driving, "and doesn't go on to leave")
        verify(s.dash.drivePart.barsUp)
    }

    function test_backFromAnOpenRecordingGoesToTheList() {
        const s = driven()
        const recordings = find(s.main.contentItem, "recordingsPage")
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        tryCompare(Bridge, "logActive", true, 3000)
        Bridge.stopLog()
        tryVerify(() => recordings.viewing, 5000, "Stop log opens the viewer")
        compare(s.main.currentPage, s.main.recordingsPage)
        compare(back(s), true, "there's a list to go back to")
        compare(recordings.viewing, false)
        compare(s.main.currentPage, s.main.recordingsPage)
        compare(back(s), false, "at the list: left to the system")
    }

    // Two panes have no subpage to close: a phone on its side can be this wide
    function test_backInTwoPanesIsLeftToTheSystem() {
        const s = phoneMain()
        s.main.width = 1000
        s.main.height = 500
        const settings = find(s.main.contentItem, "settingsPage")
        s.main.gotoPage(s.main.settingsPage)
        settings.open("display")
        tryCompare(settings, "twoPane", true, 2000)
        compare(back(s), false, "nothing to close")
        compare(settings.pageKey, "display", "the page stays")
    }

    function test_keyFocusComesBackWhenNothingHoldsIt() {
        const s = phoneMain()
        const zoomRoot = find(s.main.contentItem, "zoomRoot")
        s.main.gotoPage(s.main.settingsPage)
        tryVerify(() => s.main.activeFocusItem !== null, 2000)
        // A control that takes the focus and goes away leaves nothing with it
        const settings = find(s.main.contentItem, "settingsPage")
        settings.open("display")
        settings.forceActiveFocus()
        tryVerify(() => settings.activeFocus, 2000)
        s.main.gotoPage(2)
        tryVerify(() => zoomRoot.activeFocus, 2000, "held where the back key is heard")
    }

    // Qt leaves the active focus on an item that hides
    function test_backStillWorksWithAFocusedControlHidden() {
        const s = phoneMain()
        fronted(s)
        s.main.gotoPage(s.main.logPage)
        const field = createTemporaryObject(fieldComponent, find(s.main.contentItem, "zoomRoot"))
        field.forceActiveFocus()
        tryVerify(() => field.activeFocus, 2000)
        field.visible = false
        compare(back(s), true, "with no page change")
        compare(s.main.currentPage, s.main.dashboardPage)
    }

    // A control that goes away drops the focus to the window's root
    function test_aFocusedControlDestroyedLeavesBackWorking() {
        const s = phoneMain()
        fronted(s)
        const field = fieldComponent.createObject(find(s.main.contentItem, "zoomRoot"))
        field.forceActiveFocus()
        tryVerify(() => field.activeFocus, 2000)
        field.destroy()
        wait(0)
        verify(s.main.activeFocusItem !== field, "gone")
        tryVerify(() => focusReachesTheHandler(s), 2000, "held where the back key is heard")
    }

    function test_leavingTheFrontKeepsAFieldsFocus() {
        const s = phoneMain()
        fronted(s)
        const field = createTemporaryObject(fieldComponent, s.dash)
        field.forceActiveFocus()
        tryVerify(() => field.activeFocus, 2000)
        // No parent: a window counts as active while a transient child of its parent is
        const other = otherWindow.createObject(null)
        other.requestActivate()
        tryVerify(() => !s.main.active, 3000, "another window in front")
        verify(!field.activeFocus)
        fronted(s)
        other.destroy()
        tryVerify(() => field.activeFocus, 2000, "the field has it again")
    }

    // ---- The alert band

    // In the view the band takes the page's top slot under the status line
    // and overlays the gauges' top: the pages stay where they were, the bars
    // stay down, Main's band is hidden; it goes with the view
    Component { id: alertRowsC; ListModel {} }

    function test_theDriveBandOverlaysTheGaugesTop() {
        const s = driven()
        enterOn(s)
        const pages = find(s.dash, "dashPages")
        const before = pages.mapToItem(s.main.contentItem, 0, 0).y
        const rows = createTemporaryObject(alertRowsC, testCase)
        const mainBand = find(s.main.contentItem, "alertBand")
        const band = find(s.dash, "driveAlertBand")
        mainBand.conditions = rows
        band.conditions = rows
        rows.append({ key: "a", name: "Coolant hot", enabled: true, level: "danger", banner: true,
                      live: true, state: "on", stale: false, unavailable: false, fired: 1,
                      value: 112, unit: "°C", decimals: 0, staleSinceMs: -1 })
        tryVerify(() => band.visible, 2000, "the drive view's band shows")
        verify(waitForPolish(s.main), "laid out")
        verify(!mainBand.visible, "Main's band is hidden in the view")
        const status = find(s.dash, "driveStatusLine")
        compare(band.mapToItem(s.dash, 0, 0).y, status.y + status.height, "under the status line")
        verify(band.z > pages.z, "over the gauges")
        compare(pages.mapToItem(s.main.contentItem, 0, 0).y, before, "the gauges didn't move")
        verify(!s.dash.drivePart.barsUp && !s.dash.drivePart.barsHeld, "an alert raises no bars")
        s.dash.drivePart.leaveDrive(true)
        verify(waitForPolish(s.main), "laid out")
        verify(!band.visible, "the view's band goes with it")
        verify(mainBand.visible, "and Main's shows")
    }
}
