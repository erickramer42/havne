// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "SettingsUri"
    when: windowShown

    Component {
        id: pageComponent
        // Tall enough that the device list sits inside the page's
        // scroll area, which clips what's below it from a click
        SettingsPage { width: 1000; height: 1400 }
    }
    Component { id: dashComponent; DashboardPage { width: 1280; height: 800 } }
    Component { id: hubComponent; SignalHub {} }

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
    Settings {
        id: uiStore
        category: "ui"
    }

    // A binding error in the app's QML fails the test. Each case starts
    // with nothing stored, or the last case's URI would fill the field
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        stored.setValue("uri", "")
        stored.setValue("recentUris", "[]")
        stored.setValue("recording", false)
        stored.setValue("adapterVersions", "{}")
        stored.sync()
        storedVehicles.setValue("book", "")
        storedVehicles.setValue("document", "")
        storedVehicles.sync()
        // After a turn: the last case's page writes its Settings as it goes
        wait(0)
        uiStore.setValue("driveHideMs", undefined)
        uiStore.setValue("driveOrientation", undefined)
        uiStore.sync()
    }

    // Every connect here is asynchronous, so a case that fails between
    // pressing Connect and the completion leaves one in flight: let it
    // land before the disconnect, or the next case meets a connected
    // bridge and fails for the wrong reason
    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
        Theme.compact = false
        Theme.dense = false
        Theme.look = "dark"
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.nightFrom = "19:00"
        Theme.nightUntil = "07:00"
        Bridge.unitSystem = 0
        Bridge.recording = false
        tryVerify(() => !Bridge.connecting, 10000, "a connect in flight ends")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
        Harness.stopElm327Emulator()
        Bridge.vehicleLabel = ""
        Bridge.dropImport()                   // no file waits for the next case
        for (const p of Bridge.plugins())     // and the store starts empty
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

    // The Display page's Night buttons set Theme.night at once and the
    // chosen one is chosen (Theme.qml stores it: tst_theme's case)
    function test_theNightButtonsSetNight() {
        const page = createPage()
        page.open("display")
        for (const name of ["on", "schedule", "off"]) {
            const b = find(page, "night_" + name)
            verify(b.visible, name + " is offered")
            b.clicked()
            compare(Theme.night, name)
            verify(b.selected, name + " is chosen")
            for (const other of Theme.nightModes.filter(n => n !== name))
                verify(!find(page, "night_" + other).selected, other + " is not")
        }
        compare(find(page, "night_off").text, "Off")
        compare(find(page, "night_on").text, "On")
        compare(find(page, "night_schedule").text, "Scheduled")
    }

    // The style is chosen with Night off too, and shows when Night is on
    function test_theNightStyleButtonsSetTheStyle() {
        const page = createPage()
        page.open("display")
        compare(Theme.night, "off")
        for (const name of ["red", "amber"]) {
            const b = find(page, "nightStyle_" + name)
            verify(b.visible && b.enabled, name + " is offered")
            b.clicked()
            compare(Theme.nightStyle, name)
            verify(b.selected, name + " is chosen")
            verify(!find(page, "nightStyle_" + (name === "red" ? "amber" : "red")).selected)
        }
        compare(find(page, "nightStyle_amber").text, "Amber")
        compare(find(page, "nightStyle_red").text, "Red")
        find(page, "night_on").clicked()
        find(page, "nightStyle_red").clicked()
        compare(Theme.active, "nightRed")
    }

    // The Display page's Look buttons set Theme.look at once, the chosen
    // one is chosen, and Night still wins over the look
    function test_theLookButtonsSetTheLook() {
        const page = createPage()
        page.open("display")
        for (const name of ["daylight", "dark"]) {
            const b = find(page, "look_" + name)
            verify(b.visible, name + " is offered")
            b.clicked()
            compare(Theme.look, name)
            compare(Theme.active, name)
            verify(b.selected, name + " is chosen")
            verify(!find(page, "look_" + (name === "dark" ? "daylight" : "dark")).selected)
        }
        compare(find(page, "look_dark").text, "Dark")
        compare(find(page, "look_daylight").text, "Light")
        find(page, "night_on").clicked()
        find(page, "look_daylight").clicked()
        compare(Theme.active, "nightAmber", "Night over Light")
        find(page, "night_off").clicked()
        compare(Theme.active, "daylight")
    }

    // A stored mode, style or look outside the lists reads as the default,
    // and the buttons agree
    function test_anUnknownNightValueHighlightsTheDefault() {
        const page = createPage()
        page.open("display")
        Theme.night = "nonsense"
        Theme.nightStyle = "nonsense"
        Theme.look = "nonsense"
        verify(find(page, "look_dark").selected, "Dark is shown as the look in use")
        verify(!find(page, "look_daylight").selected)
        verify(find(page, "night_off").selected, "off is shown as the mode in use")
        verify(!find(page, "night_on").selected)
        verify(!find(page, "night_schedule").selected)
        verify(find(page, "nightStyle_amber").selected)
        verify(!find(page, "nightStyle_red").selected)
    }

    // From and Until show under Scheduled and nowhere else; a valid time is
    // stored, a bad one or an equal pair refused with the words
    function test_theScheduleFieldsStoreATimeAndRefuseABadOne() {
        const page = createPage()
        page.open("display")
        const sched = find(page, "nightSchedule")
        compare(sched.visible, false, "hidden while Night is off")
        find(page, "night_schedule").clicked()
        compare(sched.visible, true, "shown under Scheduled")
        const from = find(page, "nightFrom"), until = find(page, "nightUntil")
        const problem = find(page, "nightProblem")
        compare(from.text, "19:00")
        compare(until.text, "07:00")
        compare(problem.visible, false)
        from.text = "20:30"
        from.editingFinished()
        compare(Theme.nightFrom, "20:30", "a valid time is stored")
        compare(problem.visible, false)
        from.text = "25:00"
        from.editingFinished()
        compare(Theme.nightFrom, "20:30", "a bad From is refused")
        compare(from.text, "20:30", "and the field shows the stored time again")
        until.text = "25:00"
        until.editingFinished()
        compare(Theme.nightUntil, "07:00", "a bad time is refused")
        compare(problem.text, "Not a time")
        verify(problem.visible)
        compare(until.text, "07:00", "the field shows the stored time again")
        until.text = "20:30"
        until.editingFinished()
        compare(Theme.nightUntil, "07:00", "an equal pair is refused")
        compare(problem.text, "From and Until are the same time")
        compare(until.text, "07:00")
        until.text = "06:15"
        until.editingFinished()
        compare(Theme.nightUntil, "06:15")
        compare(problem.visible, false, "the words go with a good time")
        compare(find(page, "nightWords").text, "Scheduled 20:30 to 06:15")
        Theme.nightUntil = "05:45"
        compare(until.text, "05:45", "and follows it after an edit")
        until.text = "99:99"
        until.editingFinished()
        verify(problem.visible)
        find(page, "night_off").clicked()
        compare(sched.visible, false, "hidden again")
        find(page, "night_schedule").clicked()
        compare(problem.visible, false, "the words went with the schedule")
    }

    // The list's row says the night state, and the look when it isn't Dark
    function test_theDisplayRowSaysTheNightState() {
        const page = createPage()
        const summary = () => page.pageList.find(p => p.key === "display").summary
        compare(summary(), "100 % · metric · Night off")
        Theme.night = "on"
        Theme.nightStyle = "red"
        compare(summary(), "100 % · metric · Night on, red")
        Theme.nightStyle = "amber"
        compare(summary(), "100 % · metric · Night on, amber")
        Theme.night = "schedule"
        compare(summary(), "100 % · metric · Night scheduled 19:00 to 07:00")
        Theme.night = "off"
        Theme.look = "daylight"
        compare(summary(), "100 % · metric · Light · Night off", "the look when it isn't Dark")
        Theme.look = "dark"
        compare(summary(), "100 % · metric · Night off")
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
        verify(find(page, "profile_default").checked)

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

    // keepalive_ms is a key all three adapter schemes take, so a switch
    // between them keeps it
    function test_anotherSchemeKeepsKeepaliveMsAndGap() {
        const page = createPage()
        const uri = find(page, "uriField")

        type(uri, "elm327://COM5?profile=uds-generic&keepalive_ms=2000&gap=10")
        press(page, "preset_j2534")
        compare(uri.text, "j2534://?profile=uds-generic&keepalive_ms=2000&gap=10",
                "J2534 takes keepalive_ms and gap")
        press(page, "preset_wifi_obdx")
        compare(uri.text, "elm327://192.168.4.1:23?profile=uds-generic&keepalive_ms=2000&gap=10",
                "ELM327 takes keepalive_ms and gap")
    }

    // The Units row sets the bridge's unit system, connected or not, and
    // shows the one in force
    function test_unitsRowSetsTheBridge() {
        const page = createPage()
        const metric = find(page, "units_metric")
        const imperial = find(page, "units_imperial")
        compare(Bridge.unitSystem, 0)
        verify(metric.selected && !imperial.selected)
        press(page, "units_imperial")
        compare(Bridge.unitSystem, 1)
        verify(imperial.selected && !metric.selected)

        compare(Bridge.connectSession("mock://demo"), "", "connects")
        verify(metric.enabled, "units change while connected")
        press(page, "units_metric")
        compare(Bridge.unitSystem, 0)
        verify(metric.selected)
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
        // The rows' detail is caption text, never under the 12 px floor
        compare(find(list.itemAtIndex(1), "deviceDetail").font.pixelSize, Theme.fontCaption)
        verify(Theme.fontCaption >= 12)

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

    // The phone's words and list (Theme.mobile is the platform's, so the
    // section's own `mobile` stands in): the count names Bluetooth alone,
    // and the list takes all its rows where the desktop's stops at its cap
    function test_theMobileFormsConnectionWords_data() {
        return [{ tag: "phone", mobile: true }, { tag: "desktop", mobile: false }]
    }

    function test_theMobileFormsConnectionWords(data) {
        const page = createPage()
        const section = find(page, "adapterSection")
        const list = find(page, "deviceList")
        const count = find(page, "deviceCount")
        section.mobile = data.mobile
        page.j2534Devices = []
        page.canInterfaces = []
        page.bluetoothAdapters = [{ name: "Adapter A", address: "00:11:22:33:44:55", kind: "bt" }]
        const ports = []
        for (let i = 0; i < 12; ++i)
            ports.push({ name: "COM" + (10 + i), device: "" })
        page.serialPorts = ports
        compare(list.count, 13)
        if (data.mobile) {
            compare(count.text, "Bluetooth: 1 found; tap one, then Connect")
            page.bluetoothAdapters = [{ name: "Adapter A", address: "00:11:22:33:44:55", kind: "bt" },
                                      { name: "Headset", address: "AA:BB:CC:DD:EE:FF", kind: "bt", other: true }]
            compare(count.text, "Bluetooth: 1 found; tap one, then Connect",
                    "a folded headset is not an adapter found")
            page.bluetoothAdapters = [{ name: "Adapter A", address: "00:11:22:33:44:55", kind: "bt" }]
            tryVerify(() => list.contentHeight > 0 && Math.abs(list.height - list.contentHeight) < 1,
                      3000, "the page alone scrolls")
        } else {
            verify(count.text.indexOf("J2534 interfaces: 0, serial ports: 12") >= 0, count.text)
            page.bluetoothAdapters = [{ name: "Adapter A", address: "00:11:22:33:44:55", kind: "bt" },
                                      { name: "Headset", address: "AA:BB:CC:DD:EE:FF", kind: "bt", other: true }]
            verify(count.text.indexOf(", Bluetooth: 1") >= 0, "a folded headset is not counted: " + count.text)
            tryVerify(() => list.contentHeight > 0 && list.height < list.contentHeight,
                      3000, "the desktop's list stops at its cap")
        }
    }

    // A phone with only a headset paired has no adapter to connect to: the
    // count and the Connection row say so, as the connect list does
    function test_aFoldedHeadsetAloneIsNoAdapterFound_data() {
        return [{ tag: "phone", mobile: true }, { tag: "desktop", mobile: false }]
    }

    function test_aFoldedHeadsetAloneIsNoAdapterFound(data) {
        const page = createPage()
        const section = find(page, "adapterSection")
        const count = find(page, "deviceCount")
        section.mobile = data.mobile
        page.j2534Devices = []
        page.canInterfaces = []
        page.serialPorts = []
        page.bluetoothAdapters = [{ name: "Headset", address: "AA:BB:CC:DD:EE:FF", kind: "bt", other: true }]
        verify(count.text.indexOf("None found") === 0, count.text)
        verify(page.pageList.find(p => p.key === "connection").summary.indexOf("none found") === 0,
               "the Connection row agrees")
        page.bluetoothAdapters = [{ name: "Headset", address: "AA:BB:CC:DD:EE:FF", kind: "bt", other: true },
                                  { name: "Adapter A", address: "00:11:22:33:44:55", kind: "bt" }]
        verify(count.text.indexOf("None found") !== 0, count.text)
        verify(page.pageList.find(p => p.key === "connection").summary.indexOf("1 found") === 0)
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
        uri.text = "j2534://?profile=uds-generic&did=1E1C:u16&identify=0&keepalive_ms=2000"
        compare(page.uriPart.profileChoice, "uds-generic")
        tap(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "socketcan://vcan0?profile=uds-generic&did=1E1C:u16&identify=0&keepalive_ms=2000")
        compare(page.uriPart.profileChoice, "uds-generic")
        uri.text = "j2534://?identify=0&poll=0C&baud=500000"
        compare(page.uriPart.profileChoice, "default")
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
        verify(top >= flick.headerHeight && top + uri.height <= flick.height,
               "the field is in view, under the title row, at " + top)

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
        verify(find(page, "profile_uds-generic").checked, "the row reads its profile back")
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

    // The extended session is a box under uds-generic's DID field, composed
    // as did= is: on adds session=03, off takes it away, and a typed
    // session=03 ticks it
    function test_theExtendedSessionBoxComposesSession03() {
        const page = createPage()
        const uri = find(page, "uriField")
        const box = find(page, "extSessionBox")
        verify(!box.visible, "not under the default profile")
        type(uri, "j2534://Vendor")
        press(page, "profile_uds-generic")
        verify(box.visible, "under uds-generic")
        verify(find(page, "optionsNote").visible, "with the line on more options")
        compare(find(page, "optionsNote").text, "More options go after ?: see the address reference")
        verify(!box.checked)
        box.toggle()
        box.toggled()
        compare(uri.text, "j2534://Vendor?profile=uds-generic&session=03")
        find(page, "didField").text = "F190:text"
        compare(uri.text, "j2534://Vendor?profile=uds-generic&session=03&did=F190:text")
        box.toggle()
        box.toggled()
        compare(uri.text, "j2534://Vendor?profile=uds-generic&did=F190:text")
        type(uri, "j2534://Vendor?profile=uds-generic&session=0x03")
        verify(box.checked, "a typed session=0x03 ticks the box")
        press(page, "profile_default")
        verify(!box.visible)
        compare(uri.text, "j2534://Vendor", "core refuses session= under another profile: dropped")
        press(page, "profile_uds-generic")
        verify(box.checked, "the choice is kept for the way back")
        compare(uri.text, "j2534://Vendor?profile=uds-generic&session=03")
        press(page, "profile_raw")
        compare(uri.text, "j2534://Vendor?profile=raw", "and dropped again under Raw")

        // core refuses session= on mock:// and replay://: no box, no key
        press(page, "profile_uds-generic")
        verify(box.visible, "an adapter takes it")
        press(page, "preset_mock")
        verify(!box.visible, "not over mock://")
        verify(uri.text.indexOf("session=") < 0, "no session= composed there: " + uri.text)
        type(uri, "replay:///some/folder?profile=uds-generic")
        verify(!box.visible, "not over replay://")
        type(uri, "socketcan://vcan0?profile=uds-generic")
        verify(box.visible, "a CAN interface requesting under uds-generic takes it")
    }
}
