// File/layer: tests/qml/tst_settings_rows.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings_rows (test_qml)
// What a plugin row of the Signals list says it reads (ADR-0064): a file on
// obd2 adds signals and replaces some of obd2's by name, and the row names
// both. Core's counts are test_bridge's (BridgeTest.VariantShape*).

import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "SettingsRows"
    when: windowShown

    Component {
        id: pageComponent
        SettingsPage { width: 1000; height: 1400 }
    }

    function row(props) {
        return Object.assign({ obd2: true, count: 0, own: 0, replacedNames: [], mine: false }, props)
    }

    function test_aRowOnNoBaseCountsItsSignals() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        compare(page.rowReads(row({ obd2: false, count: 1 })), "1 signal")
        compare(page.rowReads(row({ obd2: false, count: 3, mine: true })), "3 signals · yours")
    }

    function test_aRowOnObd2SaysWhatItAddsAndWhatItReplaces() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        compare(page.rowReads(row({ own: 1 })), "obd2 + 1 signal")
        compare(page.rowReads(row({ own: 4, mine: true })), "obd2 + 4 signals · yours")
        compare(page.rowReads(row({ replacedNames: ["Boost"] })), "obd2, Boost replaced")
        compare(page.rowReads(row({ replacedNames: ["Boost", "RPM"] })), "obd2, Boost and RPM replaced")
        compare(page.rowReads(row({ replacedNames: ["Boost", "RPM", "Vehicle Speed"] })),
                "obd2, 3 signals replaced")
        compare(page.rowReads(row({ replacedNames: ["Boost"], own: 2, mine: true })),
                "obd2, Boost replaced, 2 more · yours")
        compare(page.rowReads(row({ replacedNames: ["Boost"], own: 1 })), "obd2, Boost replaced, 1 more")
        compare(page.rowReads(row({})), "obd2 + 0 signals", "a file that changes nothing")
        compare(page.rowReads(row({ replacedNames: ["Odd %2"], own: 3 })),
                "obd2, Odd %2 replaced, 3 more", "a name is no placeholder")
    }
}
