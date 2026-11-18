// File/layer: tests/qml/tst_settings_unloadable.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings_unloadable (test_qml)
//
// A kept plugin copy core no longer loads. test_qml stages one before the
// bridge is made (the store reads its copies once, at its first list), so
// this file's first case meets it.

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "SettingsUnloadable"
    when: windowShown

    Component {
        id: pageComponent
        SettingsPage { width: 1000; height: 1400 }
    }

    Settings {
        id: storedVehicles
        category: "vehicles"
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        storedVehicles.setValue("book", "")
        storedVehicles.setValue("document", "")
        storedVehicles.sync()
        wait(0)
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // A copy that no longer loads is listed at the Signals page with the
    // loader's words and a Remove that deletes it; a pick of the plugin
    // it was is "not in the store" and can be unpicked
    function test_aCopyThatNoLongerLoadsIsListedAndRemoved() {
        const past = String(Harness.pluginFormatPast())
        compare(Harness.storedPlugins().length, 1, "the staged copy")
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        page.open("connection")
        page.open("signals")
        const sha = Harness.storedPlugins()[0].replace(".havne-plugin.json", "")
        compare(page.picksPart.refusedPlugins.map(p => p.sha256), [sha])
        compare(page.picksPart.pluginList.filter(p => p.ok).length, 0, "none loaded")
        const row = find(page, "pluginRefused_0")
        verify(row.visible)
        const words = find(row, "pluginRefusedText").text
        verify(words.startsWith("A plugin that no longer loads: It no longer loads: "), words)
        verify(words.indexOf("format " + past) >= 0, words)

        // The plugin's pick: nothing registers, so it is missing and undone
        find(page, "vehicleNew").clicked()
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["plugin.example-sim.base"]))
        tryVerify(() => page.picksPart.missingPicks.length === 1, 1000)
        const missing = find(page, "pickMissing_0")
        compare(find(missing, "pickMissingText").text, "plugin.example-sim.base: not in the store")
        find(missing, "pickMissingUnpick").clicked()
        compare(page.picksPart.picks, [])

        const remove = find(row, "pluginRefusedRemove")
        verify(remove.enabled)
        remove.clicked()
        compare(Harness.storedPlugins(), [], "the copy is deleted")
        tryVerify(() => findChild(page, "pluginRefused_0") === null, 1000)
        compare(Bridge.plugins(), [])
    }
}
