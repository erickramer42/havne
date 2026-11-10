// File/layer: tests/qml/tst_driveplatform.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.driveplatform (test_qml)

import QtQuick
import QtTest
import App

// The drive view's platform layer (ADR-0059): the keep-awake rule, its
// timer, and the immersive helper, on the desktop's stand-in for the phone
TestCase {
    id: testCase
    name: "DrivePlatform"
    when: windowShown

    Component { id: awakeComponent; KeepAwake {} }
    // As the drive view will write it: bindings, not assignments from a script
    Component {
        id: declaredComponent
        KeepAwake { mobile: true; active: true; connected: true; dashShown: true; readPower: () => false }
    }
    // Stands in for a window: visibility is all Immersive touches
    Component { id: windowComponent; QtObject { property int visibility: Window.Maximized } }
    SignalSpy { id: keepSpy; target: PhoneWindow; signalName: "keepScreenOnChanged" }
    SignalSpy { id: orientationSpy; target: PhoneWindow; signalName: "orientationChanged" }

    function cleanup() {
        Immersive.leave()
        PhoneWindow.setKeepScreenOn(false)
        PhoneWindow.lockOrientation("follow")
    }

    // ADR-0059 section 6's table, row by row

    function test_keepAwakeIsTheTable_data() {
        return [{ tag: "connected, power, any page", c: true, p: true, d: false, want: true },
                { tag: "connected, power, on Dash", c: true, p: true, d: true, want: true },
                { tag: "connected, battery, on Dash", c: true, p: false, d: true, want: true },
                { tag: "connected, battery, another page", c: true, p: false, d: false, want: false },
                { tag: "no session, power", c: false, p: true, d: true, want: false },
                { tag: "no session, battery", c: false, p: false, d: true, want: false }]
    }
    function test_keepAwakeIsTheTable(data) {
        const a = createTemporaryObject(awakeComponent, testCase)
        compare(a.keepAwake(data.c, data.p, data.d), data.want)
    }

    function awake(props) {
        const a = createTemporaryObject(awakeComponent, testCase, Object.assign({ mobile: true,
            active: true, readPower: () => false }, props))
        verify(a)
        return a
    }

    // Power through readPower and a tick of the timer, so a later tick
    // reads the same and a slow run can't undo it
    function test_theScreenFlagFollowsTheRule() {
        let power = false
        const a = awake({ readPower: () => power })
        compare(PhoneWindow.keepScreenOn, false)
        a.connected = true
        a.dashShown = true
        tryCompare(PhoneWindow, "keepScreenOn", true)
        a.dashShown = false
        tryCompare(PhoneWindow, "keepScreenOn", false, 3000, "off Dash on battery it sleeps")
        power = true
        findChild(a, "powerTimer").triggered()
        tryCompare(PhoneWindow, "keepScreenOn", true)
        a.connected = false
        tryCompare(PhoneWindow, "keepScreenOn", false, 3000, "no session, no flag")
    }

    function test_theDesktopFormNeverAsks() {
        const a = awake({ mobile: false })
        a.connected = true
        a.dashShown = true
        compare(a.wanted, false)
        compare(PhoneWindow.keepScreenOn, false)
    }

    function test_leavingTheForegroundClearsIt() {
        const a = awake()
        a.connected = true
        a.dashShown = true
        tryCompare(PhoneWindow, "keepScreenOn", true)
        a.active = false
        tryCompare(PhoneWindow, "keepScreenOn", false)
        a.active = true
        tryCompare(PhoneWindow, "keepScreenOn", true, 3000, "and it's asked again on return")
    }

    function test_powerIsReadOnTheTimerOnlyWhileItMatters() {
        let reads = 0
        const a = awake({ readPower: () => { reads++; return true } })
        const timer = findChild(a, "powerTimer")
        compare(timer.interval, 5000)
        verify(!timer.running, "no session, no reading")
        compare(reads, 0)
        a.connected = true
        tryVerify(() => reads === 1, 3000, "read as soon as a session connects")
        compare(a.hasPower, true)
        a.active = false
        verify(!timer.running, "not in the background")
        a.active = true
        verify(timer.running, "running again in front")
        a.mobile = false
        verify(!timer.running, "never on the desktop")
    }

    // The immersive helper (section 5)

    function test_immersiveRestoresTheExactVisibility_data() {
        return [{ tag: "a phone's maximized window", from: Window.Maximized },
                { tag: "a windowed window", from: Window.Windowed },
                { tag: "already full screen", from: Window.FullScreen }]
    }
    function test_immersiveRestoresTheExactVisibility(data) {
        const w = createTemporaryObject(windowComponent, testCase, { visibility: data.from })
        Immersive.enter(w, "phone")
        compare(w.visibility, Window.FullScreen)
        Immersive.leave()
        compare(w.visibility, data.from)
    }

    function test_immersiveIsThePhoneFormsAlone() {
        const w = createTemporaryObject(windowComponent, testCase)
        Immersive.enter(w, "desktop")
        compare(w.visibility, Window.Maximized)
        compare(Immersive.window, null)
        Immersive.leave()
        compare(w.visibility, Window.Maximized, "leaving without entering changes nothing")
    }

    function test_immersiveEnteredTwiceKeepsTheFirstValue() {
        const w = createTemporaryObject(windowComponent, testCase)
        Immersive.enter(w, "phone")
        Immersive.enter(w, "phone")
        Immersive.leave()
        compare(w.visibility, Window.Maximized, "not the full screen the second call saw")
        w.visibility = Window.Windowed
        Immersive.leave()
        compare(w.visibility, Window.Windowed, "a second leave does nothing")
        Immersive.enter(w, "phone")
        compare(w.visibility, Window.FullScreen, "and it can be entered again")
    }

    function test_lockOrientationTakesTheThreeWords_data() {
        return [{ tag: "upright", word: "upright", stored: "upright" },
                { tag: "on the side", word: "side", stored: "side" },
                { tag: "follow", word: "follow", stored: "follow" },
                { tag: "anything else follows", word: "sideways", stored: "follow" }]
    }
    function test_lockOrientationTakesTheThreeWords(data) {
        PhoneWindow.lockOrientation(data.word === "follow" ? "upright" : "follow")
        PhoneWindow.lockOrientation(data.word)
        compare(PhoneWindow.orientation, data.stored)
    }

    function test_aBoundDashShownTakesEffect() {
        const a = createTemporaryObject(declaredComponent, testCase)
        verify(a)
        tryCompare(PhoneWindow, "keepScreenOn", true)
    }

    function test_powerIsReadAgainAndAgain() {
        let reads = 0
        const a = awake({ connected: true, readPower: () => { reads++; return false } })
        findChild(a, "powerTimer").interval = 10
        tryVerify(() => reads >= 3, 3000, "the reading repeats")
    }

    // Left to its own bindings, as the app makes it on a desktop
    function test_theDefaultsAreTheDesktops() {
        const a = createTemporaryObject(awakeComponent, testCase)
        compare(a.mobile, false)
        compare(a.readPower(), false, "it asks PhoneWindow, which has no power here")
    }

    function test_aRepeatedCallTellsNobody() {
        keepSpy.clear()
        orientationSpy.clear()
        PhoneWindow.setKeepScreenOn(true)
        PhoneWindow.setKeepScreenOn(true)
        compare(keepSpy.count, 1)
        PhoneWindow.lockOrientation("upright")
        PhoneWindow.lockOrientation("upright")
        compare(orientationSpy.count, 1)
    }

    function test_noDesktopHasAnOrientationLock() {
        compare(PhoneWindow.orientationLockAvailable, false)
        compare(PhoneWindow.externalPower(), false)
    }
}
