// File/layer: tests/qml/tst_liveview.qml | Qt Quick Test, QML tier
// Testing: LiveView's trace styles keyed by model row (color from
//          Theme.seriesColors, dashes past the palette), each row's
//          swatch under mock://demo, filled while plotted and hollow
//          while not, a text row under uds-generic (the mock answering
//          its DIDs), and the link banner's message for every state.
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

    function test_colorComesFromTheRow() {
        const page = createTemporaryObject(pageComponent, testCase)
        const n = Theme.seriesColors.length
        for (let row = 0; row < 3 * n; ++row)
            sameColor(page.seriesColor(row), Theme.seriesColors[row % n], "row " + row)
    }

    // Rows within the palette draw solid; past it, rows reuse its colors,
    // and no two rows that share a color share a dash
    function test_dashesTellSharedColorsApart() {
        const page = createTemporaryObject(pageComponent, testCase)
        const n = Theme.seriesColors.length
        for (let row = 0; row < n; ++row)
            compare(page.seriesDash(row).length, 0, "row " + row + " draws solid")
        for (let a = 0; a < 3 * n; ++a) {
            for (let b = a + n; b < 3 * n; b += n)
                verify(JSON.stringify(page.seriesDash(a)) !== JSON.stringify(page.seriesDash(b)),
                       "rows " + a + " and " + b + " share a color and a dash")
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

    // Unplotted, a row's swatch keeps its color as an outline, and the
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

    // A text row (a uds-generic TEXT def, which the mock answers each
    // tick) shows its text where a number would go and can't be plotted;
    // the number row beside it formats as usual
    function test_textRowShowsTextAndCannotPlot() {
        const page = createTemporaryObject(pageComponent, testCase)
        compare(Bridge.connectSession("mock://demo?profile=uds-generic&did=F190:text,1E1C:u16"),
                "", "uds-generic connects")
        const list = findChild(page, "signalList")
        compare(list.count, 2, "one row per DID")
        tryVerify(() => list.itemAtIndex(1) !== null, 2000, "both rows have delegates")
        const textRow = list.itemAtIndex(0)
        const numberRow = list.itemAtIndex(1)
        verify(textRow.isText, "F190:text is a text row")
        verify(!numberRow.isText, "1E1C:u16 is a number row")
        tryVerify(() => textRow.hasValue && numberRow.hasValue, 3000, "the mock answered both DIDs")

        const textValue = findChild(textRow, "valueLabel")
        verify(textValue.text.startsWith("MOCK F190 #"), "text shown: " + textValue.text)
        compare(textValue.text, textRow.text)
        verify(!findChild(textRow, "plotCheck").enabled, "a text row can't be plotted")
        verify(!textRow.subscribed)

        const numberValue = findChild(numberRow, "valueLabel")
        verify(/^\d+/.test(numberValue.text.trim()), "a number shown: " + numberValue.text)
        verify(findChild(numberRow, "plotCheck").enabled, "a number row can be plotted")
    }

    function test_bannerText_data() {
        return [
            { tag: "quiet", state: "quiet", episodes: 0, code: 0,
              text: "Vehicle not responding — quiet bus; check the ignition" },
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
