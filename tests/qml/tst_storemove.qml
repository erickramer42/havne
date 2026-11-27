// File/layer: tests/qml/tst_storemove.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.storemove (test_qml)
// The app on an earlier version's folder after the move into the plugin store (ADR-0064 Q6). For this file test_qml stages that folder before the bridge is made (the vehicle book corpus's two-vehicles case, its signal files by their hashes, the saved addresses and the example plugin) and runs the move as main() does, since the store's mark of a finished move is read once

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "StoreMove"
    when: windowShown

    Component { id: settingsComponent; SettingsPage { width: 1000; height: 1400 } }
    Component { id: dashComponent; DashboardPage { width: 1280; height: 800 } }
    Component { id: hubComponent; SignalHub {} }
    Settings { id: storedVehicles; category: "vehicles" }
    Settings { id: storedSession; category: "session" }

    readonly property string shared: "plugin.shared-pids.base"
    readonly property string obdb: "plugin.daily-driver-obdb.base"
    readonly property string own: "plugin.daily-driver-own.base"
    readonly property string example: "plugin.example-sim.my2019"

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }

    // No case changes the store: each reads what the move made
    function cleanup() {
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // The page's gauges (GaugeBase items), by label
    function gauges(page) {
        const found = {}
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.label !== undefined && child.note !== undefined
                        && child.valueText !== undefined)
                    found[child.label] = child
                walk(child)
            }
        }
        walk(page)
        return found
    }

    // Named to run first: the pages of the later cases write the book
    // again. The move wrote the book under its own key as the corpus says,
    // left the earlier key as it was and rewrote the saved addresses
    function test_theBookIsTheCorpussAndTheEarlierKeyIsAsItWas() {
        verify(Harness.storesMoved().length > 0, "the move ran before the pages")
        storedVehicles.sync()
        storedSession.sync()
        const book = VehicleBook.parse(String(storedVehicles.value("book", "")))
        compare(book.format, 2)
        compare(book, VehicleBook.parse(Harness.vehicleBookText("two-vehicles", "expect.json")))
        compare(storedVehicles.value("document"), Harness.vehicleBookText("two-vehicles", "json"),
                "the earlier version's book is an older Havne's to read")
        compare(storedSession.value("uri"), "elm327://COM5?profile=" + shared)
        compare(JSON.parse(storedSession.value("recentUris")), ["elm327://COM5?profile=" + shared])
    }

    // A Dashboard's cells keep reading across the move: a cell naming a
    // moved file's own signal by its key, and one naming obd2's by its id,
    // bind the same rows under the plugin's profile
    function test_theDashboardBindsTheMovedFilesRefsUnderThePluginsProfile() {
        const rows = Bridge.profileSignals("mock://?profile=" + shared)
        verify(rows.some(r => r.ref === "key:shared_load"), "the file's own def keeps its key")
        const rpm = Bridge.gaugeSignals.rpm.find(ref => rows.some(r => r.ref === ref))
        verify(rpm !== undefined, "obd2's defs keep their ids")
        compare(Bridge.profileSignals("mock://?profile=" + own).filter(r => r.ref.startsWith("key:"))
                    .map(r => r.ref), ["key:own_pressure"], "the form's own variant")

        const page = createTemporaryObject(dashComponent, testCase.Window.window.contentItem)
        const doc = DashboardBook.builtIn("desktop")
        doc.pages = [{ name: "Moved", cells: [
            { kind: "digital", w: 2, h: 4, label: "Shared load", refs: ["key:shared_load"],
              unit: "%", options: {} },
            { kind: "digital", w: 2, h: 4, label: "Engine speed", refs: [rpm], unit: "RPM",
              options: {} }] }]
        page.doc = doc
        const hub = createTemporaryObject(hubComponent, testCase)
        const all = gauges(page)
        verify(all["Shared load"] && all["Engine speed"], "both cells draw a gauge")
        compare(all["Shared load"].note, "no session")

        // A recording of obd2's PIDs, which the mock never sends
        compare(Bridge.connectSession("replay://" + Harness.replayFixtures
                                      + "/2026-10-25-engine-start?profile=" + shared + "&speed=100"), "")
        tryVerify(() => hub.byRef["key:shared_load"] !== undefined, 3000, "the row is the session's")
        compare(hub.byRef["key:shared_load"].name, "Shared load")
        compare(hub.lookup(["key:shared_load"]).unit, "%")
        tryVerify(() => all["Engine speed"].hasValue, 5000, "obd2's row has a value")
        verify(all["Shared load"].note !== "not in profile", all["Shared load"].note)
    }

    // A layout file the earlier version saved names a signal file by its
    // hash: the plugin the move made of that file answers for it, so a
    // vehicle that picks it is told of nothing missing, and one that
    // doesn't is told which
    function test_aLayoutSavedBeforeTheMoveFindsItsFileUnderThePick() {
        const earlier = JSON.parse(Harness.vehicleBookText("two-vehicles", "json"))
        const file = earlier.list[0].signals.find(f => f.name === "Shared PIDs")
        verify(file !== undefined, "the earlier book names the file")
        const layout = DashboardBook.builtIn("desktop")
        layout.pages = [{ name: "Earlier", cells: [
            { kind: "digital", w: 2, h: 4, label: "Shared load", refs: ["key:shared_load"],
              unit: "%", options: {} }] }]
        const text = DashboardBook.exportText(layout, "desktop", [{ name: file.name, sha256: file.sha256 }])
        verify(text !== "")
        const url = Harness.writeScratchFile("layout-earlier.json", text)
        const book = VehicleBook.parse(Harness.vehicleBookText("two-vehicles", "expect.json"))

        const page = createTemporaryObject(dashComponent, testCase.Window.window.contentItem)
        page.vehicles = VehicleBook.pick(book, "v1")
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("doesn't pick") < 0, page.layoutFilePart.layoutNote)

        page.vehicles = VehicleBook.setPicks(VehicleBook.pick(book, "v1"), [own])
        page.layoutFilePart.loadLayoutFrom(url)   // the layout already shown: only the note is new
        verify(page.layoutFilePart.layoutNote.indexOf("plugins this vehicle doesn't pick: Shared PIDs") >= 0,
               page.layoutFilePart.layoutNote)
    }

    // The Signals page shows what the move made: every moved file a
    // plugin of the user's own, each vehicle's picks in its files' order
    // with the plugin it had picked last, the form's file as its own
    // variant, and the saved address reading the plugin's profile
    function test_thePageShowsTheMovedPicksInOrderWithTheFormsOwn() {
        const page = createTemporaryObject(settingsComponent, testCase.parent)
        verify(page, "Settings loads")
        page.open("signals")
        compare(page.pickedVehicle.label, "Weekend car")
        compare(page.picksPart.picks, ["obd2", shared])
        compare(page.rowActionsPart.unmigratedRows, [], "every file moved")
        compare(page.picksPart.pluginList.filter(p => p.mine).map(p => p.id).sort(),
                ["daily-driver-obdb", "daily-driver-own", "shared-pids"])
        verify(!page.picksPart.pluginList.find(p => p.id === "example-sim").mine, "the example is as it was")
        compare(page.picksPart.variantRows.filter(r => r.picked).map(r => r.profile), [shared])
        compare(page.ownSignals, [], "this vehicle wrote none")

        page.ownPart.pickVehicle("v1")
        compare(page.picksPart.picks, ["obd2", shared, obdb, own, example])
        compare(page.pickedVehicle.own, own)
        compare(page.ownSignals.map(s => s.key), ["own_pressure"], "the form edits its file's variant")
        compare(find(find(page, "ownSignal_0"), "ownSignalText").text, "Own pressure: 22 D110 on 7E8")
        compare(page.connectPart.uriField.text, "elm327://COM5?profile=" + shared, "the vehicle's saved address")
        compare(page.uriPart.profileChoice, shared)
        // The picks read together: one button for the vehicle's set
        compare(page.uriPart.profiles.map(p => p.name),
                ["default", page.picksPart.together.profile, "raw", "uds-generic"])
        const row = page.picksPart.variantRows.find(r => r.profile === own)
        compare(page.picksPart.rowTitle(row), "Daily driver own 1.0.0")
        compare(page.picksPart.rowReads(row), "obd2 + 1 signal · yours")
        verify(row.forCar)
        compare(page.picksPart.pluginList.find(p => p.id === "daily-driver-own").importedFor, "v1",
                "moved for this vehicle")
    }
}
