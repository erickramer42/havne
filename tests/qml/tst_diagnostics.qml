// File/layer: tests/qml/tst_diagnostics.qml | Qt Quick Test, QML tier
// Testing: the Diagnostics page (ADR-0037) against the mock's report
//          (core/src/mock_ecu.h): the empty state before any connect,
//          the MIL, the three lists' codes, eight complete monitors, the
//          freeze frame's code and values in either unit system, Read
//          again (a second read logged), diagnose=0's state, and the last
//          read kept after a disconnect; the clear (ADR-0038): its
//          confirmation, Cancel sending nothing, Clear once at the mock's
//          stopped engine (its first 15 s), and on Windows the fake's
//          running engine leaving the codes. The entries themselves are
//          test_bridge's (BridgeTest.DiagnosticsEntryIsWhatThePageShows,
//          .AClear*).

import QtQuick
import QtTest
import App
import AppTest
TestCase {
    id: testCase
    name: "Diagnostics"
    when: windowShown

    Component {
        id: pageComponent
        // Tall enough that the whole card lays out; made in the test's
        // window (testCase.parent), so visible means shown
        DiagnosticsPage { width: 900; height: 1600 }
    }

    SignalSpy {
        id: logSpy
        target: Bridge
        signalName: "logMessage"
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Bridge.unitSystem = 0
        logSpy.clear()
    }

    function cleanup() {
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.unitSystem = 0
    }

    // The activity log's lines holding text, of those logged since init
    function logged(text) {
        const out = []
        for (let i = 0; i < logSpy.count; ++i) {
            const line = String(logSpy.signalArguments[i][0])
            if (line.indexOf(text) >= 0) out.push(line)
        }
        return out
    }

    function find(page, name) {
        const item = findChild(page, name)
        verify(item, name + " exists")
        return item
    }

    // The mock's one ECU, once its read has reached the page
    function connectAndRead(page, uri) {
        compare(Bridge.connectSession(uri), "", uri + " connects")
        tryVerify(() => Bridge.diagnostics.length === 1, 2000, "the report reaches the page")
        return find(page, "ecu_0x7E8")
    }

    function texts(item, name) {
        const out = []
        for (let i = 0; i < item.children.length; ++i) {
            const c = item.children[i]
            if (c.objectName === name) out.push(c.text)
            out.push(...texts(c, name))
        }
        return out
    }

    function diagnosticsLines() {
        let n = 0
        for (let i = 0; i < logSpy.count; ++i) {
            if (String(logSpy.signalArguments[i][0]).indexOf(" diagnostics: ") >= 0) ++n
        }
        return n
    }

    // Named to run first (Qt Quick Test runs a file's tests in name
    // order): the bridge keeps the last read after a disconnect, so the
    // empty state shows only before this process's first connect
    function test_beforeAnyConnectThePageOffersOne() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        compare(Bridge.diagnostics.length, 0)
        verify(find(page, "diagnosticsStatus").text.indexOf("Connect a session") === 0)
        verify(!find(page, "readAgain").visible, "no Read again without a session")
        verify(!find(page, "clearCodes").visible, "nor a clear")
    }

    function test_theMocksReportFillsThePage() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const card = connectAndRead(page, "mock://demo")
        compare(find(card, "ecuState").text, "Read")
        compare(find(card, "milLabel").text, "Check-engine light on · 3 confirmed codes")
        compare(find(card, "codes_stored").text, "P0301  P0420  U0100")
        compare(find(card, "codes_pending").text, "P0171  C0035  B1001")
        compare(find(card, "codes_permanent").text, "P0420")
        const states = texts(find(card, "monitors"), "monitorState")
        compare(states.length, 8, "the mock's eight monitors")
        verify(states.every(s => s === "Complete"), states.join(", "))
        compare(find(card, "freezeLabel").text, "Stored by P0301")
        const values = texts(find(card, "freezeValues"), "freezeValue")
        compare(values, [page.valueText(0xB3 * 100 / 255, 1, "%"),
                         page.valueText(90, 0, "°C"),
                         page.valueText(3000, 0, "RPM"),
                         page.valueText(80, 0, "km/h")])
        compare(find(page, "diagnosticsStatus").text,
                "Read at connect · Clear codes… asks first and saves the report")
        verify(!find(card, "clearState").visible, "no clear yet")

        // Imperial: the freeze frame's values convert, as Live's do. The
        // entries are rebuilt, and with them the card
        Bridge.unitSystem = 1
        const shown = () => texts(find(find(page, "ecu_0x7E8"), "freezeValues"), "freezeValue")
        tryVerify(() => shown()[3].indexOf("mph") > 0, 1000, "speed in mph")
        verify(shown()[1].indexOf("°F") > 0, shown()[1])
    }

    function test_readAgainReadsOnceMore() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectAndRead(page, "mock://demo")
        const button = find(page, "readAgain")
        tryVerify(() => button.enabled, 2000, "Read again once the read has settled")
        tryVerify(() => diagnosticsLines() === 1, 2000, "the first read is logged")
        button.clicked()
        tryVerify(() => diagnosticsLines() === 2, 2000, "Read again logs a second read")
    }

    // Clear codes… opens the confirmation in place and locks both buttons;
    // Cancel closes it and nothing is saved or sent
    function test_clearAsksFirstAndCancelSendsNothing() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectAndRead(page, "mock://demo")
        const clear = find(page, "clearCodes")
        tryVerify(() => clear.enabled, 2000, "Clear codes… once the read has settled")
        const panel = find(page, "clearConfirmPanel")
        verify(!panel.visible)
        clear.clicked()
        verify(panel.visible, "the confirmation opens")
        verify(!clear.enabled && !find(page, "readAgain").enabled, "both wait for it")
        const words = find(page, "clearConfirmText").text
        for (const part of ["stored and pending codes", "freeze frame",
                            "check-engine light off", "back to incomplete",
                            "Permanent codes stay", "emissions inspection",
                            "leave the ignition on", "saved to the recordings folder first"])
            verify(words.indexOf(part) >= 0, "the confirmation says: " + part)
        find(page, "clearCancel").clicked()
        verify(!panel.visible)
        verify(clear.enabled)
        wait(300)
        compare(logged("Diagnostics saved").length, 0, "nothing saved")
        compare(logged(" clear: ").length, 0, "nothing sent")
        compare(find(find(page, "ecu_0x7E8"), "codes_stored").text, "P0301  P0420  U0100")
    }

    // The mock's engine is stopped for its first 15 s: Clear saves the
    // report once, clears, and the card shows the read after it
    function test_clearAtTheMocksStoppedEngineEmptiesTheCodes() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectAndRead(page, "mock://demo")
        const clear = find(page, "clearCodes")
        tryVerify(() => clear.enabled, 2000)
        clear.clicked()
        find(page, "clearConfirm").clicked()
        verify(!find(page, "clearConfirmPanel").visible, "the confirmation closes")
        compare(logged("Diagnostics saved before the clear: ").length, 1, "saved once")
        const card = () => find(page, "ecu_0x7E8")
        tryVerify(() => find(card(), "clearState").text.indexOf("Cleared") === 0, 3000,
                  "the clear's outcome on the card")
        compare(logged("ECU 0x7E8 clear: "), ["ECU 0x7E8 clear: cleared"], "sent once")
        compare(find(card(), "codes_stored").text, "None")
        compare(find(card(), "codes_stored").font.family, page.font.family,
                "the page's own face, not an empty family's bitmap one")
        compare(find(card(), "codes_pending").text, "None")
        compare(find(card(), "codes_permanent").text, "P0420", "permanent stays")
        compare(find(card(), "milLabel").text, "Check-engine light off")
        compare(find(card(), "freezeLabel").text, "None stored")
        const status = find(page, "diagnosticsStatus").text
        verify(status.indexOf("The report was saved before the clear: ") === 0, status)
        verify(status.endsWith(".jsonl"), status)
        tryVerify(() => clear.enabled, 2000, "a clear can run again")
    }

    // The fake J2534 DLL's engine turns (2000 rpm): the clear goes
    // unsent, the card says why and keeps the codes
    function test_clearWithTheFakesEngineRunningSendsNothing() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectAndRead(page, "j2534://" + Harness.fakeDllPath + "?probe=0&identify=0")
        const clear = find(page, "clearCodes")
        tryVerify(() => clear.enabled, 5000)
        clear.clicked()
        find(page, "clearConfirm").clicked()
        const card = () => find(page, "ecu_0x7E8")
        tryVerify(() => find(card(), "clearState").text !== "", 5000)
        compare(find(card(), "clearState").text,
                "Not sent: the engine is running (2000 rpm). Stop it, leave the ignition on, and clear again")
        compare(find(card(), "codes_stored").text, "P0301  P0420  U0100")
    }

    // Items whose right edge lies past page's (a Label by what it paints)
    function pastTheEdge(item, page, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(page, painted, 0).x
            if (right > page.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, page, out)
        }
        return out
    }

    // The narrowest phone, 320 px: the title's two actions and the
    // confirmation's wrap inside the page, which scrolls only up and down
    function test_theClearFitsAPhonesWidth() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        page.width = 320
        connectAndRead(page, "mock://demo")
        const clear = find(page, "clearCodes")
        tryVerify(() => clear.enabled, 2000)
        clear.clicked()
        waitForRendering(page)
        const out = pastTheEdge(page, page, [])
        compare(out.length, 0, "past 320 px: " + out.join(", "))
        find(page, "clearCancel").clicked()
    }

    function test_diagnoseOffSaysTheEcuIsNotRead() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const card = connectAndRead(page, "mock://demo?diagnose=0")
        verify(find(card, "ecuState").text.indexOf("Not read") === 0)
        verify(!find(card, "milLabel").visible, "no report to show")
        verify(!find(page, "readAgain").enabled, "nothing to read again")
        verify(!find(page, "clearCodes").enabled, "nothing to clear")
    }

    function test_lastReadKeptAfterADisconnect() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectAndRead(page, "mock://demo")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        compare(find(page, "diagnosticsStatus").text, "Disconnected: the last read is shown")
        compare(find(find(page, "ecu_0x7E8"), "codes_stored").text, "P0301  P0420  U0100")
        verify(!find(page, "readAgain").visible)
        verify(!find(page, "clearCodes").visible)
    }
}
