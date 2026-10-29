// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: Settings composes the URI from a preset (the base), the
//          profile row and uds-generic's DID field, reads a typed URI
//          back into the row and the field and keeps its other keys (the
//          vehicle run sheet's URIs pass through untouched), refuses
//          query characters in the DID field, shows core's refusal of a
//          DID list in the error line, connects a composed uds-generic
//          URI through the mock, lists serial ports after the interfaces
//          (a port's row and the elm327:// preset filling the field) and
//          the phone's Bluetooth adapters after them (a bt: or ble: row's
//          stream URI; on the desktop a bt: URI refused, its reason in the
//          error line), and
//          locks every control, the device rows among them, while a
//          connect is in flight (Windows: the fake's open stalled by a
//          knob); the Units row sets the bridge's unit system, connected
//          or not; a new page (a restart) gets back the last URI that
//          connected, read into the row, and the Record switch, while a
//          refused URI isn't kept; a device's row shows the versions last
//          seen there (Windows: the fake's, from a connect), across a
//          restart, and a damaged store shows none. The Vehicle section
//          (ADR-0027): New, Rename, Duplicate and the defaults, Forget
//          and Forget all asking once, kept across a restart; picking
//          fills the field; locked during a session; a mock session
//          leaves the vehicle alone, and (Windows, the fake) an adapter's
//          connect keeps its URI and plotted signals and its VIN is
//          learned and logged. A recording that ends is listed, and its
//          row fills the field with a replay that connects, under the
//          profile and DIDs recorded. At the narrowest phone's width (320 px) every item lies
//          inside the page, a long URI in the field and a recording's row
//          too; the URI and
//          DID fields ask the keyboard for no
//          capitals or predictions; the WiFi presets fill their
//          addresses. The live registry, and
//          the units kept across a restart (Main.qml's Settings), are
//          manual smoke.

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
        Bridge.unitSystem = 0
        Bridge.recording = false
        tryVerify(() => !Bridge.connecting, 10000, "a connect in flight ends")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
        Bridge.vehicleLabel = ""
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    readonly property var sections: ["adapterSection", "vehicleSection", "profileSection",
                                     "recordingSection", "displaySection"]

    // The page goes under the window's root, not the TestCase: TestCase is
    // an invisible item, and visible reads the effective visibility, so a
    // child of it would read invisible whatever the page did. Its sections
    // start folded; a case reading or clicking what's in them opens them
    // all, unless it asks for the page as it opens
    function createPage(asItOpens) {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        if (!asItOpens) {
            for (const name of sections)
                find(page, name).open = true
        }
        return page
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
        page.recordings = [{ title: "2026-10-28 14:30:12 · A vehicle label forty characters long",
                             uri: "replay://C:/rec/x?profile=raw",
                             source: "elm327://192.168.4.1:23?profile=raw&poll=0x0C",
                             profile: "raw" }]
        const column = find(page, "settingsColumn")
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

    // The page opens on the connect bar and five folded sections, each
    // heading naming what its section holds now; a heading opens and
    // folds its section
    function test_sectionsStartFoldedAndOpenFromTheirHeadings_data() {
        return [
            { tag: "adapter", section: "adapterSection", title: "Adapter", summary: "1 found",
              parts: ["deviceCount", "deviceList", "preset_wifi_obdx", "preset_j2534"] },
            { tag: "vehicle", section: "vehicleSection", title: "Vehicle", summary: "None",
              parts: ["vehicle_none", "vehicleLabelField", "vehicleNew"] },
            { tag: "profile", section: "profileSection", title: "What to read", summary: "Default",
              parts: ["profile_raw", "profileHint"] },
            { tag: "recording", section: "recordingSection", title: "Recording",
              summary: "off · 1 saved", parts: ["recordBox", "recordingsCaption", "recordingList"] },
            { tag: "display", section: "displaySection", title: "Display",
              summary: "100 % · metric", parts: ["units_metric", "developerBox"] },
        ]
    }
    function test_sectionsStartFoldedAndOpenFromTheirHeadings(data) {
        const page = createPage(true)
        page.recordings = [{ title: "2026-10-28 14:30:12", uri: "replay://C:/rec/x?profile=mock",
                             source: "mock://demo", profile: "mock" }]
        page.j2534Devices = []
        page.serialPorts = [{ name: "COM5", device: "" }]
        for (const name of sections)
            verify(!find(page, name).open, name + " starts folded")
        verify(find(page, "uriField").visible, "the connect bar shows")
        verify(find(page, "connectButton").visible)

        const section = find(page, data.section)
        const heading = find(page, data.section + "Heading")
        const chevron = find(heading, "sectionChevron")
        compare(heading.Accessible.name, data.title)
        compare(find(heading, "sectionSummary").text, data.summary)
        compare(chevron.rotation, 0, "pointing right while folded")
        for (const name of data.parts)
            verify(!find(page, name).visible, name + " folded")
        heading.clicked()
        verify(section.open)
        compare(chevron.rotation, 90, "pointing down while open")
        for (const name of data.parts)
            verify(find(page, name).visible, name + " shown")
        heading.clicked()
        for (const name of data.parts)
            verify(!find(page, name).visible, name + " folded again")
    }

    // Connect… elsewhere opens what to connect to, at the page's top
    function test_showAdaptersOpensWhatToConnectTo() {
        const page = createPage(true)
        page.j2534Devices = []
        page.serialPorts = [{ name: "COM5", device: "" }]
        page.showAdapters()
        verify(find(page, "adapterSection").open)
        verify(find(page, "deviceList").visible)
        verify(!find(page, "vehicleSection").open, "the rest stay folded")
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
        mouseClick(find(list.itemAtIndex(1), "deviceRowArea"))
        compare(uri.text, "elm327://COM5?profile=raw", "the row's base, the query kept")
        mouseClick(find(list.itemAtIndex(2), "deviceRowArea"))
        compare(uri.text, "elm327:///dev/ttyUSB0?profile=raw")
        mouseClick(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "j2534://Fake?profile=raw")
    }

    // The phone's Bluetooth adapters join the list after the ports
    // (ADR-0028 §2): a paired classic one fills elm327://bt:<address>, a
    // BLE one elm327://ble:<address>, the query kept; the count names
    // them. The desktop lists none, so the test sets them
    function test_bluetoothRowsFillAStreamUri() {
        const page = createPage()
        const uri = find(page, "uriField")
        const list = find(page, "deviceList")
        const count = find(page, "deviceCount")
        page.j2534Devices = []
        page.serialPorts = []
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
        mouseClick(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "elm327://bt:00:11:22:33:44:55?profile=raw")
        mouseClick(find(list.itemAtIndex(1), "deviceRowArea"))
        compare(uri.text, "elm327://ble:AA:BB:CC:DD:EE:FF?profile=raw")

        // The row in the field is outlined, whatever the query
        compare(list.itemAtIndex(0).border.width, 0)
        verify(list.itemAtIndex(1).border.width > 0, "the chosen row is outlined")
        uri.text = "elm327://bt:00:11:22:33:44:55"
        verify(list.itemAtIndex(0).border.width > 0, "a typed URI outlines its row")
        compare(list.itemAtIndex(1).border.width, 0)
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
        page.bluetoothAdapters = [{ name: "OBDLink MX+", address: "00:11:22:33:44:55", kind: "bt" }]
        const flick = find(page, "settingsFlick")
        const list = find(page, "deviceList")
        const uri = find(page, "uriField")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        waitForPolish(find(page, "settingsColumn"))

        // The list at the top of the view, the field above it
        flick.contentY = list.mapToItem(flick.contentItem, 0, 0).y
        verify(uri.mapToItem(flick, 0, 0).y < 0, "the field is out of view")
        mouseClick(find(list.itemAtIndex(0), "deviceRowArea"))
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

    // A recording that ends joins the list, and its row fills the field
    // with its replay under the profile and DIDs it was recorded with
    // (test_qml's recordings go to a scratch folder)
    function test_aRecordingsRowFillsTheFieldWithItsReplay() {
        const page = createPage()
        const uri = find(page, "uriField")
        const list = find(page, "recordingList")
        const before = list.count

        Bridge.recording = true
        uri.text = "mock://recorded?profile=uds-generic&did=F190:text"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        Bridge.disconnectSession()
        tryVerify(() => list.count === Math.min(before + 1, page.recordingsListed), 10000,
                  "the recording that ended is listed")
        verify(find(page, "recordingsCaption").visible)
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        compare(find(list.itemAtIndex(0), "recordingDetail").text,
                "mock://recorded?profile=uds-generic&did=F190:text · replays as uds-generic")

        press(page, "profile_raw")
        mouseClick(find(list.itemAtIndex(0), "recordingRowArea"))
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

    // ADR-0027 §1: every change to the list is made in the section, a
    // label typed or left to its default
    function test_theVehicleSectionMakesNamesAndForgetsVehicles() {
        let page = createPage()
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
        mouseClick(v1)
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

    function test_pickingAVehicleFillsItsConnection() {
        const page = createPage()
        press(page, "vehicleNew")
        page.saveVehicles(VehicleBook.rememberUri(page.vehicles,
                                                  "elm327://COM6?profile=uds-generic&did=F190"))
        press(page, "vehicle_none")
        find(page, "uriField").text = "mock://demo"
        mouseClick(find(page, "vehicle_v1"))
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

    // ADR-0027 §2-§4 through the fake: the connect keeps the URI, the VIN
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

        Bridge.signalModel.toggleSubscribed(0)
        const unticked = Bridge.signalModel.unplottedNames()
        compare(unticked.length, 1)
        compare(page.pickedVehicle.unplotted, unticked, "an untick is kept")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        log = []
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects again")
        compare(Bridge.signalModel.unplottedNames(), unticked, "the untick comes back")
        tryVerify(() => log.includes("Vehicle: Daily driver"), 5000, "recognized: " + log.join("\n"))
        Bridge.logMessage.disconnect(collect)
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
