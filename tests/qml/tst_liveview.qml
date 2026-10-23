// File/layer: tests/qml/tst_liveview.qml | Qt Quick Test, QML tier
// Testing: LiveView's trace styles keyed by model row (colour from
//          Theme.seriesColors, dashes past the palette), each row's
//          swatch under mock://demo, filled while plotted and hollow
//          while not, and the link banner's message for every state.
//          The banner on screen in each state is tst_linkhealth.qml's.

import QtQuick
import QtTest
import QtTune

TestCase {
    id: testCase
    name: "LiveView"
    when: windowShown

    Component {
        id: pageComponent
        // Tall enough that every mock row has a delegate
        LiveViewPage { width: 900; height: 1000 }
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/QtTune\//)
    }

    function cleanup() {
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }

    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function connectMock(page) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const list = findChild(page, "signalList")
        verify(list.count > 0, "the mock set decodes signals")
        tryVerify(() => list.itemAtIndex(list.count - 1) !== null, 2000, "every row has a delegate")
        return list
    }

    function swatch(list, row) {
        const s = findChild(list.itemAtIndex(row), "swatch")
        verify(s, "row " + row + " has a swatch")
        return s
    }

    function test_colourComesFromTheRow() {
        const page = createTemporaryObject(pageComponent, testCase)
        const n = Theme.seriesColors.length
        for (let row = 0; row < 3 * n; ++row)
            sameColor(page.seriesColor(row), Theme.seriesColors[row % n], "row " + row)
    }

    // Rows within the palette draw solid; past it, rows reuse its colours,
    // and no two rows that share a colour share a dash
    function test_dashesTellSharedColoursApart() {
        const page = createTemporaryObject(pageComponent, testCase)
        const n = Theme.seriesColors.length
        for (let row = 0; row < n; ++row)
            compare(page.seriesDash(row).length, 0, "row " + row + " draws solid")
        for (let a = 0; a < 3 * n; ++a) {
            for (let b = a + n; b < 3 * n; b += n)
                verify(JSON.stringify(page.seriesDash(a)) !== JSON.stringify(page.seriesDash(b)),
                       "rows " + a + " and " + b + " share a colour and a dash")
        }
    }

    function test_swatchesMatchTheirRows() {
        const page = createTemporaryObject(pageComponent, testCase)
        const list = connectMock(page)
        for (let row = 0; row < list.count; ++row) {
            const s = swatch(list, row)
            sameColor(s.trace, page.seriesColor(row), "row " + row + " trace")
            sameColor(s.color, s.trace, "row " + row + " fill")
            sameColor(s.border.color, s.trace, "row " + row + " border")
            compare(s.opacity, 1.0)
        }
    }

    // Unplotted, a row's swatch keeps its colour as an outline, and the
    // other rows keep theirs
    function test_unplottedRowHasAHollowSwatch() {
        const page = createTemporaryObject(pageComponent, testCase)
        const list = connectMock(page)
        verify(list.count > 2)

        Bridge.signalModel.toggleSubscribed(1)
        const s = swatch(list, 1)
        sameColor(s.color, "transparent", "unplotted fill")
        sameColor(s.border.color, page.seriesColor(1), "unplotted border")
        compare(s.opacity, 0.5)
        sameColor(swatch(list, 2).color, page.seriesColor(2), "row 2 fill")

        Bridge.signalModel.toggleSubscribed(1)
        sameColor(s.color, page.seriesColor(1), "plotted again")
        compare(s.opacity, 1.0)
    }

    function test_bannerText_data() {
        return [
            { tag: "quiet", state: "quiet", episodes: 0, code: 0,
              text: "Vehicle not responding — quiet bus" },
            { tag: "requests", state: "requests", episodes: 1, code: 10,
              text: "Requests failing — check the vehicle connection and ignition" },
            { tag: "fault", state: "fault", episodes: 3, code: 0,
              text: "Adapter fault — 3 episode(s), retrying with backoff" },
            { tag: "degraded", state: "degraded", episodes: 0, code: 42,
              text: "Link degraded (error 42)" },
            { tag: "unknown", state: "a later state", episodes: 0, code: 43,
              text: "Link degraded (error 43)" },
        ]
    }
    function test_bannerText(data) {
        const page = createTemporaryObject(pageComponent, testCase)
        compare(page.bannerText(data.state, data.episodes, data.code), data.text)
    }
}
