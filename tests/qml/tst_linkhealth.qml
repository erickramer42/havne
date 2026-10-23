// File/layer: tests/qml/tst_linkhealth.qml | Qt Quick Test, QML tier
// Testing: every link-health surface, in the app's own window (Main),
//          for each link state: the header pill, Home's status card,
//          the Dashboard header, LiveView's banner, Settings' status
//          label and its activity log. Disconnected and ok everywhere
//          (mock://demo); quiet, fault and requests on Windows, through
//          the fake vendor DLL in-proc with a knob armed, as the manual
//          smoke recipe in docs/testing.md does.

import QtQuick
import QtTest
import QtTune
import QtTuneTest

TestCase {
    id: testCase
    name: "LinkHealth"
    when: windowShown

    Component { id: mainComponent; Main {} }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/QtTune\//)
    }

    function cleanup() {
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
    }

    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function connect(uri) {
        compare(Bridge.connectSession(uri), "", uri + " connects")
    }

    function requireFake() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
    }

    function fakeUri(query) {
        return "j2534://" + Harness.fakeDllPath + query
    }

    function createMain() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        return main
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // The page named `name`, brought on screen
    function showPage(main, name) {
        const page = find(main.contentItem, name)
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        verify(page.visible, name + " is on screen")
        return page
    }

    function activityLog(main) {
        return find(find(main.contentItem, "settingsPage"), "activityLog").text
    }

    // Every surface shows `state`; LiveView's banner reads `banner`
    // (empty: no banner)
    function checkSurfaces(main, state, banner) {
        compare(Bridge.linkState, state)
        const tone = Theme.linkColor(state)
        const label = Theme.linkLabel(state)
        const uri = Bridge.currentUri

        sameColor(find(main.contentItem, "linkPill").tone, tone, "header pill")
        compare(find(main.contentItem, "linkPillText").text,
                Bridge.connected ? label + " · " + uri : label, "header pill")

        const home = showPage(main, "homePage")
        sameColor(find(home, "linkDot").color, tone, "Home dot")
        compare(find(home, "linkLabel").text, label, "Home label")

        const dashboard = showPage(main, "dashboardPage")
        sameColor(find(dashboard, "linkDot").color, tone, "Dashboard dot")
        const dashboardLabel = find(dashboard, "linkLabel")
        compare(dashboardLabel.text, state === "ok" ? "Live · " + uri : label, "Dashboard label")
        sameColor(dashboardLabel.color, tone, "Dashboard label")

        const live = showPage(main, "livePage")
        compare(find(live, "linkBanner").visible, banner !== "", "LiveView banner shown")
        if (banner !== "") {
            const text = find(live, "linkBannerText")
            compare(text.text, banner, "LiveView banner")
            sameColor(text.color, tone, "LiveView banner")
        }

        const settings = showPage(main, "settingsPage")
        const status = find(settings, "sessionStatus")
        compare(status.text, Bridge.connected ? "Connected: " + uri : "Not connected",
                "Settings status")
        sameColor(status.color, tone, "Settings status")
    }

    function test_disconnected() {
        const main = createMain()
        checkSurfaces(main, "disconnected", "")
    }

    function test_ok() {
        const main = createMain()
        connect("mock://demo")
        checkSurfaces(main, "ok", "")
        wait(300)   // a few health polls: a healthy mock session stays ok
        checkSurfaces(main, "ok", "")
    }

    function test_quiet() {
        requireFake()
        Harness.armFakeKnob("ECUIO_FAKE_READ_STATUS", "9")   // ERR_TIMEOUT
        const main = createMain()
        connect(fakeUri("?profile=raw&filter=0x7E8"))
        tryCompare(Bridge, "linkState", "quiet", 5000)
        checkSurfaces(main, "quiet", "Vehicle not responding — quiet bus")
        verify(activityLog(main).includes("Link: vehicle not responding"), "quiet log line")
    }

    function test_fault() {
        requireFake()
        Harness.armFakeKnob("ECUIO_FAKE_READ_STATUS", "7")   // ERR_FAILED
        const main = createMain()
        connect(fakeUri("?profile=raw&filter=0x7E8"))
        tryCompare(Bridge, "linkState", "fault", 5000)
        compare(Bridge.errorEpisodes, 1, "one episode while the failure lasts")
        checkSurfaces(main, "fault", "Adapter fault — 1 episode(s), retrying with backoff")
        verify(activityLog(main).includes("Link: adapter fault (1 episode(s))"), "fault log line")
    }

    // A bare j2534:// URI runs the obd2 scheduler, which writes every cycle
    function test_requests() {
        requireFake()
        Harness.armFakeKnob("ECUIO_FAKE_WRITE_STATUS", "7")   // ERR_FAILED
        const main = createMain()
        connect(fakeUri(""))
        tryCompare(Bridge, "linkState", "requests", 5000)
        checkSurfaces(main, "requests", "Requests failing — check the vehicle connection and ignition")
        verify(activityLog(main).includes("Link: requests failing (1 episode(s))"),
               "requests log line")
    }

    // After a fault, a disconnect shows plain disconnected everywhere, not
    // the last session's state
    function test_disconnectClearsTheLastState() {
        requireFake()
        Harness.armFakeKnob("ECUIO_FAKE_READ_STATUS", "7")
        const main = createMain()
        connect(fakeUri("?profile=raw&filter=0x7E8"))
        tryCompare(Bridge, "linkState", "fault", 5000)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        checkSurfaces(main, "disconnected", "")
    }
}
