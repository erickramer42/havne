import QtCore
import QtQuick
import QtTest
import App
import AppTest

SettingsKit {
    id: testCase
    name: "SettingsTogether"

    function signalFileText(name, did, obd2) {
        const doc = {
            havne_signals: 1, name: name, source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "a", name: "A", read: { service: "22", number: did || "D100" },
                        field: { byte: 0, bits: 8 } }]
        }
        if (obd2)
            doc.extends = "obd2"
        return JSON.stringify(doc)
    }

    function importFile(page, fileName, name, did, obd2) {
        const url = Harness.writeScratchFile(fileName, signalFileText(name, did, obd2))
        verify(url.toString() !== "", "the scratch file is written")
        return page.importPart.importFile(url) && page.importPart.keepPending("", "")
    }

    function signalsRow(page) {
        return page.pageList.find(r => r.key === "signals")
    }

    function profileOf(uri) {
        const m = /[?&]profile=([^&]*)/.exec(uri)
        return m === null ? "" : m[1]
    }

    function test_theRowReadsTheVehiclesSet() {
        const page = createPage()
        press(page, "vehicleNew")
        compare(page.pickedVehicle.picks, ["obd2"])
        const field = find(page, "uriField")
        field.text = "elm327://COM9"
        compare(page.uriPart.profiles.map(p => p.name), ["default", "raw", "uds-generic"],
                "obd2 alone is what Default reads on an adapter")

        verify(importFile(page, "plus.json", "Plus", "D100", true))
        compare(page.picksPart.picks, ["obd2", "plugin.plus.base"])
        const set = page.picksPart.together
        compare(set.reads, "merged")
        verify(set.profile.startsWith("merged."), set.profile)
        compare(page.uriPart.profiles.map(p => p.name), ["default", set.profile, "raw", "uds-generic"])
        compare(find(page, "signalNote").text, "Plus 1.0.0 added: picked for Vehicle 1")
        compare(field.text, "elm327://COM9?profile=" + set.profile, "a pick under Default is read at once")
        const button = find(page, "profile_" + set.profile)
        compare(button.text, "Vehicle 1")
        verify(button.checked)
        compare(find(page, "profileHint").text,
                "obd2's standard signals and those of the plugin picked for Vehicle 1, read together")

        field.text = "elm327://COM9"
        verify(find(page, "profile_default").checked)
        compare(find(page, "profileHint").text,
                "Plain OBD-II, without the signals picked for Vehicle 1; choose Vehicle 1 to read them")
        verify(signalsRow(page).summary.endsWith("(Default reads plain OBD-II)"), signalsRow(page).summary)

        page.uriPart.chooseProfile(set.profile)
        verify(importFile(page, "more.json", "More", "D101", true))
        compare(find(page, "signalNote").text, "More 1.0.0 added: picked for Vehicle 1")
        const next = page.picksPart.together.profile
        verify(next !== set.profile, "a new pick is a new set")
        compare(field.text, "elm327://COM9?profile=" + next, "the field follows the vehicle's set")
        compare(page.uriPart.profiles.map(p => p.name), ["default", next, "raw", "uds-generic"])

        field.text = "mock://demo"
        verify(find(page, "profileHint").text.startsWith("Default reads OBD-II values"))
        verify(!signalsRow(page).summary.includes("Default reads"))
    }

    // Default is obd2 on an adapter already; a CAN interface that sends
    // reads the set, a listening one listens; a refused set refuses
    function test_aListConnectAddsTheVehiclesSet() {
        const page = createPage()
        page.canInterfaces = [{ name: "vcan0", kind: "vcan", up: true },
                              { name: "can0", kind: "can", up: true }]
        press(page, "vehicleNew")
        const from = page.connectFromPart
        compare(from.listConnectUri("elm327://COM5"), "elm327://COM5")
        compare(from.listConnectUri("socketcan://vcan0"), "socketcan://vcan0?profile=obd2")
        compare(from.listProfileWord("socketcan://vcan0"), "obd2")
        compare(from.listConnectUri("socketcan://can0"), "socketcan://can0")

        verify(importFile(page, "plus.json", "Plus", "D100", true))
        const set = page.picksPart.together.profile
        compare(from.listConnectUri("elm327://COM5"), "elm327://COM5?profile=" + set)
        compare(from.listConnectUri("socketcan://vcan0"), "socketcan://vcan0?profile=" + set)
        compare(from.listConnectUri("socketcan://can0"), "socketcan://can0")
        compare(from.listConnectUri("elm327://COM5?profile=raw"), "elm327://COM5?profile=raw")
        compare(from.listConnectUri("mock://demo"), "mock://demo")
        compare(from.connectProfileWords["socketcan://vcan0"], "Vehicle 1")

        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["obd2", "plugin.gone.base"]))
        compare(page.picksPart.together.reads, "refused")
        const words = page.picksPart.together.words
        verify(words.includes("plugin.gone.base") || words.includes("gone"), words)
        from.connectFromList("elm327://COM5")
        verify(!Bridge.connecting && !Bridge.connected, "nothing connects")
        compare(page.connectPart.errorLabel.text, words)
        verify(page.connectPart.errorLabel.visible)
        compare(from.setRefusal("elm327://COM5?profile=raw"), "", "a profile named reads as named")
    }

    // A kept merged name reads as the vehicle's set now, never dropped
    function test_aKeptListeningAddressKeepsItsSet() {
        const page = createPage()
        page.canInterfaces = [{ name: "can0", kind: "can", up: true }]
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100", true))
        const set = page.picksPart.together.profile
        const from = page.connectFromPart
        const kept = "socketcan://can0?profile=merged.0123456789abcdef"
        compare(from.listAddress(kept), kept)
        compare(from.listConnectUri(kept), "socketcan://can0?profile=" + set)
        compare(from.listProfileWord(kept), "Vehicle 1")
        compare(from.listConnectUri("mock://demo?profile=merged.0123456789abcdef"),
                "mock://demo?profile=merged.0123456789abcdef", "a typed mock reads as typed")

        const typed = "mock://demo?profile=" + set
        from.requestConnect(typed)
        tryVerify(() => Bridge.connected, 5000)
        compare(Bridge.currentUri, typed)
    }

    function test_theExportNeverListsObd2AsNotInTheStore() {
        const page = createPage()
        press(page, "vehicleNew")
        const dash = createTemporaryObject(dashComponent, testCase.parent)
        dash.vehicles = page.vehicles
        compare(VehicleBook.picked(dash.vehicles).picks, ["obd2"])
        const url = Harness.writeScratchFile("together-layout.json", "")
        verify(dash.layoutFilePart.exportLayoutTo(url), dash.layoutFilePart.layoutNote)
        verify(!dash.layoutFilePart.layoutNote.includes("aren't in the store"), dash.layoutFilePart.layoutNote)
    }
}
