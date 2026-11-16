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

    // A refusal's words break at spaces, never inside a word: a refused
    // replay address wrapped mid-word on the phone
    function test_aRefusalWrapsAtWordsNotInThem() {
        const page = createPage()
        page.width = 320
        const column = find(page, "settingsConnectionPageColumn")
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        const label = find(page, "errorLabel")
        label.visible = true
        // A word of about 0.6 of the label's width: one fits, two don't
        label.text = "W"
        waitForPolish(column)
        const word = "W".repeat(Math.floor(label.width * 0.6 / label.contentWidth))
        label.text = word
        waitForPolish(column)
        const one = label.contentWidth
        verify(one > label.width * 0.5 && one < label.width * 0.7, "a word: " + one + " in " + label.width)
        label.text = word + " " + word + " " + word
        waitForPolish(column)
        tryCompare(label, "lineCount", 3, 1000, "a word to a line")
        verify(label.contentWidth <= one + 1,
               "no line runs past a whole word: " + label.contentWidth + " vs " + one)
    }

    SignalSpy {
        id: logSpy
        signalName: "logRequested"
    }
    Component { id: logSpyComponent; SignalSpy {} }

    // On the tab-bar form the list ends with the Log, its summary the
    // newest activity line; a tap asks Main for the Log page, no subpage
    function test_aLogRowOpensTheLogOnThePhone() {
        Theme.compact = true
        const page = createPage(true)
        page.width = 384
        logSpy.target = page
        logSpy.clear()
        compare(page.pageList[page.pageList.length - 1].key, "log", "last, after Advanced")
        const row = () => find(find(page, "settingsList"), "settingsRow_log")
        verify(row().visible, "the row shows")
        const before = row()
        compare(find(row(), "settingsRowSummary").text, "Ready.", "the Log's first line until another")
        Bridge.logEvent("Connect failed: no reply from the adapter")
        tryVerify(() => find(row(), "settingsRowSummary").text
                        === "Connect failed: no reply from the adapter", 2000, "the newest line")
        verify(row() === before, "a log line doesn't remake the list's rows")
        tap(row())
        compare(logSpy.count, 1, "the Log asked for")
        compare(page.pageKey, "", "no subpage")
        verify(find(page, "settingsListPage").visible, "the list stays")
    }

    // The display's inset lines come after a connect and say nothing about
    // the session, so the row keeps the connect
    function test_theLogRowSummarizesTheSessionNotTheDisplay() {
        Theme.compact = true
        const page = createPage(true)
        page.width = 384
        const summary = () => find(find(find(page, "settingsList"), "settingsRow_log"),
                                   "settingsRowSummary")
        const spy = createTemporaryObject(logSpyComponent, testCase,
                                          { target: Bridge, signalName: "logMessage" })
        Bridge.logEvent("Connected: mock://demo")
        tryCompare(spy, "count", 1)
        compare(summary().text, "Connected: mock://demo")
        Bridge.logEvent("Display: screen 1080 x 2400 (portrait, dpr 2.8), available 1080 x 2300")
        tryCompare(spy, "count", 2)
        compare(summary().text, "Connected: mock://demo", "the Display line doesn't take it")
    }

    // Beside the sidebar, which lists the Log, the row isn't there
    function test_theLogRowIsHiddenBesideTheSidebar() {
        const page = createPage(true)
        verify(!Theme.compact, "the desktop form")
        verify(page.pageList.every(p => p.key !== "log"), "no Log in the list")
        compare(findChild(find(page, "settingsSideList"), "settingsRow_log"), null)
    }

    // A refusal's words have the Log beside them
    function test_aRefusalOffersTheLog() {
        const page = createPage()
        logSpy.target = page
        logSpy.clear()
        const show = find(page, "errorShowLog")
        verify(!show.visible, "no refusal, no Show log")
        page.requestConnect("mock://demo?profile=no-such-profile")
        tryVerify(() => find(page, "errorLabel").visible, 5000, "the refusal")
        verify(show.visible, "Show log beside it")
        tap(show)
        compare(logSpy.count, 1, "the Log asked for")
    }

    // The connect lists offer the addresses Connection offers, no others:
    // both read the same rows, so the URI, the title and the kind agree
    function test_theConnectListsOfferConnectionsAddresses() {
        const page = createPage()
        page.j2534Devices = [{ vendorName: "Vendor X", architecture: "x64", dllPath: "C:/x.dll" }]
        page.serialPorts = [{ name: "COM5", device: "" }]
        page.canInterfaces = [{ name: "can0", kind: "can", up: true }]
        page.bluetoothAdapters = [{ name: "", address: "00:11:22:33:44:55", kind: "bt" },
                                  { name: "Scope", address: "AA:BB:CC:DD:EE:FF", kind: "ble" }]
        compare(page.connectAdapters.map(a => a.uri), page.devices.map(d => d.base))
        compare(page.connectAdapters.map(a => a.uri),
                ["j2534://Vendor X", "elm327://COM5", "socketcan://can0",
                 "elm327://bt:00:11:22:33:44:55", "elm327://ble:AA:BB:CC:DD:EE:FF"])
        compare(page.connectAdapters.map(a => a.title),
                ["Vendor X  (x64)", "COM5", "can0", "00:11:22:33:44:55", "Scope"])
        compare(page.connectAdapters.map(a => a.kind),
                ["J2534 adapter", "Serial port", "CAN interface", "Bluetooth", "Bluetooth LE"])
    }

    // A relay's refusal is in the Log, and its Show log finds it
    function test_aRelayRefusalIsInTheLog() {
        const page = createPage()
        const show = find(page, "errorShowLog")
        verify(!show.visible, "nothing refused yet")
        const spy = createTemporaryObject(logSpyComponent, testCase,
                                          { target: Bridge, signalName: "logMessage" })
        // the relay button, with the relay refusing what the field holds
        find(page, "relayButton").clicked()
        const words = page.errorLabel.text
        verify(words !== "", "the relay's own words")
        compare(spy.count, 1, "one line logged")
        compare(spy.signalArguments[0][0], "Relay: " + words)
        verify(show.visible, "Show log beside the relay's words")
        logSpy.target = page
        logSpy.clear()
        tap(show)
        compare(logSpy.count, 1, "the Log asked for")
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
            { tag: "signals", title: "Signals", summary: "none yet",
              parts: ["signalImport", "signalsNone"] },
            { tag: "alerts", title: "Alerts", summary: "none",
              parts: ["alertAdd", "alertsNone"] },
            { tag: "recording", title: "Recording", summary: "off",
              parts: ["recordBox", "recordStatus"] },
            { tag: "display", title: "Display", summary: "100 % · metric · Night off",
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
            // A button, or a ComboBox, which has no checkable
            if (child.down !== undefined
                    && (child.checkable !== undefined || child.popup !== undefined))
                out.push(child)
            buttonsUnder(child, out)
        }
        return out
    }

    // Density on a phone (qml.md): every page of Settings at 384 x 832,
    // 100 %, has buttons and choice boxes a touch's size and nothing past 320 px
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

    // The side list's value reads whole at the default zoom: the Display row
    // goes under its title on up to two lines, in a row that holds it, and elides nothing
    function test_theSideListsDisplayValueIsWhole() {
        const page = createPage(true)
        page.width = 1000
        // A look changes the summary, which makes the rows again: find them each time
        const row = () => find(find(page, "settingsSideList"), "settingsRow_display")
        const summary = () => find(row(), "settingsRowSummary")
        compare(Theme.zoom, 1, "the default zoom")
        for (const look of ["dark", "daylight"]) {
            Theme.look = look
            tryVerify(() => summary().text.indexOf("100 %") === 0 && summary().visible, 2000,
                      "the summary shows: " + summary().text)
            verify(!summary().truncated, look + ": " + summary().text + " is cut")
            verify(summary().lineCount <= 2, look + ": two lines at most")
            verify(row().height >= summary().mapToItem(row(), 0, summary().height).y,
                   look + ": the row holds it")
        }
        Theme.look = "dark"
    }

    // The list keeps its 200 and the page beside it gets the rest: a layout
    // fills its row by default, and the list once took the whole width and
    // pushed the pages past the edge. Geometry, since the window is 200 px
    // and a click would miss
    function test_theListLeavesThePageItsWidth() {
        const page = createPage(true)
        const list = find(page, "settingsSideList")
        compare(list.width, 200, "the list is 200 wide")
        const shown = ["settingsConnectionPage", "settingsDisplayPage"]
        for (let i = 0; i < shown.length; ++i) {
            if (i === 1)
                page.open("display")
            const p = find(page, shown[i])
            verify(p.visible, shown[i] + " shows")
            const left = p.mapToItem(page, 0, 0).x
            verify(left >= list.mapToItem(page, list.width, 0).x,
                   shown[i] + " starts past the list: " + left)
            verify(p.width > page.width - 200 - 4 * Theme.pageMargin,
                   shown[i] + " has its width: " + p.width)
            verify(left + p.width <= page.width + 1, shown[i] + " ends inside the page")
        }
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

    // The rows of a page's buttons, top to bottom, each in the order given:
    // [[button, ...], ...]. A button is in a row when its height overlaps the
    // row's by more than half of it, since buttons of other heights centered
    // in one row have other tops (the fonts set the heights)
    function rowsOf(buttons, within) {
        const rows = []
        for (const b of buttons) {
            const top = b.mapToItem(within, 0, 0).y
            const bottom = top + b.height
            const row = rows.find(r => Math.min(r.bottom, bottom) - Math.max(r.top, top) > b.height / 2)
            if (row) {
                row.items.push(b)
                row.top = Math.min(row.top, top)
                row.bottom = Math.max(row.bottom, bottom)
            } else {
                rows.push({ top: top, bottom: bottom, items: [b] })
            }
        }
        return rows.sort((p, q) => p.top - q.top).map(r => r.items)
    }

    // rowsOf's rule: two heights centered in a row are one row, two stacked
    // are two
    Component {
        id: rowOfTwoComponent
        Row {
            Rectangle { objectName: "low"; width: 60; height: 36; anchors.verticalCenter: parent.verticalCenter }
            Rectangle { objectName: "high"; width: 60; height: 48; anchors.verticalCenter: parent.verticalCenter }
        }
    }
    Component {
        id: columnOfTwoComponent
        Column {
            Rectangle { objectName: "first"; width: 60; height: 36 }
            Rectangle { objectName: "second"; width: 60; height: 48 }
        }
    }
    function test_rowsOfGroupsByOverlapNotByTop() {
        const row = createTemporaryObject(rowOfTwoComponent, testCase)
        const low = find(row, "low"), high = find(row, "high")
        verify(low.mapToItem(row, 0, 0).y !== high.mapToItem(row, 0, 0).y, "their tops differ")
        const one = rowsOf([low, high], row)
        compare(one.length, 1, "one row")
        verify(one[0][0] === low && one[0][1] === high, "in the order given")
        const column = createTemporaryObject(columnOfTwoComponent, testCase)
        const first = find(column, "first"), second = find(column, "second")
        const two = rowsOf([second, first], column)
        compare(two.length, 2, "two rows")
        verify(two[0][0] === first && two[1][0] === second, "top first")
    }

    // A phone's page scrolled: the title row stays at the top, its back
    // arrow quiet at the left edge, and the content runs under it
    function test_theTitleRowStaysWhileThePageScrolls() {
        const page = createPage(true)
        // Short, so the page scrolls past its title row whatever the fonts' heights
        page.width = 384
        page.height = 240
        page.open("connection")
        const flick = find(page, "settingsConnectionPage")
        const header = find(flick, "settingsConnectionPageHeader")
        const back = find(flick, "settingsBack")
        const column = find(flick, "settingsConnectionPageColumn")
        waitForPolish(column)
        verify(flick.contentHeight - flick.height > header.height + 10,
               "the page scrolls past the title row: " + flick.contentHeight + " in " + flick.height)
        verify(back.visible && back.quiet, "a quiet back control")
        compare(back.mapToItem(flick, 0, 0).x, Theme.pageMargin, "at the left edge")
        compare(back.Accessible.name, "Back to Settings")
        compare(header.mapToItem(flick, 0, 0).y, 0, "the title row at the top")
        verify(column.mapToItem(flick, 0, 0).y >= header.height, "the content starts under it")
        flick.contentY = Math.min(150, flick.contentHeight - flick.height)
        compare(header.mapToItem(flick, 0, 0).y, 0, "still at the top, scrolled")
        verify(column.mapToItem(flick, 0, 0).y < 0, "the content scrolled under it")
        back.clicked()
        compare(page.pageKey, "", "the arrow goes back")
    }

    // The list's values give way whole in a narrow row (a phone zoomed far
    // in), instead of eliding to a few letters
    function test_theListsValuesGiveWayInANarrowRow() {
        const page = createPage(true)
        page.width = 384
        // The phone's list, not the hidden side list's row of the same name
        const row = find(find(page, "settingsList"), "settingsRow_display")
        const summary = find(row, "settingsRowSummary")
        tryVerify(() => row.width >= 200, 2000, "a phone's row")
        verify(summary.visible, "the value shows at a phone's width")
        page.width = 224
        tryVerify(() => row.width < 200, 2000, "a narrow row")
        verify(!summary.visible, "the value gives way")
        page.width = 384
        tryVerify(() => summary.visible, 2000, "and comes back")
    }

    // The Vehicle page's rows on a phone: the field on its own row, New,
    // Duplicate and More sharing the next, and More's four quiet actions
    // two to a row, so no row holds one button alone
    function test_theVehicleRowsNeverLeaveAButtonAlone() {
        const page = createPage(true)
        page.width = 384
        page.open("vehicle")
        press(page, "vehicleNew")
        find(page, "vehicleMore").clicked()
        const column = find(page, "settingsVehiclePageColumn")
        waitForPolish(column)
        const field = find(page, "vehicleLabelField")
        const top = rowsOf(["vehicleNew", "vehicleDuplicate", "vehicleMore"].map(n => find(page, n)),
                           column)
        compare(top.length, 1, "New, Duplicate and More share a row")
        const grid = field.parent
        verify(field.mapToItem(column, 0, 0).y < top[0][0].mapToItem(column, 0, 0).y,
               "the field has the row above them: field " + field.width + "@" + field.x + "," + field.y
               + " New @" + top[0][0].x + "," + top[0][0].y + " grid " + grid.width + " x " + grid.height
               + " columns " + grid.columns + " section " + grid.parent.width + " column " + column.width
               + " past: " + pastTheEdge(column, column, []).join("; "))
        compare(Math.round(field.width), Math.round(column.width), "the field takes the row")
        const more = ["vehicleRename", "vehicleForgetVin", "vehicleForget", "vehicleForgetAll"]
                     .map(n => find(page, n))
        for (const b of more)
            verify(b.quiet, b.objectName + " is quiet")
        const rows = rowsOf(more, column)
        compare(rows.map(r => r.length), [2, 2], "two to a row")
        for (const b of more.concat(top[0]))
            verify(b.width >= 48 - 0.5, b.objectName + " is " + b.width + " wide")

        // Wide, each row is one row
        page.width = 1000
        const moreRow = find(page, "vehicleMoreRow")
        tryVerify(() => moreRow.parent.width >= 560, 3000, "the section widens")
        verify(waitForPolish(moreRow), "the actions laid out")
        verify(waitForPolish(find(page, "vehicleNew").parent), "the field's row laid out")
        compare(rowsOf(more, column).length, 1, "one row of actions: "
                + more.map(b => b.objectName + " " + b.width + "@" + b.x + "," + b.y).join(" "))
        compare(rowsOf([field].concat(top[0]), column).length, 1, "one row with the field")
    }

    // Night's three modes share a row under their caption on a phone,
    // instead of Scheduled wrapping alone under Off and On
    function test_theNightModesShareARowOnAPhone() {
        const page = createPage(true)
        page.width = 320
        page.open("display")
        const column = find(page, "settingsDisplayPageColumn")
        waitForPolish(column)
        const modes = ["off", "on", "schedule"].map(n => find(page, "night_" + n))
        const rows = rowsOf(modes, column)
        compare(rows.length, 1, "one row: "
                + rows.map(r => r.map(b => b.objectName + " " + b.width + "@" + b.x).join("+")).join(" / "))
        // Under the caption, which has the row above to itself
        const caption = modes[0].parent.parent.parent.children[0]
        compare(caption.text, "Night:")
        verify(modes[0].mapToItem(column, 0, 0).y >= caption.mapToItem(column, 0, caption.height).y - 0.5,
               "the modes start under the caption")
        for (const b of modes)
            verify(b.width >= 48 - 0.5, b.objectName + " is " + b.width + " wide")
        verify(modes[2].mapToItem(column, modes[2].width, 0).x <= column.width + 0.5,
               "Scheduled ends inside the column")
    }

    // The signal form names what each row of choices chooses, and Save is
    // the form's one primary
    function test_theFormNamesItsChoicesAndSaveIsPrimary() {
        const page = createPage(true)
        press(page, "vehicleNew")
        page.open("ownform")
        page.formKind = "number"
        compare(find(page, "formReadBy").label, "Read by:")
        compare(find(page, "formType").label, "Type:")
        const order = find(page, "formOrderLabel")
        verify(order.visible, "a number has a byte order")
        compare(order.text, "Byte order:")
        compare(find(page, "formOrder_be").text, "big")
        compare(find(page, "formOrder_le").text, "little")
        press(page, "formKind_hex")
        verify(!order.visible, "hex has none")
        verify(find(page, "formSave").primary, "Save is primary")
        verify(!find(page, "formReadOnce").primary)
        verify(find(page, "formClear").quiet)
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
    // open on a tap of their row, and close again; the mock's is there
    // for everyone, and j2534:// is offered where there are J2534 drivers
    function test_thePresetsStartClosedAndOpenOnATap() {
        const page = createPage()
        const adapter = find(page, "adapterSection")
        const toggle = find(page, "presetsToggle")
        const names = ["preset_elm327", "preset_wifi_obdx", "preset_wifi_elm"]
        verify(toggle.visible, "the row is there")
        verify(!adapter.presetsOpen, "closed by default")
        for (const n of names.concat(["preset_j2534", "preset_mock"]))
            verify(!find(page, n).visible, n + " hidden while closed")
        compare(toggle.text, "WiFi and other addresses: show")
        toggle.clicked()
        verify(adapter.presetsOpen)
        compare(toggle.text, "WiFi and other addresses: hide")
        for (const n of names)
            verify(find(page, n).visible, n + " shown once open")
        verify(find(page, "preset_mock").visible, "the demo address, with developer mode off")
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
        compare(page.profileChoice, "uds-generic")
        tap(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "socketcan://vcan0?profile=uds-generic&did=1E1C:u16&identify=0&keepalive_ms=2000")
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

    // The vehicle, signals and recording pages say why they are dim, and
    // what a vehicle is for
    function test_thePagesSayWhyTheyAreLockedAndWhatAVehicleIsFor() {
        const page = createPage()
        page.open("vehicle")
        verify(find(page, "vehicleNote").visible, "what a vehicle is for")
        verify(!find(page, "vehicleLockedNote").visible, "idle: nothing locked")
        page.open("recording")
        verify(!find(page, "recordLockedNote").visible)
        verify(find(page, "recordNote").visible, "what Record sessions keeps, always")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => Bridge.connected, 5000)
        verify(find(page, "recordLockedNote").visible, "connected: Recording's line")
        compare(find(page, "recordLockedNote").text, "Disconnect to turn recording on or off.")
        page.open("vehicle")
        verify(find(page, "vehicleLockedNote").visible, "connected: Vehicle's line")
        Bridge.disconnectSession()
        tryVerify(() => !find(page, "vehicleLockedNote").visible, 5000, "the line goes with the session")
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
        verify(find(page, "profile_uds-generic").checked)
        verify(!find(page, "profile_default").checked)
        verify(didRow.visible)
        compare(did.text, "1E1C:u16")
        verify(hint.text.indexOf("Service 0x22") >= 0, "the hint is uds-generic's: " + hint.text)

        press(page, "profile_default")
        compare(uri.text, "mock://demo")
        press(page, "profile_uds-generic")
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16", "the DIDs are back")

        type(uri, "j2534://Vendor?profile=raw&filter=0x7E9")
        compare(page.profileChoice, "raw")
        verify(find(page, "profile_raw").checked)
        verify(!didRow.visible)
        compare(uri.text, "j2534://Vendor?profile=raw&filter=0x7E9", "left as typed")
        press(page, "profile_default")
        compare(uri.text, "j2534://Vendor?filter=0x7E9", "a typed filter stays")

        // A profile the row doesn't offer: no button lit, the hint says so
        type(uri, "j2534://Vendor?profile=obd2")
        compare(page.profileChoice, "obd2")
        verify(!find(page, "profile_default").checked)
        verify(!find(page, "profile_raw").checked)
        verify(hint.text.indexOf("obd2") >= 0, "the hint names it: " + hint.text)
        type(uri, "j2534://Vendor")
        compare(page.profileChoice, "default")
        verify(find(page, "profile_default").checked)
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

    // A restart gets back the Record switch. Recording is switched on only
    // after any connect, so no session here records
    function test_theRecordSwitchComesBack() {
        let page = createPage()
        Bridge.recording = true
        verify(find(page, "recordBox").checked)

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)
        Bridge.recording = false   // a fresh bridge starts with it off

        page = createPage()
        verify(Bridge.recording, "the Record switch comes back")
        verify(find(page, "recordBox").checked)
    }

    // An adapter's URI on every platform: havne-cli's ELM327 emulator on
    // loopback (Harness), since the mock and a replay are never kept. A
    // tree with havne-cli fails when it won't start, never skips
    function emulatorUri() {
        if (!Harness.hasHavneCli())
            skip("needs havne-cli, which this tree doesn't build")
        const uri = Harness.startElm327Emulator()
        verify(uri !== "", Harness.emulatorWhy())
        return uri
    }

    // Connects the field's URI and disconnects. The emulator serves one
    // host at a time, so this waits until it has seen the host go
    function connectAndLeave(page, uri) {
        const closed = Harness.emulatorClients("closed")
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, uri + " connects")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        tryVerify(() => Harness.emulatorClients("closed") > closed, 5000,
                  "the emulator sees the host go")
    }

    // A restart gets back the last URI that connected, not a refused one
    function test_theLastConnectedUriComesBack() {
        const emu = emulatorUri()
        let page = createPage()
        const error = find(page, "errorLabel")
        connectAndLeave(page, emu + "?profile=raw")

        find(page, "uriField").text = emu + "?profile=uds-generic"
        press(page, "connectButton")
        tryVerify(() => error.visible, 5000, "core refuses it: no ?did=")

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)

        page = createPage()
        compare(find(page, "uriField").text, emu + "?profile=raw",
                "the URI that connected, not the refused one")
        verify(find(page, "profile_raw").checked, "read back into the row")
    }

    function sameList(a, b, what) {
        compare(JSON.stringify(a), JSON.stringify(b), what)
    }

    // The recent addresses: newest first, a duplicate moved to the top, at
    // most five
    function test_recentUrisAreNewestFirstAndCapped() {
        const page = createPage()
        sameList(page.withRecentUri([], "a"), ["a"], "the first")
        sameList(page.withRecentUri(["a", "b"], "c"), ["c", "a", "b"], "newest first")
        sameList(page.withRecentUri(["a", "b", "c"], "b"), ["b", "a", "c"], "a duplicate moves up")
        sameList(page.withRecentUri(["a", "b"], "a"), ["a", "b"], "the top stays")
        sameList(page.withRecentUri(["a", "b", "c", "d", "e"], "f"), ["f", "a", "b", "c", "d"],
                 "the oldest goes past five")
        sameList(page.withRecentUri(["a", "b", "c", "d", "e"], "e"), ["e", "a", "b", "c", "d"],
                 "a duplicate doesn't cost a row")
    }

    // The stored list as it reads back: strings only, damage reads as none,
    // and with none the last URI seeds it
    function test_recentUrisReadBackFromTheStoreOrTheLastUri() {
        const page = createPage()
        sameList(page.parseRecentUris('["a","b"]', "z"), ["a", "b"], "the stored list")
        sameList(page.parseRecentUris('[]', "z"), ["z"], "seeded from the last URI")
        sameList(page.parseRecentUris('[]', ""), [], "nothing at all")
        sameList(page.parseRecentUris('not json', "z"), ["z"], "damage reads as none")
        sameList(page.parseRecentUris('{"a":1}', ""), [], "not a list")
        sameList(page.parseRecentUris('["a",3,"",null]', ""), ["a"], "strings only")
        sameList(page.parseRecentUris('["a","b","c","d","e","f"]', ""), ["a", "b", "c", "d", "e"],
                 "at most five")
    }

    function test_aFirstRunWithAnOlderUriSeedsTheList() {
        stored.setValue("uri", "elm327://COM1?profile=obd2")
        stored.sync()
        const page = createPage()
        sameList(page.recentUris, ["elm327://COM1?profile=obd2"], "what session/uri holds")
    }

    // Only a connect that succeeded is written, the newest first, and the
    // list survives a restart
    function test_onlyASuccessfulConnectWritesTheRecentUris() {
        const emu = emulatorUri()
        const raw = emu + "?profile=raw"
        let page = createPage()
        sameList(page.recentUris, [], "nothing yet")
        for (const uri of [emu, raw, emu])
            connectAndLeave(page, uri)
        sameList(page.recentUris, [emu, raw], "newest first, once")

        find(page, "uriField").text = emu + "?profile=uds-generic"
        press(page, "connectButton")
        tryVerify(() => find(page, "errorLabel").visible, 5000, "core refuses it: no ?did=")
        sameList(page.recentUris, [emu, raw], "a refusal writes nothing")

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)
        page = createPage()
        sameList(page.recentUris, [emu, raw], "kept between runs")
    }

    // A replay or the mock is never kept (Recordings is a replay's way
    // back), and one an earlier version stored is dropped as it's read
    function test_aReplayOrTheMockIsNeverKept() {
        const kept = "elm327://COM1?profile=obd2"
        const replay = "replay://" + Harness.replayFixtures + "/2026-10-25-engine-start?speed=max"
        stored.setValue("uri", replay)
        stored.setValue("recentUris", JSON.stringify([replay, kept, " MOCK://demo"]))
        stored.sync()
        let page = createPage()
        compare(find(page, "uriField").text, "", "the stored replay isn't the field's")
        sameList(page.recentUris, [kept], "nor in the list")
        sameList(page.parseRecentUris("[]", "mock://demo"), [], "the mock seeds nothing")
        verify(page.keptUri("socketcan://can0") && page.keptUri("j2534://Fake"), "an adapter is kept")

        const field = find(page, "uriField")
        for (const uri of ["mock://demo", replay]) {
            field.text = uri
            press(page, "connectButton")
            tryVerify(() => Bridge.connected, 10000, uri + " connects")
            Bridge.disconnectSession()
            tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        }
        sameList(page.recentUris, [kept], "a connect to either writes nothing")

        page.destroy()
        wait(10)
        stored.sync()
        compare(stored.value("uri"), "", "the stored replay is gone")
        compare(stored.value("recentUris"), JSON.stringify([kept]), "and the list without either")
    }

    // The bridge's URI can differ from the request: a connect from this
    // page leaves its field as it was typed
    function test_aConnectLeavesTheFieldAsTyped() {
        const page = createPage()
        const field = find(page, "uriField")
        field.text = "  mock://demo  "
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connects")
        compare(field.text, "  mock://demo  ")
    }

    // Dash's list: the address connects through the field, like one typed
    function test_aConnectFromTheListFillsTheFieldFirst() {
        const page = createPage()
        const field = find(page, "uriField")
        field.text = "mock://other"
        page.connectFromList("mock://demo")
        compare(field.text, "mock://demo", "before the connect answers")
        verify(Bridge.connecting)
        tryVerify(() => Bridge.connected, 5000, "connects")
        compare(Bridge.currentUri, "mock://demo")
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

    // ADR-0027 section 1: every change to the list is made in the section, a
    // label typed or left to its default
    function test_theVehicleSectionMakesNamesAndForgetsVehicles() {
        let page = createPage()
        page.open("vehicle")
        const field = find(page, "vehicleLabelField")
        verify(find(page, "vehicle_none").checked, "no vehicle to start")
        verify(!find(page, "vehicleDuplicate").enabled, "nothing to copy")
        compare(find(page, "vehicleLabelHint").text, "Left empty, New makes “Vehicle 1”")

        press(page, "vehicleNew")
        compare(labels(page), ["Vehicle 1"], "the default, with the field empty")
        compare(Bridge.vehicleLabel, "Vehicle 1", "the recording's label follows the pick")
        verify(find(page, "vehicle_v1").checked)

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
        verify(find(page, "vehicle_none").checked)

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

    // More opens a row of its own in the page, not a menu in the window's
    // overlay: the buttons are the page's, so they follow its zoom
    function test_moreOpensItsActionsInPlace() {
        const page = createPage()
        page.open("vehicle")
        press(page, "vehicleNew")
        const row = find(page, "vehicleMoreRow")
        verify(!row.visible, "closed to start")
        find(page, "vehicleMore").clicked()
        verify(row.visible, "More opens the row")
        for (const n of ["vehicleRename", "vehicleForgetVin", "vehicleForget", "vehicleForgetAll"]) {
            const b = find(page, n)
            verify(b.visible, n + " shows")
            verify(b.mapToItem(page, b.width, 0).x <= page.width + 0.5, n + " is inside the page")
        }
        find(page, "vehicleMore").clicked()
        verify(!row.visible, "More again closes it")
        find(page, "vehicleMore").clicked()
        press(page, "vehicleForget")
        verify(!row.visible, "a choice closes the row")
        compare(page.confirming, "forget", "and asks in place")
        press(page, "vehicleCancel")
        verify(!row.visible, "and Cancel leaves it closed")
    }

    // ---- The one store's list and Import (ADR-0064) ----

    // A signal file of one DID on the engine ECU; with obd2, one that
    // reads all of obd2 too
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

    // A signal file read through Import and kept as a new plugin
    function importFile(page, fileName, name, did, obd2) {
        const url = Harness.writeScratchFile(fileName, signalFileText(name, did, obd2))
        verify(url.toString() !== "", "the scratch file is written")
        return page.importFile(url) && page.keepPending("", "")
    }

    // The example plugin's package read through Import, under version
    // when given
    function importExample(page, version) {
        const url = version === undefined
            ? Harness.writeExamplePlugin("example-sim.havne-plugin.json")
            : Harness.writeExamplePlugin("example-sim-" + version + ".havne-plugin.json", version)
        verify(url.toString() !== "", "the example plugin is packed")
        return page.importFile(url)
    }

    // The list's row for a profile: what the page holds, and its item
    function rowData(page, profile) {
        const r = page.variantRows.find(e => e.profile === profile)
        verify(r !== undefined, profile + " is listed")
        return r
    }
    function rowOf(page, profile) {
        const i = page.variantRows.findIndex(e => e.profile === profile)
        verify(i >= 0, profile + " is listed")
        return find(page, "signalRow_" + i)
    }
    // A press on a row: picked for the vehicle, or its pick undone
    function pressRow(page, profile) {
        const b = find(rowOf(page, profile), "rowPick")
        verify(b.enabled, "the row takes a press")
        b.clicked()
    }
    function signalsRow(page) {
        return page.pageList.find(r => r.key === "signals")
    }

    // The drive view's settings (ADR-0059 section 9), a phone's: stored at
    // once and read back, each value one segment
    function test_theDriveViewsSettingsRoundTrip_data() {
        return [{ tag: "2 s", ms: 2000 }, { tag: "4 s", ms: 4000 },
                { tag: "8 s", ms: 8000 }, { tag: "Never", ms: 0 }]
    }
    function test_theDriveViewsSettingsRoundTrip(data) {
        const page = createPage()
        page.open("display")
        const display = find(page, "driveHideRow").parent
        display.driveShown = true
        display.orientationShown = true
        waitForRendering(page)
        // From another value: a click on the stored one writes nothing
        mouseClick(find(page, "driveHide_" + (data.ms === 4000 ? 2000 : 4000)))
        const seg = find(page, "driveHide_" + data.ms)
        mouseClick(seg)
        verify(seg.selected, "the chosen segment shows it")
        for (const other of [2000, 4000, 8000, 0]) {
            if (other !== data.ms)
                verify(!find(page, "driveHide_" + other).selected, "only one")
        }
        compare(find(page, "driveSettings").driveHideMs, data.ms)
        for (const mode of ["side", "follow", "upright"]) {
            const button = find(page, "driveOrientation_" + mode)
            mouseClick(button)
            compare(find(page, "driveSettings").driveOrientation, mode)
            verify(button.selected, mode + " shows it")
        }
        // A Settings object writes a change on a short timer: until it has, a read from the store sees the old value
        tryVerify(() => { uiStore.sync(); return uiStore.value("driveHideMs", -1) === data.ms }, 3000, "written where it's read")
        tryVerify(() => { uiStore.sync(); return uiStore.value("driveOrientation", "") === "upright" }, 3000, "and the orientation")
    }

    function test_theDriveViewsSettingsStartAtFourSecondsAndFollow() {
        const page = createPage()
        page.open("display")
        const stored = find(page, "driveSettings")
        compare(stored.driveHideMs, 4000)
        compare(stored.driveOrientation, "follow")
    }

    function test_theDriveSettingsAreAPhonesAndTheLockWhereThereIsOne() {
        const page = createPage()
        page.open("display")
        const row = find(page, "driveHideRow")
        const display = row.parent
        const lock = find(page, "driveOrientationRow")
        compare(display.driveShown, false, "a desktop's")
        compare(display.orientationShown, false, "a desktop's")
        display.orientationShown = true
        verify(!lock.visible, "the lock is a phone's too")
        display.orientationShown = false
        display.driveShown = false
        verify(!row.visible, "none on the desktop")
        display.driveShown = true
        display.orientationShown = false
        verify(row.visible)
        verify(!lock.visible, "no lock where the phone can't hold one")
        display.orientationShown = true
        verify(lock.visible)
    }

    // With no session a page lists its signals from Settings' choice: the
    // field's URI, or the chosen profile alone when no URI is typed
    function test_theOfflineUriFollowsTheFieldAndTheChoice() {
        const page = createPage()
        page.uriField.text = ""
        compare(page.offlineUri, "", "nothing typed, nothing chosen")

        page.chooseProfile("obd2")
        compare(page.uriField.text, "", "no URI is made up in the field")
        compare(page.offlineUri, "mock://?profile=obd2")
        verify(Bridge.profileSignals(page.offlineUri).some(r => r.ref === Bridge.gaugeSignals["speed"][1]),
               "a built-in profile's rows, with no URI typed")

        page.uriField.text = "mock://demo"
        compare(page.offlineUri, "mock://demo", "a typed URI is read as it is")
        page.chooseProfile("obd2")
        compare(page.offlineUri, "mock://demo?profile=obd2", "with the choice it composes")

        page.uriField.text = ""
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "offline.json", "Offline"))
        page.chooseProfile("plugin.offline.base")
        compare(page.offlineUri, "mock://?profile=plugin.offline.base")
        compare(Bridge.profileSignals(page.offlineUri).map(r => r.name), ["A"],
                "the picked vehicle's own plugin, before any connect")
    }

    // With Default chosen and no URI typed (as after a restart) the list
    // falls back to the picked vehicle's first pick; the picks and the
    // profiles' names reach the Dashboard
    function test_theOfflineUriFallsBackToTheVehiclesFirstPick() {
        const page = createPage()
        page.uriField.text = ""
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "fallback.json", "Fallback"))
        page.chooseProfile("default")
        compare(page.offlineUri, "mock://?profile=plugin.fallback.base", "the vehicle's pick")
        compare(Bridge.profileSignals(page.offlineUri).map(r => r.name), ["A"])

        verify(importExample(page))
        pressRow(page, "plugin.example-sim.my2019")
        page.chooseProfile("default")
        compare(page.offlineUri, "mock://?profile=plugin.fallback.base", "the first pick, in the picks' order")
        compare(JSON.stringify(page.pickedSets),
                JSON.stringify([{ profile: "plugin.fallback.base", name: "Fallback", variant: "base" },
                                { profile: "plugin.example-sim.my2019",
                                  name: "Example: the simulated car", variant: "my2019" }]))
        compare(page.profileNames["plugin.example-sim.base"], "Example: the simulated car",
                "every variant of a loaded plugin by its name")
        compare(page.profileNames["plugin.fallback.base"], "Fallback")

        page.uriField.text = "mock://demo"
        compare(page.offlineUri, "mock://demo", "a typed URI still wins")
        page.uriField.text = ""
        press(page, "vehicle_none")
        compare(page.offlineUri, "", "no vehicle, nothing to fall back to")
        compare(page.pickedSets.length, 0)
    }

    // An adapter's address under Default (the vehicle's saved one, restored
    // at a restart) lists the vehicle's first pick that reads all of obd2,
    // which stands in for Default there; a typed profile= or the Profile
    // row's choice still wins, and a plugin removed after its pick leaves
    // the address as it is
    function test_anAdaptersAddressListsTheVehiclesObd2StandIn() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "standin.json", "Standin"))
        page.uriField.text = "elm327://COM3"
        compare(page.profileChoice, "default")
        compare(page.offlineUri, "elm327://COM3", "a pick that doesn't read obd2 stands in for nothing")

        verify(importExample(page))
        pressRow(page, "plugin.example-sim.my2019")
        page.uriField.text = "elm327://COM3"
        compare(page.offlineUri, "mock://?profile=plugin.example-sim.my2019", "the pick that extends obd2")
        verify(Bridge.profileSignals(page.offlineUri).some(r => r.ref === "key:clutch_slip"),
               "the plugin's own signals are listed")
        page.uriField.text = "j2534://Some Adapter"
        compare(page.offlineUri, "mock://?profile=plugin.example-sim.my2019", "on any adapter")
        page.uriField.text = "mock://demo"
        compare(page.offlineUri, "mock://demo", "not on the demo, whose Default isn't obd2")

        page.uriField.text = "elm327://COM3?profile=raw"
        compare(page.offlineUri, "elm327://COM3?profile=raw", "a typed profile= wins")
        page.uriField.text = "elm327://COM3"
        page.chooseProfile("uds-generic")
        compare(page.offlineUri, page.uriField.text, "the Profile row's choice wins")
        verify(page.offlineUri.indexOf("profile=uds-generic") >= 0, page.offlineUri)

        page.chooseProfile("default")
        compare(page.uriField.text, "elm327://COM3")
        verify(page.removePlugin(page.pluginList.find(p => p.id === "example-sim").sha256))
        compare(page.picks, ["plugin.standin.base"], "the pick goes with the plugin")
        compare(page.offlineUri, "elm327://COM3", "and the address is listed as it is")
        page.uriField.text = ""
        compare(page.offlineUri, "mock://?profile=plugin.standin.base", "the vehicle's pick, with no address")
    }

    // A plugin is everyone's: it is imported and listed with no vehicle,
    // and says so until a vehicle picks one of its variants
    function test_aPluginNoVehiclePicksSaysItIsNotInUse() {
        const page = createPage()
        page.open("signals")
        verify(find(page, "signalsNone").visible)
        compare(signalsRow(page).summary, "none yet")
        verify(find(page, "signalImport").visible, "Import needs no vehicle")
        verify(!find(page, "settingsOwnSignalsPart").visible, "no own signals with no vehicle")
        verify(importFile(page, "extras.json", "Extras"))
        compare(find(page, "signalNote").text, "Extras 1.0.0 added")
        verify(!find(page, "signalsNone").visible)
        compare(signalsRow(page).summary, "1 plugin")
        let row = rowOf(page, "plugin.extras.base")
        verify(!find(row, "rowPick").enabled, "no vehicle to pick it for")
        verify(find(row, "rowUnused").visible)
        compare(find(row, "rowUnused").text, "Not in use: add or pick a vehicle, then pick it here.")
        verify(!find(row, "rowCaption").visible, "no car, no groups")

        press(page, "vehicleNew")
        compare(signalsRow(page).summary, "1 plugin, none picked")
        row = rowOf(page, "plugin.extras.base")
        verify(find(row, "rowUnused").visible, "still no vehicle's")
        compare(find(row, "rowUnused").text, "Not in use: press it to pick it for Vehicle 1.")
        verify(!find(row, "rowCaption").visible, "imported for no vehicle, and not picked")
        verify(find(page, "settingsOwnSignalsPart").visible, "own signals with a vehicle")
        pressRow(page, "plugin.extras.base")
        compare(page.picks, ["plugin.extras.base"])
        row = rowOf(page, "plugin.extras.base")
        compare(find(row, "rowCaption").text, "For this car:", "a pick is this car's")
        verify(find(row, "rowPick").checked)
        verify(find(row, "rowPickMark").visible, "a mark, not the fill alone")
        verify(!find(row, "rowUnused").visible)
        compare(find(row, "rowPick").Accessible.name, "Extras 1.0.0, 1 signal · yours, picked")
        compare(signalsRow(page).summary, "1 plugin: Extras")
        verify(signalsRow(page).warn !== true)

        // Another vehicle's pick keeps it in use; a press again undoes a pick
        press(page, "vehicleNew")
        compare(page.picks, [])
        verify(!find(rowOf(page, "plugin.extras.base"), "rowUnused").visible, "Vehicle 1 picks it")
        verify(!rowData(page, "plugin.extras.base").forCar, "Vehicle 1's pick, not this one's")
        compare(signalsRow(page).summary, "1 plugin, none picked")
        pressRow(page, "plugin.extras.base")
        verify(rowData(page, "plugin.extras.base").forCar)
        pressRow(page, "plugin.extras.base")
        compare(page.picks, [], "pressed again, undone")
        verify(!rowData(page, "plugin.extras.base").forCar, "and no longer this car's")
    }

    // One Import, a signal file: it waits for where it goes, and As a new
    // plugin makes it a plugin of the user's own, imported for the picked
    // vehicle and picked for it at once. The book is stored under its own
    // key, the earlier version's left as it was
    function test_aSignalFileWaitsThenIsKeptAsAPluginAndPicked() {
        storedVehicles.setValue("document", "an earlier version's text")
        storedVehicles.sync()
        let page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(page.importFile(Harness.writeScratchFile("extras.json", signalFileText("Extras"))))
        verify(find(page, "importPanel").visible, "it waits for where it goes")
        compare(find(page, "importText").text, "Extras: a signal file")
        verify(findChild(page, "importInto_0") === null, "no plugin of the user's own to take it")
        compare(Harness.storedPlugins(), [], "nothing kept yet")
        press(page, "importAsNew")
        verify(!find(page, "importPanel").visible)
        compare(Harness.storedPlugins().length, 1)
        compare(page.picks, ["plugin.extras.base"], "picked at once for the picked vehicle")
        compare(find(page, "signalNote").text, "Extras 1.0.0 added: picked for Vehicle 1")
        const row = rowOf(page, "plugin.extras.base")
        compare(find(row, "rowCaption").text, "For this car:")
        compare(find(row, "rowTitle").text, "Extras 1.0.0")
        compare(find(row, "rowReads").text, "1 signal · yours")
        verify(find(row, "rowPick").checked)

        // Its profile is a choice under Profile, and a session decodes
        // with it; nothing changes the store or the picks meanwhile
        find(page, "uriField").text = "mock://demo"
        const choice = find(page, "profile_plugin.extras.base")
        compare(choice.text, "Extras")
        choice.clicked()
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.extras.base")
        compare(find(page, "profileHint").text, "The signals of Extras 1.0.0, picked for this vehicle")
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "a session decodes with it")
        compare(find(page, "errorLabel").text, "")
        page.open("signals")
        verify(find(page, "signalLockedNote").visible)
        verify(!find(page, "signalImport").enabled)
        verify(!find(rowOf(page, "plugin.extras.base"), "rowPick").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)

        page.destroy()
        wait(10)
        tryVerify(() => { storedVehicles.sync(); return String(storedVehicles.value("book", "")) !== "" },
                  2000, "the book is written")
        const book = JSON.parse(storedVehicles.value("book"))
        compare(book.format, 2)
        compare(book.list[0].picks, ["plugin.extras.base"])
        compare(storedVehicles.value("document"), "an earlier version's text",
                "the earlier version's key is never written")
        page = createPage()
        compare(page.picks, ["plugin.extras.base"], "a restart keeps the pick")
        verify(find(rowOf(page, "plugin.extras.base"), "rowPick").checked)

        // The same file again is the same plugin; two signals are counted
        verify(importFile(page, "again.json", "Extras"))
        compare(find(page, "signalNote").text, "Extras 1.0.0 is here already")
        compare(Harness.storedPlugins().length, 1)
        const pair = JSON.parse(signalFileText("Pair"))
        pair.signals.push({ key: "b", name: "B", read: { service: "22", number: "D101" },
                            field: { byte: 0, bits: 8 } })
        verify(page.importFile(Harness.writeScratchFile("pair.json", JSON.stringify(pair))))
        press(page, "importAsNew")
        compare(find(rowOf(page, "plugin.pair.base"), "rowReads").text, "2 signals · yours")
        compare(signalsRow(page).summary, "2 plugins: Extras and 1 more")
    }

    // Through the phone's picker: the Signals page's one open hands its
    // pick to the import, and a picker with no document says so, with no
    // earlier note left beside the words and Replace's target dropped
    function test_theSignalsPagesPickReachesItsImport() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        DocumentPicker.setPickerForTesting(true)
        const dialog = find(page, "signalImportDialog")
        dialog.usePicker = true
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1,
            Harness.writeScratchFile("picked (1).json", signalFileText("Picked")).toString(),
            "picked (1).json")
        verify(find(page, "importPanel").visible, "the pick reached the import: the file waits")
        compare(find(page, "importText").text, "Picked: a signal file")
        verify(page.keepPending("", ""))
        compare(find(page, "signalNote").text, "Picked 1.0.0 added: picked for Vehicle 1")

        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, "", "")
        compare(find(page, "signalProblem").text, "The picker returned no document")
        compare(find(page, "signalNote").text, "", "no earlier note beside the words")

        page.signalProblem = ""
        page.startReplace(rowData(page, "plugin.picked.base"))
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, "", "")
        compare(find(page, "signalProblem").text, "The picker returned no document")
        compare(page.replaceTarget, null, "and the next Import is an import")
    }

    // Through the phone's save picker: the Signals page's one save offers
    // the page's own name each time and hands its pick to the write; a
    // picker with no document says so, with no earlier note left beside
    // the words and no save left waiting, and a cancel leaves none either
    function test_theSignalsPagesSavePickReachesItsWrite() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(page.importFile(Harness.writeScratchFile("picked.json", signalFileText("Picked")))
               && page.keepPending("", ""))
        page.open("signals")
        DocumentPicker.setPickerForTesting(true)
        const row = rowData(page, "plugin.picked.base")
        const save = find(page, "signalSaveDialog")
        save.usePicker = true
        page.saveVariantAs(row)
        compare(DocumentPicker.titleForTesting(), "picked.json")
        const out = Harness.writeScratchFile("picked out (1).json", "")
        DocumentPicker.deliver(save.request, -1, out.toString(), "89", -1)
        compare(find(page, "signalProblem").text, "")
        compare(find(page, "signalNote").text, "Picked saved to the file you chose")
        verify(Harness.readScratchFile(out).includes("Picked"), "the picked document holds the file")

        page.saveVariantAs(row)
        compare(DocumentPicker.titleForTesting(), "picked.json",
                "the page's name again, never the last answer's")
        DocumentPicker.deliver(save.request, -1, "", "")
        compare(find(page, "signalProblem").text, "The picker returned no document")
        compare(find(page, "signalNote").text, "", "no earlier note beside the words")
        compare(page.saving, null, "and no save waits")

        page.exportPluginAs(row.sha256)
        compare(DocumentPicker.titleForTesting(), "picked.havne-plugin.json",
                "an export's own name")
        DocumentPicker.deliver(save.request, 0, "", "")
        compare(page.saving, null, "a cancel leaves no save waiting")
        compare(find(page, "signalProblem").text, "", "and says nothing")

        // A pick answered with no save waiting is written nowhere, and
        // its document's verdict is taken as any refused save's is
        const uri = "content://com.android.externalstorage.documents/document/"
                    + "primary%3ADownload%2Fpicked.json"
        page.saveVariantAs(row)
        page.saving = null
        DocumentPicker.deliver(save.request, -1, uri, "picked.json",
                               DocumentPicker.launchedMsForTesting() + 50)
        compare(DocumentPicker.verdictOfForTesting(uri), 0, "the refusal took the verdict")
    }

    // A file core refuses, or one that is none of the three kinds, keeps
    // nothing and says why, never naming the path
    function test_aFileCoreRefusesOrOfNoKindIsNotKept() {
        const page = createPage()
        press(page, "vehicleNew")
        const bad = Harness.writeScratchFile("bad.json", signalFileText("Bad").replace("bits", "bitz"))
        verify(!(page.importFile(bad) && page.keepPending("", "")))
        let words = find(page, "signalProblem").text
        verify(words.includes("bitz") && words.includes("line"), words)
        verify(!words.includes("bad.json"), "never the path")
        compare(page.picks, [])
        compare(Harness.storedPlugins(), [])
        page.discardImport()

        verify(!page.importFile(Harness.writeScratchFile("other.json", "{\"ecu\": 1}")))
        compare(find(page, "signalProblem").text,
                "The file is neither a plugin, a signal file nor an OBDb signal set")
        verify(!find(page, "importPanel").visible, "nothing waits")

        verify(!page.importFile(Harness.writeScratchFile("broken.havne-plugin.json",
                                                         "{\"havne_plugin\": 2}")))
        words = find(page, "signalProblem").text
        verify(words !== "" && !words.includes("broken.havne"), words)
        compare(Harness.storedPlugins(), [])

        verify(importFile(page, "good.json", "Extras"))
        compare(find(page, "signalProblem").text, "", "a good import clears the last refusal")
    }

    // Into: the waiting file as a variant of a plugin of the user's own,
    // under the id typed, new or in a variant's place. Replace... on a row
    // puts the next signal file read in that row's place, and nothing else
    function test_aSignalFileGoesIntoAPluginOfTheUsersOwnOrInAVariantsPlace() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "extras.json", "Extras"))
        verify(page.importFile(Harness.writeScratchFile("track.json", signalFileText("Track day", "D101"))))
        const into = find(page, "importInto_0")
        compare(into.text, "Into Extras")
        verify(!find(page, "importVariantField").visible, "no variant asked before Into")
        into.clicked()
        verify(find(page, "importVariantField").visible)
        compare(page.importVariantField.text, "track-day", "the id the file's name gives")
        verify(!find(page, "importReplaces").visible)
        page.importVariantField.text = "base"
        compare(find(page, "importReplaces").text, "replaces base")
        page.importVariantField.text = "Bad Id"
        press(page, "importKeep")
        verify(find(page, "signalProblem").text !== "", "core refuses the id")
        verify(find(page, "importPanel").visible, "and the file still waits")
        compare(page.pluginList[0].version, "1.0.0", "nothing changed")
        page.importVariantField.text = "track"
        press(page, "importKeep")
        compare(find(page, "signalProblem").text, "")
        verify(!find(page, "importPanel").visible)
        compare(find(page, "signalNote").text,
                "Extras 1.1.0 now has the variant track: picked for Vehicle 1")
        compare(page.variantRows.map(r => page.rowTitle(r)),
                ["Extras 1.1.0: base", "Extras 1.1.0: track"])
        compare(page.picks, ["plugin.extras.base", "plugin.extras.track"])
        compare(Harness.storedPlugins().length, 1, "one copy: the plugin's newest")
        compare(signalsRow(page).summary, "1 plugin: Extras: base and 1 more")

        // The same id again takes that variant's place; its pick stays
        verify(page.importFile(Harness.writeScratchFile("other.json", signalFileText("Other", "D102"))))
        find(page, "importInto_0").clicked()
        page.importVariantField.text = "track"
        press(page, "importKeep")
        compare(find(page, "signalNote").text, "Extras 1.2.0: variant track replaced")
        compare(page.picks.length, 2)

        // The field emptied: the id is the one the file's name gives, so
        // the variant of that id is the one said, replaced and picked
        pressRow(page, "plugin.extras.base")
        compare(page.picks, ["plugin.extras.track"])
        verify(page.importFile(Harness.writeScratchFile("base.json", signalFileText("Base", "D104"))))
        find(page, "importInto_0").clicked()
        compare(page.importVariantField.text, "base")
        page.importVariantField.text = ""
        compare(find(page, "importReplaces").text, "replaces base")
        press(page, "importKeep")
        compare(find(page, "signalNote").text, "Extras 1.3.0: variant base replaced: picked for Vehicle 1")
        compare(page.picks, ["plugin.extras.track", "plugin.extras.base"], "not the plugin's last variant")
        verify(Bridge.pluginVariantText(page.pluginList[0].sha256, "base").text.includes("D104"))

        // Replace...: only a signal file takes a variant's place
        page.startReplace(rowData(page, "plugin.extras.base"))
        compare(find(page, "signalImportDialog").title, "Replace Extras: base with a signal file")
        verify(!page.importFile(Harness.writeExamplePlugin("example-sim.havne-plugin.json")))
        compare(find(page, "signalProblem").text, "Not a signal file: only one takes a variant's place")
        compare(page.pluginList.length, 1, "the package was not imported")
        compare(page.replaceTarget, null, "and the next Import is an import")
        // It picks nothing: a variant the vehicle doesn't pick stays so
        pressRow(page, "plugin.extras.base")
        compare(page.profileChoice, "default")
        page.startReplace(rowData(page, "plugin.extras.base"))
        verify(page.importFile(Harness.writeScratchFile("third.json", signalFileText("Third", "D103"))))
        verify(!find(page, "importPanel").visible, "kept at once")
        compare(find(page, "signalNote").text, "Extras 1.4.0: variant base replaced")
        verify(Bridge.pluginVariantText(page.pluginList[0].sha256, "base").text.includes("D103"),
               "the row reads the new file")
        compare(page.picks, ["plugin.extras.track"], "Replace picks nothing")
        compare(page.profileChoice, "default", "and the field reads what it read")
        pressRow(page, "plugin.extras.base")

        // One variant of several is removed alone, its pick with it
        page.openRow = "plugin.extras.track"
        const remove = find(rowOf(page, "plugin.extras.track"), "rowRemoveVariant")
        verify(remove.visible)
        remove.clicked()
        find(rowOf(page, "plugin.extras.track"), "signalsConfirmRemove").clicked()
        compare(page.picks, ["plugin.extras.base"])
        compare(page.variantRows.map(r => page.rowTitle(r)), ["Extras 1.5.0"])
        page.openRow = "plugin.extras.base"
        const row = rowOf(page, "plugin.extras.base")
        verify(find(row, "rowReplace").visible)
        verify(!find(row, "rowRemoveVariant").visible, "never a plugin's last variant")
        verify(!find(row, "rowCopy").visible, "the user's own needs no copy")
    }

    // One Import, a package: it goes in as it is, each variant a row the
    // picked vehicle picks; the car's VIN (typed in the lookup here: the
    // mock has none) puts the variants it covers first. A pick under
    // Default is read at once and connects; a newer version takes the
    // earlier one's place, and a removal takes the picks
    function test_aPackageGoesInAsItIsAndItsVariantsArePickedHere() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importExample(page))
        verify(!find(page, "importPanel").visible, "a package asks nothing")
        compare(find(page, "signalNote").text,
                "Example: the simulated car 1.0.0 added: pick a variant below")
        compare(page.variantRows.map(r => page.rowTitle(r)),
                ["Example: the simulated car 1.0.0: base", "Example: the simulated car 1.0.0: my2019"])
        // Its Boost takes the place of obd2's, so base adds four to obd2
        compare(page.variantRows.map(r => page.rowReads(r)), ["obd2 + 4 signals", "obd2 + 5 signals"])
        compare(page.picks, [], "several variants: the pick is the user's")
        verify(!find(rowOf(page, "plugin.example-sim.base"), "rowCaption").visible, "no VIN, no groups")
        compare(find(rowOf(page, "plugin.example-sim.my2019"), "rowUnused").text,
                "Not in use: press a variant to pick it for Vehicle 1.")
        compare(Harness.storedPlugins().length, 1)

        find(page, "vinLookupField").text = "ZZZFAKE11G0000001"   // 2016: before my2019
        compare(page.variantRows.map(r => r.variant), ["base", "my2019"])
        compare(page.variantRows.map(r => r.caption), ["For this car:", ""], "a match lifts the plugin whole")
        verify(!find(rowOf(page, "plugin.example-sim.my2019"), "rowCaption").visible)
        find(page, "vinLookupField").text = "ZZZFAKE11M0000001"   // 2021
        compare(page.variantRows.map(r => r.caption), ["For this car:", ""])
        find(page, "vinLookupField").text = ""
        verify(!rowData(page, "plugin.example-sim.base").forCar, "no VIN, no match")

        const field = find(page, "uriField")
        field.text = "mock://demo"
        pressRow(page, "plugin.example-sim.my2019")
        compare(page.picks, ["plugin.example-sim.my2019"])
        compare(field.text, "mock://demo?profile=plugin.example-sim.my2019", "read at once under Default")
        compare(page.variantRows.map(r => r.variant), ["base", "my2019"])
        compare(page.variantRows.map(r => r.caption), ["For this car:", ""], "a pick lifts its plugin")
        verify(find(rowOf(page, "plugin.example-sim.my2019"), "rowPickMark").visible, "with its mark")
        verify(!rowData(page, "plugin.example-sim.base").picked, "the other variant beside it")
        const choice = find(page, "profile_plugin.example-sim.my2019")
        compare(choice.text, "Example: the simulated car: my2019")
        verify(choice.checked)
        compare(find(page, "profileHint").text,
                "obd2's signals and those of Example: the simulated car 1.0.0: my2019, "
                + "picked for this vehicle")
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "a session decodes with the variant")
        compare(find(page, "errorLabel").text, "")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)

        verify(importExample(page, "1.1.0"))
        compare(find(page, "signalNote").text, "Example: the simulated car 1.1.0 replaces 1.0.0")
        compare(Harness.storedPlugins().length, 1)
        compare(page.picks, ["plugin.example-sim.my2019"], "the pick is the new version's")
        verify(importExample(page, "1.1.0"))
        compare(find(page, "signalNote").text, "Example: the simulated car 1.1.0 is here already")

        // Someone else's plugin can be imported again: its removal asks nothing
        page.openRow = "plugin.example-sim.base"
        const row = rowOf(page, "plugin.example-sim.base")
        compare(find(row, "rowRemove").text, "Remove the plugin")
        find(row, "rowRemove").clicked()
        compare(page.confirming, "")
        verify(find(page, "signalsNone").visible)
        compare(Harness.storedPlugins(), [])
        compare(page.picks, [], "a removed plugin's variant is no pick")
        compare(field.text, "mock://demo", "the field reads Default again")
        tryVerify(() => findChild(page, "profile_plugin.example-sim.my2019") === null, 1000,
                  "a variant no plugin registers isn't offered")
    }

    // A plugin is this car's when it was imported for the vehicle, a VIN
    // matches one of its variants or the vehicle picks one. It then lists
    // whole under "For this car:", its other variants beside that one, so
    // a first pick lifts the plugin's rows once and a second moves nothing
    function test_aPluginWithAVariantOfThisCarsListsWhole() {
        const page = createPage()
        page.open("signals")
        verify(importExample(page))   // with no vehicle: nobody's import
        press(page, "vehicleNew")
        verify(importFile(page, "extras.json", "Extras"))
        const extras = "plugin.extras.base"
        const base = "plugin.example-sim.base"
        const my2019 = "plugin.example-sim.my2019"
        pressRow(page, extras)   // the import's pick, undone: still this vehicle's import
        compare(page.picks, [])
        compare(page.variantRows.map(r => r.profile), [extras, base, my2019])
        compare(page.variantRows.map(r => r.caption), ["For this car:", "Other plugins:", ""])
        compare(find(rowOf(page, base), "rowCaption").text, "Other plugins:")

        pressRow(page, my2019)
        compare(page.picks, [my2019])
        compare(page.variantRows.map(r => r.caption), ["For this car:", "", ""], "the pick lifts its plugin")
        verify(rowData(page, base).forCar, "the variant beside the picked one")
        verify(!rowData(page, base).picked)
        verify(find(rowOf(page, my2019), "rowPickMark").visible)
        const order = page.variantRows.map(r => r.profile)
        compare(order.indexOf(my2019), order.indexOf(base) + 1, "the plugin's variants in their order")
        pressRow(page, my2019)
        pressRow(page, base)
        compare(page.variantRows.map(r => r.profile), order, "another variant picked moves nothing")
        pressRow(page, base)
        compare(page.picks, [])
        compare(page.variantRows.map(r => r.caption), ["For this car:", "Other plugins:", ""],
                "the picks undone, the plugin is back with the others")

        find(page, "vinLookupField").text = "ZZZFAKE11G0000001"   // 2016: base's, before my2019
        compare(page.variantRows.map(r => r.caption), ["For this car:", "", ""], "a VIN's match lifts it too")
        verify(rowData(page, my2019).forCar, "the variant the VIN doesn't cover, beside the one it does")
    }

    // A press keeps the focus on its variant's row: the rows stay as they
    // are through a pick and its undoing, and a first pick, which lifts
    // the plugin past another, takes the focus with the row
    function test_aPickKeepsTheFocusOnItsRow() {
        const page = createPage()
        page.open("signals")
        verify(importExample(page))
        verify(importFile(page, "zed.json", "Zed"))   // with no vehicle: nobody's, after the example
        press(page, "vehicleNew")
        const my2019 = "plugin.example-sim.my2019"
        const zed = "plugin.zed.base"
        compare(page.variantRows.map(r => r.profile), ["plugin.example-sim.base", my2019, zed])

        const button = find(rowOf(page, my2019), "rowPick")
        button.forceActiveFocus()
        verify(button.activeFocus)
        button.clicked()
        compare(page.picks, [my2019])
        verify(find(rowOf(page, my2019), "rowPick") === button, "the same button after the pick")
        verify(button.checked)
        verify(button.activeFocus, "the pick keeps the focus")
        button.clicked()
        compare(page.picks, [], "pressed again, undone")
        verify(find(rowOf(page, my2019), "rowPick") === button && button.activeFocus, "and once undone")

        // Zed's pick lifts it past the example: the focus goes with the row
        const last = find(rowOf(page, zed), "rowPick")
        last.forceActiveFocus()
        last.clicked()
        compare(page.variantRows.map(r => r.profile), [zed, "plugin.example-sim.base", my2019])
        const moved = find(rowOf(page, zed), "rowPick")
        verify(moved !== last, "the row is in another place")
        verify(moved.activeFocus, "the focus is on the variant pressed")
        verify(moved.checked)
        moved.clicked()
        compare(page.variantRows.map(r => r.profile), ["plugin.example-sim.base", my2019, zed])
        verify(find(rowOf(page, zed), "rowPick").activeFocus, "and back with it")
    }

    // A session's matches are kept on the vehicle its VIN identified, by
    // their profiles alone and never another car's, and stay once the VIN
    // goes with the session: through a page made again, a VIN typed in the
    // lookup, which is asked about in their place, and another vehicle's
    // turn, which has none of them. A replaced plugin takes its own, and a
    // session whose VIN is no vehicle's marks only while it runs
    function test_aSessionsVinMatchesStayWithItsVehicle() {
        let page = createPage()
        press(page, "vehicleNew")
        const car = page.pickedVehicle.id
        page.open("signals")
        verify(importExample(page))
        const base = "plugin.example-sim.base"
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        Bridge.identifyForTesting("ZZZFAKE11G0000001")   // a 2016: base alone
        tryVerify(() => rowData(page, base).forCar, 1000, "marked while it runs")
        compare(page.pickedVehicle.vinMatches, [], "the VIN is no vehicle's: nothing kept")

        // The VIN names the picked vehicle (an adapter's recognize does
        // it; the mock's URI is no adapter's, so the key is set here)
        const salt = page.vehicles.salt !== "" ? page.vehicles.salt : Bridge.newSalt()
        const doc = VehicleBook.copy(page.vehicles)
        doc.salt = salt
        page.saveVehicles(VehicleBook.change(doc, v => v.vinKey = Bridge.vinKey(salt)))
        Bridge.identifyForTesting("ZZZFAKE11G0000001")
        compare(page.pickedVehicle.vinMatches, [base])
        verify(!VehicleBook.serialize(page.vehicles).includes("ZZZFAKE"), "never the VIN")

        // Another car's VIN with this vehicle picked (a replay of it, which
        // recognizes no vehicle) keeps nothing
        Bridge.identifyForTesting("ZZZFAKE11M0000001")   // 2021: both variants
        compare(page.pickedVehicle.vinMatches, [base], "not another car's")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        compare(Bridge.pluginMatches(""), [], "the VIN went with the session")
        verify(rowData(page, base).forCar, "the vehicle's matches stay")

        // The page made again (the app's next start) reads them from the book
        page.destroy()
        wait(10)
        page = createPage()
        page.open("signals")
        compare(page.pickedVehicle.id, car)
        verify(rowData(page, base).forCar)

        // A VIN typed in the lookup is the one asked about, in their place
        find(page, "vinLookupField").text = "1FAFAKE13Y0000001"   // another manufacturer
        verify(!rowData(page, base).forCar, "the typed VIN's: none")
        find(page, "vinLookupField").text = ""
        verify(rowData(page, base).forCar)

        // Another vehicle has none of them; its own turn back has them
        press(page, "vehicleNew")
        verify(!rowData(page, base).forCar, "not this one's")
        page.pickVehicle(car)
        verify(rowData(page, base).forCar)

        // A replacement takes the replaced plugin's matches (its new
        // version's match may not cover the car) and keeps the pick
        pressRow(page, base)
        verify(importExample(page, "1.1.0"))
        compare(page.pickedVehicle.vinMatches, [])
        compare(page.picks, [base], "the pick kept")
        pressRow(page, base)
        verify(!rowData(page, base).forCar, "no match left, and no pick")

        // A session whose VIN is no vehicle's: marked while it runs alone
        press(page, "vehicleNew")
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected again")
        Bridge.identifyForTesting("ZZZFAKE11M0000001")
        tryVerify(() => rowData(page, base).forCar, 1000, "marked")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        tryVerify(() => !rowData(page, base).forCar, 1000, "gone with the VIN")
        compare(page.pickedVehicle.vinMatches, [], "nothing kept on this one")
    }

    // A session's VIN puts the variants it covers first (ADR-0052 section
    // 5); nothing is picked while it runs, and once it ends the matches
    // stay on the vehicle its VIN identified, though the VIN goes
    function test_aSessionsVinMarksItsVariantsPickedOnceItEnds() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importExample(page))
        verify(importFile(page, "extras.json", "Extras"))
        pressRow(page, "plugin.extras.base")   // the import's pick, undone
        // The car's VIN names the picked vehicle
        const doc = VehicleBook.copy(page.vehicles)
        doc.salt = Bridge.newSalt()
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        Bridge.identifyForTesting("ZZZFAKE11M0000001")   // a 2021 the example covers
        page.saveVehicles(VehicleBook.change(doc, v => v.vinKey = Bridge.vinKey(doc.salt)))
        Bridge.identifyForTesting("ZZZFAKE11M0000001")
        tryVerify(() => rowData(page, "plugin.example-sim.my2019").forCar, 1000, "marked")
        compare(page.variantRows.map(r => r.caption), ["For this car:", "", ""],
                "the plugin imported for this vehicle and the VIN's two")
        verify(!find(rowOf(page, "plugin.example-sim.base"), "rowPick").enabled)
        verify(!find(page, "signalImport").enabled)
        verify(!find(page, "formSave").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        compare(page.pickedVehicle.vinMatches,
                ["plugin.example-sim.base", "plugin.example-sim.my2019"], "kept on the vehicle")
        verify(rowData(page, "plugin.example-sim.base").forCar, "the session's matches stay")
        pressRow(page, "plugin.example-sim.base")
        compare(page.picks, ["plugin.example-sim.base"])
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.example-sim.base")
        pressRow(page, "plugin.example-sim.base")
        compare(page.picks, [], "pressed again, undone")
        compare(find(page, "uriField").text, "mock://demo")

        // Another vehicle has no matches of this car's
        press(page, "vehicleNew")
        verify(!rowData(page, "plugin.example-sim.base").forCar)
    }

    // On a phone the removal's question is drawn where Remove was pressed:
    // the last plugin of a list longer than the page asks inside the
    // page's view, never at the top of a page whose controls then all read
    // disabled
    function test_aRemovalAsksInsideAPhonesView() {
        const page = createPage()
        page.width = 384
        page.height = 832
        press(page, "vehicleNew")
        page.open("signals")
        for (let i = 1; i <= 24; ++i)
            verify(importFile(page, "p" + i + ".json", "Plugin " + i, "D1" + String(i).padStart(2, "0")))
        const last = page.variantRows[page.variantRows.length - 1].profile
        page.openRow = last
        const flick = find(page, "settingsSignalsPage")
        const row = rowOf(page, last)
        const remove = find(row, "rowRemove")
        verify(waitForPolish(page.Window.window), "laid out")
        // Scrolled as a thumb would: Remove at the view's foot
        const bottomOf = item => item.mapToItem(flick.contentItem, 0, item.height).y
        flick.contentY = Math.max(0, bottomOf(remove) - flick.height)
        // The page's top, Import and all, is scrolled out of the view
        const importBottom = bottomOf(find(page, "signalImport"))
        verify(importBottom < flick.contentY + flick.headerHeight,
               "Import ends at " + importBottom + ", the view starts at " + (flick.contentY + flick.headerHeight))
        remove.clicked()
        const question = find(row, "signalsConfirm")
        tryVerify(() => question.visible && question.height > 0, 2000, "the row asks")
        verify(waitForPolish(page.Window.window), "laid out")
        for (const name of ["signalsConfirmText", "signalsConfirmRemove", "signalsConfirmCancel"]) {
            const item = find(question, name)
            const top = item.mapToItem(flick.contentItem, 0, 0).y
            verify(top >= flick.contentY + flick.headerHeight - 0.5
                   && bottomOf(item) <= flick.contentY + flick.height + 0.5,
                   name + " from " + top + " to " + bottomOf(item) + " in a view from "
                   + (flick.contentY + flick.headerHeight) + " to " + (flick.contentY + flick.height))
        }
        find(question, "signalsConfirmCancel").clicked()
        compare(page.confirming, "")
    }

    // Remove this variant asks in its row first, as the whole plugin's
    // Remove does: the list is as it was until the question is answered
    function test_removeThisVariantAsksFirst() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "trip.json", "Trip extras"))
        verify(page.importFile(Harness.writeScratchFile("two.json", signalFileText("Two", "D101"))))
        verify(page.keepPending(page.pluginList[0].sha256, "two"))
        const two = "plugin.trip-extras.two"
        const listed = () => page.variantRows.map(r => r.profile).sort()
        const before = listed()
        compare(before.length, 2)
        page.openRow = two
        const row = rowOf(page, two)
        find(row, "rowRemoveVariant").clicked()
        compare(listed(), before, "asked first")
        const question = find(row, "signalsConfirm")
        verify(question.visible, "in the variant's row")
        compare(find(question, "signalsConfirmText").text,
                "Remove the variant two of Trip extras? It is yours, and no other copy exists")
        verify(!find(row, "rowActions").visible, "the question in the actions' place")
        verify(!find(page, "signalImport").enabled, "nothing else past the question")
        find(question, "signalsConfirmCancel").clicked()
        compare(listed(), before, "Cancel keeps it")
        find(row, "rowRemoveVariant").clicked()
        find(question, "signalsConfirmRemove").clicked()
        compare(listed(), ["plugin.trip-extras.base"])
        compare(page.confirming, "")
    }

    // A signal file waiting on Into keeps no choice of a plugin that goes
    // or is rewritten (a variant dropped gives it a new sha256): Keep
    // hides until Into is chosen again, and the file still waits
    function test_aWaitingIntoChoiceGoesWithItsPlugin() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "trip.json", "Trip extras"))
        verify(page.importFile(Harness.writeScratchFile("two.json", signalFileText("Two", "D101"))))
        verify(page.keepPending(page.pluginList[0].sha256, "two"))
        verify(page.importFile(Harness.writeScratchFile("three.json", signalFileText("Three", "D102"))))
        press(page, "importInto_0")
        verify(find(page, "importKeep").visible, "Into chosen: Keep shows")

        verify(page.removeVariant(rowData(page, "plugin.trip-extras.two")))
        compare(page.importInto, "", "the rewritten plugin's old sha256 is no choice")
        verify(!find(page, "importKeep").visible, "Keep waits for Into again")
        press(page, "importInto_0")
        verify(find(page, "importKeep").visible)

        verify(page.removePlugin(page.pluginList[0].sha256))
        compare(page.importInto, "")
        verify(!find(page, "importKeep").visible)
        verify(page.pendingImport !== null, "the file still waits")
    }

    // Mine marks an imported plugin as the user's: its variants can then
    // be changed here, its removal asks, and a newer export of the same
    // plugin updates it in place, saying from which version to which
    function test_mineMarksAPluginAsTheUsersAndItsNewerExportUpdatesIt() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "trip.json", "Trip extras"))
        verify(page.importFile(Harness.writeScratchFile("two.json", signalFileText("Two", "D101"))))
        verify(page.keepPending(page.pluginList[0].sha256, "two"))
        const first = Harness.writeScratchFile("trip-1.havne-plugin.json", "")
        page.saving = page.pluginSave(page.pluginList[0].sha256)
        compare(page.saving.file, "trip-extras.havne-plugin.json", "the name the dialog starts from")
        verify(page.saveTo(first))
        compare(find(page, "signalNote").text, "Trip extras exported to the file you chose")
        verify(page.importFile(Harness.writeScratchFile("three.json", signalFileText("Three", "D102"))))
        verify(page.keepPending(page.pluginList[0].sha256, "three"))
        const second = Harness.writeScratchFile("trip-2.havne-plugin.json", "")
        page.saving = page.pluginSave(page.pluginList[0].sha256)
        verify(page.saveTo(second))

        // The user's own has no copy anywhere else: its removal asks, in
        // its row, in the actions' place
        page.openRow = "plugin.trip-extras.base"
        const asking = rowOf(page, "plugin.trip-extras.base")
        find(asking, "rowRemove").clicked()
        verify(find(asking, "signalsConfirm").visible)
        verify(!find(asking, "rowActions").visible, "the question in the actions' place")
        compare(find(asking, "signalsConfirmText").text,
                "Remove Trip extras? It is yours, and no other copy exists")
        verify(!find(page, "signalImport").enabled, "nothing else past the question")
        verify(!find(asking, "rowMore").enabled, "the asking row stays open")
        find(asking, "signalsConfirmCancel").clicked()
        compare(page.pluginList.length, 1, "Cancel keeps it")
        verify(find(asking, "rowActions").visible, "the actions back")
        find(asking, "rowRemove").clicked()
        find(asking, "signalsConfirmRemove").clicked()
        compare(page.pluginList.length, 0)
        compare(page.picks, [])

        // As it comes to another device: someone else's until Mine
        verify(page.importFile(first))
        compare(find(page, "signalNote").text, "Trip extras 1.1.0 added: pick a variant below")
        page.openRow = "plugin.trip-extras.base"
        let row = rowOf(page, "plugin.trip-extras.base")
        compare(find(row, "rowReads").text, "1 signal")
        verify(find(row, "rowMine").visible)
        verify(find(row, "rowCopy").visible)
        verify(!find(row, "rowReplace").visible, "someone else's variants aren't changed here")
        verify(find(row, "rowMineLine").visible)
        compare(find(row, "rowMineLine").text,
                "Mine marks it as yours, so its variants can be changed here; a newer export "
                + "of the same plugin then updates it in place.")
        find(row, "rowCopy").clicked()
        verify(find(row, "rowCopyPanel").visible)
        find(row, "rowMine").clicked()
        compare(find(page, "signalNote").text, "Trip extras is yours now")
        row = rowOf(page, "plugin.trip-extras.base")
        compare(find(row, "rowReads").text, "1 signal · yours")
        verify(find(row, "rowReplace").visible)
        verify(!find(row, "rowMine").visible)
        verify(!find(row, "rowMineLine").visible)
        verify(!find(row, "rowCopy").visible)
        verify(!find(row, "rowCopyPanel").visible, "Copy's choices go with Copy")
        verify(!page.copyShown)
        pressRow(page, "plugin.trip-extras.two")

        verify(page.importFile(second))
        compare(find(page, "signalNote").text, "Trip extras updated from 1.1.0 to 1.2.0")
        compare(page.variantRows.map(r => r.variant), ["base", "two", "three"])
        verify(page.variantRows.every(r => r.mine), "still the user's")
        compare(page.picks, ["plugin.trip-extras.two"], "the pick stays")
        compare(Harness.storedPlugins().length, 1)
    }

    // More opens a row's actions and leaves the row as wide as it was, so
    // a title beside it keeps its lines, on a phone too
    function test_anOpenRowIsAsWideAsAClosedOne_data() {
        return [{ tag: "desktop", width: 1000, compact: false }, { tag: "phone", width: 384, compact: true }]
    }
    function test_anOpenRowIsAsWideAsAClosedOne(data) {
        Theme.compact = data.compact
        const page = createPage(data.compact)
        page.width = data.width
        press(page, "vehicleNew")
        verify(importExample(page))
        page.open("signals")
        const profile = "plugin.example-sim.my2019"
        const pick = find(rowOf(page, profile), "rowPick")
        waitForPolish(find(page, "settingsSignalsPageColumn"))
        const closed = { width: pick.width, height: pick.height }
        verify(closed.width > 100, "laid out")
        find(rowOf(page, profile), "rowMore").clicked()
        compare(page.openRow, profile)
        waitForPolish(find(page, "settingsSignalsPageColumn"))
        const open = find(rowOf(page, profile), "rowPick")
        compare(open.width, closed.width)
        compare(open.height, closed.height, "the title wraps no further")
        verify(find(rowOf(page, profile), "rowMore").checked)
    }

    // Copy to my plugin puts a variant of someone else's plugin into a
    // plugin of the user's own, new or one of theirs; Save as a signal
    // file writes a variant as the store has it, session or not
    function test_aVariantIsCopiedIntoAPluginOfTheUsersOwnAndSavedAsASignalFile() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importExample(page))
        const theirs = page.pluginList[0].sha256
        page.openRow = "plugin.example-sim.my2019"
        let row = rowOf(page, "plugin.example-sim.my2019")
        verify(!find(row, "rowCopyPanel").visible)
        find(row, "rowCopy").clicked()
        verify(find(row, "rowCopyPanel").visible)
        verify(findChild(row, "copyInto_0") === null, "no plugin of the user's own yet")
        find(row, "copyAsNew").clicked()
        compare(find(page, "signalProblem").text, "")
        compare(page.ownPlugins.length, 1)
        const copy = page.ownPlugins[0]
        compare(find(page, "signalNote").text,
                "Example: the simulated car: my2019 copied into " + copy.name + " " + copy.version
                + " (The copy won't follow Example: the simulated car's updates to my2019)")
        verify(!page.copyShown, "the choice closes")
        compare(page.pluginList.length, 2, "the original stays")
        compare(page.picks, [], "a copy is no pick")
        verify(rowData(page, copy.profiles[0]).mine)

        page.openRow = "plugin.example-sim.base"
        row = rowOf(page, "plugin.example-sim.base")
        find(row, "rowCopy").clicked()
        const intoOwn = find(row, "copyInto_0")
        compare(intoOwn.text, "Into " + copy.name)
        intoOwn.clicked()
        compare(find(page, "signalProblem").text, "")
        compare(page.ownPlugins.length, 1)
        compare(page.ownPlugins[0].variants.length, 2, "a variant of its own beside the first")

        const what = page.variantSave(rowData(page, "plugin.example-sim.my2019"))
        compare(what.file, "example-sim-my2019.json", "the variant, where the plugin has several")
        const url = Harness.writeScratchFile("saved.json", "x".repeat(2000))
        page.saving = what
        verify(page.saveTo(url))
        compare(page.saving, null)
        compare(Harness.readScratchFile(url), Bridge.pluginVariantText(theirs, "my2019").text)
        compare(find(page, "signalNote").text,
                "Example: the simulated car: my2019 saved to the file you chose")
        verify(page.importFile(url), "it reads back as a signal file")
        compare(page.pendingImport.kind, "signals")
        press(page, "importDiscard")
        compare(page.pendingImport, null)
        verify(!find(page, "importPanel").visible)

        verify(importFile(page, "extras.json", "Extras"))
        compare(page.variantSave(rowData(page, "plugin.extras.base")).file, "extras.json")
        page.saving = { kind: "variant", sha256: "a".repeat(64), variant: "base", name: "Gone",
                        file: "gone.json" }
        verify(!page.saveTo(url))
        compare(find(page, "signalProblem").text, "The store has no such plugin")
        compare(find(page, "signalNote").text, "")

        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        page.open("signals")
        page.openRow = "plugin.example-sim.base"
        row = rowOf(page, "plugin.example-sim.base")
        verify(find(row, "rowSave").enabled, "Save reads the copy alone")
        verify(find(row, "rowExport").enabled)
        verify(!find(row, "rowCopy").enabled)
        verify(!find(row, "rowRemove").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
    }

    // Export plugin... writes the store's copy, on every plugin; one with
    // a share-alike variant says what that asks before the dialog. The
    // file imports again as the same plugin
    function test_exportSaysWhatAShareAlikeVariantAsksAndWritesTheStoresCopy() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, "2021"))
        press(page, "obdbKeep")
        const profile = "plugin.daily-driver-obdb.my2021"
        const sha = rowData(page, profile).sha256
        page.openRow = profile
        const row = rowOf(page, profile)
        verify(!find(row, "rowExportNote").visible)
        verify(page.exportWaits(sha), "the note comes before the dialog")
        verify(find(row, "rowExportNote").visible)
        compare(find(row, "exportNoteText").text,
                "Variant 'my2021' holds data under CC-BY-SA-4.0, a share-alike license: whoever "
                + "passes the plugin on keeps that license and the variant's source with it")
        find(row, "exportCancel").clicked()
        verify(!find(row, "rowExportNote").visible)

        const url = Harness.writeScratchFile("out.havne-plugin.json", "")
        page.saving = page.pluginSave(sha)
        compare(page.saving.file, "daily-driver-obdb.havne-plugin.json")
        verify(page.saveTo(url))
        compare(find(page, "signalNote").text, "Daily driver OBDb exported to the file you chose")
        verify(page.importFile(url))
        compare(find(page, "signalNote").text, "Daily driver OBDb 1.0.0 is here already")
        compare(page.pluginList.length, 1)

        // Someone else's plugin under no such license goes straight to
        // the dialog, and its export is the package byte for byte
        const packed = Harness.writeExamplePlugin("example-sim.havne-plugin.json")
        verify(page.importFile(packed))
        const example = page.pluginList.find(p => p.id === "example-sim").sha256
        verify(!page.exportWaits(example))
        compare(page.exportNote, "")
        const copy = Harness.writeScratchFile("example-out.havne-plugin.json", "")
        page.saving = page.pluginSave(example)
        verify(page.saveTo(copy))
        compare(Harness.readScratchFile(copy), Harness.readScratchFile(packed))
        verify(page.exportWaits("0123"), "a plugin the store lacks opens no dialog")
        compare(find(page, "signalProblem").text, "The store has no such plugin")
    }

    // The profile row with one and two picks that read all of obd2: on an
    // adapter, where Default is obd2, the first such pick takes Default's
    // place and says "obd2 + <name>"; a pick made while the field reads
    // Default is read at once, one made under a chosen profile leaves the
    // field alone; on mock:// Default isn't obd2, so nothing stands in
    function test_theFirstPickThatReadsObd2StandsInForDefaultOnAnAdapter() {
        const page = createPage()
        press(page, "vehicleNew")
        const field = find(page, "uriField")
        field.text = "elm327://COM9"
        compare(page.profileChoice, "default")
        verify(importFile(page, "plus.json", "Plus", "D100", true))
        compare(find(page, "signalNote").text,
                "Plus 1.0.0 added: picked for Vehicle 1, now reading obd2 + Plus")
        compare(field.text, "elm327://COM9?profile=plugin.plus.base")
        compare(page.rowReads(rowData(page, "plugin.plus.base")), "obd2 + 1 signal · yours")
        const plus = find(page, "profile_plugin.plus.base")
        compare(plus.text, "obd2 + Plus")
        verify(plus.checked)
        compare(page.profiles.filter(p => p.name === "plugin.plus.base").length, 1,
                "one button for the pick: the stand-in")
        tryVerify(() => findChild(page, "profile_default") === null, 1000,
                  "the pick reads all of obd2: Default goes")
        compare(find(page, "profileHint").text,
                "obd2's signals and those of Plus 1.0.0, picked for this vehicle")

        // Typed back to Default, both show, so the row is the URI's
        field.text = "elm327://COM9"
        verify(find(page, "profile_default").checked)
        verify(!find(page, "profile_plugin.plus.base").checked)

        // A second such pick is read too, but the first still stands in
        verify(importFile(page, "more.json", "More", "D101", true))
        compare(page.picks, ["plugin.plus.base", "plugin.more.base"])
        compare(field.text, "elm327://COM9?profile=plugin.more.base")
        field.text = "elm327://COM9"
        compare(page.standIn.profile, "plugin.plus.base", "the first pick, not the last")
        compare(page.profiles.map(p => p.name),
                ["default", "plugin.plus.base", "raw", "uds-generic", "plugin.more.base"])
        compare(find(page, "profile_plugin.more.base").text, "obd2 + More")

        // The first unpicked, the next stands in
        pressRow(page, "plugin.plus.base")
        compare(page.standIn.profile, "plugin.more.base")
        pressRow(page, "plugin.more.base")
        compare(page.standIn, null)
        compare(page.profiles.map(p => p.name), ["default", "raw", "uds-generic"])

        // A profile the user chose stays when a pick is made
        press(page, "vehicleNew")
        field.text = "elm327://COM9?profile=raw"
        verify(importFile(page, "third.json", "Third", "D102", true))
        compare(field.text, "elm327://COM9?profile=raw")
        compare(page.profileChoice, "raw")
        compare(find(page, "signalNote").text, "Third 1.0.0 added: picked for Vehicle 2")
        compare(find(page, "profile_plugin.third.base").text, "obd2 + Third")
        tryVerify(() => findChild(page, "profile_default") === null, 1000)

        // On mock://, Default isn't obd2: the pick is a choice of its own
        field.text = "mock://demo"
        compare(page.standIn, null)
        compare(find(page, "profile_plugin.third.base").text, "Third")
        verify(find(page, "profile_default"))
    }

    // A pick no plugin here has (a version that dropped the variant, a
    // plugin removed elsewhere) is listed so it can be seen and undone,
    // and the list's Signals row says so, in the danger color
    function test_aPickTheStoreLacksIsListedAndUnpicked() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "extras.json", "Extras"))
        page.saveVehicles(VehicleBook.setPicks(page.vehicles,
            ["plugin.gone.base", "plugin.extras.base", "plugin.extras.dropped"]))
        compare(page.missingPicks.map(m => m.text),
                ["plugin.gone.base: not in the store", "dropped is no longer in Extras"])
        compare(signalsRow(page).summary, "1 plugin: Extras, 2 picks missing")
        verify(signalsRow(page).warn, "flagged for the list's danger color")
        const first = find(page, "pickMissing_0")
        verify(first.visible)
        compare(find(first, "pickMissingText").text, "plugin.gone.base: not in the store")
        find(first, "pickMissingUnpick").clicked()
        compare(page.picks, ["plugin.extras.base", "plugin.extras.dropped"])
        compare(signalsRow(page).summary, "1 plugin: Extras, 1 pick missing")
        find(find(page, "pickMissing_0"), "pickMissingUnpick").clicked()
        compare(page.picks, ["plugin.extras.base"])
        verify(signalsRow(page).warn !== true)
        tryVerify(() => findChild(page, "pickMissing_0") === null, 1000)

        // A missing first pick stands in for nothing: the next one does
        verify(importFile(page, "plus.json", "Plus", "D101", true))
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["plugin.gone.base", "plugin.plus.base"]))
        find(page, "uriField").text = "elm327://COM9"
        compare(page.standIn.profile, "plugin.plus.base")
    }

    // The signal files the move into the plugin store left behind are
    // listed with why, the picked vehicle's then the book's own; Save
    // writes one out and Remove drops its record, asking first in the
    // file's row where the kept copy goes with it
    function test_theFilesThatDidntMoveAreListedAndRemoved() {
        const a = "a".repeat(64)
        const b = "b".repeat(64)
        const book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "Daily driver"), v => {
            v.unmigrated = [{ sha256: a, name: "Old extras", words: "line 3: no such key 'bitz'" }]
        })
        book.unmigrated = [{ sha256: b, name: "Loose", words: "Its vehicle was not read", kept: false }]
        storedVehicles.setValue("book", VehicleBook.serialize(book))
        storedVehicles.sync()
        const page = createPage()
        page.open("signals")
        verify(find(page, "unmigratedTitle").visible)
        compare(find(page, "unmigratedTitle").text, "Not moved from the earlier version:")
        compare(find(find(page, "unmigrated_0"), "unmigratedText").text,
                "Old extras: line 3: no such key 'bitz'")
        compare(find(find(page, "unmigrated_1"), "unmigratedText").text,
                "Loose: Its vehicle was not read (its copy is still in the earlier version's folder)")
        compare(page.unmigratedSave(page.unmigratedRows[0]).file, "signals.json")

        // Neither copy is anywhere in this test's folders: Save says so
        page.saving = page.unmigratedSave(page.unmigratedRows[0])
        verify(!page.saveTo(Harness.writeScratchFile("left.json", "")))
        compare(find(page, "signalProblem").text, "Its copy is gone from the earlier version's folder")

        const kept = find(page, "unmigrated_0")
        find(kept, "unmigratedRemove").clicked()
        compare(page.unmigratedRows.map(r => r.name), ["Old extras", "Loose"], "asked first")
        const question = find(kept, "unmigratedConfirm")
        verify(question.visible, "in the file's row")
        compare(find(question, "signalsConfirmText").text, "Remove Old extras? Its kept copy goes with it")
        verify(!find(kept, "unmigratedSave").visible, "the question in the buttons' place")
        verify(!find(page, "signalImport").enabled, "nothing else past the question")
        find(question, "signalsConfirmCancel").clicked()
        compare(page.unmigratedRows.length, 2, "Cancel keeps it")
        find(kept, "unmigratedRemove").clicked()
        find(question, "signalsConfirmRemove").clicked()
        compare(page.unmigratedRows.map(r => r.name), ["Loose"])
        verify(page.pickedVehicle.unmigrated === undefined, "the vehicle's record is gone")
        // Its copy is still in the earlier version's folder: nothing to ask
        find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
        compare(page.unmigratedRows, [])
        verify(page.vehicles.unmigrated === undefined)
        tryVerify(() => !find(page, "unmigratedTitle").visible, 1000)

        // Forget all keeps the book's own records: they are no vehicle's
        page.saveVehicles(Object.assign(VehicleBook.copy(page.vehicles),
                                        { unmigrated: [{ sha256: b, name: "Loose", words: "w" }] }))
        page.forgetVehicles(true)
        compare(page.unmigratedRows.map(r => r.name), ["Loose"])
    }

    // The bytes the move kept of a file go only once no vehicle lists the
    // file; until then Save as a signal file writes them out
    function test_aFileThatDidntMoveKeepsItsBytesWhileAVehicleListsIt() {
        const c = "c".repeat(64)
        const text = signalFileText("Kept", "D105")
        verify(Harness.keepUnmigrated(c, text))
        try {
            const listed = v => {
                v.unmigrated = [{ sha256: c, name: "Kept", words: "line 2: no such key" }]
            }
            let book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "Daily driver"), listed)
            book = VehicleBook.change(VehicleBook.add(book, "Weekend car"), listed)
            storedVehicles.setValue("book", VehicleBook.serialize(book))
            storedVehicles.sync()
            const page = createPage()
            page.open("signals")
            compare(page.pickedVehicle.label, "Weekend car")
            const out = Harness.writeScratchFile("kept.json", "")
            page.saving = page.unmigratedSave(page.unmigratedRows[0])
            verify(page.saveTo(out))
            compare(Harness.readScratchFile(out), text, "the bytes as the move kept them")
            compare(find(page, "signalNote").text, "Kept saved to the file you chose")

            find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
            compare(page.unmigratedRows, [], "another vehicle lists it: no question")
            compare(Harness.keptUnmigrated(), [c], "the other vehicle lists it still")
            press(page, "vehicle_" + page.vehicles.list[0].id)
            compare(page.unmigratedRows.map(r => r.name), ["Kept"])
            find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
            compare(Harness.keptUnmigrated(), [c], "the last listing asks first")
            find(find(page, "unmigrated_0"), "signalsConfirmRemove").clicked()
            compare(page.unmigratedRows, [])
            compare(Harness.keptUnmigrated(), [], "no vehicle lists it: the bytes go")
        } finally {
            Bridge.removeUnmigratedFile(c)
        }
    }

    // A forgotten vehicle's file that didn't move is the book's own from
    // then on: listed, saved and removed as before, its bytes kept until
    // it is removed
    function test_aForgottenVehiclesFileThatDidntMoveStaysListed() {
        const d = "d".repeat(64)
        const text = signalFileText("Kept", "D106")
        verify(Harness.keepUnmigrated(d, text))
        try {
            const book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "Daily driver"), v => {
                v.unmigrated = [{ sha256: d, name: "Kept", words: "line 2: no such key" }]
            })
            storedVehicles.setValue("book", VehicleBook.serialize(book))
            storedVehicles.sync()
            const page = createPage()
            page.open("signals")
            compare(page.unmigratedRows.map(r => r.loose), [false], "the vehicle's")
            press(page, "vehicleForget")
            press(page, "vehicleConfirm")
            compare(page.pickedVehicle, null)
            compare(page.unmigratedRows.map(r => r.name), ["Kept"], "listed still")
            compare(page.unmigratedRows.map(r => r.loose), [true], "the book's own now")
            compare(Harness.keptUnmigrated(), [d])
            const out = Harness.writeScratchFile("kept.json", "")
            page.saving = page.unmigratedSave(page.unmigratedRows[0])
            verify(page.saveTo(out))
            compare(Harness.readScratchFile(out), text, "and saved as before")
            find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
            find(find(page, "unmigrated_0"), "signalsConfirmRemove").clicked()
            compare(page.unmigratedRows, [])
            compare(Harness.keptUnmigrated(), [], "removed, its bytes go")
        } finally {
            Bridge.removeUnmigratedFile(d)
        }
    }

    // A book a later Havne saved is left as it is: no vehicle is shown,
    // the page says why, and nothing is written over it
    function test_aBookFromALaterHavneIsLeftAsItIs() {
        const later = JSON.stringify({ format: 3, salt: "s", picked: "v1",
                                       list: [{ id: "v1", label: "Daily driver", shape: "new" }] })
        storedVehicles.setValue("book", later)
        storedVehicles.sync()
        const page = createPage()
        verify(page.bookLater)
        compare(page.pickedVehicle, null)
        page.open("vehicle")
        verify(find(page, "vehicleBookLater").visible)
        compare(find(page, "vehicleBookLater").text,
                "This vehicle list was saved by a later Havne and is left as it is.")
        verify(!find(page, "vehicleNew").enabled)
        page.saveVehicles(VehicleBook.add(VehicleBook.empty(), "Mine"))
        compare(page.pickedVehicle, null, "no vehicle is made over it")
        verify(importFile(page, "extras.json", "Extras"), "the store works without the book")
        page.destroy()
        wait(10)
        storedVehicles.sync()
        compare(storedVehicles.value("book"), later, "the stored book is as it was")
    }

    // ---- A connect from a list reads the vehicle's plugin (ADR-0064 Q5) ----

    // The rule: an address that names no profile takes the picked
    // vehicle's first pick that loads and reads all of obd2, where obd2
    // is asked: a J2534 or ELM327 adapter, or a CAN interface that sends.
    // Any other address passes as it is, and each row has its word
    function test_aListsAddressTakesTheStandInOnlyWhereObd2IsAsked() {
        const page = createPage()
        page.j2534Devices = []
        page.serialPorts = [{ name: "COM5", device: "" }]
        page.bluetoothAdapters = []
        page.canInterfaces = [{ name: "vcan0", kind: "vcan", up: true },
                              { name: "can0", kind: "can", up: true, modes: ["one-shot"] },
                              { name: "can1", kind: "can", up: true, modes: ["listen-only"] },
                              { name: "can2", kind: "can", up: false, modes: ["one-shot"] }]
        press(page, "vehicleNew")
        compare(page.listConnectUri("elm327://COM5"), "elm327://COM5", "no pick, nothing composed")
        verify(importFile(page, "plain.json", "Plain"))
        compare(page.listConnectUri("elm327://COM5"), "elm327://COM5", "a pick that doesn't read obd2")
        compare(page.connectProfileWords["elm327://COM5"], undefined)
        verify(importFile(page, "plus.json", "Plus", "D101", true))
        verify(importFile(page, "more.json", "More", "D102", true))
        const plus = "plugin.plus.base"
        compare(page.picks, ["plugin.plain.base", plus, "plugin.more.base"])

        compare(page.listConnectUri("elm327://COM5"), "elm327://COM5?profile=" + plus,
                "the first pick that reads obd2, not the last")
        compare(page.listConnectUri("j2534://Vendor X"), "j2534://Vendor X?profile=" + plus)
        compare(page.listConnectUri("elm327://192.168.0.10:35000?poll=0C"),
                "elm327://192.168.0.10:35000?poll=0C&profile=" + plus,
                "appended last, the other keys where they were")
        compare(page.listConnectUri("elm327://COM5?profile=obd2"), "elm327://COM5?profile=obd2",
                "an address that names a profile wins")
        compare(page.listConnectUri("elm327://COM5?profile=raw&poll=0C"),
                "elm327://COM5?profile=raw&poll=0C")
        compare(page.listConnectUri("mock://demo"), "mock://demo", "the demo's Default isn't obd2")
        compare(page.listConnectUri("replay://C:/recordings/one"), "replay://C:/recordings/one")

        compare(page.listConnectUri("socketcan://vcan0"), "socketcan://vcan0?profile=" + plus,
                "a virtual bus sends")
        compare(page.listConnectUri("socketcan://can0"), "socketcan://can0?profile=" + plus,
                "a controller up in one-shot mode sends")
        compare(page.listConnectUri("socketcan://can1"), "socketcan://can1", "a listener keeps listening")
        compare(page.listConnectUri("socketcan://can2"), "socketcan://can2", "down")
        compare(page.listConnectUri("socketcan://can9"), "socketcan://can9", "not listed")

        compare(page.connectProfileWords["elm327://COM5"], "obd2 + Plus")
        compare(page.connectProfileWords["socketcan://vcan0"], "obd2 + Plus")
        compare(page.connectProfileWords["socketcan://can1"], undefined,
                "no word on a row that isn't composed")
        compare(page.listProfileWord("elm327://COM5?profile=plugin.more.base"), "More",
                "a plugin's profile by its name")
        compare(page.listConnectUri("elm327://COM5?profile=plugin.more.base"),
                "elm327://COM5?profile=plugin.more.base", "a pick's profile, named, is read")
        compare(page.listProfileWord("elm327://COM5?profile=raw"), "")
        compare(page.listProfileWord("mock://demo"), "")

        // A pick the store lacks stands in for nothing: the next does
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["plugin.gone.base", "plugin.more.base"]))
        compare(page.listConnectUri("elm327://COM5"), "elm327://COM5?profile=plugin.more.base")
        compare(page.listProfileWord("elm327://COM5?profile=plugin.gone.base"), "a plugin not in the store")
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["plugin.gone.base"]))
        compare(page.listConnectUri("elm327://COM5"), "elm327://COM5")

        // A plugin's or a signal file's profile the vehicle doesn't pick is
        // dropped, as the field drops it; a built-in profile stays
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, [plus]))
        compare(page.listConnectUri("elm327://COM5?profile=plugin.more.base&poll=0C"),
                "elm327://COM5?poll=0C&profile=" + plus, "the vehicle's own pick is read in its place")
        compare(page.listProfileWord("elm327://COM5?profile=plugin.more.base"), "obd2 + Plus")
        compare(page.listConnectUri("mock://demo?profile=plugin.more.base"), "mock://demo")
        compare(page.listConnectUri("elm327://COM5?profile=user.old"), "elm327://COM5?profile=" + plus)
        press(page, "vehicle_none")
        compare(page.listConnectUri("elm327://COM5"), "elm327://COM5", "no vehicle, no pick")
        compare(page.listConnectUri("elm327://COM5?profile=" + plus), "elm327://COM5",
                "nor a plugin's profile")
        compare(page.listConnectUri("elm327://COM5?profile=raw"), "elm327://COM5?profile=raw")
        compare(page.connectProfileWords["elm327://COM5"], undefined)
    }

    // A list's connect puts the composed address in the field, as typed
    // text, before the connect; one that fails is kept nowhere. The
    // address here is a closed TCP port, an adapter's on every platform
    function test_aListConnectFillsTheFieldWithTheProfileItReads() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100", true))
        const field = find(page, "uriField")
        field.text = "mock://other"
        const closed = "elm327://127.0.0.1:1"
        page.connectFromList(closed)
        compare(field.text, closed + "?profile=plugin.plus.base", "before the connect answers")
        compare(page.profileChoice, "plugin.plus.base", "the row follows the field")
        verify(find(page, "profile_plugin.plus.base").checked)
        tryVerify(() => !Bridge.connecting, 10000, "the connect ends")
        verify(!Bridge.connected, "nothing listens there")
        verify(find(page, "errorLabel").text !== "")
        compare(page.recentUris, [], "a failed connect is no recent address")
        compare(page.pickedVehicle.uri, "")

        // The demo is left as it is
        page.connectFromList("mock://demo")
        compare(field.text, "mock://demo")
        tryVerify(() => Bridge.connected, 5000, "connects")
        compare(Bridge.currentUri, "mock://demo")
    }

    // A session on the emulator, ended. It serves one host at a time,
    // so this waits until it has seen the host go
    function endSession() {
        const closed = Harness.emulatorClients("closed")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        tryVerify(() => Harness.emulatorClients("closed") > closed, 5000,
                  "the emulator sees the host go")
    }

    // On an adapter (havne-cli's ELM327 emulator, on every platform) the
    // connect runs under the pick's profile, and what is kept is the
    // row's address without it: the vehicle's address and
    // the recent list name no profile, so each connect adds the pick of
    // the vehicle picked then, and another vehicle reads its own
    function test_aListConnectOnAnAdapterReadsThePluginAndKeepsTheRowsAddress() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        const uri = emulatorUri()
        const composed = uri + "?profile=plugin.plus.base"
        const field = find(page, "uriField")
        field.text = ""
        page.connectFromList(uri)
        compare(field.text, composed)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, composed)
        compare(find(page, "errorLabel").text, "")
        tryVerify(() => Bridge.signalModel.rowCount() > 0, 5000)
        verify(Bridge.profileSignals(Bridge.currentUri).some(r => r.ref === "key:a"),
               "the session reads the plugin's signal with obd2's")
        tryVerify(() => page.pickedVehicle.uri === uri, 10000, "the vehicle keeps the row's address")
        endSession()
        sameList(page.connectUris, [uri])
        compare(page.connectProfileWords[uri], "obd2 + Plus", "the row says what it reads")

        // Another vehicle, which picks nothing, reads the row as it is
        press(page, "vehicleNew")
        compare(page.picks, [])
        compare(page.connectProfileWords[uri], undefined)
        page.connectFromList(uri)
        compare(field.text, uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, uri, "not the first vehicle's plugin")
        tryVerify(() => page.pickedVehicle.uri === uri, 10000)
        endSession()

        // The first again: the field reads its address as its row connects
        press(page, "vehicle_" + page.vehicles.list[0].id)
        compare(field.text, composed)
    }

    // The recent list keeps the row's address too; an address typed
    // after it is kept as typed
    function test_aListConnectKeepsTheRowsAddressInTheRecentList() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        const uri = emulatorUri()
        find(page, "uriField").text = ""
        page.connectFromList(uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, uri + "?profile=plugin.plus.base")
        sameList(page.recentUris, [uri])
        endSession()
        const raw = uri + "?profile=raw"
        find(page, "uriField").text = raw
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects")
        sameList(page.recentUris, [raw, uri])
        endSession()
    }

    // An address typed with a plugin's profile is kept as typed. A list
    // reads it for a vehicle that picks the plugin; for another it drops
    // the profile, so the plugin is never remembered as that vehicle's
    function test_aRowNamingAPluginReadsItOnlyForAVehicleThatPicksIt() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        const uri = emulatorUri()
        const composed = uri + "?profile=plugin.plus.base"
        const field = find(page, "uriField")
        field.text = composed
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects")
        tryVerify(() => page.pickedVehicle.uri === composed, 10000, "kept as typed")
        sameList(page.recentUris, [composed])
        endSession()
        compare(page.listConnectUri(composed), composed, "its vehicle picks the plugin")
        compare(page.connectProfileWords[composed], "Plus")

        press(page, "vehicleNew")
        compare(page.listConnectUri(composed), uri)
        compare(page.connectProfileWords[composed], undefined, "no word: it reads Default")
        page.connectFromList(composed)
        compare(field.text, uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, uri, "not the first vehicle's plugin")
        tryVerify(() => page.pickedVehicle.uri === uri, 10000, "nor is it remembered as this vehicle's")
        sameList(page.recentUris, [uri, composed])
        compare(page.vehicles.list[0].uri, composed, "the first vehicle's stays")
        endSession()
    }

    // A kept row follows the vehicle's picks: with the first undone the
    // next that reads obd2 is read, at a start as at a connect, and with
    // the plugin removed the row connects as it is
    function test_aKeptRowFollowsTheVehiclesPicks() {
        let page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        verify(importFile(page, "more.json", "More", "D102", true))
        const uri = emulatorUri()
        const plus = uri + "?profile=plugin.plus.base"
        const more = uri + "?profile=plugin.more.base"
        find(page, "uriField").text = ""
        page.connectFromList(uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, plus)
        tryVerify(() => page.pickedVehicle.uri === uri, 10000)
        endSession()

        page.open("signals")
        pressRow(page, "plugin.plus.base")
        compare(page.picks, ["plugin.more.base"])
        sameList(page.connectUris, [uri])
        compare(page.connectProfileWords[uri], "obd2 + More")
        compare(page.listConnectUri(plus), more, "a row that names the pick undone reads the pick of now")
        page.connectFromList(uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, more)
        endSession()

        // As at a quit and a start
        page.destroy()
        wait(10)
        stored.sync()
        compare(stored.value("uri"), uri, "the last address is the row's")
        page = createPage()
        compare(find(page, "uriField").text, more, "and the field reads the pick of now")

        verify(page.removePlugin(page.pluginList.find(p => p.id === "more").sha256))
        compare(page.picks, [])
        compare(page.listConnectUri(uri), uri)
        compare(page.connectProfileWords[uri], undefined)
    }

    // ---- An OBDb set imported (ADR-0051's appendix of 2026-11-06, ADR-0064 Q3) ----

    // The OBDb fixture read through Import under the year typed
    function obdbRead(page, year) {
        if (!page.importFile(Harness.obdbFixture))
            return false
        if (year !== "") {
            find(page, "obdbYearField").text = ""   // a year typed, even the one a VIN gave
            find(page, "obdbYearField").text = year
        }
        return page.obdbReport !== null
    }

    // One Import, an OBDb set: the report shows what converted, what
    // differs and what's left out, converted again when the year is
    // changed, and nothing is kept until Keep; kept, it is a plugin of the
    // user's own, picked for the vehicle
    function test_anObdbSetShowsItsReportBeforeItIsKept() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        page.open("signals")
        verify(!find(page, "obdbReport").visible)
        verify(obdbRead(page, ""))
        verify(find(page, "obdbReport").visible)
        compare(find(page, "obdbSummary").text,
                "Daily driver OBDb from OBDb: 6 signals converted, 5 fields differ, 6 left out, "
                + "no model year, so the commands OBDb gives for some years only are left out")
        find(page, "obdbYearField").text = "2021"
        compare(find(page, "obdbSummary").text,
                "Daily driver OBDb from OBDb: 7 signals converted, 6 fields differ, "
                + "5 left out, model year 2021")
        compare(find(page, "obdbChanged").count, 6)
        compare(find(page, "obdbSkipped").count, 5)
        compare(find(page, "obdbFileNotes").count, 2)
        compare(find(page, "obdbKeep").text, "Keep")
        compare(page.pluginList.length, 0, "nothing kept yet")
        compare(Harness.storedPlugins(), [])

        press(page, "obdbKeep")
        verify(!find(page, "importPanel").visible)
        compare(page.picks, ["plugin.daily-driver-obdb.my2021"], "a typed year names the variant")
        compare(find(page, "signalNote").text, "Daily driver OBDb 1.0.0 added: picked for Daily driver")
        const row = rowOf(page, "plugin.daily-driver-obdb.my2021")
        compare(find(row, "rowCaption").text, "For this car:")
        compare(find(row, "rowTitle").text, "Daily driver OBDb 1.0.0")
        compare(find(row, "rowReads").text, "obd2 + 7 signals · yours")
        compare(Harness.storedPlugins().length, 1)
        verify(page.pluginList[0].obdb)
    }

    function test_anObdbSetDiscardedIsNotKept() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, ""))
        verify(find(page, "obdbReport").visible)
        press(page, "importDiscard")
        verify(!find(page, "importPanel").visible)
        compare(page.pluginList.length, 0)
        verify(!page.keepObdb(), "nothing to keep")
        compare(page.pluginList.length, 0)

        // A report is the vehicle's it was read for
        verify(obdbRead(page, "2021"))
        press(page, "vehicleNew")
        compare(page.pendingImport, null, "another vehicle picked: the file no longer waits")
        verify(!page.keepObdb())
        compare(page.pluginList.length, 0)
    }

    // The set read again takes the place of the vehicle's earlier import,
    // and says so before Keep; the pick follows it. A plugin the user
    // named so is never replaced
    function test_anObdbSetReadAgainReplacesTheEarlierImport() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, "2019"))
        press(page, "obdbKeep")
        const earlier = page.pluginList[0].sha256
        compare(page.picks, ["plugin.daily-driver-obdb.my2019"])

        verify(obdbRead(page, "2021"))
        verify(find(page, "obdbSummary").text.endsWith(
                   "; replaces the vehicle's earlier OBDb import"),
               find(page, "obdbSummary").text)
        compare(find(page, "obdbKeep").text, "Keep, replacing the earlier")
        // The report at a phone's width, as test_everyItemFitsAPhonesWidth
        const width = page.width
        page.width = 320
        const column = find(page, "settingsSignalsPageColumn")
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        waitForPolish(column)
        compare(pastTheEdge(column, column, []).join("; "), "",
                "nothing past the column's right edge, " + column.width)
        page.width = width
        press(page, "obdbKeep")
        compare(page.pluginList.length, 1)
        verify(page.pluginList[0].sha256 !== earlier)
        compare(page.picks, ["plugin.daily-driver-obdb.my2021"], "the pick follows the import")
        compare(find(page, "signalNote").text,
                "Daily driver OBDb replaced the vehicle's earlier OBDb import")
        compare(Harness.storedPlugins().length, 1, "the earlier copy went")

        // The same year again is the same plugin
        verify(obdbRead(page, "2021"))
        press(page, "obdbKeep")
        compare(page.pluginList.length, 1)
        compare(find(page, "signalNote").text, "Daily driver OBDb is here already")

        // A plugin the user named so is the user's: the import takes an
        // id of its own beside it
        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        verify(importFile(page, "mine.json", "Weekend car OBDb"))
        verify(obdbRead(page, "2021"))
        verify(!find(page, "obdbSummary").text.includes("replaces"))
        press(page, "obdbKeep")
        compare(page.pluginList.filter(p => p.name === "Weekend car OBDb").map(p => p.id),
                ["weekend-car-obdb", "weekend-car-obdb-2"])
    }

    // The year: typed, else a typed VIN's, else the connected car's once
    // its VIN names the picked vehicle, kept once the session ends. A
    // VIN's year fills the field and names no variant; one that isn't a
    // year is refused, and the set waits for another
    function test_theObdbYearComesFromAVin() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        compare(page.obdbVinYear(), 0)
        find(page, "vinLookupField").text = "1FAFAKE10M0000001"
        compare(page.obdbVinYear(), 2021)
        find(page, "vinLookupField").text = "not one"
        compare(page.obdbVinYear(), 0)
        find(page, "vinLookupField").text = ""

        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        verify(!find(page, "signalImport").enabled, "nothing changes the store while it runs")
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
        verify(find(page, "signalImport").enabled)

        verify(obdbRead(page, "19x") === false)
        compare(find(page, "signalProblem").text, "A model year is 1980 to 2039, or empty for none")
        verify(!find(page, "obdbReport").visible)
        verify(find(page, "importPanel").visible, "the set waits for a year")
        find(page, "obdbYearField").text = "1979"
        compare(find(page, "signalProblem").text, "A model year is 1980 to 2039, or empty for none")
        find(page, "obdbYearField").text = "2021"
        compare(find(page, "signalProblem").text, "")
        verify(find(page, "obdbReport").visible)
        compare(page.obdbReport.year, 2021)

        verify(obdbRead(page, ""))
        compare(find(page, "obdbYearField").text, "2000", "the field takes the car's year")
        verify(page.obdbYearFromVin)
        compare(page.obdbReport.year, 2000)
        press(page, "obdbKeep")
        compare(page.picks, ["plugin.vehicle-1-obdb.base"], "a VIN's year names no variant")
    }

    // A model year typed digit by digit refuses nothing until its fourth
    // digit or the field's end, and meanwhile no report waits to be kept
    function test_aModelYearTypedIsJudgedWhole() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, "2019"))
        const field = find(page, "obdbYearField")
        field.text = ""
        for (const t of ["2", "20", "202"]) {
            field.text = t
            compare(find(page, "signalProblem").text, "", t + " is a year on its way")
            verify(!find(page, "obdbReport").visible, "no report waits beside " + t)
            verify(!find(page, "obdbKeep").visible, "nor Keep")
        }
        field.editingFinished()
        compare(find(page, "signalProblem").text, "A model year is 1980 to 2039, or empty for none")
        field.text = "2021"
        compare(find(page, "signalProblem").text, "")
        compare(page.obdbReport.year, 2021)
    }

    // The year in the field is the waiting set's: it goes with a discard
    // and with the vehicle, so one vehicle's year never converts another's
    function test_theObdbYearGoesWithItsSet() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        find(page, "vinLookupField").text = "1FAFAKE10M0000001"
        verify(obdbRead(page, ""))
        compare(find(page, "obdbYearField").text, "2021")
        verify(page.obdbYearFromVin)
        page.discardImport()
        compare(find(page, "obdbYearField").text, "")
        verify(!page.obdbYearFromVin)
        find(page, "vinLookupField").text = ""

        verify(obdbRead(page, "2019"))
        compare(page.obdbReport.year, 2019)
        press(page, "vehicleNew")
        compare(find(page, "obdbYearField").text, "", "the last vehicle's year isn't this one's")
        verify(obdbRead(page, ""))
        compare(page.obdbReport.year, 0)
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
        verify(page.importFile(url))
        compare(page.obdbReport.skipped.length, 120)
        compare(find(page, "obdbSkipped").count, page.obdbRowsShown)
        compare(find(page, "obdbSkippedMore").text, "and " + (120 - page.obdbRowsShown) + " more")
        page.open("signals")
        verify(find(page, "obdbSkippedMore").visible)
        verify(!find(page, "obdbChangedMore").visible)
    }

    // With no vehicle picked an OBDb set is imported all the same: named
    // "OBDb set", imported for none, and not in use until a vehicle picks it
    function test_anObdbSetWithNoVehicleIsImportedForNone() {
        const page = createPage()
        page.open("signals")
        verify(obdbRead(page, ""))
        verify(find(page, "obdbSummary").text.startsWith("OBDb set from OBDb: "),
               find(page, "obdbSummary").text)
        press(page, "obdbKeep")
        compare(find(page, "signalNote").text, "OBDb set 1.0.0 added")
        compare(page.pluginList[0].importedFor, "")
        verify(find(rowOf(page, "plugin.obdb-set.base"), "rowUnused").visible)
    }

    // The Signals page's states on a phone (qml.md's density): at 384 x
    // 832 every button is a touch's size, and at 320 px, the narrowest
    // phone, nothing lies past the column
    function test_theSignalsPagesStatesFitAPhone_data() {
        return [{ tag: "list" }, { tag: "open row" }, { tag: "copy" }, { tag: "import panel" },
                { tag: "obdb report" }, { tag: "export note" }, { tag: "confirm" },
                { tag: "missing pick" }, { tag: "not moved" }]
    }
    function test_theSignalsPagesStatesFitAPhone(data) {
        Theme.compact = true
        Theme.dense = true
        if (data.tag === "not moved") {
            const book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "W".repeat(40)), v => {
                v.unmigrated = [{ sha256: "a".repeat(64), name: "A file's name as long as a name may be",
                                  words: "line 12, column 40: the words a loader gives, at their length" }]
            })
            book.unmigrated = [{ sha256: "b".repeat(64), name: "Loose", words: "w", kept: false }]
            storedVehicles.setValue("book", VehicleBook.serialize(book))
            storedVehicles.sync()
        }
        const page = createPage(true)
        page.width = 384
        page.height = 832
        if (page.pickedVehicle === null) {
            find(page, "vehicleLabelField").text = "W".repeat(40)
            press(page, "vehicleNew")
        }
        verify(importExample(page))
        verify(importFile(page, "long.json", "A plugin of the user's own with a name that wraps"))
        const own = page.ownPlugins[0]
        const theirs = "plugin.example-sim.my2019"
        if (data.tag === "open row") {
            page.openRow = theirs
        } else if (data.tag === "copy") {
            page.openRow = theirs
            page.copyShown = true
        } else if (data.tag === "import panel") {
            verify(page.importFile(Harness.writeScratchFile("track.json", signalFileText("Track day", "D101"))))
            page.importInto = own.sha256
        } else if (data.tag === "obdb report") {
            verify(obdbRead(page, "2021"))
        } else if (data.tag === "export note") {
            verify(obdbRead(page, "2021"))
            verify(page.keepObdb())
            const obdb = page.pluginList.find(p => p.obdb)
            page.openRow = obdb.profiles[0]
            verify(page.exportWaits(obdb.sha256))
        } else if (data.tag === "confirm") {
            page.askRemovePlugin(own.sha256)
            verify(page.confirmingPlugin() !== null)
        } else if (data.tag === "missing pick") {
            page.saveVehicles(VehicleBook.setPicks(page.vehicles,
                                                   page.picks.concat(["plugin.gone-from-the-store.base"])))
        }
        page.open("signals")
        const shown = find(page, "settingsSignalsPage")
        verify(shown.visible, "Signals shows")
        const column = find(page, "settingsSignalsPageColumn")
        waitForPolish(column)
        const buttons = buttonsUnder(shown, [])
        verify(buttons.length > 2, "buttons to measure")
        for (const b of buttons) {
            const o = b.mapToItem(null, 0, 0)
            const e = b.mapToItem(null, b.width, b.height)
            const name = b.objectName !== "" ? b.objectName : b.text
            verify(e.y - o.y >= 48 - 0.5, name + " is " + (e.y - o.y) + " tall")
            verify(e.x - o.x >= 48 - 0.5, name + " is " + (e.x - o.x) + " wide")
        }
        page.width = 320
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        waitForPolish(column)
        tryVerify(() => pastTheEdge(column, column, []).length === 0, 2000)
        compare(pastTheEdge(column, column, []).join("; "), "", "nothing past the column at 320")
    }

    // What the store holds for a vehicle goes with it: after Forget a
    // plugin imported for it is no longer first for a vehicle that takes
    // its id, and the plugins themselves stay
    function test_forgetReleasesWhatWasImportedForTheVehicle() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "extras.json", "Extras"))
        compare(page.pluginList[0].importedFor, "v1")
        press(page, "vehicleDuplicate")
        compare(page.picks, ["plugin.extras.base"], "Duplicate copies the picks")
        verify(rowData(page, "plugin.extras.base").forCar, "the copy picks it")
        pressRow(page, "plugin.extras.base")
        verify(!rowData(page, "plugin.extras.base").forCar, "imported for the first, not the copy")
        press(page, "vehicleForget")
        press(page, "vehicleConfirm")
        page.pickVehicle("v1")
        pressRow(page, "plugin.extras.base")
        compare(page.picks, [])
        verify(rowData(page, "plugin.extras.base").forCar, "imported for this one, picked or not")
        press(page, "vehicleForget")
        press(page, "vehicleConfirm")
        compare(page.vehicles.list.length, 0)
        compare(page.pluginList.length, 1, "the plugin stays")
        compare(page.pluginList[0].importedFor, "", "but is no vehicle's import")
        verify(find(rowOf(page, "plugin.extras.base"), "rowUnused").visible)

        press(page, "vehicleNew")
        verify(obdbRead(page, ""))
        press(page, "obdbKeep")
        compare(page.pluginList.filter(p => p.importedFor !== "").length, 1)
        press(page, "vehicleForgetAll")
        press(page, "vehicleConfirm")
        compare(page.pluginList.length, 2, "Forget all keeps the plugins")
        compare(page.pluginList.filter(p => p.importedFor !== "").length, 0)
        verify(findChild(page, "profile_plugin.extras.base") === null, "no vehicle, no pick offered")
    }

    // A pick's profile is its vehicle's: a pick of another vehicle, or
    // New, puts the field back to Default (its connection kept), so it
    // never connects, and is remembered, with the last one's signals; a
    // built-in profile stays
    function test_aPickLeavesAnotherVehiclesProfileBehind() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        verify(importFile(page, "extras.json", "Extras"))
        find(page, "uriField").text = "mock://demo"
        page.chooseProfile("plugin.extras.base")
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.extras.base")
        press(page, "vehicleNew")
        compare(find(page, "uriField").text, "mock://demo", "New: Default")
        compare(page.profileChoice, "default")

        const second = page.pickedVehicle.id
        press(page, "vehicle_" + first)
        page.chooseProfile("plugin.extras.base")
        press(page, "vehicleDuplicate")
        verify(page.pickedVehicle.id !== first && page.pickedVehicle.id !== second)
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.extras.base",
                "the copy has the pick: its profile stays")
        press(page, "vehicle_" + second)
        compare(find(page, "uriField").text, "mock://demo", "another's goes")

        page.chooseProfile("raw")
        press(page, "vehicle_" + first)
        compare(find(page, "uriField").text, "mock://demo?profile=raw", "a built-in stays")
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
        compare(page.pickedVehicle.own, "plugin.vehicle-1-copy-signals.base")
        compare(page.ownFile.name, "Vehicle 1 (copy) signals")
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
        compare(page.ownFile.name, "Vehicle 1 signals")
        compare(page.pickedVehicle.own, "plugin.vehicle-1-signals-2.base", "an id of its own")
    }

    // The form and the section's words are the picked vehicle's: a pick or
    // New starts them afresh, an Edit's signal never lands in another
    // vehicle, and a question asked about one goes with it
    function test_aPickStartsTheFormAndNotesAfresh() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        const bad = Harness.writeScratchFile("bad.json", signalFileText("Bad").replace("bits", "bitz"))
        verify(!(page.importFile(bad) && page.keepPending("", "")))
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

    // Edit then a new name renames the signal under the key it has; a new
    // signal whose name makes a key a saved one keeps gets a key of its
    // own, never the other's place; the same name again replaces, as
    // designed; and an Edit can't take another saved signal's name
    function test_anEditKeepsTheKeyAndAKeyIsNeverTakenTwice() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        press(page, "ownEdit_0")
        find(page, "formName").text = "Trans Fluid Temp"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.name), ["Trans Fluid Temp"])
        compare(page.ownSignals.map(s => s.key), ["trans_temp"], "renamed, the key it was saved with")
        compare(page.editingKey, "", "the Edit is over")

        // The name the first was saved under, typed for a new signal
        page.clearForm()
        typeSignal(page, "Trans Temp", "D100")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.key), ["trans_temp", "trans_temp_2"])
        compare(page.ownSignals.map(s => s.read.number), ["1E1C", "D100"], "neither saved over")

        page.clearForm()
        typeSignal(page, "Boost (psi)", "D101")
        press(page, "formSave")
        page.clearForm()
        typeSignal(page, "Boost psi", "D102")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.key), ["trans_temp", "trans_temp_2", "boost_psi", "boost_psi_2"])
        page.clearForm()
        typeSignal(page, "Boost (psi)", "D103")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.read.number), ["1E1C", "D100", "D103", "D102"],
                "the same name replaces")
        compare(page.ownSignals[2].key, "boost_psi")

        // The loaded variant has the keys the form kept
        compare(Bridge.profileSignals("mock://?profile=" + page.pickedVehicle.own).map(r => r.ref)
                    .filter(r => r.startsWith("key:")).sort(),
                ["key:boost_psi", "key:boost_psi_2", "key:trans_temp", "key:trans_temp_2"])

        // An Edit renamed to another saved signal's name is refused
        press(page, "ownEdit_0")
        find(page, "formName").text = "Boost psi"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "Boost psi is saved already: give this one another name")
        compare(page.ownSignals.map(s => s.name),
                ["Trans Fluid Temp", "Trans Temp", "Boost (psi)", "Boost psi"])
    }

    // A Dashboard gauge names a form signal by its key (ADR-0057), so it
    // reads on after the signal is renamed
    function test_aGaugeBoundToAFormSignalReadsOnAfterItsRename() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        const own = page.pickedVehicle.own
        const uri = "mock://demo?profile=" + own

        const dash = createTemporaryObject(dashComponent, testCase.Window.window.contentItem)
        const doc = DashboardBook.builtIn("desktop")
        doc.pages = [{ name: "Own", cells: [
            { kind: "digital", w: 2, h: 4, label: "Fluid temp", refs: ["key:trans_temp"], unit: "",
              options: {} }] }]
        dash.doc = doc
        const hub = createTemporaryObject(hubComponent, testCase)
        const gauge = gaugeLabeled(dash, "Fluid temp")
        verify(gauge, "the cell draws a gauge")

        compare(Bridge.connectSession(uri), "")
        tryVerify(() => gauge.hasValue, 5000, "the gauge reads the signal")
        compare(hub.byRef["key:trans_temp"].name, "Trans Temp")
        Bridge.disconnectSession()
        tryVerify(() => page.idle, 5000, "the session ends")

        press(page, "ownEdit_0")
        find(page, "formName").text = "Transmission fluid temperature"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.pickedVehicle.own, own, "the same variant")

        compare(Bridge.connectSession(uri), "")
        tryVerify(() => hub.byRef["key:trans_temp"] !== undefined
                        && hub.byRef["key:trans_temp"].name === "Transmission fluid temperature", 5000,
                  "the renamed signal under its key")
        tryVerify(() => gauge.hasValue, 5000, "the gauge reads on")
        verify(gauge.note !== "not in profile", gauge.note)
    }

    // The page's gauge (a GaugeBase item) of that label, null for none
    function gaugeLabeled(within, label) {
        for (let i = 0; i < within.children.length; ++i) {
            const child = within.children[i]
            if (child.label === label && child.note !== undefined && child.valueText !== undefined)
                return child
            const found = gaugeLabeled(child, label)
            if (found !== null)
                return found
        }
        return null
    }

    // A Read once's reply shows only on the vehicle it was asked for
    function test_aReadOncesReplyShowsOnlyOnItsVehicle() {
        const page = createPage()
        press(page, "vehicleNew")
        const reply = { ok: true, request: "22 1E 1C", reply: "62 1E 1C 12 34",
                        value: "4660", nrc: 0, words: "" }
        page.readOnceFor = "another vehicle"
        Bridge.readOnceResult(reply)
        compare(page.readResult, null, "asked for another vehicle")
        page.readOnceFor = page.pickedId
        Bridge.readOnceResult(reply)
        compare(page.readResult.reply, "62 1E 1C 12 34")
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
        compare(find(page, "formNote").text, "Gear saved in Vehicle 1 signals")
        typeSignal(page, "Oil", "D102")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = "7E2"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(find(page, "formNote").text, "Oil saved in Vehicle 1 signals; 7E9 asks on 7E2 for Gear too")
        compare(page.ownFile.ecus.map(e => e.tx), ["7E2"])
        typeSignal(page, "Pressure", "D103")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = ""
        press(page, "formSave")
        compare(find(page, "formNote").text, "Pressure saved in Vehicle 1 signals")
        compare(page.ownFile.ecus.map(e => e.tx), ["7E2"], "blank keeps the module's")
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
        compare(find(page, "formNote").text, "Trans Temp saved in Vehicle 1 signals")
        compare(ownText(page, 0), "Trans Temp: 22 1E1C on 7E8")
        const own = "plugin.vehicle-1-signals.base"
        compare(page.pickedVehicle.own, own, "the vehicle's own variant")
        compare(page.picks, [own], "made with the first save, and picked")
        compare(find(rowOf(page, own), "rowTitle").text, "Vehicle 1 signals 1.0.0")
        compare(find(rowOf(page, own), "rowReads").text, "1 signal · yours")
        compare(find(rowOf(page, own), "rowCaption").text, "For this car:")
        verify(findChild(page, "profile_" + own), "a profile under Profile")
        compare(Harness.storedPlugins().length, 1)

        // A second signal goes into the same variant; the last copy goes
        page.fillForm(SignalForm.emptyFields())
        typeSignal(page, "Gear", "D101")
        press(page, "formKind_enum")
        find(page, "formValues").text = "0=Park, 3=Drive"
        press(page, "formSave")
        compare(page.ownSignals.map(s => s.key), ["trans_temp", "gear"])
        compare(page.ownSignals[1].values, { "0": "Park", "3": "Drive" })
        compare(Harness.storedPlugins().length, 1, "one copy: the plugin's newest")
        compare(page.pluginList.length, 1, "no second plugin")
        compare(page.pickedVehicle.own, own)
        compare(page.picks, [own])
        compare(find(rowOf(page, own), "rowReads").text, "2 signals · yours")

        // Edit fills the form; Save replaces the signal of that name
        press(page, "ownEdit_0")
        compare(find(page, "formName").text, "Trans Temp")
        compare(find(page, "formBits").text, "16")
        compare(page.formKind, "number")
        find(page, "formNumber").text = "1E1D"
        press(page, "formSave")
        compare(page.ownSignals.map(s => s.read.number), ["1E1D", "D101"])

        // The variant as a signal file, named by its plugin
        const what = page.variantSave(rowData(page, own))
        compare(what.file, "vehicle-1-signals.json")
        const url = Harness.writeScratchFile("own.json", "")
        page.saving = what
        verify(page.saveTo(url))
        compare(JSON.parse(Harness.readScratchFile(url)).signals.map(s => s.name),
                ["Trans Temp", "Gear"])

        // The last signal's Remove takes the plugin, once asked: it was
        // written here and has no copy elsewhere
        press(page, "ownRemove_1")
        compare(page.ownSignals.length, 1)
        compare(find(page, "formNote").text, "Removed from Vehicle 1 signals")
        press(page, "ownRemove_0")
        compare(page.ownSignals.length, 1, "asked first")
        const question = find(page, "ownConfirm")
        verify(question.visible, "asked under the own signals, where it was pressed")
        compare(find(question, "signalsConfirmText").text,
                "Remove Vehicle 1 signals? It is yours, and no other copy exists")
        find(question, "signalsConfirmRemove").clicked()
        compare(page.picks, [])
        compare(page.pickedVehicle.own, "")
        compare(Harness.storedPlugins(), [])

        // The next save makes the variant again
        typeSignal(page, "Oil Temp", "D103")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.pickedVehicle.own, own)
        compare(page.picks, [own])
    }

    // The last signal's Remove of a plugin with several variants asks as
    // well, and drops that variant alone once answered
    function test_theLastSignalsRemoveAsksWhereThePluginHasSeveralVariants() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        const own = page.pickedVehicle.own
        const url = Harness.writeScratchFile("second.json", signalFileText("Second", "D105"))
        verify(page.importFile(url))
        verify(page.keepPending(page.ownPlace.plugin.sha256, "second"))
        compare(page.ownPlace.plugin.variants.length, 2, "a second variant")
        press(page, "ownRemove_0")
        compare(page.pickedVehicle.own, own, "asked first, nothing removed")
        const question = find(page, "ownConfirm")
        verify(question.words !== "", "asked under the own signals")
        verify(find(question, "signalsConfirmText").text.startsWith("Remove the variant "),
               find(question, "signalsConfirmText").text)
        find(question, "signalsConfirmCancel").clicked()
        compare(page.pickedVehicle.own, own, "Cancel keeps it")
        press(page, "ownRemove_0")
        find(find(page, "ownConfirm"), "signalsConfirmRemove").clicked()
        compare(page.pickedVehicle.own, "", "the variant went once asked")
        compare(Harness.storedPlugins().length, 1, "the plugin stayed with its other variant")
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
        compare(Harness.storedPlugins(), [])

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
        verify(find(page, "profile_uds-generic").checked, "read back into the row")
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

    // A text row's tick is the vehicle's too, though it changes no plotted
    // count: the ticked count carries it
    function test_aTextRowsTickIsKeptForTheVehicle() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath
            + "?profile=uds-generic&did=F190:text,1E1C:u16&identify=0&diagnose=0"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        tryVerify(() => page.plottedApplied, 5000, "the vehicle's choices applied")
        const plotted = Bridge.signalModel.subscribedCount
        const writes = logSpyComponent.createObject(testCase,
            { target: page, signalName: "vehiclesChanged" })
        Bridge.signalModel.toggleSubscribed(0)   // DID F190, a text row
        compare(Bridge.signalModel.subscribedCount, plotted, "it plots nothing")
        const names = Bridge.signalModel.plottedNames()
        compare(names.length, 1)
        compare(page.pickedVehicle.plotted, names, "the tick is kept")
        compare(writes.count, 1, "one write for the text row's tick")
        // A number's untick changes both counts, and is still one write
        Bridge.signalModel.toggleSubscribed(1)   // DID 1E1C
        compare(writes.count, 2, "one write for the number's untick")
        writes.destroy()
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
        page.chooseProfile("plugin.extras.base")
        compare(find(page, "uriField").text, uri + "?profile=plugin.extras.base")
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.id === daily, 10000, "the VIN picks Daily driver")
        compare(find(page, "uriField").text, uri + "?profile=plugin.extras.base",
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
        page.chooseProfile("plugin.extras.base")
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.id !== weekend, 30000, "the VIN adds a vehicle")
        // The connect result keeps the URI on its own turn, after the VIN
        tryVerify(() => Bridge.connected && page.pickedVehicle.uri === uri, 30000,
                  "the connect keeps the URI")
        compare(page.pickedVehicle.picks, [])
        compare(page.pickedVehicle.uri, uri, "no profile of Weekend car's pick")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 30000)
        tryVerify(() => find(page, "uriField").text === uri, 5000, "Default at the session's end")
    }

    // ADR-0056 section 4: the row lists Off and the bridge's choices,
    // shows the bridge's and sets it, and says what it is for
    function test_theMinimumTimeBetweenSweepsIsARowOfChoices() {
        const page = createPage(true)
        page.open("advanced")
        const choice = find(page, "minSweepChoice")
        verify(choice.visible)
        verify(find(page, "minSweepNote").text.includes("file size"))
        // Every requesting session's, not a log's alone
        verify(find(page, "minSweepNote").text.includes("on Live, the Dashboard and in a log"))
        // Core paces a pick alone, never the whole set
        verify(find(page, "minSweepNote").text.includes("asking every signal (its profile ranks none) isn't held to it"))
        compare(choice.Accessible.name, "Minimum time between sweeps")
        compare(choice.count, 5)
        compare(choice.textAt(0), "Off")
        compare(choice.textAt(2), "250 ms")
        compare(Bridge.minSweepMs, 0)
        compare(choice.currentIndex, 0)
        Bridge.minSweepMs = 500
        compare(choice.currentIndex, 3, "it shows the bridge's before any pick")
        Bridge.minSweepMs = 0
        choice.currentIndex = 2
        choice.activated(2)
        compare(Bridge.minSweepMs, 250)
        Bridge.minSweepMs = 1000
        compare(choice.currentIndex, 4, "it follows the bridge")
        choice.activated(0)
        compare(Bridge.minSweepMs, 0)
        compare(choice.currentIndex, 0)
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
