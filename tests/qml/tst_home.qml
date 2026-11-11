// File/layer: tests/qml/tst_home.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.home (test_qml)

import QtQuick
import QtTest
import App
TestCase {
    id: testCase
    name: "Home"
    when: windowShown

    Component {
        id: pageComponent
        HomePage {}
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }

    // A singleton's state, shared by every case
    function cleanup() {
        Theme.developerMode = false
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }

    SignalSpy {
        id: connectSpy
        signalName: "connectToUri"
    }

    readonly property var someAdapters: [
        { uri: "j2534://Vendor A", title: "Vendor A", kind: "J2534 adapter" },
        { uri: "elm327://COM3", title: "COM3", kind: "Serial port" }]

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // Under the window's root, so visible reads the page's own
    function createPage(w, h) {
        const page = createTemporaryObject(pageComponent, testCase.parent,
                                           { width: w, height: h })
        verify(page, "Home loads")
        return page
    }

    // The visible items under item whose right edge lies past page's
    function pastTheEdge(item, page, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const right = child.mapToItem(page, child.width, 0).x
            if (right > page.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, page, out)
        }
        return out
    }

    // With no session Home is the connect list: the recent addresses and
    // the adapters, in a column at most 560 wide, under the logo and the
    // app's name
    function test_homeIsTheConnectList() {
        const page = createPage(1280, 720)
        page.recents = ["elm327://COM9"]
        page.adapters = someAdapters
        const list = find(page, "homeConnectList")
        verify(list.visible, "the list")
        verify(!find(page, "homeSession").visible, "no session card")
        tryVerify(() => findChild(list, "connectRow_2") !== null, 2000, "the recent and two adapters")
        compare(find(find(list, "connectRow_1"), "connectRowAddress").text, "Vendor A")
        compare(find(find(list, "connectRow_1"), "connectRowKind").text, "J2534 adapter")
        compare(list.chosen, "elm327://COM9", "the newest recent")
        const column = find(page, "homeColumn")
        compare(column.width, 560)
        fuzzyCompare(column.x, (1280 - 560) / 2, 1, "centered")
        compare(find(page, "homeCoreVersion").text, "core " + Bridge.coreVersion)
        compare(find(page, "homeHint").text,
                "Connect from here, or from Settings > Connection to choose a profile")
        const logo = find(page, "homeLogo")
        verify(logo.visible, "the logo")
        verify(logo.mapToItem(page, 0, logo.height).y <= list.mapToItem(page, 0, 0).y,
               "above the list")
    }

    // Connect hands Main the chosen row; a tapped one is the choice
    function test_homeConnectsTheChosenRow() {
        const page = createPage(1280, 720)
        page.recents = []
        page.adapters = someAdapters
        connectSpy.target = page
        connectSpy.clear()
        const list = find(page, "homeConnectList")
        tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "two adapters")
        compare(list.chosen, "", "two: none chosen")
        verify(!find(list, "connectGo").enabled, "Connect waits")
        find(list, "connectRow_1").clicked()
        compare(page.tapped, "elm327://COM3", "Home keeps the tap")
        find(list, "connectGo").clicked()
        compare(connectSpy.count, 1)
        compare(connectSpy.signalArguments[0][0], "elm327://COM3")
    }

    // A session turns the list into the session's card: the state, the
    // address, the profile, Dashboard and a quiet Disconnect
    function test_homeShowsTheSessionOnceConnected() {
        const page = createPage(1280, 720)
        const card = find(page, "homeSession")
        verify(!card.visible, "no session: no card")
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryVerify(() => card.visible, 5000, "the card")
        verify(!find(page, "homeConnectList").visible, "the list gives way")
        verify(find(page, "homeLogo").visible, "the logo stays")
        compare(find(card, "linkLabel").text, Theme.linkLabel(Bridge.linkState))
        compare(find(card, "homeSessionAddress").text, Bridge.uriLabel)
        verify(find(card, "homeSessionDetail").text.startsWith("Profile default · since "),
               find(card, "homeSessionDetail").text)
        const dash = find(card, "homeDashboard")
        verify(!dash.highlighted && !dash.flat, "Dashboard is secondary")
        const off = find(card, "homeDisconnect")
        verify(off.flat, "ending a session is quiet")
        off.clicked()
        tryVerify(() => !card.visible && find(page, "homeConnectList").visible, 5000, "the list again")
    }

    // Held still, in front of the user (the offscreen window may not count
    // as active)
    function stillPage() {
        const page = createTemporaryObject(pageComponent, testCase.parent,
                                           { width: 1280, height: 720, animated: false })
        verify(page, "Home loads")
        find(page, "homeLogo").appActive = true
        return page
    }

    // The first time Home shows, the logo sweeps to the redline and back
    // to idle, as a cluster does at ignition; never again after
    function test_homesLogoSweepsOnceThenIdles() {
        const page = stillPage()
        const logo = find(page, "homeLogo")
        compare(logo.rpm, logo.idle, "held still at idle")
        page.animated = true
        verify(logo.sweeping, "the sweep starts")
        tryVerify(() => logo.rpm >= logo.redline - 1, 1000, "up to the redline")
        tryVerify(() => !logo.sweeping, 1500, "the sweep ends")
        verify(Math.abs(logo.rpm - logo.idle) < 1, "back at idle")
        page.visible = false
        page.visible = true
        verify(logo.live && !logo.sweeping, "shown again: no second sweep")
    }

    // The logo moves only while Home shows, the app is in front and nothing
    // holds it still
    function test_homesLogoRunsOnlyOnScreenAndInFront() {
        const page = stillPage()
        const logo = find(page, "homeLogo")
        verify(!logo.live, "held still")
        page.animated = true
        verify(logo.live, "on screen and in front")
        const blip = find(logo, "logoBlip")
        tryVerify(() => logo.swept && !logo.sweeping, 3000, "the sweep ends")
        verify(blip.running, "the blips run while it shows")
        logo.appActive = false
        verify(!logo.live && !logo.sweeping, "behind another app")
        verify(!blip.running, "no blip behind another app")
        compare(logo.rpm, logo.idle, "at idle behind another app")
        logo.appActive = true
        verify(blip.running, "and again in front")
        page.visible = false
        verify(!logo.live && !logo.sweeping, "off screen")
        verify(!blip.running, "no blip off screen")
        compare(logo.rpm, logo.idle, "still while off screen")
    }

    // Between blips the needle holds still at idle: nothing hunts
    function test_homesLogoHoldsStillAtIdleBetweenBlips() {
        const page = stillPage()
        const logo = find(page, "homeLogo")
        page.animated = true
        tryVerify(() => logo.swept && !logo.sweeping, 3000, "the sweep ends")
        tryVerify(() => Math.abs(logo.rpm - logo.idle) < 1, 1500, "at idle")
        const start = logo.target
        wait(1500)   // two ticks of the old hunt; the first blip is 3.5 s out
        compare(logo.target, start, "the target hasn't moved")
        compare(logo.rpm, logo.idle, "still at idle")
    }

    function test_uprightEverythingFitsTheWidth() {
        Theme.developerMode = true
        const page = createPage(380, 800)
        page.recents = ["elm327://bt:00:11:22:33:44:55?profile=obd2"]
        page.adapters = someAdapters
        const column = find(page, "homeColumn")
        waitForPolish(column)
        compare(pastTheEdge(page, page, []).join("; "), "",
                "nothing past the page's right edge")
    }

    function test_onItsSideThePageScrollsToTheLastItem() {
        const page = createPage(800, 330)
        page.recents = ["elm327://COM9"]
        page.adapters = someAdapters
        const flick = find(page, "homeFlick")
        const column = find(page, "homeColumn")
        waitForPolish(column)
        tryVerify(() => column.height > page.height, 2000, "the column is taller than the page")
        compare(column.y, Theme.pageMargin, "the column starts at the margin")
        verify(flick.contentHeight >= column.y + column.height,
               "the page scrolls to the column's end: " + flick.contentHeight)
    }

}
