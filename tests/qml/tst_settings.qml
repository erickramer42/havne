// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

SettingsKit {
    id: testCase
    name: "Settings"

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
        page.connectFromPart.requestConnect("mock://demo?profile=no-such-profile")
        tryVerify(() => find(page, "errorLabel").visible, 5000, "the refusal")
        verify(show.visible, "Show log beside it")
        tap(show)
        compare(logSpy.count, 1, "the Log asked for")
    }

    // A refusal quotes what core was given, and an adapter's reply: drawn as
    // text, never as markup (an <img> in markup loads a file: URL's host with
    // no interceptor asked), so a tag in the address shows as typed
    function test_aRefusalDrawsItsWordsAsPlainText() {
        const page = createPage()
        page.connectFromPart.requestConnect("mock://demo?diagnose=<b>x</b>")
        const label = find(page, "errorLabel")
        tryVerify(() => label.visible, 5000, "the refusal")
        verify(label.text.includes("<b>x</b>"), label.text)
        compare(label.textFormat, Text.PlainText)
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
        compare(page.connectFromPart.connectAdapters.map(a => a.uri), page.devices.map(d => d.base))
        compare(page.connectFromPart.connectAdapters.map(a => a.uri),
                ["j2534://Vendor X", "elm327://COM5", "socketcan://can0",
                 "elm327://bt:00:11:22:33:44:55", "elm327://ble:AA:BB:CC:DD:EE:FF"])
        compare(page.connectFromPart.connectAdapters.map(a => a.title),
                ["Vendor X  (x64)", "COM5", "can0", "00:11:22:33:44:55", "Scope"])
        compare(page.connectFromPart.connectAdapters.map(a => a.kind),
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
        const words = page.connectPart.errorLabel.text
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
        compare(page.ownPart.editingKey, "trans_temp")
        page.back()
        compare(page.pageKey, "signals", "back from the form lands on Signals")

        press(page, "ownAdd")
        compare(page.pageKey, "ownform", "Add opens the form")
        compare(find(page, "formName").text, "", "Add starts empty")
        compare(page.ownPart.editingKey, "")
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
        page.ownPart.formKind = "number"
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

    // ---- The vehicle's own signals (ADR-0051's form) ----

    function typeSignal(page, name, number, bits) {
        find(page, "formName").text = name
        find(page, "formNumber").text = number
        if (bits !== undefined)
            find(page, "formBits").text = bits
    }
}
