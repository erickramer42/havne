// File/layer: tests/qml/tst_diagnostics.qml | Qt Quick Test, QML tier
// Testing: the Diagnostics page (ADR-0037) against the mock's report
//          (core/src/mock_ecu.h): the empty state before any connect,
//          the MIL, the three lists' codes, eight complete monitors, the
//          freeze frame's code and values in either unit system, Read
//          again (a second read logged), diagnose=0's state, and the last
//          read kept after a disconnect. The entries themselves are
//          test_bridge's (BridgeTest.DiagnosticsEntryIsWhatThePageShows).

import QtQuick
import QtTest
import App
TestCase {
    id: testCase
    name: "Diagnostics"
    when: windowShown

    Component {
        id: pageComponent
        // Tall enough that the whole card lays out
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
        const page = createTemporaryObject(pageComponent, testCase)
        compare(Bridge.diagnostics.length, 0)
        verify(find(page, "diagnosticsStatus").text.indexOf("Connect a session") === 0)
        verify(!find(page, "readAgain").visible, "no Read again without a session")
    }

    function test_theMocksReportFillsThePage() {
        const page = createTemporaryObject(pageComponent, testCase)
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
        verify(find(page, "diagnosticsStatus").text.indexOf("can't clear") > 0,
               "the page says it only reads")

        // Imperial: the freeze frame's values convert, as Live's do. The
        // entries are rebuilt, and with them the card
        Bridge.unitSystem = 1
        const shown = () => texts(find(find(page, "ecu_0x7E8"), "freezeValues"), "freezeValue")
        tryVerify(() => shown()[3].indexOf("mph") > 0, 1000, "speed in mph")
        verify(shown()[1].indexOf("°F") > 0, shown()[1])
    }

    function test_readAgainReadsOnceMore() {
        const page = createTemporaryObject(pageComponent, testCase)
        connectAndRead(page, "mock://demo")
        const button = find(page, "readAgain")
        tryVerify(() => button.enabled, 2000, "Read again once the read has settled")
        tryVerify(() => diagnosticsLines() === 1, 2000, "the first read is logged")
        button.clicked()
        tryVerify(() => diagnosticsLines() === 2, 2000, "Read again logs a second read")
    }

    function test_diagnoseOffSaysTheEcuIsNotRead() {
        const page = createTemporaryObject(pageComponent, testCase)
        const card = connectAndRead(page, "mock://demo?diagnose=0")
        verify(find(card, "ecuState").text.indexOf("Not read") === 0)
        verify(!find(card, "milLabel").visible, "no report to show")
        verify(!find(page, "readAgain").enabled, "nothing to read again")
    }

    function test_lastReadKeptAfterADisconnect() {
        const page = createTemporaryObject(pageComponent, testCase)
        connectAndRead(page, "mock://demo")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        compare(find(page, "diagnosticsStatus").text, "Disconnected: the last read is shown")
        compare(find(find(page, "ecu_0x7E8"), "codes_stored").text, "P0301  P0420  U0100")
        verify(!find(page, "readAgain").visible)
    }
}
