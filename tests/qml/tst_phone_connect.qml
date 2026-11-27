// File/layer: tests/qml/tst_phone_connect.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.phone_connect (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

PhoneKit {
    id: testCase
    name: "Phone"

    // Dash with no session on the phone: the recent addresses, the newest
    // chosen, and one Connect; the list goes once a session runs
    // Dash's list with these recent addresses, and these Bluetooth adapters
    // in place of the machine's own interfaces and ports (none by default),
    // which a list on screen reads again
    function connectListOn(main, uris, bluetooth) {
        const dash = find(main.contentItem, "dashboardPage")
        const settings = find(main.contentItem, "settingsPage")
        dash.form = "phone"
        settings.connectFromPart.recentUris = uris
        tryVerify(() => findChild(dash, "dashConnectList") !== null, 2000, "the list loads")
        const list = findChild(dash, "dashConnectList")
        settings.j2534Devices = []
        settings.serialPorts = []
        settings.canInterfaces = []
        settings.bluetoothAdapters = bluetooth || []
        verify(waitForPolish(main), "laid out")
        return list
    }

    // Main hands both lists Settings' words: with a picked vehicle that
    // picks a plugin, an adapter's row names the vehicle whose set a
    // connect reads, and the connect puts the address in Settings' field
    // with its profile
    function test_theListsSayTheSetAConnectReads() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        verify(settings.importPart.importFile(Harness.writeExamplePlugin("list-words.havne-plugin.json")))
        try {
            settings.vehicles = VehicleBook.setPicks(VehicleBook.add(VehicleBook.empty(), "Desk car"),
                                                     ["plugin.example-sim.my2019"])
            const address = "elm327://bt:00:11:22:33:44:55"
            const list = connectListOn(main, [], [
                { name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" }])
            tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "the adapter and the demo")
            const word = "Desk car"
            const row = find(list, "connectRow_0")
            compare(find(row, "connectRowAddress").text, "OBD adapter")
            const kind = find(row, "connectRowKind")
            compare(kind.text, word, "in the kind's place on a phone")
            verify(kind.mapToItem(row, kind.width, 0).x <= row.width + 0.5, "inside its row")
            compare(row.Accessible.name, "OBD adapter, Bluetooth, " + word)
            compare(find(find(list, "connectRow_1"), "connectRowKind").text, "Demo data",
                    "the demo reads no plugin")
            compare(find(main.contentItem, "homePage").profileWords[address], word, "Home's list too")

            tryCompare(list, "chosen", address, 2000, "the sole adapter")
            find(list, "connectGo").clicked()
            compare(find(settings, "uriField").text, address + "?profile=" + settings.picksPart.together.profile,
                    "the address Settings connects")
            tryVerify(() => !Bridge.connecting, 5000, "the desktop refuses a Bluetooth link")
        } finally {
            for (const p of settings.picksPart.pluginList)
                settings.rowActionsPart.removePlugin(p.sha256)
            settings.saveVehicles(VehicleBook.empty())
        }
    }

    // The desktop's Home connects the chosen row through Settings, as
    // Dash's list does: the address in its field, then the session
    function test_homeConnectsThroughSettings() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1280
        main.height = 720
        const settings = find(main.contentItem, "settingsPage")
        const home = find(main.contentItem, "homePage")
        main.gotoPage(Array.from(home.parent.children).indexOf(home))
        settings.connectFromPart.recentUris = ["mock://demo"]
        const list = find(home, "homeConnectList")
        tryCompare(list, "chosen", "mock://demo", 2000, "the recent address")
        find(list, "connectGo").clicked()
        tryVerify(() => Bridge.connected, 5000, "it connects")
        compare(settings.connectPart.uriField.text, "mock://demo", "through Settings' field")
        tryVerify(() => find(home, "homeSession").visible, 2000, "the session's card")
    }

    // A fresh phone: no recent address, the paired adapter and a BLE one
    // on the list, neither chosen while there are two
    function test_aFreshPhoneListsItsPairedAdapters() {
        const main = phoneMain()
        const list = connectListOn(main, [], [
            { name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" },
            { name: "", address: "AA:BB:CC:DD:EE:FF", kind: "ble" }])
        tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "two rows")
        const first = find(list, "connectRow_0")
        compare(find(first, "connectRowAddress").text, "OBD adapter")
        compare(find(first, "connectRowKind").text, "Bluetooth")
        const second = find(list, "connectRow_1")
        compare(find(second, "connectRowAddress").text, "AA:BB:CC:DD:EE:FF", "no name: the address")
        compare(find(second, "connectRowKind").text, "Bluetooth LE")
        verify(!find(list, "connectRecent").visible, "no Recent group")
        verify(find(list, "connectAdapters").visible, "the Adapters group")
        compare(list.chosen, "", "two adapters: none chosen")
        verify(find(list, "connectPickHint").visible, "says to pick one")
        const go = find(list, "connectGo")
        verify(!go.enabled, "Connect waits for a pick")
        mouseClick(first)
        tryCompare(list, "chosen", "elm327://bt:00:11:22:33:44:55", 2000, "the tap")
        verify(go.enabled && go.highlighted, "then Connect is the primary")
        verify(!find(list, "connectPickHint").visible)
    }

    // Without the permission the Adapters group says why and offers Allow,
    // which asks only when tapped; with it and nothing paired, where to pair
    // one and a scan
    function test_withoutThePermissionTheListAsks() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM1"])
        compare(list.bluetoothAsks, 0, "nothing asked at the list's first show")
        list.bluetoothAvailable = true
        list.bluetoothPermission = "undetermined"
        verify(waitForPolish(main), "laid out")
        const allow = find(list, "connectAllow")
        verify(allow.visible && find(list, "connectBluetoothWhy").visible, "Allow, and why")
        compare(allow.text, "Allow")
        compare(list.bluetoothAsks, 0, "still nothing asked")
        mouseClick(allow)
        compare(list.bluetoothAsks, 1, "a tap asks")
        list.bluetoothNeedsSettings = true
        compare(allow.text, "Open settings", "asked no more: the settings")
        list.bluetoothNeedsSettings = false
        list.bluetoothPermission = "granted"
        verify(waitForPolish(main), "laid out")
        verify(!allow.visible, "allowed: no Allow")
        verify(find(list, "connectBluetoothNone").visible && find(list, "connectScan").visible,
               "nothing paired: where to pair, and Scan")
        find(main.contentItem, "settingsPage").bluetoothAdapters =
            [{ name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" }]
        tryVerify(() => !find(list, "connectBluetoothNone").visible, 2000, "one paired: its row instead")
        list.bluetoothAvailable = false
        list.bluetoothPermission = "unavailable"
    }

    // Where to pair stays within the helper text's two lines at the
    // narrowest phone, beside Scan
    function test_theNoPairedAdapterWordsFitTwoLinesAt320() {
        const main = phoneMain()
        main.width = 320
        const list = connectListOn(main, ["elm327://COM1"])
        list.bluetoothAvailable = true
        list.bluetoothPermission = "granted"
        verify(waitForPolish(main), "laid out")
        const words = find(list, "connectBluetoothNone")
        verify(words.visible, "nothing paired")
        verify(words.lineCount <= 2, "the words take " + words.lineCount + " lines at 320")
    }

    // The demo row is every user's, last under Adapters, never chosen
    // by default; a recent address of a listed device shows its name
    function test_theDemoRowIsAlwaysThereAndNeverTheDefault() {
        const main = phoneMain()
        Theme.developerMode = false
        const list = connectListOn(main, [])
        tryVerify(() => findChild(list, "connectRow_0") !== null, 2000, "the demo row")
        compare(find(find(list, "connectRow_0"), "connectRowKind").text, "Demo data")
        compare(list.chosen, "", "nothing chosen with only the demo")
        verify(!find(list, "connectRow_0").checked)
        const hint = find(list, "connectPickHint")
        verify(hint.visible, "the hint shows with nothing chosen")
        compare(hint.text, "Pick a row, then connect", "it asks for a row, not an adapter")
    }

    // Audio, wearable and health gear the phone has paired sits under a
    // fold, never among the adapters; a pick from it keeps it open
    function test_otherPairedDevicesSitUnderAFold() {
        const main = phoneMain()
        const list = connectListOn(main, [], [
            { name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" },
            { name: "Headset", address: "AA:BB:CC:DD:EE:FF", kind: "bt", other: true },
            { name: "Heart strap", address: "FF:FF:FF:FF:FF:FF", kind: "ble", other: true }])
        const fold = find(list, "connectOtherDevices")
        tryVerify(() => fold.visible, 2000, "the fold shows")
        compare(fold.text, "Other paired devices (2)")
        verify(findChild(list, "connectRow_0") !== null, "the adapter")
        compare(find(find(list, "connectRow_0"), "connectRowAddress").text, "OBD adapter")
        // The demo is the adapters' last row; the others are not drawn closed
        compare(find(find(list, "connectRow_1"), "connectRowKind").text, "Demo data")
        compare(findChild(list, "connectRow_2"), null, "the headset is hidden")
        fold.clicked()
        tryVerify(() => findChild(list, "connectRow_3") !== null, 2000, "opened")
        compare(find(find(list, "connectRow_2"), "connectRowAddress").text, "Headset")
        compare(fold.text, "Hide other paired devices (2)")
        find(list, "connectRow_2").clicked()
        tryCompare(list, "chosen", "elm327://bt:AA:BB:CC:DD:EE:FF", 2000, "still pickable")
        verify(!fold.enabled, "no Hide while the pick is inside")
        compare(fold.text, "Other paired devices (2)", "and it does not promise one")
        fold.clicked()
        tryCompare(list, "otherShown", true, 2000, "open while one of them is chosen")
        verify(findChild(list, "connectRow_2") !== null, "the picked row stays")
    }

    // A paired headset is no adapter: with only gear of that kind paired,
    // the list still says where to pair one
    function test_onlyOtherPairedDevicesStillAskForAnAdapter() {
        const main = phoneMain()
        const list = connectListOn(main, [])
        list.bluetoothAvailable = true
        list.bluetoothPermission = "granted"
        find(main.contentItem, "settingsPage").bluetoothAdapters =
            [{ name: "Headset", address: "AA:BB:CC:DD:EE:FF", kind: "bt", other: true }]
        verify(waitForPolish(main), "laid out")
        tryVerify(() => find(list, "connectBluetoothNone").visible, 2000, "where to pair, with a headset only")
        list.bluetoothAvailable = false
        list.bluetoothPermission = "unavailable"
    }

    function test_aRecentAddressOfAListedDeviceTakesItsName() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM9?profile=raw"])
        list.adapters = [{ uri: "elm327://COM9", title: "Bench adapter", kind: "ELM327 adapter" }]
        verify(waitForPolish(main), "laid out")
        const row = find(list, "connectRow_0")
        compare(find(row, "connectRowAddress").text, "Bench adapter")
        compare(find(row, "connectRowKind").text, "ELM327 adapter")
        compare(list.recentRows[0].detail.indexOf("elm327://COM9") >= 0, true, "the address is its detail")
        compare(find(list, "connectAdapters").visible, true)
    }

    // A log is a recording: Mark stands beside Stop log from 720 px of
    // header, and the readout says it is the log's time
    function test_markStandsBesideStopLogOnAWideHeader() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1280
        main.height = 800
        const start = find(main.contentItem, "startLogButton")
        const mark = find(main.contentItem, "markButton")
        const readout = find(main.contentItem, "logReadout")
        compare(Bridge.connectSession("mock://logged"), "")
        try {
            tryVerify(() => start.visible && start.enabled, 5000, "Start log")
            verify(!mark.visible, "no Mark before the log")
            start.clicked()
            verify(Bridge.logActive)
            tryVerify(() => mark.visible, 2000, "Mark beside Stop log")
            verify(/^Logging \d+:\d\d$/.test(readout.text), readout.text)
            const before = Bridge.markCount
            mark.clicked()
            tryVerify(() => Bridge.markCount === before + 1, 2000, "a marker in the log")
            main.width = 600
            waitForRendering(main.contentItem)
            verify(!mark.visible, "under 720 px of header the phone's rule holds")
            verify(!/^Logging/.test(readout.text), readout.text)
        } finally {
            if (Bridge.logActive)
                Bridge.stopLog()
        }
    }

    // With no recent address and exactly one adapter, it is chosen; the
    // demo row doesn't count
    function test_aSoleAdapterIsChosen() {
        const main = phoneMain()
        const list = connectListOn(main, [])
        try {
            find(main.contentItem, "settingsPage").serialPorts = [{ name: "COM9", device: "" }]
            tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "the port and the demo")
            compare(find(find(list, "connectRow_1"), "connectRowKind").text, "Demo data")
            compare(list.chosen, "elm327://COM9", "the one adapter")
            verify(find(list, "connectRow_0").checked, "its row marked")
            const go = find(list, "connectGo")
            verify(go.enabled && go.highlighted && go.text === "Connect", "one tap connects")
            find(main.contentItem, "settingsPage").serialPorts =
                [{ name: "COM9", device: "" }, { name: "COM8", device: "" }]
            tryCompare(list, "chosen", "", 2000, "two: none chosen")
        } finally {
            Theme.developerMode = false
        }
    }

    // An adapter that is also a recent address shows once, under Recent
    function test_anAdapterAlsoRecentShowsOnce() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM9?profile=obd2"])
        find(main.contentItem, "settingsPage").serialPorts =
            [{ name: "COM9", device: "" }, { name: "COM8", device: "" }]
        tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "the recent and COM8")
        compare(find(find(list, "connectRow_1"), "connectRowAddress").text, "COM8")
        compare(find(find(list, "connectRow_2"), "connectRowKind").text, "Demo data", "COM9 once, then the demo")
        compare(findChild(list, "connectRow_3"), null, "COM9 once")
        compare(list.chosen, "elm327://COM9?profile=obd2", "the recent one")
    }

    // One adapter connected under two profiles is two recents: on a phone
    // the profile's word stands where the kind goes, so the rows differ
    function test_twoRecentsOnOneAdapterReadDifferentOnAPhone() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM9", "elm327://COM9?profile=raw",
                                          "elm327://COM9?profile=uds-generic&did=F190:text"])
        find(main.contentItem, "settingsPage").serialPorts = [{ name: "COM9", device: "" }]
        tryVerify(() => findChild(list, "connectRow_2") !== null
                        && find(findChild(list, "connectRow_0"), "connectRowKind").text === "Default",
                  2000, "the profile's word where the kind goes")
        verify(Theme.compact, "a phone's form")
        const words = [0, 1, 2].map(i => find(findChild(list, "connectRow_" + i), "connectRowKind").text)
        compare(words, ["Default", "Raw", "UDS DIDs"])
        const names = [0, 1, 2].map(i => findChild(list, "connectRow_" + i).Accessible.name)
        verify(names[0] !== names[1] && names[1] !== names[2] && names[0] !== names[2],
               "each row has a name of its own: " + names.join(" | "))
        // an adapter with one recent keeps its kind
        list.recents = ["elm327://COM9"]
        tryVerify(() => find(findChild(list, "connectRow_0"), "connectRowKind").text !== "Default",
                  2000, "a lone recent keeps its kind")
    }

    // At most six adapter rows; the rest are Other address...'s
    function test_atMostSixAdaptersShow() {
        const main = phoneMain()
        const list = connectListOn(main, [])
        const ports = []
        for (let i = 1; i <= 8; ++i)
            ports.push({ name: "COM" + i, device: "" })
        find(main.contentItem, "settingsPage").serialPorts = ports
        tryVerify(() => findChild(list, "connectRow_5") !== null, 2000, "six rows")
        compare(find(find(list, "connectRow_6"), "connectRowKind").text, "Demo data", "the demo after six")
        compare(findChild(list, "connectRow_7"), null, "no seventh adapter")
        verify(find(list, "connectOther").visible, "the rest through Other address")
    }

    // Every row and button on the list is a touch's size, inside the glass
    function test_theListsTargetsAreATouchsSize() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM1"],
                                   [{ name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" }])
        list.bluetoothAvailable = true
        list.bluetoothPermission = "undetermined"
        verify(waitForPolish(main), "laid out")
        try {
            for (const name of ["connectRow_0", "connectRow_1", "connectAllow", "connectOther", "connectGo"]) {
                const item = find(list, name)
                const top = item.mapToItem(main.contentItem, 0, 0).y
                const bottom = item.mapToItem(main.contentItem, 0, item.height).y
                verify(item.visible && bottom - top >= 47.5, name + " is " + (bottom - top) + " tall")
            }
            const over = []
            pastTheEdge(list, main.contentItem, over)
            compare(over, [], "past the edge: " + over.join(", "))
        } finally {
            list.bluetoothAvailable = false
            list.bluetoothPermission = "unavailable"
        }
    }

    function test_dashsConnectListConnectsTheChosenAddress() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["mock://newest", "elm327://bt:00:11:22:33:44:55",
                                          "mock://older"])
        const first = find(list, "connectRow_0")
        const second = find(list, "connectRow_1")
        verify(first.checked, "the newest is chosen")
        verify(find(first, "connectRowMark").visible, "and marked")
        verify(!find(second, "connectRowMark").visible)
        compare(find(first, "connectRowAddress").text, "mock://newest")
        compare(find(first, "connectRowKind").text, "Demo data")
        compare(find(second, "connectRowAddress").text, "elm327://bt:…:44:55", "the short form")
        compare(find(list, "connectGo").text, "Connect")
        verify(find(list, "connectGo").highlighted, "a chosen row: Connect is the list's primary")

        const third = find(list, "connectRow_2")
        mouseClick(third)
        tryVerify(() => third.checked && !first.checked, 2000, "a tap chooses the row")
        mouseClick(third)
        wait(0)
        verify(third.checked, "a second tap keeps it chosen")

        mouseClick(find(list, "connectGo"))
        tryVerify(() => Bridge.connected, 5000, "the chosen address connects")
        compare(Bridge.currentUri, "mock://older")
        compare(JSON.stringify(settings.connectFromPart.recentUris), JSON.stringify(
                    ["mock://newest", "elm327://bt:00:11:22:33:44:55", "mock://older"]),
                "the mock is never kept: the list as it was")
        compare(find(settings, "uriField").text, "mock://older", "Settings shows it")
        const dash = find(main.contentItem, "dashboardPage")
        tryVerify(() => findChild(dash, "dashConnectList") === null, 2000, "a session ends the list")

        // The list comes back as it was, the tapped row still chosen
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        tryVerify(() => findChild(dash, "dashConnectList") !== null, 2000, "the list returns")
        const again = findChild(dash, "dashConnectList")
        tryVerify(() => findChild(again, "connectRow_2") !== null, 2000, "its rows")
        verify(find(again, "connectRow_2").checked, "the tap still stands")
        compare(find(find(again, "connectRow_2"), "connectRowAddress").text, "mock://older")
    }

    // Nothing in the list takes a tap while a session starts or ends
    function test_theConnectListWaitsWhileASessionStartsOrEnds() {
        const main = phoneMain()
        const dash = find(main.contentItem, "dashboardPage")
        const list = connectListOn(main, ["mock://demo"])
        const go = find(list, "connectGo")
        verify(go.enabled, "ready")
        go.clicked()
        verify(Bridge.connecting, "the connect is under way")
        verify(!go.enabled, "Connect waits")
        verify(!find(list, "connectRow_0").enabled, "a row waits")
        verify(!find(list, "connectOther").enabled, "Other address waits")
        compare(go.text, "Connecting…")
        tryVerify(() => Bridge.connected, 5000, "connected")

        Bridge.disconnectSession()
        verify(Bridge.disconnecting, "the teardown is under way")
        const back = findChild(dash, "dashConnectList")
        verify(back !== null, "the list is back at once")
        verify(!find(back, "connectGo").enabled, "Connect waits")
        verify(!find(back, "connectRow_0").enabled, "a row waits")
        compare(find(back, "connectGo").text, "Disconnecting…")
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        tryVerify(() => find(back, "connectGo").enabled, 2000, "and it's ready again")
    }

    // A relay holds the adapter: the list can't connect, as Settings can't
    function test_aRelayStopsTheListsConnect() {
        const main = phoneMain()
        const list = connectListOn(main, ["mock://demo"])
        const go = find(list, "connectGo")
        verify(go.enabled)
        const note = find(list, "connectRelayNote")
        verify(!note.visible, "nothing to say before the relay")
        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        tryVerify(() => !go.enabled, 2000, "no connect while it runs")
        tryVerify(() => note.visible, 2000, "and the list says why")
        compare(note.text, "Stop the relay to connect")
        Relay.stop()
        tryVerify(() => go.enabled, 2000, "and again after")
        verify(!note.visible, "the words go with the relay")
    }

    function test_aRelayLeavesTheLoneConnectToSettings() {
        const main = phoneMain()
        const list = connectListOn(main, [])
        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        wait(0)
        verify(find(list, "connectOther").enabled, "Other address... still opens Settings")
    }

    function vehicleWith(label, uri) {
        const doc = VehicleBook.add(VehicleBook.empty(), label)
        return uri === "" ? doc : VehicleBook.rememberUri(doc, uri)
    }

    function rowIs(list, index, uri) {
        const row = findChild(list, "connectRow_" + index)
        return row !== null && row.checked && find(row, "connectRowAddress").text === uri
    }

    // Picking a vehicle fills Settings' field, so the row Dash has chosen
    // is that vehicle's address, or a connect would save another into it
    function test_theChosenRowIsThePickedVehiclesAddress() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["elm327://COM1", "elm327://COM2"])
        verify(rowIs(list, 0, "elm327://COM1"), "no vehicle: the newest")

        settings.vehicles = vehicleWith("Daily driver", "elm327://COM2")
        tryVerify(() => rowIs(list, 1, "elm327://COM2"), 2000, "its address, not the newest")
        compare(list.chosen, "elm327://COM2")

        settings.vehicles = vehicleWith("Weekend car", "elm327://COM9")
        tryVerify(() => rowIs(list, 0, "elm327://COM9"), 2000, "listed first when it isn't recent")
        tryCompare(list, "chosen", "elm327://COM9")
        compare(JSON.stringify(settings.connectFromPart.recentUris),
                JSON.stringify(["elm327://COM1", "elm327://COM2"]),
                "listing it writes nothing to the recent addresses")

        settings.vehicles = vehicleWith("New car", "")
        tryVerify(() => rowIs(list, 0, "elm327://COM1"), 2000, "none saved: the newest")
    }

    // A tapped row is a choice, as a typed URI is in Settings, until
    // another vehicle is picked
    function test_aTappedRowOutranksTheVehiclesAddress() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["elm327://COM1", "elm327://COM2", "elm327://COM3"])
        settings.vehicles = vehicleWith("Daily driver", "elm327://COM2")
        tryVerify(() => rowIs(list, 1, "elm327://COM2"), 2000, "its address")
        mouseClick(find(list, "connectRow_0"))
        tryVerify(() => rowIs(list, 0, "elm327://COM1"), 2000, "the tap stands")
        compare(list.chosen, "elm327://COM1")
        settings.vehicles = vehicleWith("Weekend car", "elm327://COM2")
        wait(0)
        verify(rowIs(list, 0, "elm327://COM1"), "the same address changes nothing")
        settings.vehicles = vehicleWith("Other car", "elm327://COM3")
        tryVerify(() => rowIs(list, 2, "elm327://COM3"), 2000, "another vehicle's address replaces the tap")
    }

    // A list that changed under a tap, as a connect from Settings changes
    // it, goes back to the newest
    function test_aNewListDropsTheTappedRow() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["mock://first", "mock://second"])
        mouseClick(find(list, "connectRow_1"))
        tryVerify(() => rowIs(list, 1, "mock://second"), 2000, "the tap")
        settings.connectFromPart.recentUris = ["mock://third", "mock://first", "mock://second"]
        tryVerify(() => rowIs(list, 0, "mock://third"), 2000, "the newest again")
        compare(list.chosen, "mock://third")
    }

    // A build without Bluetooth has no permission to ask for: a list
    // reading it offers no Allow (Android's answers are the phone's)
    function test_aComputerHasNoBluetoothPermissionToAsk() {
        verify(!Bluetooth.available, "no Bluetooth in this build")
        compare(Bluetooth.permission, "unavailable")
    }

    // Settings' Log row opens the Log page itself, and Back returns to
    // Settings' list
    function test_backFromTheLogReturnsToSettings() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        main.gotoPage(main.settingsPage)
        const row = find(find(settings, "settingsList"), "settingsRow_log")
        tryVerify(() => row.visible && row.height > 0, 2000, "the Log row")
        verify(waitForPolish(main), "laid out")
        const at = row.mapToItem(main.contentItem, row.width / 2, row.height / 2)
        verify(at.y > 0 && at.y < main.contentItem.height, "the row on the glass at " + at.y)
        mouseClick(row)
        tryCompare(main, "currentPage", main.logPage, 2000, "the Log")
        verify(main.goBack(), "Back is taken")
        compare(main.currentPage, main.settingsPage, "back on Settings")
        compare(settings.pageKey, "", "its list")
    }

    // A failed connect on Dash's list offers the Log beside its words; the
    // Log opens on its activity at the newest line, in developer mode too,
    // and Back returns to Dash
    function test_aFailedConnectOffersTheLog() {
        const main = phoneMain()
        Theme.developerMode = true
        const log = find(main.contentItem, "logPage")
        log.view = "frames"
        try {
            const list = connectListOn(main, ["mock://demo?profile=no-such-profile"])
            const show = find(list, "connectShowLog")
            verify(!show.visible, "no failure, no Show log")
            mouseClick(find(list, "connectGo"))
            tryVerify(() => show.visible, 5000, "Show log beside the bridge's words")
            verify(show.height >= Theme.touchTarget - 0.5, "a touch's size: " + show.height)
            const right = show.mapToItem(main.contentItem, show.width, 0).x
            verify(right <= main.contentItem.width + 0.5, "inside the glass: " + right)
            mouseClick(show)
            tryCompare(main, "currentPage", main.logPage, 2000, "the Log")
            verify(!log.showingFrames, "the activity, not the frames")
            const text = find(log, "activityLog")
            compare(text.cursorPosition, text.length, "at its newest line")
            verify(main.goBack(), "Back is taken")
            compare(main.currentPage, main.dashboardPage, "back on Dash")
        } finally {
            Theme.developerMode = false
            log.view = "activity"
        }
    }

    // The Log's back arrow on the tab-bar form: a touch's size at the left
    // edge, back to the page that opened the Log, and Dash from no tab
    function test_theLogsBackArrowReturnsWhereTheLogCameFrom_data() {
        return [{ tag: "Settings", from: 6, name: "Back to Settings" },
                { tag: "Dash", from: 1, name: "Back to Dash" },
                { tag: "no tab", from: 0, name: "Back to Dash" }]
    }
    function test_theLogsBackArrowReturnsWhereTheLogCameFrom(data) {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const log = find(main.contentItem, "logPage")
        main.gotoPage(data.from)
        main.openLog()
        compare(main.currentPage, main.logPage, "the Log")
        verify(waitForPolish(main), "laid out")
        const back = find(log, "logBack")
        verify(back.visible, "the arrow beside the tab bar")
        compare(back.Accessible.name, data.name)
        const top = back.mapToItem(main.contentItem, 0, 0)
        const bottom = back.mapToItem(main.contentItem, back.width, back.height)
        verify(bottom.y - top.y >= 47.5 && bottom.x - top.x >= 47.5,
               "a touch's size: " + (bottom.x - top.x) + " x " + (bottom.y - top.y))
        verify(top.x < 24, "at the left edge: " + top.x)
        mouseClick(back)
        compare(main.currentPage, data.from > 0 ? data.from : main.dashboardPage, "back")
        if (data.from === main.settingsPage)
            compare(settings.pageKey, "", "Settings' list")
    }

    // Beside the sidebar the Log is a listed page: nothing to go back to
    function test_theLogsBackArrowHidesBesideTheSidebar() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 720
        main.height = 800
        main.openLog()
        verify(waitForPolish(main), "laid out")
        verify(!find(main.contentItem, "logBack").visible, "no arrow beside the sidebar")
        main.width = 384
        tryVerify(() => find(main.contentItem, "logBack").visible, 2000, "the arrow with the tab bar")
    }

    // At 320 px in developer mode the arrow, the title and a compact Copy
    // share the title row and the Activity | Frames switch takes a row of
    // its own under it
    function test_theLogsHeaderFitsTheNarrowestPhone() {
        const main = phoneMain()
        main.width = 320
        Theme.developerMode = true
        const log = find(main.contentItem, "logPage")
        main.openLog()
        verify(waitForPolish(main), "laid out")
        const row = find(log, "logTitleRow")
        verify(row.implicitWidth <= log.availableWidth + 0.5,
               "the title row needs " + row.implicitWidth + " of " + log.availableWidth)
        verify(find(log, "copyLog").compact, "Copy is compact")
        const rowBottom = row.mapToItem(main.contentItem, 0, row.height).y
        for (const name of ["logBack", "copyLog", "view_activity", "view_frames"]) {
            const item = find(log, name)
            const right = item.mapToItem(main.contentItem, item.width, 0).x
            verify(item.visible && right <= main.contentItem.width + 0.5, name + " ends at " + right)
        }
        const toggle = find(log, "logViewSwitch")
        verify(toggle.mapToItem(main.contentItem, 0, 0).y >= rowBottom - 0.5,
               "the switch under the title row")
        compare(pastTheEdge(log, main.contentItem, []).length, 0, "nothing past the edge")
    }

    function test_aFailedConnectFromTheListSaysWhyAndChangesNothing() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const refused = "mock://demo?profile=no-such-profile"
        const list = connectListOn(main, [refused, "mock://demo"])
        const error = find(list, "connectError")
        verify(!error.visible, "nothing to say yet")
        mouseClick(find(list, "connectGo"))
        tryVerify(() => error.visible && error.text !== "", 5000, "the bridge's words")
        verify(!Bridge.connected)
        compare(JSON.stringify(settings.connectFromPart.recentUris), JSON.stringify([refused, "mock://demo"]),
                "a failed connect writes nothing")
        const second = find(list, "connectRow_1")
        mouseClick(second)
        tryVerify(() => second.checked, 2000)
        mouseClick(find(list, "connectGo"))
        tryVerify(() => Bridge.connected, 5000, "the other connects")
    }

    function test_otherAddressOpensSettingsConnectField() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["mock://demo"])
        mouseClick(find(list, "connectOther"))
        tryCompare(main, "currentPage", main.settingsPage, 2000)
        tryCompare(settings, "shownKey", "connection", 2000)
    }

    function test_withNoRecentAddressConnectGoesToSettings() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, [])
        compare(find(find(list, "connectRow_0"), "connectRowKind").text, "Demo data", "the demo row alone")
        const go = find(list, "connectGo")
        compare(go.text, "Connect")
        verify(!go.enabled && !go.highlighted, "the demo is never chosen by default")
        mouseClick(find(list, "connectOther"))
        tryCompare(main, "currentPage", main.settingsPage, 2000)
        tryCompare(settings, "shownKey", "connection", 2000)
        verify(!Bridge.connecting && !Bridge.connected, "nothing to connect to")
    }

    function test_theConnectListGoesInEditModeAndOnTheDesktopForm() {
        const main = phoneMain()
        const list = connectListOn(main, ["mock://demo"])
        const dash = find(main.contentItem, "dashboardPage")
        verify(list.visible)
        dash.startEditing()
        tryVerify(() => findChild(dash, "dashConnectList") === null, 2000, "edit mode's own panels")
        dash.stopEditing()
        tryVerify(() => findChild(dash, "dashConnectList") !== null, 2000, "back after Done")
        dash.form = "desktop"
        tryVerify(() => findChild(dash, "dashConnectList") === null, 2000, "the desktop keeps Home")
    }

    // Edit mode's panel and the More row use the themed field: a touch's
    // height on the glass, and inside the phone's width
    function test_theDashboardsFieldsAreThemedAndATouchsSize_data() {
        return [{ tag: "384 upright", w: 384, h: 832 }, { tag: "320 upright", w: 320, h: 640 }]
    }

    function test_theDashboardsFieldsAreThemedAndATouchsSize(data) {
        const main = phoneMain()
        main.width = data.w
        main.height = data.h
        verify(waitForPolish(main), "laid out")
        const dash = find(main.contentItem, "dashboardPage")
        dash.form = "phone"
        dash.startEditing()
        dash.select(1)
        verify(waitForPolish(main), "panel laid out")
        waitForRendering(main.contentItem)
        const names = ["panelLabel", "panelCaption", "panelWarnFrom", "panelDangerFrom"]
        let shown = 0
        for (const n of names) {
            const f = find(dash, n)
            verify(f, n + " exists")
            verify(f.touchMin !== undefined, n + " is the themed field")
            if (!f.visible)
                continue
            ++shown
            verify(f.height >= Theme.touchTarget - 0.5, n + " is " + f.height + " tall")
            tryVerify(() => f.mapToItem(main.contentItem, f.width, 0).x
                            <= main.contentItem.width + 0.5, 2000,
                      n + " is past the right edge: " + f.mapToItem(main.contentItem, f.width, 0).x + " of " + main.contentItem.width)
        }
        verify(shown >= 2, "fields shown: " + shown)
        dash.moreShown = true
        waitForRendering(main.contentItem)
        const rename = find(dash, "moreRename")
        verify(rename.touchMin !== undefined, "moreRename is the themed field")
        verify(rename.height >= Theme.touchTarget - 0.5, "moreRename is " + rename.height + " tall")
    }

    // Five rows of long addresses fit a phone's width, every target a
    // touch's size on the glass, and the page scrolls to the gauges
    function test_theConnectListFitsAPhone_data() {
        return [{ tag: "384 upright", w: 384, h: 832 }, { tag: "320 upright", w: 320, h: 640 },
                { tag: "on its side", w: 832, h: 384 }]
    }

    function test_theConnectListFitsAPhone(data) {
        const main = phoneMain()
        main.width = data.w
        main.height = data.h
        verify(waitForPolish(main), "laid out")
        const uris = []
        for (let i = 0; i < 5; ++i)
            uris.push("mock://a-long-name-for-an-adapter-that-cannot-fit-in-its-row-" + i)
        const list = connectListOn(main, uris)
        tryVerify(() => list.width > 0 && find(list, "connectRow_4").width > 0, 2000)
        waitForRendering(main.contentItem)
        const out = pastTheEdge(list, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        const targets = [find(list, "connectGo"), find(list, "connectOther")]
        for (let i = 0; i < 5; ++i)
            targets.push(find(list, "connectRow_" + i))
        for (const t of targets) {
            const r = onGlass(t, main)
            verify(r.height >= 48 - 0.5, t.objectName + " is " + r.height + " tall")
            verify(r.width >= 48 - 0.5, t.objectName + " is " + r.width + " wide")
        }
        if (data.w < 500)
            verify(find(find(list, "connectRow_0"), "connectRowAddress").truncated, "the address elides")
        const view = find(main.contentItem, "dashPage_0")
        verify(view.contentHeight > list.height, "the gauges follow the list in the page")
        const under = find(view, "dashPanelSurface").mapToItem(list, 0, 0).y
        verify(under >= list.height, "the joined panel starts " + under + " down a list "
               + list.height + " tall")
    }

    // The readout's time: m:ss, then h:mm:ss past an hour
    function test_theLogsTimeReadsMinutesThenHours() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        compare(main.logTimeText(0), "0:00")
        compare(main.logTimeText(750 * 1000 + 999), "12:30")
        compare(main.logTimeText(3599 * 1000), "59:59")
        compare(main.logTimeText(3600 * 1000), "1:00:00")
        compare(main.logTimeText((10 * 3600 + 5 * 60 + 7) * 1000), "10:05:07")
        compare(main.logTimeText(-5000), "0:00", "a clock stepped back")
        compare(main.logTimeText(NaN), "0:00")
        compare(main.logTimeText(Infinity), "0:00")
        compare(main.logTimeText(undefined), "0:00")
    }

    // Diagnostics and Recordings take Settings' vehicles, so each report
    // names the picked vehicle's baseline
    function test_theReportsPagesTakeSettingsVehicles() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const folder = Harness.recordingFixtures + "/findings"
        find(main.contentItem, "settingsPage").vehicles = { salt: "", picked: "v1", list: [{
            id: "v1", label: "Daily driver", vinKey: "", uri: "", plotted: [], unplotted: [],
            picks: [], own: "", baseline: folder }] }
        compare(find(main.contentItem, "diagnosticsPage").baselineFolder, folder)
        compare(find(main.contentItem, "recordingsPage").baselineFolder, folder)
    }

    // Main hands the Dashboard Settings' picked sets and profile names, so
    // its offline list and its gauges' notes name the vehicle's plugin
    function test_theDashboardTakesSettingsPickedSetsAndNames() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const settings = find(main.contentItem, "settingsPage")
        const dash = find(main.contentItem, "dashboardPage")
        verify(settings.importPart.importFile(Harness.writeExamplePlugin("main-sets.havne-plugin.json")))
        try {
            settings.vehicles = VehicleBook.setPicks(VehicleBook.add(VehicleBook.empty(), "Desk car"),
                                                     ["plugin.example-sim.my2019"])
            compare(JSON.stringify(dash.pickedSets),
                    JSON.stringify([{ profile: "plugin.example-sim.my2019",
                                      name: "Example: the simulated car", variant: "my2019" }]))
            compare(dash.profileNames["plugin.example-sim.base"], "Example: the simulated car")
            compare(dash.offlineUri, settings.uriPart.offlineUri)
        } finally {
            for (const p of settings.picksPart.pluginList)
                settings.rowActionsPart.removePlugin(p.sha256)
        }
    }

    // Main hands the bridge the Dashboard's cells (ADR-0056's default
    // pick): one list of refs for each cell, its counterparts in the
    // built-in sets among them, a blank cell's none
    function test_theDashboardsGaugesJoinTheDefaultPick() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const dash = find(main.contentItem, "dashboardPage")
        let cells = 0
        for (const p of dash.pages)
            cells += p.cells.length
        verify(cells > 0, "the built-in layout has cells")
        compare(Bridge.pickGauges.length, cells)
        compare(dash.pickRefs.length, cells)
        const rpm = Bridge.gaugeSignals.rpm
        verify(Bridge.pickGauges.some(refs => rpm.every(r => refs.indexOf(r) >= 0)),
               "the tach's cell names the RPM of both built-in sets")
    }

    // The relay's line beside its button (ADR-0047): the traffic wraps in
    // its own width rather than running under Stop relaying
    function test_relayTrafficFitsBesideItsButton() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.developerMode = true   // after Main, which loads the stored one
        main.width = 384
        main.height = 800
        main.font.pixelSize = 16
        const page = find(main.contentItem, "settingsPage")
        main.gotoPage(stackIndex(page))
        page.open("connection")   // the relay's box is in it
        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        const traffic = find(page, "relayTraffic")
        const button = find(page, "relayButton")
        tryVerify(() => traffic.visible && button.text === "Stop relaying", 2000,
                  "the relay's line and Stop relaying are shown")
        waitForRendering(main.contentItem)
        verify(traffic.contentWidth <= traffic.width + 0.5,
               "the traffic, " + Math.round(traffic.contentWidth) + " px, fits its "
               + Math.round(traffic.width) + " px")
        verify(traffic.mapToItem(page, traffic.contentWidth, 0).x
               <= button.mapToItem(page, 0, 0).x + 0.5, "it ends before the button")
    }

    // The minimum time between a log's sweeps is kept between runs: written
    // as it changes, and the next Main gives it to the bridge
    function test_theMinimumTimeBetweenSweepsIsKept() {
        Bridge.minSweepMs = 0
        const first = createTemporaryObject(mainComponent, testCase)
        verify(first)
        Bridge.minSweepMs = 500
        tryVerify(() => storedUi("minSweepMs") === 500, 2000, "written as it changes")
        first.destroy()
        wait(0)
        Bridge.minSweepMs = 0
        const next = createTemporaryObject(mainComponent, testCase)
        verify(next)
        compare(Bridge.minSweepMs, 500, "the next run starts with it")
        Bridge.minSweepMs = 0
        tryVerify(() => storedUi("minSweepMs") === 0, 2000, "and Off is kept too")
    }

    // The desktop's phone-layout frame (B1): the page is 384 wide and
    // centered while the switch is on, and fills its area again after Done
    function test_theDesktopsPhoneFrameIsCenteredAndEndsWithDone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1100
        main.height = 800
        const dash = find(main.contentItem, "dashboardPage")
        const frame = find(main.contentItem, "dashFrame")
        main.gotoPage(1)
        verify(waitForPolish(main), "laid out")
        dash.startEditing()
        verify(waitForPolish(main), "laid out")
        compare(dash.width, frame.width)
        const sw = find(dash, "dashPhoneSwitch")
        verify(sw.visible)
        mouseClick(sw)
        verify(dash.phoneEdit, "the switch is on")
        verify(waitForPolish(main), "laid out")
        compare(dash.width, 384)
        compare(dash.x, (frame.width - 384) / 2)
        verify(find(frame, "dashFrameEdge").visible)
        dash.stopEditing()
        verify(waitForPolish(main), "laid out")
        compare(dash.width, frame.width)
        verify(!find(frame, "dashFrameEdge").visible)
    }
}
