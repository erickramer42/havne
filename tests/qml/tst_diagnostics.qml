// File/layer: tests/qml/tst_diagnostics.qml | Qt Quick Test, QML tier
// Testing: the Diagnostics page (ADR-0037) against the mock's report
//          (core/src/mock_ecu.h): the empty state before any connect,
//          the MIL, the three lists' codes, eight complete monitors, done
//          this drive cycle too, the
//          freeze frame's code and values in either unit system, Read
//          again (a second read logged), diagnose=0's state, and the last
//          read kept after a disconnect; the clear (ADR-0038): its
//          confirmation (naming the ECUs Bridge.clearTargets lists, the
//          bridge's own rule for a clear; that the title reads that list
//          and no other, by code reading: the mock has one ECU, so any
//          list names it), Cancel sending nothing, Clear once at the mock's
//          stopped engine (its first 15 s), and on Windows the fake's
//          running engine leaving the codes; at 320 x 400 the
//          confirmation scrolls, so Clear and Cancel can be reached; the
//          car's inspection line (ADR-0045), hidden until a read. The
//          entries themselves are
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
        // A knob a failed case armed never reaches the next case
        Harness.clearFakeKnobs()
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

    // ADR-0044: each titled code on a line of its own under its list; a
    // chassis or a manufacturer's code stays in the codes line alone
    function test_codesCarryTheirTitles() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const card = connectAndRead(page, "mock://demo")
        compare(find(card, "titles_stored").text,
                "P0301  Cylinder 1 Misfire Detected\n" +
                "P0420  Catalyst System Efficiency Below Threshold Bank 1\n" +
                "U0100  Lost Communication With ECM/PCM A")
        compare(find(card, "titles_pending").text, "P0171  System Too Lean Bank 1",
                "C0035 and B1001 have none")
        compare(find(card, "titles_permanent").text,
                "P0420  Catalyst System Efficiency Below Threshold Bank 1")
    }

    // ADR-0045: the car's verdict above the ECUs, hidden with nothing read
    // (the cases share the bridge, which keeps the last read: a session
    // that reads nothing empties it)
    function test_theInspectionLineJudgesTheCar() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const card = find(page, "inspectionCard")
        connectAndRead(page, "mock://demo?diagnose=0")
        verify(!card.visible, "nothing read, nothing judged")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        connectAndRead(page, "mock://demo")
        tryVerify(() => card.visible, 2000)
        compare(find(page, "inspectionText").text,
                "Would fail an emissions inspection: an ECU commands the " +
                "check-engine light on.")
        verify(find(page, "inspectionRule").text.indexOf("40 CFR 85.2222") > 0)
    }

    // ADR-0044: a typed code is named with or without a session
    function test_aTypedCodeIsNamed() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const field = find(page, "dtcLookupField")
        const result = find(page, "dtcLookupResult")
        verify(!result.visible, "nothing typed, nothing said")
        verify(find(page, "dtcLookupHint").text.indexOf("Wal33D/dtc-database ") > 0)
        field.text = "p 0301"
        compare(result.text, "P0301: Cylinder 1 Misfire Detected")
        field.text = "P1234"
        verify(result.text.indexOf("P1234: no title here") === 0, result.text)
        field.text = "P5301"
        verify(result.text.indexOf("Not a trouble code") === 0, result.text)
        field.text = ""
        verify(!result.visible)
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
        const cycle = texts(find(card, "monitors"), "monitorCycle")
        compare(cycle.length, 8, "this drive cycle, beside each")
        verify(cycle.every(s => s === "Done"), cycle.join(", "))
        compare(find(card, "freezeLabel").text, "Stored by P0301 (Cylinder 1 Misfire Detected)")
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

    // Clear codes... opens the confirmation in place and locks both buttons;
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
        compare(Bridge.clearTargets, ["0x7E8"], "the mock's one ECU")
        compare(find(page, "clearConfirmTitle").text,
                "Clear the trouble codes of ECU 0x7E8?",
                "the ECUs the bridge clears")
        const words = find(page, "clearConfirmText").text
        for (const part of ["stored and pending codes", "freeze frame",
                            "check-engine light off", "back to incomplete",
                            "Permanent codes stay", "emissions inspection",
                            "the engine off and the ignition on", "refuse it while the engine runs",
                            "asks once more before clearing a moving vehicle",
                            "saved to the recordings folder first"])
            verify(words.indexOf(part) >= 0, "the confirmation says: " + part)
        verify(words.indexOf("No codes are stored") < 0, "the mock has codes")
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
        compare(logged("ECU 0x7E8 clear: "), ["ECU 0x7E8 clear: cleared (0 rpm, 0 km/h)"], "sent once")
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

        // Nothing left to erase: a clear can still go out, and says what
        // it would do
        tryVerify(() => Bridge.clearFindsNothing, 2000)
        clear.clicked()
        const words = find(page, "clearConfirmText").text
        verify(words.indexOf("No codes are stored and the check-engine light is off: a clear now only sets the readiness monitors back to incomplete") === 0,
               words)
        verify(find(page, "clearConfirm").enabled, "and can still be sent")
        find(page, "clearCancel").clicked()
    }

    // The fake J2534 DLL's engine turns (2000 rpm) and refuses to clear
    // running (ECUIO_FAKE_CLEAR_NRC 0x22): the clear goes out on the
    // confirmation's word, and the card says why nothing was erased
    function test_clearTheFakesRunningEngineRefusesSaysWhy() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        Harness.armFakeKnob("ECUIO_FAKE_CLEAR_NRC", "0x22")
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectAndRead(page, "j2534://" + Harness.fakeDllPath + "?probe=0&identify=0")
        const clear = find(page, "clearCodes")
        tryVerify(() => clear.enabled, 5000)
        clear.clicked()
        find(page, "clearConfirm").clicked()
        const card = () => find(page, "ecu_0x7E8")
        tryVerify(() => find(card(), "clearState").text !== "", 5000)
        compare(find(card(), "clearState").text,
                "This ECU won't clear with the engine running (7F 04 22): stop and turn it off, then try again. Nothing was erased")
        compare(find(card(), "codes_stored").text, "P0301  P0420  U0100")
    }

    // Moving (ECUIO_FAKE_SPEED_KMH 48): the clear stops at the speed and
    // the page asks once more, naming it in the unit system; Cancel sends
    // nothing more, and asked again, Clear while moving clears
    function test_clearOfAMovingVehicleAsksOnceMore() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        Harness.armFakeKnob("ECUIO_FAKE_SPEED_KMH", "48")
        Bridge.unitSystem = 0 /* metric; cleanup() restores it */
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectAndRead(page, "j2534://" + Harness.fakeDllPath + "?probe=0&identify=0")
        const panel = find(page, "clearMovingPanel")
        const card = () => find(page, "ecu_0x7E8")
        for (const word of [false, true]) {
            const clear = find(page, "clearCodes")
            tryVerify(() => clear.enabled, 5000, "a clear can run")
            clear.clicked()
            find(page, "clearConfirm").clicked()
            tryVerify(() => panel.visible, 5000, "asked once more")
            compare(find(page, "clearMovingTitle").text,
                    "The vehicle reads 48 km/h. Clear ECU 0x7E8 anyway?")
            verify(!clear.enabled, "no second clear while it asks")
            compare(find(card(), "clearState").text,
                    "Not sent: the vehicle was moving (48 km/h)")
            if (!word) {
                find(page, "clearMovingCancel").clicked()
                verify(!panel.visible)
                compare(find(card(), "codes_stored").text, "P0301  P0420  U0100")
                continue
            }
            find(page, "clearMovingConfirm").clicked()
            verify(!panel.visible)
            tryVerify(() => find(card(), "clearState").text.indexOf("Cleared") === 0,
                      5000, "cleared while moving")
            tryVerify(() => find(card(), "codes_stored").text === "None", 5000)
        }
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

    // A short page, 320 x 400 (a phone on its side, or its text set
    // large): the confirmation scrolls with the page, so Clear and Cancel
    // can be reached however tall its text runs
    function test_theClearsButtonsCanBeReachedOnAShortPage() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        page.width = 320
        page.height = 400
        connectAndRead(page, "mock://demo")
        const clear = find(page, "clearCodes")
        tryVerify(() => clear.enabled, 2000)
        clear.clicked()
        waitForRendering(page)
        const flick = find(page, "diagnosticsFlick")
        for (const name of ["clearConfirm", "clearCancel"]) {
            const button = find(page, name)
            const y = button.mapToItem(flick.contentItem, 0, 0).y
            flick.contentY = Math.max(0, Math.min(y, flick.contentHeight - flick.height))
            waitForRendering(page)
            const top = button.mapToItem(page, 0, 0).y
            verify(top >= 0 && top + button.height <= page.height + 0.5,
                   name + " from " + Math.round(top) + " to "
                   + Math.round(top + button.height) + " of " + page.height)
        }
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
