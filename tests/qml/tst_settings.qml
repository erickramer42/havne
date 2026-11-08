// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "Settings"
    when: windowShown

    Component {
        id: pageComponent
        // Tall enough that the device list sits inside the page's
        // scroll area, which clips what's below it from a click
        SettingsPage { width: 1000; height: 1400 }
    }

    // The page's stored URI, Record switch and versions (test_qml keeps
    // them in a scratch ini), written straight to the store
    Settings {
        id: stored
        category: "session"
    }
    Settings {
        id: storedVehicles
        category: "vehicles"
    }

    // A binding error in the app's QML fails the test. Each case starts
    // with nothing stored, or the last case's URI would fill the field
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        stored.setValue("uri", "")
        stored.setValue("recording", false)
        stored.setValue("adapterVersions", "{}")
        stored.sync()
        storedVehicles.setValue("document", "")
        storedVehicles.sync()
    }

    // Every connect here is asynchronous, so a case that fails between
    // pressing Connect and the completion leaves one in flight: let it
    // land before the disconnect, or the next case meets a connected
    // bridge and fails for the wrong reason
    function cleanup() {
        Theme.compact = false
        Theme.dense = false
        Bridge.unitSystem = 0
        Bridge.recording = false
        tryVerify(() => !Bridge.connecting, 10000, "a connect in flight ends")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
        Bridge.vehicleLabel = ""
        Bridge.deleteUnusedSignalFiles([])   // the store starts empty too
        for (const p of Bridge.plugins())     // and so do the plugins
            Bridge.removePlugin(p.sha256)
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // The page goes under the window's root, not the TestCase: TestCase is
    // an invisible item, and visible reads the effective visibility, so a
    // child of it would read invisible whatever the page did. It is
    // 1000 wide, so Connection shows beside the list; a case reading what
    // another page shows opens that page first
    function createPage(asItOpens) {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        if (!asItOpens)
            page.open("connection")
        return page
    }

    // A tap on an item that takes it with a MouseArea or a Button: the
    // handler runs, as a click would run it. The page is 1000 wide in a
    // 200 px window and a mouse event outside the window's bounds goes
    // nowhere, which the list beside the pages now puts every row past
    function tap(item) {
        verify(item.enabled, "the item takes taps")
        item.clicked(item instanceof MouseArea ? null : undefined)
    }

    // A button's click, through its own handler
    function press(page, name) {
        find(page, name).clicked()
    }

    // The visible items under item whose right edge lies past column's,
    // named by objectName, text or type. A Text's (it has elide) is its
    // painted text's when wider: an unwrapped label paints past its width
    function pastTheEdge(item, column, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(column, painted, 0).x
            if (right > column.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, column, out)
        }
        return out
    }

    // A phone's width (docs/bench-sessions/2026-10-28-android.md): the
    // page scrolls only up and down, so every item lies inside its
    // column, Connect among them. 320 px is the narrowest phone, where
    // the URI field's natural 200 px and Connect don't fit side by side
    function test_everyItemFitsAPhonesWidth() {
        const page = createPage()
        page.width = 320
        // The longest vehicle label, wider than the column on any font (a
        // wider one, Linux's, put two presets past the edge)
        page.vehicles = VehicleBook.add(VehicleBook.empty(), "W".repeat(40))
        const column = find(page, "settingsConnectionPageColumn")
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        waitForPolish(column)
        compare(pastTheEdge(column, column, []).join("; "), "",
                "nothing past the column's right edge, " + column.width)
        const connect = find(page, "connectButton")
        verify(connect.mapToItem(column, connect.width, 0).x <= column.width,
               "Connect inside the column")

        // The phone run's URI, with which only "Dis" of the button showed
        find(page, "uriField").text = "elm327://192.168.4.1:23?profile=raw&poll=0x0C"
        waitForPolish(column)
        compare(pastTheEdge(column, column, []).join("; "), "",
                "nothing past the edge with a long URI")

        // Connected, the status line names it too: the third phone run's
        // page ran past the edge then (the mock's label stands in)
        find(page, "uriField").text =
            "mock://a-label-as-long-as-the-phone-runs-wifi-uri?profile=raw"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        waitForPolish(column)
        compare(pastTheEdge(column, column, []).join("; "), "",
                "nothing past the edge once connected")
    }

    // A URI isn't prose: no capital first letter, no predicted words
    // ("Replay://" is a scheme core doesn't know)
    function test_theUriFieldTakesAUriNotProse() {
        const page = createPage()
        for (const name of ["uriField", "didField"]) {
            const hints = find(page, name).inputMethodHints
            verify(hints & Qt.ImhNoAutoUppercase, name + " doesn't capitalize")
            verify(hints & Qt.ImhNoPredictiveText, name + " doesn't predict")
        }
    }

    // A phone (384 wide) opens on the list of pages, each row naming what
    // its page holds now; a row opens its page, whose back arrow returns
    function test_thePagesOpenFromTheListAndGoBack_data() {
        return [
            { tag: "connection", title: "Connection", summary: "1 found · Default",
              parts: ["deviceCount", "deviceList", "presetsToggle", "profile_raw", "uriField"] },
            { tag: "vehicle", title: "Vehicle", summary: "None",
              parts: ["vehicle_none", "vehicleLabelField", "vehicleNew", "vehicleMore"] },
            { tag: "signals", title: "Signals", summary: "pick a vehicle",
              parts: ["pluginsNone"] },
            { tag: "recording", title: "Recording", summary: "off",
              parts: ["recordBox", "recordStatus"] },
            { tag: "display", title: "Display", summary: "100 % · metric",
              parts: ["units_metric"] },
            { tag: "advanced", title: "Advanced", summary: "off", parts: ["developerBox"] },
        ]
    }
    function test_thePagesOpenFromTheListAndGoBack(data) {
        const page = createPage(true)
        page.width = 384
        page.j2534Devices = []
        page.serialPorts = [{ name: "COM5", device: "" }]
        page.canInterfaces = []
        verify(find(page, "settingsList").visible, "the list is the root")
        verify(!find(page, "settingsSideList").visible, "no side list on a phone")
        const row = find(page, "settingsRow_" + data.tag)
        compare(row.Accessible.name, data.title)
        compare(find(row, "settingsRowSummary").text, data.summary)
        for (const name of data.parts)
            verify(!find(page, name).visible, name + " not shown on the list")
        row.clicked()
        verify(!find(page, "settingsList").visible, "the page pushes over the list")
        for (const name of data.parts)
            verify(find(page, name).visible, name + " shown")
        find(find(page, "settings" + data.title.charAt(0).toUpperCase() + data.tag.slice(1) + "Page"),
             "settingsBack").clicked()
        verify(find(page, "settingsList").visible, "back to the list")
        for (const name of data.parts)
            verify(!find(page, name).visible, name + " gone again")
    }

    // The visible buttons under item, by their size. Only what has down
    // and checkable is measured (a Button, CheckBox or Switch): a MouseArea
    // row or a TapHandler is not
    function buttonsUnder(item, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible)
                continue
            if (child.down !== undefined && child.checkable !== undefined)
                out.push(child)
            buttonsUnder(child, out)
        }
        return out
    }

    // Density on a phone (qml.md): every page of Settings at 384 x 832,
    // 100 %, has buttons a touch's size and nothing past 320 px
    function test_everyPageMeetsAPhonesDensity_data() {
        return [
            { tag: "list", key: "", obj: "settingsListPage" },
            { tag: "connection", key: "connection", obj: "settingsConnectionPage" },
            { tag: "vehicle", key: "vehicle", obj: "settingsVehiclePage" },
            { tag: "signals", key: "signals", obj: "settingsSignalsPage" },
            { tag: "ownform", key: "ownform", obj: "settingsOwnFormPage" },
            { tag: "recording", key: "recording", obj: "settingsRecordingPage" },
            { tag: "display", key: "display", obj: "settingsDisplayPage" },
            { tag: "advanced", key: "advanced", obj: "settingsAdvancedPage" },
        ]
    }
    function test_everyPageMeetsAPhonesDensity(data) {
        Theme.compact = true
        Theme.dense = true
        const page = createPage(true)
        page.width = 384
        page.height = 832
        page.vehicles = VehicleBook.add(VehicleBook.empty(), "Daily driver")
        page.j2534Devices = []
        page.serialPorts = [{ name: "COM5", device: "" }]
        page.open(data.key)
        const shown = find(page, data.obj)
        verify(shown.visible, data.tag + " shows")
        waitForPolish(find(page, data.obj + "Column"))
        const buttons = buttonsUnder(shown, [])
        verify(buttons.length > 0, "buttons to measure")
        for (const b of buttons) {
            const o = b.mapToItem(null, 0, 0)
            const e = b.mapToItem(null, b.width, b.height)
            const name = b.objectName !== "" ? b.objectName : b.text
            verify(e.y - o.y >= 48 - 0.5, name + " is " + (e.y - o.y) + " tall")
            verify(e.x - o.x >= 48 - 0.5, name + " is " + (e.x - o.x) + " wide")
        }
        page.width = 320
        const column = find(page, data.obj + "Column")
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        waitForPolish(column)
        tryVerify(() => pastTheEdge(column, column, []).length === 0, 2000)
        compare(pastTheEdge(column, column, []).join("; "), "", "nothing past the column at 320")
    }

    // 900 wide and more: the list at the left and Connection open beside it
    function test_aWidePageShowsTheListBesideConnection() {
        const page = createPage(true)
        verify(find(page, "settingsSideList").visible)
        verify(!find(page, "settingsList").visible, "the phone's root list is hidden")
        verify(find(page, "uriField").visible, "Connection opens first")
        find(page, "settingsRow_display").clicked()
        verify(find(page, "units_metric").visible)
        verify(!find(page, "uriField").visible)
        verify(find(page, "settingsSideList").visible, "the list stays")
    }

    // The back key repeats the arrow where Qt delivers it; the form's back
    // lands on Signals, not the list
    function test_theBackKeyPopsAPage() {
        const page = createPage(true)
        page.width = 384
        page.open("display")
        keyClick(Qt.Key_Back)
        compare(page.pageKey, "")
        page.vehicles = VehicleBook.add(VehicleBook.empty(), "A")
        page.open("ownform")
        page.back()
        compare(page.pageKey, "signals")
    }

    // Save lands on Signals with its note and an empty form; Edit opens the
    // form filled; Add starts from an empty one, so a signal edited and
    // backed out of is not replaced by the next Save
    function test_theFormPageSavesEditsAndStartsAfresh() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        page.open("ownform")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.pageKey, "signals", "Save returns to Signals")
        compare(find(page, "formName").text, "", "the form empties")
        verify(find(page, "ownSignalsNote").text.indexOf("Trans Temp saved") === 0,
               "Signals says it saved")

        press(page, "ownEdit_0")
        compare(page.pageKey, "ownform", "Edit opens the form")
        compare(find(page, "formName").text, "Trans Temp")
        compare(page.editingKey, "trans_temp")
        page.back()
        compare(page.pageKey, "signals", "back from the form lands on Signals")

        press(page, "ownAdd")
        compare(page.pageKey, "ownform", "Add opens the form")
        compare(find(page, "formName").text, "", "Add starts empty")
        compare(page.editingKey, "")
        typeSignal(page, "Gear", "D101")
        press(page, "formSave")
        compare(page.ownSignals.map(s => s.key).sort(), ["gear", "trans_temp"], "added, not replaced")
    }

    // A recording's Replay puts its URI in the field, which is on
    // Connection: a phone lands there, not on the list or another page
    function test_aReplayOnAPhoneLandsOnConnection() {
        const page = createPage(true)
        page.width = 384
        page.open("display")
        page.pickRecording("replay://x?profile=raw")
        compare(page.pageKey, "connection")
        compare(find(page, "uriField").text, "replay://x?profile=raw")
        verify(find(page, "uriField").visible)
    }

    // The Bluetooth permission is asked for when Connection is seen: not
    // while Settings is hidden, wide or not, and not at launch
    function test_theBluetoothAskWaitsUntilConnectionIsSeen() {
        const page = createPage(true)
        page.visible = false
        const before = page.bluetoothAsks
        page.width = 384
        page.width = 1000      // crosses 900 hidden: shownKey changes
        page.open("display")
        page.open("connection")
        compare(page.bluetoothAsks, before, "hidden, no ask")
        page.visible = true
        compare(page.bluetoothAsks, before + 1, "shown on Connection: one ask")
        page.open("display")
        compare(page.bluetoothAsks, before + 1, "another page: none")
        page.open("connection")
        compare(page.bluetoothAsks, before + 2, "Connection again: one ask")
    }

    // The back key does nothing beside the list: nothing is pushed there
    function test_theBackKeyDoesNothingInTwoPanes() {
        const page = createPage(true)
        page.open("display")
        keyClick(Qt.Key_Back)
        compare(page.pageKey, "display")
        compare(page.shownKey, "display")
    }

    // A page open at a phone's width stays when the window widens past 900
    // (the arrow goes: the list is beside it) and when it narrows again
    function test_aResizeKeepsThePageShown() {
        const page = createPage(true)
        page.width = 384
        page.open("display")
        verify(find(find(page, "settingsDisplayPage"), "settingsBack").visible)
        page.width = 1000
        compare(page.shownKey, "display")
        verify(find(page, "units_metric").visible)
        verify(find(page, "settingsSideList").visible)
        verify(!find(find(page, "settingsDisplayPage"), "settingsBack").visible, "no arrow beside the list")
        page.width = 384
        compare(page.shownKey, "display")
        verify(find(page, "units_metric").visible)
        verify(!find(page, "settingsSideList").visible)
        verify(find(find(page, "settingsDisplayPage"), "settingsBack").visible)
    }

    // Connect... elsewhere opens what to connect to, at the page's top
    function test_showAdaptersOpensWhatToConnectTo() {
        const page = createPage(true)
        page.width = 384
        page.j2534Devices = []
        page.serialPorts = [{ name: "COM5", device: "" }]
        page.showAdapters()
        compare(page.pageKey, "connection")
        verify(find(page, "deviceList").visible)
        verify(!find(page, "vehicleNew").visible, "the other pages stay away")
    }

    // The bridge doesn't record a replay; the switch keeps its setting
    // for the next live session, and the line beside it says so
    function test_theRecordLineSaysAReplayIsntRecorded() {
        const page = createPage()
        const status = find(page, "recordStatus")
        Bridge.recording = true
        find(page, "uriField").text = "replay://C:/rec/x?profile=mock"
        compare(status.text, "Replays aren't recorded")
        verify(find(page, "recordBox").checked, "the setting stays")
        find(page, "uriField").text = "mock://demo"
        verify(status.text !== "Replays aren't recorded", "a live session records")
        find(page, "uriField").text = "replay://C:/rec/x"
        Bridge.recording = false
        verify(status.text !== "Replays aren't recorded", "nothing to say with Record off")
    }

    // The presets start closed (five buttons would fill a phone's page),
    // open on a tap of their row, and close again; the mock's stays out of
    // a user's view, and j2534:// is offered where there are J2534 drivers
    function test_thePresetsStartClosedAndOpenOnATap() {
        const page = createPage()
        const adapter = find(page, "adapterSection")
        const toggle = find(page, "presetsToggle")
        const names = ["preset_elm327", "preset_wifi_obdx", "preset_wifi_elm"]
        verify(toggle.visible, "the row is there")
        verify(!adapter.presetsOpen, "closed by default")
        for (const n of names.concat(["preset_j2534", "preset_mock"]))
            verify(!find(page, n).visible, n + " hidden while closed")
        compare(toggle.text, "Not listed: show")
        toggle.clicked()
        verify(adapter.presetsOpen)
        compare(toggle.text, "Not listed: hide")
        for (const n of names)
            verify(find(page, n).visible, n + " shown once open")
        verify(!find(page, "preset_mock").visible, "the mock needs developer mode")
        toggle.clicked()
        verify(!find(page, "preset_wifi_obdx").visible, "closed again")
    }

    // The row says whether it is open, for a screen reader
    function test_thePresetsRowSaysWhetherItIsOpen() {
        const page = createPage()
        const toggle = find(page, "presetsToggle")
        verify(toggle.Accessible.checkable, "it has a state")
        verify(!toggle.Accessible.checked, "closed")
        toggle.clicked()
        verify(toggle.Accessible.checked, "open")
    }

    // Back from Android's settings the page reads the permission again,
    // only while Connection is shown (the wiring to the app's state change
    // itself is the phone run's)
    function test_theBluetoothAnswerIsReadAgainOnlyWhileConnectionIsShown() {
        const page = createPage(true)
        page.width = 384
        const before = page.bluetoothRechecks
        page.recheckBluetoothIfSeen()
        compare(page.bluetoothRechecks, before, "the list is shown, not Connection")
        page.open("connection")
        page.recheckBluetoothIfSeen()
        compare(page.bluetoothRechecks, before + 1, "Connection is shown")
        const asks = page.bluetoothAsks
        page.recheckBluetoothIfSeen()
        compare(page.bluetoothAsks, asks, "a read, never an ask")
    }

    function test_j2534IsOfferedOnlyWhereTheresAJ2534Driver() {
        const page = createPage()
        const adapter = find(page, "adapterSection")
        compare(adapter.j2534Offered, Qt.platform.os === "windows",
                "by platform: only Windows has a J2534 driver")
        adapter.presetsOpen = true
        adapter.j2534Offered = false
        verify(!find(page, "preset_j2534").visible, "no J2534 drivers here")
        adapter.j2534Offered = true
        verify(find(page, "preset_j2534").visible, "Windows offers it")
    }

    // The WiFi presets name the adapter and show the address smaller
    function test_theWifiPresetsNameTheAdapterAndShowTheAddress() {
        const page = createPage()
        find(page, "adapterSection").presetsOpen = true
        compare(find(page, "preset_wifi_obdx").text, "WiFi: OBDX")
        compare(find(page, "preset_wifi_obdx").Accessible.name, "WiFi: OBDX 192.168.4.1:23")
        compare(find(page, "preset_wifi_elm").text, "WiFi: other adapters")
        compare(find(page, "preset_elm327").text, "elm327:// (type an address)")
    }

    // Open settings is the way to Android's Nearby devices switch: a
    // phone build's, where Android refuses the dialog. A computer has
    // no Bluetooth to ask for, so the button is never there (the
    // Denied-then-ask path itself needs Android: the phone run)
    function test_openSettingsIsNeverOnAComputer() {
        const page = createPage()
        verify(!Bluetooth.available, "no Bluetooth on this build")
        verify(!Bluetooth.needsSettings)
        verify(!find(page, "bluetoothSettingsButton").visible)
    }

    // WiFi adapters aren't discovered: the two common addresses are
    // presets, and the field's query stays
    function test_wifiPresetsFillTheirAddress() {
        const page = createPage()
        const uri = find(page, "uriField")
        press(page, "preset_wifi_obdx")
        compare(uri.text, "elm327://192.168.4.1:23")
        press(page, "profile_raw")
        press(page, "preset_wifi_elm")
        compare(uri.text, "elm327://192.168.0.10:35000?profile=raw")
    }

    // A preset or row of another scheme keeps the query's keys that
    // scheme takes and drops the rest, and the mock profile on an
    // adapter; one of the same scheme keeps the query whole
    function test_anotherSchemeDropsWhatItCantUse() {
        const page = createPage()
        const uri = find(page, "uriField")

        type(uri, "replay://C:/rec/x?profile=mock&speed=10&loop=1")
        press(page, "preset_wifi_obdx")
        compare(uri.text, "elm327://192.168.4.1:23", "a replay's keys and the mock profile go")
        verify(find(page, "profile_default").highlighted)

        type(uri, "replay://C:/rec/x?profile=uds-generic&did=1E1C:u16&speed=max")
        press(page, "preset_j2534")
        compare(uri.text, "j2534://?profile=uds-generic&did=1E1C:u16",
                "an adapter takes the recorded profile and DIDs")

        type(uri, "elm327://COM5?profile=raw&poll=0x0C&serial=1")
        press(page, "preset_mock")
        compare(uri.text, "mock://demo?profile=raw", "the mock takes no adapter's keys")
        press(page, "preset_wifi_obdx")
        compare(uri.text, "elm327://192.168.4.1:23?profile=raw")

        // A key outside the page's list (core's to judge) stays too
        type(uri, "elm327://COM5?poll=0x0C&later=1")
        press(page, "preset_wifi_elm")
        compare(uri.text, "elm327://192.168.0.10:35000?poll=0x0C&later=1",
                "the same scheme keeps the query whole")
    }

    // The Units row sets the bridge's unit system, connected or not, and
    // shows the one in force
    function test_unitsRowSetsTheBridge() {
        const page = createPage()
        const metric = find(page, "units_metric")
        const imperial = find(page, "units_imperial")
        compare(Bridge.unitSystem, 0)
        verify(metric.highlighted && !imperial.highlighted)
        press(page, "units_imperial")
        compare(Bridge.unitSystem, 1)
        verify(imperial.highlighted && !metric.highlighted)

        compare(Bridge.connectSession("mock://demo"), "", "connects")
        verify(metric.enabled, "units change while connected")
        press(page, "units_metric")
        compare(Bridge.unitSystem, 0)
        verify(metric.highlighted)
    }

    function test_presetsProfilesAndDidsComposeTheUri() {
        const page = createPage()
        const uri = find(page, "uriField")
        const did = find(page, "didField")
        const didRow = find(page, "didRow")
        compare(uri.text, "", "nothing chosen, nothing composed")
        verify(!didRow.visible, "the DID field belongs to uds-generic")

        press(page, "preset_mock")
        compare(uri.text, "mock://demo")
        press(page, "profile_raw")
        compare(uri.text, "mock://demo?profile=raw")
        press(page, "profile_uds-generic")
        compare(uri.text, "mock://demo?profile=uds-generic", "no DIDs yet: core will say so")
        verify(didRow.visible)
        did.text = " 1E1C:u16, F190:text "
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16,F190:text",
                "the DIDs as typed, minus whitespace")

        press(page, "preset_j2534")
        compare(uri.text, "j2534://?profile=uds-generic&did=1E1C:u16,F190:text",
                "a new base keeps the profile and the DIDs")
        press(page, "profile_raw")
        compare(uri.text, "j2534://?profile=raw",
                "core gives raw on an adapter the engine ECU's channel")
        verify(!didRow.visible)
        press(page, "profile_default")
        compare(uri.text, "j2534://", "the default profile needs no query")

        // A hand-written URI connects as written; the next choice composes
        // again from it, keeping its other keys in place
        uri.text = "j2534://Vendor?poll=0x0C"
        press(page, "profile_uds-generic")
        compare(uri.text, "j2534://Vendor?poll=0x0C&profile=uds-generic&did=1E1C:u16,F190:text")
        compare(did.text, " 1E1C:u16, F190:text ", "the field keeps what was typed")
    }

    // Serial ports join the device list after the interfaces; the
    // elm327:// preset gives the scheme alone and a port's row the whole
    // base, keeping the query either way
    function test_serialPortRowsFillAnElm327Uri() {
        const page = createPage()
        const uri = find(page, "uriField")
        const list = find(page, "deviceList")
        const count = find(page, "deviceCount")

        page.j2534Devices = []
        page.serialPorts = []
        page.canInterfaces = []
        verify(count.text.indexOf("None found") >= 0, "the empty state: " + count.text)
        verify(!list.visible)

        page.j2534Devices = [{ vendorName: "Fake", dllPath: "C:/drivers/fake.dll",
                               architecture: "x64" }]
        page.serialPorts = [{ name: "COM5", device: "\\Device\\BthModem0" },
                            { name: "/dev/ttyUSB0", device: "" }]
        compare(list.count, 3)
        verify(count.text.indexOf("J2534 interfaces: 1, serial ports: 2") >= 0, count.text)
        tryVerify(() => list.itemAtIndex(2) !== null, 2000, "the rows have delegates")
        compare(find(list.itemAtIndex(0), "deviceTitle").text, "Fake  (x64)")
        compare(find(list.itemAtIndex(1), "deviceTitle").text, "COM5")
        compare(find(list.itemAtIndex(1), "deviceDetail").text,
                "Serial port, for elm327:// · \\Device\\BthModem0")
        compare(find(list.itemAtIndex(2), "deviceDetail").text, "Serial port, for elm327://")

        press(page, "preset_elm327")
        compare(uri.text, "elm327://", "the port is the user's pick")
        press(page, "profile_raw")
        compare(uri.text, "elm327://?profile=raw")
        tap(find(list.itemAtIndex(1), "deviceRowArea"))
        compare(uri.text, "elm327://COM5?profile=raw", "the row's base, the query kept")
        tap(find(list.itemAtIndex(2), "deviceRowArea"))
        compare(uri.text, "elm327:///dev/ttyUSB0?profile=raw")
        tap(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "j2534://Fake?profile=raw")
    }

    // The phone's Bluetooth adapters join the list after the ports
    // (ADR-0028 section 2): a paired classic one fills elm327://bt:<address>, a
    // BLE one elm327://ble:<address>, the query kept; the count names
    // them. The desktop lists none, so the test sets them
    function test_bluetoothRowsFillAStreamUri() {
        const page = createPage()
        const uri = find(page, "uriField")
        const list = find(page, "deviceList")
        const count = find(page, "deviceCount")
        page.j2534Devices = []
        page.serialPorts = []
        page.canInterfaces = []
        page.bluetoothAdapters = [
            { name: "OBDLink MX+", address: "00:11:22:33:44:55", kind: "bt" },
            { name: "", address: "AA:BB:CC:DD:EE:FF", kind: "ble" }]
        compare(list.count, 2)
        verify(count.text.indexOf("Bluetooth: 2") >= 0, count.text)
        tryVerify(() => list.itemAtIndex(1) !== null, 2000, "the rows have delegates")
        compare(find(list.itemAtIndex(0), "deviceTitle").text, "OBDLink MX+")
        compare(find(list.itemAtIndex(0), "deviceDetail").text,
                "Bluetooth, paired · 00:11:22:33:44:55")
        compare(find(list.itemAtIndex(1), "deviceTitle").text, "AA:BB:CC:DD:EE:FF",
                "no name: the address")
        compare(find(list.itemAtIndex(1), "deviceDetail").text,
                "Bluetooth LE · AA:BB:CC:DD:EE:FF")

        press(page, "profile_raw")
        tap(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "elm327://bt:00:11:22:33:44:55?profile=raw")
        tap(find(list.itemAtIndex(1), "deviceRowArea"))
        compare(uri.text, "elm327://ble:AA:BB:CC:DD:EE:FF?profile=raw")

        // The row in the field is outlined, whatever the query
        compare(list.itemAtIndex(0).border.width, 0)
        verify(list.itemAtIndex(1).border.width > 0, "the chosen row is outlined")
        uri.text = "elm327://bt:00:11:22:33:44:55"
        verify(list.itemAtIndex(0).border.width > 0, "a typed URI outlines its row")
        compare(list.itemAtIndex(1).border.width, 0)
    }

    // A machine's CAN interfaces join the list after the ports (ADR-0032
    // section 7): a row fills socketcan://<name>, and its line says why core
    // would refuse it before a connect is tried. Only Linux lists any, so
    // the test sets them
    function test_canRowsFillASocketcanUri() {
        const page = createPage()
        const uri = find(page, "uriField")
        const list = find(page, "deviceList")
        const count = find(page, "deviceCount")
        page.j2534Devices = []
        page.serialPorts = []
        page.canInterfaces = [
            { name: "vcan0", kind: "vcan", up: true, listenOnly: false, fd: true, bitrate: 0,
              state: "", modes: [], modesKnown: false, modesSupported: [] },
            { name: "can0", kind: "can", up: true, listenOnly: false, fd: false, bitrate: 500000,
              state: "error-active", modes: [], modesKnown: false, modesSupported: [] },
            { name: "can1", kind: "can", up: true, listenOnly: true, fd: false, bitrate: 500000,
              state: "error-active", modes: ["listen-only"], modesKnown: false, modesSupported: [] }]
        compare(list.count, 3)
        verify(count.text.indexOf("CAN interfaces: 3") >= 0, count.text)
        tryVerify(() => list.itemAtIndex(2) !== null, 2000, "the rows have delegates")
        compare(find(list.itemAtIndex(0), "deviceTitle").text, "vcan0")
        compare(find(list.itemAtIndex(0), "deviceDetail").text,
                "Virtual CAN bus, for socketcan://")
        compare(find(list.itemAtIndex(1), "deviceDetail").text,
                "CAN interface at 500000 bit/s, neither listen-only nor one-shot: socketcan:// refuses it until it is one")
        compare(find(list.itemAtIndex(2), "deviceDetail").text,
                "CAN interface, listen-only at 500000 bit/s, for listening")
        compare(page.canDetail({ name: "vcan1", kind: "vcan", up: false }),
                "Virtual CAN bus, down: sudo ip link set vcan1 up")
        compare(page.canDetail({ name: "can2", kind: "can", up: false }),
                "CAN interface, down: bring it up with ip link, listen-only to listen or one-shot to send requests")

        // Only the modes the driver offers are named, when the kernel says
        // (the USB2CAN offers no one-shot); the state past error-active
        const usb2can = { name: "can3", kind: "can", up: true, bitrate: 500000, state: "error-passive",
                          modes: [], modesKnown: true,
                          modesSupported: ["loopback", "listen-only", "cc-len8-dlc"] }
        compare(page.canDetail(usb2can),
                "CAN interface at 500000 bit/s, error-passive, not listen-only: its driver offers no one-shot mode, so bring it up listen-only to listen")
        usb2can.up = false
        compare(page.canDetail(usb2can),
                "CAN interface, down: bring it up listen-only with ip link (its driver offers no one-shot mode, so it only listens)")
        compare(page.canDetail({ name: "can4", kind: "can", up: false, modesKnown: true,
                                 modesSupported: ["one-shot"] }),
                "CAN interface, down: bring it up one-shot with ip link (its driver offers no listen-only mode, so it only sends requests)")
        compare(page.canDetail({ name: "can5", kind: "can", up: true, modesKnown: true,
                                 modesSupported: ["berr-reporting"] }),
                "CAN interface: its driver offers neither listen-only nor one-shot mode, which socketcan:// needs")
        compare(page.canDetail({ name: "can6", kind: "can", up: true, bitrate: 250000, state: "bus-off",
                                 modes: ["one-shot"], modesKnown: true,
                                 modesSupported: ["listen-only", "one-shot"] }),
                "CAN interface, one-shot at 250000 bit/s, bus-off, for requests")

        // SocketCAN asks an ECU through core's ISO-TP under a profile that
        // requests (ADR-0040): uds-generic stays with its request keys; under
        // Default, raw there, which only listens, the request keys go
        uri.text = "j2534://?profile=uds-generic&did=1E1C:u16&identify=0"
        compare(page.profileChoice, "uds-generic")
        tap(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "socketcan://vcan0?profile=uds-generic&did=1E1C:u16&identify=0")
        compare(page.profileChoice, "uds-generic")
        uri.text = "j2534://?identify=0&poll=0C&baud=500000"
        compare(page.profileChoice, "default")
        tap(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "socketcan://vcan0?baud=500000")
        press(page, "profile_raw")
        tap(find(list.itemAtIndex(2), "deviceRowArea"))
        compare(uri.text, "socketcan://can1?baud=500000&profile=raw", "the row's base, the query kept")
    }

    // On a phone the field and Connect are scrolled out of view by the
    // time a device row is reached: picking one brings them back, so the
    // pick doesn't seem to do nothing (docs/bench-sessions/2026-10-28-android-7.md)
    function test_aPickedRowScrollsTheFieldIntoView() {
        const page = createPage()
        page.width = 360
        page.height = 400
        page.j2534Devices = []
        page.serialPorts = []
        page.canInterfaces = []
        page.bluetoothAdapters = [{ name: "OBDLink MX+", address: "00:11:22:33:44:55", kind: "bt" }]
        const flick = find(page, "settingsConnectionPage")
        const list = find(page, "deviceList")
        const uri = find(page, "uriField")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        waitForPolish(find(page, "settingsConnectionPageColumn"))

        // The list at the top of the view, the field above it
        flick.contentY = list.mapToItem(flick.contentItem, 0, 0).y
        verify(uri.mapToItem(flick, 0, 0).y < 0, "the field is out of view")
        tap(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "elm327://bt:00:11:22:33:44:55")
        const top = uri.mapToItem(flick, 0, 0).y
        verify(top >= 0 && top + uri.height <= flick.height, "the field is in view, at " + top)

        // A field already in view stays where it is
        flick.contentY = 0
        page.showUriField()
        compare(flick.contentY, 0)
    }

    // A computer reaches a Bluetooth adapter through its serial port: a
    // bt: URI is refused before core is asked, and the error line says
    // what to do instead
    function test_aBluetoothUriIsRefusedOnTheDesktop() {
        if (Bluetooth.available)
            skip("this build reaches Bluetooth")
        const page = createPage()
        const error = find(page, "errorLabel")
        find(page, "uriField").text = "elm327://bt:00:11:22:33:44:55"
        press(page, "connectButton")
        tryVerify(() => error.visible, 5000, "refused")
        verify(error.text.indexOf("Bluetooth links open in the Android app") >= 0, error.text)
        verify(!Bridge.connected)
        verify(!find(page, "bleScanButton").visible, "no scan without Bluetooth")
    }

    // A recording's replay (the Recordings page's Replay, through Main)
    // fills the field under the profile and DIDs it was recorded with
    // (test_qml's recordings go to a scratch folder)
    function test_aRecordingsReplayFillsTheFieldAndConnects() {
        const page = createPage()
        const uri = find(page, "uriField")

        Bridge.recording = true
        uri.text = "mock://recorded?profile=uds-generic&did=F190:text"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        // Connected comes before the first frame: a recording that ends
        // with none is refused as a replay ("holds no frames")
        tryVerify(() => Bridge.frameCount > 0, 10000, "a frame is recorded")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.recordingActive, 10000, "the recording ends")
        const row = Bridge.recentRecordings(1)[0]
        compare(row.source, "mock://recorded?profile=uds-generic&did=F190:text")

        press(page, "profile_raw")
        page.pickRecording(row.uri)
        verify(uri.text.startsWith("replay://")
               && uri.text.endsWith("?profile=uds-generic&did=F190:text"), uri.text)
        verify(find(page, "profile_uds-generic").highlighted, "the row reads its profile back")
        compare(find(page, "didField").text, "F190:text")

        Bridge.recording = false
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the replay connects as it stands")
        compare(Bridge.currentUri, uri.text)
    }

    // A URI the page didn't compose: typed, pasted or set from outside
    // (UI Automation's SetValue is a plain set, as this is)
    function type(uri, text) {
        uri.text = text
    }

    // A typed URI sets the row, the hint and the DID field, and is left as
    // typed; the next click composes from those choices, so typed DIDs
    // survive a trip through another profile
    function test_aTypedUriSetsTheRow() {
        const page = createPage()
        const uri = find(page, "uriField")
        const did = find(page, "didField")
        const didRow = find(page, "didRow")
        const hint = find(page, "profileHint")

        type(uri, "mock://demo?profile=uds-generic&did=1E1C:u16")
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16", "left as typed")
        compare(page.profileChoice, "uds-generic")
        verify(find(page, "profile_uds-generic").highlighted)
        verify(!find(page, "profile_default").highlighted)
        verify(didRow.visible)
        compare(did.text, "1E1C:u16")
        verify(hint.text.indexOf("Service 0x22") >= 0, "the hint is uds-generic's: " + hint.text)

        press(page, "profile_default")
        compare(uri.text, "mock://demo")
        press(page, "profile_uds-generic")
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16", "the DIDs are back")

        type(uri, "j2534://Vendor?profile=raw&filter=0x7E9")
        compare(page.profileChoice, "raw")
        verify(find(page, "profile_raw").highlighted)
        verify(!didRow.visible)
        compare(uri.text, "j2534://Vendor?profile=raw&filter=0x7E9", "left as typed")
        press(page, "profile_default")
        compare(uri.text, "j2534://Vendor?filter=0x7E9", "a typed filter stays")

        // A profile the row doesn't offer: no button lit, the hint says so
        type(uri, "j2534://Vendor?profile=obd2")
        compare(page.profileChoice, "obd2")
        verify(!find(page, "profile_default").highlighted)
        verify(!find(page, "profile_raw").highlighted)
        verify(hint.text.indexOf("obd2") >= 0, "the hint names it: " + hint.text)
        type(uri, "j2534://Vendor")
        compare(page.profileChoice, "default")
        verify(find(page, "profile_default").highlighted)
    }

    // A DID edit changes did= only; keys typed by hand stay where they were
    function test_didEditsKeepTheOtherKeys() {
        const page = createPage()
        const uri = find(page, "uriField")
        const did = find(page, "didField")
        type(uri, "j2534://V?identify=0&profile=uds-generic&did=1E1C")
        compare(did.text, "1E1C")
        did.text = "1E1C, F190:text"
        compare(uri.text, "j2534://V?identify=0&profile=uds-generic&did=1E1C,F190:text")
        did.text = ""
        compare(uri.text, "j2534://V?identify=0&profile=uds-generic", "no DIDs, no did= key")
        press(page, "preset_mock")
        compare(uri.text, "mock://demo?profile=uds-generic",
                "a new scheme keeps the keys it takes: the mock takes no identify=")
    }

    // diagnose= (ADR-0037) is a key the mock takes too: it survives the
    // move from an adapter to the simulated car
    function test_diagnoseOffCrossesToTheMock() {
        const page = createPage()
        const uri = find(page, "uriField")
        type(uri, "j2534://V?identify=0&diagnose=0")
        press(page, "preset_mock")
        compare(uri.text, "mock://demo?diagnose=0")
    }

    // The vehicle run sheet's URIs, typed, pass through untouched
    function test_runSheetUrisPassThroughUntouched_data() {
        return [
            { tag: "bare", uri: "j2534://OBDX Pro - VX", profile: "default" },
            { tag: "raw", uri: "j2534://OBDX Pro - VX?profile=raw&filter=0x7E8", profile: "raw" },
            { tag: "poll", uri: "j2534://OBDX Pro - VX?poll=0x0C", profile: "default" },
            { tag: "bitmap", uri: "j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x00",
              profile: "raw" },
        ]
    }
    function test_runSheetUrisPassThroughUntouched(data) {
        const page = createPage()
        const uri = find(page, "uriField")
        type(uri, data.uri)
        compare(uri.text, data.uri, "left as typed")
        compare(page.profileChoice, data.profile)
        // The same row clicked again changes nothing
        press(page, "profile_" + data.profile)
        compare(uri.text, data.uri, "the row's own click keeps it")
    }

    // The page adds and takes no key of any scheme's: raw composes no
    // filter on any base (a replay takes none), and a typed filter stays
    // through every choice; core judges the keys a scheme takes
    function test_noSchemesKeysAddedOrTaken() {
        const page = createPage()
        const uri = find(page, "uriField")
        type(uri, "replay://C:/recordings/20261025-164746")
        press(page, "profile_raw")
        compare(uri.text, "replay://C:/recordings/20261025-164746?profile=raw",
                "a replay takes no filter")
        type(uri, "j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x00")
        press(page, "profile_default")
        compare(uri.text, "j2534://OBDX Pro - VX?filter=0x7E8&poll=0x00")
        press(page, "profile_raw")
        compare(uri.text, "j2534://OBDX Pro - VX?filter=0x7E8&poll=0x00&profile=raw")
    }

    // Query metacharacters can't be typed into the DID field, so a DID
    // list can never split the URI into other keys; core judges the rest
    function test_didFieldRefusesQueryCharacters() {
        const page = createPage()
        const did = find(page, "didField")
        press(page, "preset_mock")
        press(page, "profile_uds-generic")
        did.forceActiveFocus()
        tryVerify(() => did.activeFocus, 1000, "the DID field has focus")
        keyClick("1"); keyClick("E"); keyClick("1"); keyClick("C")
        compare(did.text, "1E1C")
        keyClick("&"); keyClick("="); keyClick("?"); keyClick("#")
        compare(did.text, "1E1C", "the query characters are refused")
        keyClick(":"); keyClick("u"); keyClick("1"); keyClick("6")
        keyClick(","); keyClick(" "); keyClick("F"); keyClick("1"); keyClick("9"); keyClick("0")
        keyClick("-"); keyClick("F"); keyClick("1"); keyClick("9"); keyClick("1")
        compare(did.text, "1E1C:u16, F190-F191", "the grammar's characters all pass")
        compare(find(page, "uriField").text, "mock://demo?profile=uds-generic&did=1E1C:u16,F190-F191")
    }

    // The error line shows core's words for a DID list it refuses, and
    // for the profile chosen with no DIDs at all
    function test_coresRefusalShowsInTheErrorLine() {
        const page = createPage()
        const error = find(page, "errorLabel")
        press(page, "preset_mock")
        press(page, "profile_uds-generic")
        verify(!error.visible)

        press(page, "connectButton")
        tryVerify(() => error.visible, 5000, "a refusal arrives")
        verify(error.text.indexOf("needs ?did= to say which DIDs to read") >= 0,
               "core's words: " + error.text)
        verify(!Bridge.connected)

        find(page, "didField").text = "1E1C:u12"
        press(page, "connectButton")
        tryVerify(() => error.visible && error.text.indexOf("u12") >= 0, 5000,
                  "the bad type is named: " + error.text)
        verify(error.text.indexOf("?did= needs") >= 0, "core's words: " + error.text)
        verify(!Bridge.connected)
    }

    function test_composedUdsGenericUriConnectsThroughTheMock() {
        const page = createPage()
        const error = find(page, "errorLabel")
        press(page, "preset_mock")
        press(page, "profile_uds-generic")
        find(page, "didField").text = "1E1C:u16"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "the composed URI connects")
        compare(Bridge.currentUri, "mock://demo?profile=uds-generic&did=1E1C:u16")
        verify(!error.visible, "no error line: " + error.text)
        compare(Bridge.signalModel.rowCount(), 1, "one def per DID")
        verify(!find(page, "didField").enabled, "the choices are fixed while connected")
        verify(!find(page, "profile_raw").enabled)
        verify(!find(page, "preset_mock").enabled)
        compare(find(page, "connectButton").text, "Disconnect")
    }

    // A restart gets back the last URI that connected and the Record
    // switch. Recording is switched on only after the connects, so no
    // session here records
    function test_theLastConnectedUriAndTheRecordSwitchComeBack() {
        let page = createPage()
        const error = find(page, "errorLabel")
        find(page, "uriField").text = "mock://demo?profile=raw"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "the URI connects")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")

        find(page, "uriField").text = "mock://demo?profile=uds-generic"
        press(page, "connectButton")
        tryVerify(() => error.visible, 5000, "core refuses it: no ?did=")
        Bridge.recording = true
        verify(find(page, "recordBox").checked)

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)
        Bridge.recording = false   // a fresh bridge starts with it off

        page = createPage()
        compare(find(page, "uriField").text, "mock://demo?profile=raw",
                "the URI that connected, not the refused one")
        verify(find(page, "profile_raw").highlighted, "read back into the row")
        verify(Bridge.recording, "the Record switch comes back")
        verify(find(page, "recordBox").checked)
    }

    function versionsOf(page, index) {
        const list = find(page, "deviceList")
        tryVerify(() => list.itemAtIndex(index) !== null, 2000, "the row has a delegate")
        const label = find(list.itemAtIndex(index), "deviceVersions")
        return label.visible ? label.text : ""
    }

    function listDevices(page, vendorName) {
        page.j2534Devices = [{ vendorName: vendorName, dllPath: "C:/drivers/fake.dll",
                               architecture: "x64" }]
        page.serialPorts = [{ name: "COM5", device: "" }]
    }

    // A connect keeps its adapter's versions for that row; a port shows
    // whatever answered there last, and nothing comes from a session with
    // no adapter
    function test_aDeviceShowsTheVersionsLastSeenThere() {
        let page = createPage()
        listDevices(page, "Fake")
        compare(versionsOf(page, 0), "", "nothing seen yet")
        compare(versionsOf(page, 1), "")

        page.rememberVersions("elm327://COM5?profile=raw", "ELM327 v1.4b, STN2255 v5.12.2")
        page.rememberVersions("mock://demo", "")
        compare(versionsOf(page, 1), "last seen: ELM327 v1.4b, STN2255 v5.12.2",
                "keyed by the URI's base, its query left out")
        page.rememberVersions("elm327://COM5", "ELM327 V2.1")
        compare(versionsOf(page, 1), "last seen: ELM327 V2.1", "another adapter on the port")
        compare(versionsOf(page, 0), "")

        page.destroy()
        wait(10)
        page = createPage()
        listDevices(page, "Fake")
        compare(versionsOf(page, 1), "last seen: ELM327 V2.1", "kept across a restart")
    }

    function test_aDamagedVersionStoreShowsNone() {
        stored.setValue("adapterVersions", "not json")
        stored.sync()
        const page = createPage()
        listDevices(page, "Fake")
        compare(versionsOf(page, 1), "")
    }

    function test_aConnectKeepsTheAdaptersVersions() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        listDevices(page, Harness.fakeDllPath)   // a row whose base is the URI's
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath + "?profile=raw"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        compare(versionsOf(page, 0),
                "last seen: firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        compare(versionsOf(page, 0),
                "last seen: firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04",
                "still shown with no session")
    }

    function labels(page) {
        return page.vehicles.list.map(v => v.label)
    }

    // ADR-0043: a typed VIN is decoded on the device, by the bridge's one
    // summary, and kept nowhere (made up around FAKE, its check digit by
    // 49 CFR 565.15)
    function test_aTypedVinIsDecodedOnTheDevice() {
        const page = createPage()
        const field = find(page, "vinLookupField")
        const result = find(page, "vinLookupResult")
        verify(!result.visible, "nothing typed, nothing said")
        verify(find(page, "vinLookupHint").text.indexOf("(vPICList_lite_") > 0,
               "the hint names the vPIC release")

        field.text = "1fa fake10m0000001"
        page.open("vehicle")
        verify(result.visible)
        compare(result.text, "Model year 2021; Ford (manufacturer code 1FA); check digit matches")
        compare(JSON.stringify(page.vehicles).indexOf("FAKE10"), -1, "not in the vehicles")

        field.text = "FAKEJ2534VIN00001"
        compare(result.text, "Not a VIN: 49 CFR 565 takes 17 digits and capital letters, never I, O or Q")
        field.text = ""
        verify(!result.visible)
    }

    // The signal files and the own signals belong to the picked vehicle. With
    // none picked their parts hold nothing, and a part that stays visible
    // adds a gap of its own to the Vehicle section's column
    function test_thePartsOfAPickedVehicleAreAbsentWithNone() {
        const page = createPage()
        page.open("signals")
        verify(!find(page, "settingsSignalFilesPart").visible, "no files with no vehicle")
        verify(!find(page, "settingsOwnSignalsPart").visible, "no own signals with no vehicle")
        page.changeVehicles(VehicleBook.add(page.vehicles, "A"))
        verify(find(page, "settingsSignalFilesPart").visible, "files with a vehicle")
        verify(find(page, "settingsOwnSignalsPart").visible, "own signals with a vehicle")
    }

    // ADR-0027 section 1: every change to the list is made in the section, a
    // label typed or left to its default
    function test_theVehicleSectionMakesNamesAndForgetsVehicles() {
        let page = createPage()
        page.open("vehicle")
        const field = find(page, "vehicleLabelField")
        verify(find(page, "vehicle_none").highlighted, "no vehicle to start")
        verify(!find(page, "vehicleDuplicate").enabled, "nothing to copy")
        compare(find(page, "vehicleLabelHint").text, "Left empty, New makes “Vehicle 1”")

        press(page, "vehicleNew")
        compare(labels(page), ["Vehicle 1"], "the default, with the field empty")
        compare(Bridge.vehicleLabel, "Vehicle 1", "the recording's label follows the pick")
        verify(find(page, "vehicle_v1").highlighted)

        field.text = "Daily driver"
        press(page, "vehicleRename")
        compare(labels(page), ["Daily driver"])
        compare(field.text, "", "the field empties for the next label")

        press(page, "vehicleDuplicate")
        compare(labels(page), ["Daily driver", "Daily driver (copy)"])
        compare(page.vehicles.picked, "v2")
        compare(find(page, "vehicleLabelHint").text,
                "Left empty, New makes “Vehicle 1” and Duplicate “Daily driver (copy 2)”")

        field.text = " daily DRIVER "
        verify(!find(page, "vehicleNew").enabled, "a label in use")
        verify(!find(page, "vehicleRename").enabled)
        page.open("vehicle")
        verify(find(page, "vehicleLabelProblem").visible)
        field.text = "x".repeat(41)
        verify(!find(page, "vehicleNew").enabled, "over 40 characters")
        field.text = ""

        press(page, "vehicleForget")
        compare(find(page, "vehicleConfirmText").text, "Forget Daily driver (copy)?")
        press(page, "vehicleCancel")
        compare(labels(page).length, 2, "Cancel keeps it")
        press(page, "vehicleForget")
        // A change rebuilds the buttons, and the Flow places them at its
        // next polish
        const v1 = find(page, "vehicle_v1")
        tryVerify(() => v1.x > 0, 2000, "the button is placed")
        tap(v1)
        compare(page.confirming, "", "picking another backs out")
        compare(labels(page).length, 2)
        press(page, "vehicleForget")
        press(page, "vehicleConfirm")
        compare(labels(page), ["Daily driver (copy)"])
        verify(find(page, "vehicle_none").highlighted)

        // The store writes as it changes, and a new page (a restart) reads it
        page.destroy()
        wait(10)
        page = createPage()
        compare(labels(page), ["Daily driver (copy)"])

        press(page, "vehicleForgetAll")
        compare(find(page, "vehicleConfirmText").text, "Forget all vehicles?")
        press(page, "vehicleConfirm")
        compare(page.vehicles, VehicleBook.empty())
    }

    // ---- The vehicle's signal files (ADR-0051) ----

    // A signal file of one DID on the engine ECU
    function signalFileText(name, did) {
        return JSON.stringify({
            havne_signals: 1, name: name, source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "a", name: "A", read: { service: "22", number: did || "D100" },
                        field: { byte: 0, bits: 8 } }]
        })
    }

    function importFile(page, fileName, name, did) {
        const url = Harness.writeScratchFile(fileName, signalFileText(name, did))
        verify(url.toString() !== "", "the scratch file is written")
        return page.importSignalFile(url)
    }

    function test_anImportedFileIsTheVehiclesAndAProfile() {
        let page = createPage()
        verify(!find(page, "signalFilesTitle").visible, "no vehicle, no files")
        press(page, "vehicleNew")
        page.open("signals")
        verify(find(page, "signalFilesNone").visible)
        verify(importFile(page, "extras.json", "Extras"))
        compare(page.pickedVehicle.signals.length, 1)
        compare(page.pickedVehicle.signals[0].profile, "user.extras")
        compare(find(find(page, "signalFile_0"), "signalFileText").text,
                "Extras: 1 signal, profile user.extras")
        compare(find(page, "signalNote").text, "Extras added: 1 signal, profile user.extras")
        compare(page.vehicleProfiles[0].hint, "Your signal file Extras: 1 signal")
        verify(!find(page, "signalFilesNone").visible)
        compare(Harness.storedSignalFiles(), [page.pickedVehicle.signals[0].sha256 + ".json"])

        // The file's profile is a choice under What to read
        find(page, "uriField").text = "mock://demo"
        press(page, "profile_user.extras")
        compare(find(page, "uriField").text, "mock://demo?profile=user.extras")
        compare(find(page, "errorLabel").text, "")
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "a session decodes with it")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)

        // A new page registers it again (the bridge, which keeps the file
        // loaded, outlives it; a restart's read from the copy is
        // BridgeTest.SignalFileUseRegistersItsProfile's)
        page.destroy()
        wait(10)
        page = createPage()
        compare(find(find(page, "signalFile_0"), "signalFileText").text,
                "Extras: 1 signal, profile user.extras")
        verify(findChild(page, "profile_user.extras"), "still a choice")

        // Two signals are counted as signals
        const pair = JSON.parse(signalFileText("Pair"))
        pair.signals.push({ key: "b", name: "B", read: { service: "22", number: "D101" },
                            field: { byte: 0, bits: 8 } })
        verify(page.importSignalFile(Harness.writeScratchFile("pair.json", JSON.stringify(pair))))
        compare(find(page, "signalNote").text, "Pair added: 2 signals, profile user.pair")
        compare(find(find(page, "signalFile_1"), "signalFileText").text,
                "Pair: 2 signals, profile user.pair")
        compare(page.vehicleProfiles[1].hint, "Your signal file Pair: 2 signals")
    }

    function test_aRefusedOrClashingFileIsNotAdded() {
        const page = createPage()
        press(page, "vehicleNew")
        const bad = Harness.writeScratchFile("bad.json", signalFileText("Bad").replace("bits", "bitz"))
        verify(!page.importSignalFile(bad))
        const words = find(page, "signalProblem").text
        verify(words.includes("bitz") && words.includes("line"), words)
        verify(!words.includes("bad.json"), "never the path")
        compare(page.pickedVehicle.signals, [])
        compare(Harness.storedSignalFiles(), [])

        verify(importFile(page, "a.json", "Extras"))
        compare(find(page, "signalProblem").text, "", "a good import clears the last refusal")
        press(page, "vehicleNew")
        verify(!importFile(page, "b.json", "Extras", "D101"), "another file making user.extras")
        compare(find(page, "signalProblem").text,
                "user.extras is made by another file already (Extras, in Vehicle 1)")
        compare(page.pickedVehicle.signals, [])
        compare(Harness.storedSignalFiles().length, 1, "the refused copy went")

        verify(importFile(page, "a2.json", "Extras"), "the same file in another vehicle is shared")
        compare(Harness.storedSignalFiles().length, 1)
    }

    // ---- An OBDb set imported (ADR-0051's appendix of 2026-11-06) ----

    function obdbPreview(page, year) {
        find(page, "obdbYearField").text = year
        return page.previewObdb(Harness.obdbFixture)
    }

    // The report shows what converted, what differs and what's left out,
    // and nothing is kept until Keep
    function test_anObdbSetShowsItsReportBeforeItIsKept() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        verify(find(page, "obdbImport").enabled)
        verify(!find(page, "obdbReport").visible)
        verify(obdbPreview(page, "2021"))
        page.open("signals")
        verify(find(page, "obdbReport").visible)
        compare(find(page, "obdbSummary").text,
                "Daily driver OBDb from OBDb: 7 signals converted, 6 fields differ, "
                + "5 left out, model year 2021")
        compare(find(page, "obdbChanged").count, 6)
        compare(find(page, "obdbSkipped").count, 5)
        compare(find(page, "obdbFileNotes").count, 2)
        compare(find(page, "obdbKeep").text, "Keep")
        compare(page.pickedVehicle.signals, [], "nothing kept yet")
        compare(Harness.storedSignalFiles(), [])

        press(page, "obdbKeep")
        verify(!find(page, "obdbReport").visible)
        compare(page.pickedVehicle.signals.length, 1)
        compare(page.pickedVehicle.signals[0].profile, "user.daily-driver-obdb")
        compare(find(page, "signalNote").text,
                "Daily driver OBDb added: obd2 and 7 of your own, profile user.daily-driver-obdb")
        compare(Harness.storedSignalFiles(), [page.pickedVehicle.signals[0].sha256 + ".json"])
        verify(Bridge.isObdbImport(page.pickedVehicle.signals[0].sha256))
    }

    function test_anObdbSetDiscardedIsNotKept() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(obdbPreview(page, ""))
        compare(find(page, "obdbSummary").text,
                "Vehicle 1 OBDb from OBDb: 6 signals converted, 5 fields differ, 6 left out, "
                + "no model year, so the commands OBDb gives for some years only are left out")
        press(page, "obdbDiscard")
        verify(!find(page, "obdbReport").visible)
        compare(page.pickedVehicle.signals, [])
        compare(Harness.storedSignalFiles(), [])
        verify(!page.keepObdb(), "nothing to keep")
        compare(page.pickedVehicle.signals, [])

        // A report is the vehicle's it was read for
        verify(obdbPreview(page, "2021"))
        press(page, "vehicleNew")
        verify(!find(page, "obdbReport").visible, "another vehicle picked")
        verify(!page.keepObdb())
        compare(page.pickedVehicle.signals, [])
    }

    // The set read again replaces the vehicle's earlier import, and says
    // so before Keep; a file the user named is never replaced
    function test_anObdbSetReadAgainReplacesTheEarlierImport() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        verify(obdbPreview(page, "2019"))
        press(page, "obdbKeep")
        const earlier = page.pickedVehicle.signals[0].sha256

        verify(obdbPreview(page, "2021"))
        verify(find(page, "obdbSummary").text.endsWith(
                   "; replaces the vehicle's earlier OBDb import"),
               find(page, "obdbSummary").text)
        compare(find(page, "obdbKeep").text, "Keep, replacing the earlier")
        // The report at a phone's width, as test_everyItemFitsAPhonesWidth
        const width = page.width
        page.width = 320
        const column = find(page, "settingsConnectionPageColumn")
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        waitForPolish(column)
        compare(pastTheEdge(column, column, []).join("; "), "",
                "nothing past the column's right edge, " + column.width)
        page.width = width
        press(page, "obdbKeep")
        compare(page.pickedVehicle.signals.length, 1)
        verify(page.pickedVehicle.signals[0].sha256 !== earlier)
        compare(page.pickedVehicle.signals[0].profile, "user.daily-driver-obdb")
        compare(find(page, "signalNote").text,
                "Daily driver OBDb replaced the vehicle's earlier OBDb import: obd2 and 7 "
                + "of your own, profile user.daily-driver-obdb")
        compare(Harness.storedSignalFiles(), [page.pickedVehicle.signals[0].sha256 + ".json"],
                "the earlier copy went")

        // The same year again is the same file
        verify(obdbPreview(page, "2021"))
        press(page, "obdbKeep")
        compare(page.pickedVehicle.signals.length, 1)
        compare(find(page, "signalNote").text, "Daily driver OBDb is this vehicle's already")

        // A file the user named so is the user's: the import takes a name
        // of its own beside it
        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        verify(importFile(page, "mine.json", "Weekend car OBDb"))
        verify(obdbPreview(page, "2021"))
        verify(!find(page, "obdbSummary").text.includes("replaces"))
        press(page, "obdbKeep")
        compare(page.pickedVehicle.signals.map(f => f.name),
                ["Weekend car OBDb", "Weekend car OBDb 2"])
    }

    // The year: typed, else a typed VIN's, else the connected car's once
    // its VIN names the picked vehicle, kept once the session ends; one
    // that isn't a year is refused before the file is read
    function test_theObdbYearComesFromAVin() {
        const page = createPage()
        press(page, "vehicleNew")
        compare(page.obdbVinYear(), 0)
        find(page, "vinLookupField").text = "1FAFAKE10M0000001"
        compare(page.obdbVinYear(), 2021)
        find(page, "vinLookupField").text = "not one"
        compare(page.obdbVinYear(), 0)
        find(page, "vinLookupField").text = ""

        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        verify(!find(page, "obdbImport").enabled, "nothing changes the vehicle while it runs")
        Bridge.identifyForTesting("1FAFAKE13Y0000001")
        compare(page.obdbVinYear(), 0, "not the picked vehicle's VIN")
        const salt = page.vehicles.salt !== "" ? page.vehicles.salt : Bridge.newSalt()
        const doc = VehicleBook.copy(page.vehicles)
        doc.salt = salt
        page.saveVehicles(VehicleBook.change(doc, v => v.vinKey = Bridge.vinKey(salt)))
        Bridge.identifyForTesting("1FAFAKE13Y0000001")
        compare(page.obdbVinYear(), 2000)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        compare(Bridge.vinModelYear(""), 0, "the VIN went with the session")
        compare(page.obdbVinYear(), 2000, "its year stays")
        verify(find(page, "obdbImport").enabled)

        find(page, "obdbYearField").text = "19x"
        verify(!page.previewObdb(Harness.obdbFixture))
        compare(find(page, "signalProblem").text, "A model year is 1980 to 2039, or empty for none")
        find(page, "obdbYearField").text = "1979"
        verify(!page.previewObdb(Harness.obdbFixture))
        verify(!find(page, "obdbReport").visible)
    }

    // A long report shows a list's first rows and counts the rest
    function test_aLongObdbReportCountsWhatItDoesNotShow() {
        const page = createPage()
        press(page, "vehicleNew")
        const commands = [{ hdr: "7E0", rax: "7E8", cmd: { "22": "D100" }, freq: 1,
                            signals: [{ id: "KEPT", name: "Kept", fmt: { len: 8, unit: "scalar" } }] }]
        for (let i = 0; i < 120; ++i)
            commands.push({ hdr: "7E0", rax: "7E8", cmd: { "22": (0x2000 + i).toString(16) },
                            freq: 1, signals: [{ id: "HIDDEN_" + i, hidden: true, name: "H",
                                                 fmt: { len: 8, unit: "scalar" } }] })
        const url = Harness.writeScratchFile("long.json", JSON.stringify({ commands: commands }))
        find(page, "obdbYearField").text = ""
        verify(page.previewObdb(url))
        compare(page.obdbReport.skipped.length, 120)
        compare(find(page, "obdbSkipped").count, page.obdbRowsShown)
        compare(find(page, "obdbSkippedMore").text, "and " + (120 - page.obdbRowsShown) + " more")
        page.open("signals")
        verify(find(page, "obdbSkippedMore").visible)
        verify(!find(page, "obdbChangedMore").visible)
    }

    function test_aFileThatIsNotAnObdbSetIsRefused() {
        const page = createPage()
        press(page, "vehicleNew")
        find(page, "obdbYearField").text = ""
        verify(!page.previewObdb(Harness.writeScratchFile("notobdb.json", "{\"ecu\": 1}")))
        const words = find(page, "signalProblem").text
        verify(words.startsWith("Not an OBDb set Havne can read"), words)
        verify(!words.includes("notobdb.json"), "never the path")
        verify(!find(page, "obdbReport").visible)
        compare(Harness.storedSignalFiles(), [])
    }

    // A copy goes when no vehicle keeps it; the vehicle's own file asks first
    function test_removeAndForgetDeleteTheCopiesNoVehicleKeeps() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "a.json", "Extras"))
        press(page, "vehicleDuplicate")
        compare(page.pickedVehicle.signals.length, 1, "Duplicate copies the files")
        press(page, "signalRemove_0")
        compare(page.pickedVehicle.signals, [])
        compare(Harness.storedSignalFiles().length, 1, "the first vehicle keeps it")
        page.pickVehicle("v1")
        press(page, "signalRemove_0")
        compare(Harness.storedSignalFiles(), [], "no vehicle keeps it now")

        verify(importFile(page, "own.json", "Mine"))
        const own = page.pickedVehicle.signals[0]
        page.saveVehicles(VehicleBook.setOwn(page.vehicles, own))
        press(page, "signalRemove_0")
        compare(find(page, "vehicleConfirmText").text,
                "Remove Mine? It was written here, and no other copy exists")
        compare(find(page, "vehicleConfirm").text, "Remove")
        press(page, "vehicleCancel")
        compare(page.pickedVehicle.signals.length, 1, "Cancel keeps it")
        press(page, "signalRemove_0")
        press(page, "vehicleConfirm")
        compare(page.pickedVehicle.signals, [])
        compare(Harness.storedSignalFiles(), [])

        verify(importFile(page, "c.json", "Body"))
        press(page, "vehicleForgetAll")
        press(page, "vehicleConfirm")
        compare(Harness.storedSignalFiles(), [], "Forget all takes the copies")
        verify(!findChild(page, "profile_user.body"))
    }

    function importExtending(page, fileName, name) {
        const doc = JSON.parse(signalFileText(name))
        doc.extends = "obd2"
        const url = Harness.writeScratchFile(fileName, JSON.stringify(doc))
        verify(url.toString() !== "", "the scratch file is written")
        return page.importSignalFile(url)
    }

    // A file that extends obd2 takes obd2's place on an adapter, where
    // Default is obd2 (ADR-0052): its button reads "obd2 + <name>" and
    // Default's goes once the field reads another profile. Imported while
    // the field reads Default with no profile= of its own, it's read
    // instead, and the note says so; a profile the user picked stays, a
    // second such file keeps a button of its own, and on mock:// Default
    // isn't obd2, so nothing stands in
    function test_aFileExtendingObd2TakesItsPlaceOnAnAdapter() {
        const page = createPage()
        press(page, "vehicleNew")
        const field = find(page, "uriField")
        field.text = "elm327://COM9"
        compare(page.profileChoice, "default")
        verify(importExtending(page, "plus.json", "Plus"))
        compare(find(page, "signalNote").text,
                "Plus added: now reading obd2 + Plus, profile user.plus")
        compare(field.text, "elm327://COM9?profile=user.plus")
        compare(find(find(page, "signalFile_0"), "signalFileText").text,
                "Plus: obd2 and 1 of your own, profile user.plus")
        const plus = find(page, "profile_user.plus")
        compare(plus.text, "obd2 + Plus")
        verify(plus.highlighted)
        compare(page.profiles.filter(p => p.name === "user.plus").length, 1,
                "one button for the file: the stand-in")
        tryVerify(() => findChild(page, "profile_default") === null, 1000,
                  "the file reads all of obd2: Default goes")
        compare(find(page, "profileHint").text,
                "obd2's signals and 1 of your own, from your signal file Plus")

        // Typed back to Default, both show, so the row is the URI's
        field.text = "elm327://COM9"
        verify(find(page, "profile_default").highlighted)
        verify(!find(page, "profile_user.plus").highlighted)

        // A second such file keeps its own button; Default stays read
        verify(importExtending(page, "more.json", "More"))
        compare(find(page, "signalNote").text,
                "More added: obd2 and 1 of your own, profile user.more")
        compare(field.text, "elm327://COM9")
        compare(find(page, "profile_user.more").text, "More")
        compare(page.vehicleProfiles.find(p => p.name === "user.more").hint,
                "Your signal file More: obd2 and 1 of your own", "its own, not obd2's")
        compare(find(page, "profile_user.plus").text, "obd2 + Plus")

        // A profile the user picked stays when one is imported
        press(page, "vehicleNew")
        field.text = "elm327://COM9?profile=raw"
        verify(importExtending(page, "third.json", "Third"))
        compare(field.text, "elm327://COM9?profile=raw")
        compare(page.profileChoice, "raw")
        compare(find(page, "signalNote").text,
                "Third added: obd2 and 1 of your own, profile user.third")
        compare(find(page, "profile_user.third").text, "obd2 + Third")
        tryVerify(() => findChild(page, "profile_default") === null, 1000)

        // On mock://, Default isn't obd2: the file is a choice of its own
        field.text = "mock://demo"
        compare(find(page, "profile_user.third").text, "Third")
        verify(find(page, "profile_default"))
    }

    // ---- The app's plugins (ADR-0052) ----

    // A packed plugin imported is listed with its variants; the VIN
    // (typed in the lookup here: the mock has none) proposes the variants
    // it covers for the picked vehicle, and the one picked is kept in the
    // vehicle, read through the URI's profile, offered under What to read
    // and connected; removed, it goes from the list and the row. An
    // import of its id replaces it, and a package core refuses says so
    function test_aPluginIsImportedMatchedByTheVinAndConnected() {
        const page = createPage()
        page.open("signals")
        verify(find(page, "pluginsNone").visible)
        verify(page.importPlugin(Harness.writeExamplePlugin("example-sim.havne-plugin.json")))
        compare(find(page, "pluginNote").text,
                "Example: the simulated car 1.0.0 added: plugin.example-sim.base, "
                + "plugin.example-sim.my2019")
        compare(find(find(page, "plugin_0"), "pluginText").text,
                "Example: the simulated car 1.0.0: base, my2019")
        verify(!find(page, "pluginsNone").visible)
        compare(Harness.storedPlugins().length, 1)

        press(page, "vehicleNew")
        verify(find(page, "pluginMatchesText").text.startsWith("No match yet"))
        verify(findChild(page, "pluginMatch_0") === null)
        find(page, "vinLookupField").text = "ZZZFAKE11M0000001"   // 2021
        compare(find(page, "pluginMatchesText").text, "For this car, by its VIN:")
        compare(find(page, "pluginMatch_0").text, "Example: the simulated car: base")
        compare(find(page, "pluginMatch_1").text, "Example: the simulated car: my2019")
        find(page, "vinLookupField").text = "ZZZFAKE11G0000001"   // 2016
        tryVerify(() => findChild(page, "pluginMatch_1") === null, 1000, "before my2019")
        find(page, "vinLookupField").text = "ZZZFAKE11M0000001"

        const field = find(page, "uriField")
        field.text = "mock://demo"
        press(page, "pluginMatch_1")
        compare(page.pickedVehicle.plugin, "plugin.example-sim.my2019")
        compare(field.text, "mock://demo?profile=plugin.example-sim.my2019")
        verify(find(page, "pluginMatch_1").highlighted)
        const choice = find(page, "profile_plugin.example-sim.my2019")
        compare(choice.text, "Example: the simulated car: my2019")
        verify(choice.highlighted)
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "a session decodes with the variant")
        compare(find(page, "errorLabel").text, "")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)

        // Its id again replaces it; a package core refuses says why
        verify(page.importPlugin(Harness.writeExamplePlugin("newer.havne-plugin.json", "1.1.0")))
        compare(find(page, "pluginNote").text, "Example: the simulated car 1.1.0 replaces 1.0.0")
        compare(Harness.storedPlugins().length, 1)
        verify(!page.importPlugin(Harness.writeScratchFile("bad.havne-plugin.json", "{}")))
        const words = find(page, "pluginProblem").text
        verify(words.includes("line") && !words.includes("bad.havne"), words)

        press(page, "pluginRemove_0")
        compare(find(page, "pluginProblem").text, "")
        verify(find(page, "pluginsNone").visible)
        compare(Harness.storedPlugins(), [])
        tryVerify(() => findChild(page, "profile_plugin.example-sim.my2019") === null, 1000,
                  "a variant no plugin registers isn't offered")
        compare(page.pickedVehicle.plugin, "", "a removed plugin's variant is no pick")
        compare(field.text, "mock://demo", "the field reads Default again")
    }


    // A session's VIN proposes variants (ADR-0052 section 5); nothing that
    // changes the vehicle is used while it runs, and once it ends its
    // matches stay to be picked, though the VIN goes with the session. A
    // pick pressed again is undone, and a button never runs past the page
    function test_aSessionsVinProposesVariantsPickedOnceItEnds() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(page.importPlugin(Harness.writeExamplePlugin("example.havne-plugin.json")))
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        Bridge.identifyForTesting("ZZZFAKE11M0000001")   // a 2021 the example covers
        tryVerify(() => findChild(page, "pluginMatch_1") !== null, 1000, "proposed")
        verify(!find(page, "pluginMatch_0").enabled)
        verify(!find(page, "signalImport").enabled)
        verify(!find(page, "formSave").enabled)
        verify(!find(page, "pluginImport").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        verify(findChild(page, "pluginMatch_0") !== null, "the session's matches stay")
        compare(find(page, "pluginMatchesText").text, "For this car, by its VIN:")
        verify(find(page, "pluginMatch_0").enabled)
        press(page, "pluginMatch_0")
        compare(page.pickedVehicle.plugin, "plugin.example-sim.base")
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.example-sim.base")
        press(page, "pluginMatch_0")
        compare(page.pickedVehicle.plugin, "", "pressed again, undone")
        compare(find(page, "uriField").text, "mock://demo")

        // Its row narrower than its text (a phone, a long plugin name)
        const button = find(page, "pluginMatch_0")
        button.parent.width = 100
        verify(button.implicitWidth > 100)
        verify(button.width <= 100, button.width + " in a row of 100")
    }

    // A copy's own signals are its own: its own file is copied under a
    // name of its own, so each vehicle's Save goes ahead (one file to a
    // profile); and a label used again gets an own file named apart
    function test_eachVehiclesOwnSignalsSaveApart() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        press(page, "vehicleDuplicate")
        verify(page.pickedVehicle.id !== first)
        verify(page.pickedVehicle.own !== "" && page.pickedVehicle.own !== VehicleBook.find(page.vehicles, first).own,
               "the copy's own file is a copy")
        compare(page.ownFile.name, "Vehicle 1 (copy) own")
        compare(page.ownSignals.map(s => s.key), ["trans_temp"])
        typeSignal(page, "Gear", "D101")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.length, 2)

        press(page, "vehicle_" + first)
        compare(page.ownSignals.map(s => s.key), ["trans_temp"], "the first is as it was")
        typeSignal(page, "Fluid Pressure", "D102")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.length, 2)

        // Renamed, its label is free for New, whose own file is named apart
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleRename")
        press(page, "vehicleNew")
        compare(page.pickedVehicle.label, "Vehicle 1")
        typeSignal(page, "Oil Temp", "D103")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownFile.name, "Vehicle 1 own 2")
    }

    // The form and the section's words are the picked vehicle's: a pick or
    // New starts them afresh, an Edit's signal never lands in another
    // vehicle, and a question asked about one goes with it
    function test_aPickStartsTheFormAndNotesAfresh() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        const bad = Harness.writeScratchFile("bad.json", signalFileText("Bad").replace("bits", "bitz"))
        verify(!page.importSignalFile(bad))
        verify(find(page, "signalProblem").text !== "")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        press(page, "vehicleNew")
        compare(find(page, "signalProblem").text, "", "the other vehicle's refusal goes")
        const second = page.pickedVehicle.id

        press(page, "vehicle_" + first)
        press(page, "ownEdit_0")
        compare(find(page, "formName").text, "Trans Temp")
        press(page, "vehicle_" + second)
        compare(find(page, "formName").text, "", "the Edit stays with its vehicle")
        compare(page.editingKey, "")

        press(page, "vehicle_" + first)
        press(page, "ownRemove_0")
        verify(page.confirming !== "", "asked")
        page.saveVehicles(VehicleBook.pick(page.vehicles, second))   // as a car recognized
        compare(page.confirming, "", "the question goes with the pick")
    }

    // Edit then a new name renames the signal; a new signal whose name
    // makes a saved one's key is refused, not saved over it; the same name
    // again replaces it, as designed
    function test_anEditRenamesAndAKeyIsNeverTakenTwice() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        press(page, "ownEdit_0")
        find(page, "formName").text = "Trans Fluid Temp"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.name), ["Trans Fluid Temp"])

        page.clearForm()
        typeSignal(page, "Boost (psi)", "D101")
        press(page, "formSave")
        page.clearForm()
        typeSignal(page, "Boost psi", "D102")
        press(page, "formSave")
        compare(find(page, "formProblem").text,
                "Boost (psi), saved already, has the key boost_psi too: edit it, or give this one another name")
        compare(page.ownSignals.map(s => s.read.number), ["1E1C", "D101"])
        page.clearForm()
        typeSignal(page, "Boost (psi)", "D103")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.read.number), ["1E1C", "D103"], "the same name replaces")

        // An Edit renamed to another signal's key is refused too
        press(page, "ownEdit_0")
        find(page, "formName").text = "Boost psi"
        press(page, "formSave")
        verify(find(page, "formProblem").text.startsWith("Boost (psi), saved already"))
        compare(page.ownSignals.length, 2)
    }

    // A file the vehicle has already isn't added again; a Read once's
    // reply shows only on the vehicle it was asked for
    function test_aFileAgainAndAReplyElsewhereSaySo() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "extras.json", "Extras"))
        verify(!importFile(page, "extras.json", "Extras"))
        compare(find(page, "signalNote").text, "Extras is this vehicle's already")
        compare(page.pickedVehicle.signals.length, 1)

        const reply = { ok: true, request: "22 1E 1C", reply: "62 1E 1C 12 34",
                        value: "4660", nrc: 0, words: "" }
        page.readOnceFor = "another vehicle"
        Bridge.readOnceResult(reply)
        compare(page.readResult, null, "asked for another vehicle")
        page.readOnceFor = page.pickedId
        Bridge.readOnceResult(reply)
        compare(page.readResult.reply, "62 1E 1C 12 34")
    }

    // A file is saved out as the vehicle has it, byte for byte (a Save
    // replaces what was at the target), under its profile's name; its
    // button stays on while a session runs, since it only reads the copy;
    // a missing copy says so
    function test_aFileIsSavedOutAsTheVehicleHasIt() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "extras.json", "Extras"))
        const f = page.pickedVehicle.signals[0]
        compare(page.saveFileName(f), "extras.json")
        const url = Harness.writeScratchFile("saved.json", "x".repeat(2000))
        verify(page.saveSignalFileTo(f.sha256, url))
        compare(Harness.readScratchFile(url), signalFileText("Extras"))
        compare(Harness.readScratchFile(url), Bridge.signalFileText(f.sha256))
        compare(find(page, "signalNote").text, "Extras saved to the file you chose")

        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        const own = page.pickedVehicle.signals.find(e => e.sha256 === page.pickedVehicle.own)
        compare(page.saveFileName(own), "vehicle-1-own.json")
        const ownUrl = Harness.writeScratchFile("own.json", "")
        verify(page.saveSignalFileTo(own.sha256, ownUrl))
        compare(Harness.readScratchFile(ownUrl), Bridge.signalFileText(own.sha256))
        compare(JSON.parse(Harness.readScratchFile(ownUrl)).signals.map(s => s.name),
                ["Trans Temp"])

        verify(!page.saveSignalFileTo("a".repeat(64), url))
        compare(find(page, "signalProblem").text, "Its copy is missing: import the file again")
        compare(find(page, "signalNote").text, "")

        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        verify(find(page, "signalSave_0").enabled, "Save reads the copy alone")
        verify(!find(page, "signalImport").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
    }

    // One request ID to a module: a Save whose typed one differs moves the
    // module's other signals, and its note names them; a blank one keeps
    // the module's
    function test_aModulesRequestIdMovesItsSignalsAndSaysSo() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Gear", "D101")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = ""
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(find(page, "formNote").text, "Gear saved in Vehicle 1 own")
        typeSignal(page, "Oil", "D102")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = "7E2"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(find(page, "formNote").text, "Oil saved in Vehicle 1 own; 7E9 asks on 7E2 for Gear too")
        compare(page.ownFile.ecus.map(e => e.tx), ["7E2"])
        typeSignal(page, "Pressure", "D103")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = ""
        press(page, "formSave")
        compare(find(page, "formNote").text, "Pressure saved in Vehicle 1 own")
        compare(page.ownFile.ecus.map(e => e.tx), ["7E2"], "blank keeps the module's")
    }

    // A file's or a plugin's profile is its vehicle's: a pick of another,
    // or New, puts the field back to Default (its connection kept), so it
    // never connects, and is remembered, with the last one's signals; a
    // built-in profile stays
    function test_aPickLeavesAnotherVehiclesProfileBehind() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        verify(importFile(page, "extras.json", "Extras"))
        find(page, "uriField").text = "mock://demo"
        page.chooseProfile("user.extras")
        compare(find(page, "uriField").text, "mock://demo?profile=user.extras")
        press(page, "vehicleNew")
        compare(find(page, "uriField").text, "mock://demo", "New: Default")
        compare(page.profileChoice, "default")

        const second = page.pickedVehicle.id
        press(page, "vehicle_" + first)
        page.chooseProfile("user.extras")
        press(page, "vehicleDuplicate")
        verify(page.pickedVehicle.id !== first && page.pickedVehicle.id !== second)
        compare(find(page, "uriField").text, "mock://demo?profile=user.extras",
                "the copy has the file: its profile stays")
        press(page, "vehicle_" + second)
        compare(find(page, "uriField").text, "mock://demo", "another's goes")

        page.chooseProfile("raw")
        press(page, "vehicle_" + first)
        compare(find(page, "uriField").text, "mock://demo?profile=raw", "a built-in stays")
    }

    // ---- The vehicle's own signals (ADR-0051's form) ----

    function typeSignal(page, name, number, bits) {
        find(page, "formName").text = name
        find(page, "formNumber").text = number
        if (bits !== undefined)
            find(page, "formBits").text = bits
    }

    function ownText(page, i) {
        return find(find(page, "ownSignal_" + i), "ownSignalText").text
    }

    // Each box is named above it, so a box keeps saying what it holds once
    // its placeholder has gone
    function test_theFormsBoxesKeepTheirNamesOnceTyped() {
        const page = createPage()
        press(page, "vehicleNew")
        compare(page.formKind, "number")
        const caption = name => find(page, name).parent.children[0]
        const named = { formName: "Name", formRx: "Reply ID", formTx: "Request ID",
                        formNumber: "DID", formByte: "Byte", formBits: "Bits",
                        formBit: "Start bit", formScale: "Scale", formOffset: "Offset",
                        formUnit: "Unit", formDecimals: "Decimals" }
        press(page, "formService_22")
        for (const box in named) {
            find(page, box).text = "1"
            compare(caption(box).text, named[box], box)
            page.open("ownform")
            verify(caption(box).visible, box)
        }
        press(page, "formService_01")
        compare(caption("formNumber").text, "PID")
        press(page, "formKind_hex")
        verify(!caption("formBits").visible, "no bits for a hex field")
        verify(!caption("formBit").visible)
        verify(caption("formBytes").visible)
        compare(caption("formBytes").text, "Bytes")
    }

    function test_theFormSavesTheVehiclesOwnSignals() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("ownform")
        verify(find(page, "signalForm").visible)
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(find(page, "formNote").text, "Trans Temp saved in Vehicle 1 own")
        compare(ownText(page, 0), "Trans Temp: 22 1E1C on 7E8")
        compare(find(find(page, "signalFile_0"), "signalFileText").text,
                "Vehicle 1 own: 1 signal, profile user.vehicle-1-own")
        verify(findChild(page, "profile_user.vehicle-1-own"), "a profile under What to read")
        compare(page.pickedVehicle.own, page.pickedVehicle.signals[0].sha256)
        compare(Harness.storedSignalFiles().length, 1)

        // A second signal joins the file; the last copy goes
        page.fillForm(SignalForm.emptyFields())
        typeSignal(page, "Gear", "D101")
        press(page, "formKind_enum")
        find(page, "formValues").text = "0=Park, 3=Drive"
        press(page, "formSave")
        compare(page.ownSignals.map(s => s.key), ["trans_temp", "gear"])
        compare(page.ownSignals[1].values, { "0": "Park", "3": "Drive" })
        compare(Harness.storedSignalFiles().length, 1, "one copy: the file's newest")

        // Edit fills the form; Save replaces the signal of that name
        press(page, "ownEdit_0")
        compare(find(page, "formName").text, "Trans Temp")
        compare(find(page, "formBits").text, "16")
        compare(page.formKind, "number")
        find(page, "formNumber").text = "1E1D"
        press(page, "formSave")
        compare(page.ownSignals.map(s => s.read.number), ["1E1D", "D101"])

        // The last signal's Remove takes the file, once asked: it was
        // written here and has no copy elsewhere
        press(page, "ownRemove_1")
        compare(page.ownSignals.length, 1)
        compare(find(page, "formNote").text, "Removed from Vehicle 1 own")
        press(page, "ownRemove_0")
        compare(page.ownSignals.length, 1, "asked first")
        compare(find(page, "vehicleConfirmText").text,
                "Remove Vehicle 1 own? It was written here, and no other copy exists")
        press(page, "vehicleConfirm")
        compare(page.pickedVehicle.signals, [])
        compare(page.pickedVehicle.own, "")
        compare(Harness.storedSignalFiles(), [])
    }

    function test_theFormShowsTheLoadersWords() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Bad", "1E1C", "12")
        press(page, "formSave")
        const words = find(page, "formProblem").text
        verify(words.includes("big-endian"), words)
        verify(!/^line \d/.test(words), "no line and column: " + words)
        compare(page.ownSignals, [])
        compare(Harness.storedSignalFiles(), [])

        // Read once checks the draft first, then needs an adapter
        find(page, "uriField").text = "mock://demo"
        press(page, "formReadOnce")
        verify(find(page, "formProblem").text.includes("big-endian"))
        find(page, "formBits").text = "16"
        press(page, "formReadOnce")
        compare(find(page, "formProblem").text,
                "Read once needs an adapter: j2534://, elm327:// or socketcan://")
        find(page, "formRx").text = "7B8"
        press(page, "formReadOnce")
        compare(find(page, "formProblem").text,
                "The module needs its request ID: only 7E8 to 7EF and 18DAF1xx have one by rule")

        // No Read once while a session holds the adapter
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000)
        verify(!find(page, "formReadOnce").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
    }

    // The fake DLL's ECU answers 1E1C with 12 34
    function test_readOnceShowsTheRepliesBytesAndValue() {
        if (Harness.fakeDllPath === "")
            skip("the fake J2534 DLL is Windows only")
        const page = createPage()
        press(page, "vehicleNew")
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath + "?profile=obd2"
        typeSignal(page, "Test", "1E1C", "16")
        find(page, "formScale").text = "0.1"
        find(page, "formUnit").text = "kPa"
        find(page, "formDecimals").text = "1"
        press(page, "formReadOnce")
        compare(find(page, "formProblem").text, "")
        tryVerify(() => page.readResult !== null, 15000, "the read ends")
        compare(find(page, "formReadResult").text, "Asked 22 1E 1C, got 62 1E 1C 12 34: 466.0 kPa")
        verify(!Bridge.connected, "no session left behind")
    }

    function test_pickingAVehicleFillsItsConnection() {
        const page = createPage()
        press(page, "vehicleNew")
        page.saveVehicles(VehicleBook.rememberUri(page.vehicles,
                                                  "elm327://COM6?profile=uds-generic&did=F190"))
        press(page, "vehicle_none")
        find(page, "uriField").text = "mock://demo"
        tap(find(page, "vehicle_v1"))
        compare(find(page, "uriField").text, "elm327://COM6?profile=uds-generic&did=F190")
        verify(find(page, "profile_uds-generic").highlighted, "read back into the row")
        compare(find(page, "didField").text, "F190")
    }

    // The vehicle a session runs under can't change beneath it, and a
    // mock's session isn't that car's
    function test_aMockSessionLeavesTheVehicleAloneAndLocksTheSection() {
        const page = createPage()
        press(page, "vehicleNew")
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "the mock connects")
        verify(!find(page, "vehicleNew").enabled)
        verify(!find(page, "vehicle_none").enabled)
        verify(!find(page, "vehicleForgetAll").enabled)
        Bridge.signalModel.toggleSubscribed(0)
        compare(page.pickedVehicle.uri, "", "a mock's URI isn't kept")
        compare(page.pickedVehicle.unplotted, [], "nor its plotting")
        Bridge.signalModel.toggleSubscribed(0)
    }

    // ADR-0027 sections 2-4 through the fake: the connect keeps the URI, the VIN
    // is learned and logged, Live's plotting is kept and comes back
    function test_anAdaptersConnectKeepsItsVehiclesSetup() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        let log = []
        const collect = line => log.push(line)
        Bridge.logMessage.connect(collect)
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        compare(page.pickedVehicle.uri, uri)
        tryVerify(() => page.pickedVehicle.vinKey !== "", 5000, "the VIN is learned")
        verify(page.vehicles.salt.length === 32, "a salt made for it")
        verify(log.includes("Vehicle: Daily driver (recognized from now on)"), log.join("\n"))
        verify(!JSON.stringify(page.vehicles).includes("FAKEJ2534VIN"), "no VIN in the store")

        Bridge.signalModel.toggleSubscribed(1)   // Coolant Temp: plotted by rank
        const unticked = Bridge.signalModel.unplottedNames()
        compare(unticked, ["Coolant Temp"])
        compare(page.pickedVehicle.unplotted, unticked, "an untick is kept")
        Bridge.signalModel.toggleSubscribed(5)   // Throttle Position: unranked
        compare(Bridge.signalModel.plottedNames(), ["Throttle Position"])
        compare(page.pickedVehicle.plotted, ["Throttle Position"], "a tick past the default too")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        log = []
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects again")
        compare(Bridge.signalModel.unplottedNames(), unticked, "the untick comes back")
        compare(Bridge.signalModel.plottedNames(), ["Throttle Position"], "and the tick")
        tryVerify(() => log.includes("Vehicle: Daily driver"), 5000, "recognized: " + log.join("\n"))
        Bridge.logMessage.disconnect(collect)
    }

    // A vehicle with no VIN picked on a car another vehicle is saved with:
    // the VIN picks that one, and the section says the car made the pick
    // until the user picks
    function test_theCarsVinPickingAnotherVehicleSaysSo() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        const daily = page.pickedVehicle.id
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.vinKey !== "", 10000, "the VIN is learned")
        const note = find(page, "vehicleVinPickNote")
        verify(!note.visible, "learning the VIN isn't a pick")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)

        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        const weekend = page.pickedVehicle.id
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.id === daily, 10000, "the VIN picks Daily driver")
        page.open("vehicle")
        verify(note.visible)
        compare(note.text, "The connected car's VIN picked Daily driver (Weekend car was picked)")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        verify(note.visible, "it stays after the session")
        press(page, "vehicle_" + weekend)
        verify(!note.visible, "the user's pick ends it")
    }

    // A VIN that picks another vehicle during a session leaves the last
    // vehicle's signal file in the field only while the session runs: at
    // its end the field reads Default, so the next connect keeps no other
    // vehicle's file as this one's connection
    function test_theVinsPickLeavesTheLastVehiclesFileAtTheEnd() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        const daily = page.pickedVehicle.id
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.vinKey !== "", 10000, "the VIN is learned")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)

        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        verify(importFile(page, "extras.json", "Extras"))
        find(page, "uriField").text = uri
        page.chooseProfile("user.extras")
        compare(find(page, "uriField").text, uri + "?profile=user.extras")
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.id === daily, 10000, "the VIN picks Daily driver")
        compare(find(page, "uriField").text, uri + "?profile=user.extras",
                "the session's URI stays while it runs")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        compare(find(page, "uriField").text, uri, "Default at the session's end")
        compare(page.profileChoice, "default")

        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects again")
        compare(page.pickedVehicle.id, daily)
        compare(page.pickedVehicle.uri, uri, "Daily driver keeps no file of Weekend car's")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
    }

    // A vehicle the VIN adds during a session holds no signal file, so its
    // connection doesn't name the last vehicle's, and neither does the
    // field once the session ends
    function test_aVehicleTheVinAddsKeepsNoOtherVehiclesFile() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        const weekend = page.pickedVehicle.id
        verify(importFile(page, "extras.json", "Extras"))
        // Another car's VIN: the fake's is no vehicle's yet
        page.saveVehicles(VehicleBook.change(page.vehicles, v => v.vinKey = "another-cars-key"))
        find(page, "uriField").text = uri
        page.chooseProfile("user.extras")
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.id !== weekend, 10000, "the VIN adds a vehicle")
        compare(page.pickedVehicle.signals, [])
        compare(page.pickedVehicle.uri, uri, "no profile of Weekend car's file")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        compare(find(page, "uriField").text, uri, "Default at the session's end")
    }

    // While a vendor's open blocks, nothing may rewrite the URI field or
    // start another connect: the device rows lock like every other control
    function test_controlsLockWhileAConnectIsInFlight() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        Harness.armFakeKnob("ECUIO_FAKE_OPEN_DELAY_MS", "1500")
        const page = createPage()
        page.j2534Devices = [{ vendorName: "Fake", dllPath: Harness.fakeDllPath,
                               architecture: "x64" }]
        const list = find(page, "deviceList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        const rowArea = find(list.itemAtIndex(0), "deviceRowArea")
        const uri = find(page, "uriField")
        verify(rowArea.enabled, "idle: the row can fill the field")

        uri.text = "j2534://" + Harness.fakeDllPath
        press(page, "connectButton")
        tryVerify(() => Bridge.connecting, 2000, "the connect is in flight")
        verify(!rowArea.enabled, "a row click can't rewrite the field under the connect")
        verify(!uri.enabled)
        verify(!find(page, "preset_mock").enabled)
        verify(!find(page, "profile_raw").enabled)
        verify(!find(page, "connectButton").enabled)

        tryVerify(() => Bridge.connected, 10000, "the stalled open completes")
        compare(uri.text, "j2534://" + Harness.fakeDllPath, "the field is what was connected")
        verify(!rowArea.enabled, "and locked for the session")
    }
}
