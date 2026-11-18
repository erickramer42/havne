// File/layer: tests/qml/tst_dashedit.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5), the Add flow and the signal picker:
// each control's write and what's stored
TestCase {
    id: testCase
    name: "DashboardEdit"
    when: windowShown
    // The window holds a desktop page whole, so a click lands on it
    width: 1280
    height: 800

    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }
    // The page's own store, to put each case's back
    Component {
        id: storeComponent
        Settings {
            category: "dashboards"
            property string phone: ""
            property string desktop: ""
            property int phonePage: 0
            property int desktopPage: 0
            property string phoneUnread: ""
            property string desktopUnread: ""
        }
    }

    // A binding error in the app's QML fails the test. The page's
    // Settings stores a write when the page goes, so the store is put
    // back once the last case's pages are gone, through setValue
    function init() {
        // Each case starts from no session, whatever an earlier one left
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "no session at the start")
        wait(0)
        const store = createTemporaryObject(storeComponent, testCase)
        for (const key of ["phone", "desktop", "phoneUnread", "desktopUnread"])
            store.setValue(key, "")
        store.setValue("phonePage", 0)
        store.setValue("desktopPage", 0)
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        failOnWarning(/^Model size of /)
    }

    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
        Theme.night = "off"
        Theme.look = "dark"
        Bridge.unitSystem = 0
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        for (const p of Bridge.plugins())     // the store starts empty
            Bridge.removePlugin(p.sha256)
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // A page in the window (an item's `visible` is its own there), showing
    // `doc` when one is given
    function pageOf(props, doc) {
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem, props)
        if (doc)
            page.doc = doc
        return page
    }

    // A page showing `doc` with its text stored, as the app has it after an
    // edit, so an undo stores the text it had
    function storedPageOf(props, doc) {
        const page = pageOf(props, doc)
        find(page, "dashStore")[page.form === "phone" ? "phone" : "desktop"] = DashboardBook.serialize(doc)
        return page
    }

    // A one-page document of these cells
    function docOf(cells) {
        const d = DashboardBook.builtIn("desktop")
        d.pages = [{ name: "Test", cells: cells }]
        return d
    }
    // A number by default: a dial is many times slower to build, so a case passes "analog" only where the kind matters
    function cellOf(label, kind) {
        return { kind: kind || "digital", w: 2, h: 4, label: label, refs: ["id:1001"],
                 unit: "RPM", options: { from: 0, to: 8000 } }
    }
    function labels(page) {
        return page.doc.pages[page.pageIndex].cells.map(c => c.label)
    }
    function storedText(page) {
        return find(page, "dashStore")[page.form === "phone" ? "phone" : "desktop"]
    }
    // A press where the item is, once the window is laid out and the item
    // has stopped moving (a page change slides the view). The layout comes
    // first: a row that opens keeps the place it last had, and until the
    // layout pass moves the page below it, the page covers it
    function click(page, name) {
        const item = find(page, name)
        verify(item.visible && item.enabled, name + " can be pressed")
        press(item)
    }
    function press(item) {
        verify(waitForPolish(item.Window.window), item.objectName + " is laid out")
        // A slide's first frame can come after both samples below, with the target still past the window's edge
        for (let up = item.parent; up; up = up.parent) {
            if (up.objectName === "dashPages") {
                const view = up.contentItem
                tryVerify(() => view.currentItem && Math.abs(view.contentX - view.currentItem.x) < 0.5,
                          3000, item.objectName + "'s page has slid in")
            }
        }
        // A cell below a tall panel is scrolled into the page's view first,
        // and the page put back after the press
        let scrolled = null
        let was = 0
        let inSide = false
        // The side column scrolls by itself: bring the item into its view
        for (let up = item.parent; up; up = up.parent) {
            if (up.objectName === "dashSidePanel" && up.visible) {
                inSide = true
                const at = item.mapToItem(up, item.width / 2, item.height / 2)
                if (at.y > up.height - 24 || at.y < 24)
                    up.contentY = Math.max(0, Math.min(up.contentHeight - up.height,
                                                       up.contentY + at.y - up.height / 2))
            }
        }
        for (let up = item.parent; up && !inSide; up = up.parent) {
            if (up.objectName.startsWith("dashPage_")) {
                const at = item.mapToItem(up, item.width / 2, item.height / 2)
                if (at.y > up.height - 24) {
                    was = up.contentY
                    up.contentY = Math.max(0, Math.min(up.contentHeight - up.height,
                                                       up.contentY + at.y - up.height / 2))
                    scrolled = up
                }
            }
        }
        let last = null
        tryVerify(() => {
            const at = item.mapToItem(null, 0, 0)
            const still = last !== null && at.x === last.x && at.y === last.y
            last = at
            return still
        }, 3000, item.objectName + " stops moving")
        mouseClick(item)
        if (scrolled)
            scrolled.contentY = was
    }
    // Text typed into a field and the field left; it emits textEdited, as real typing does
    function type(field, text) {
        field.text = text
        field.textEdited()
        field.editingFinished()
    }
    function spin(box, value) {
        box.value = value
        box.valueModified()
    }

    // Edit shows the bar and each cell's Remove; Done leaves edit mode,
    // and nothing was waiting to be saved
    // The finger down on a cell until the page lifts it, after a short
    // hold unless the case asks for the platform's
    function hold(page, cell, platformHold) {
        tryVerify(() => cell.width > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        if (!platformHold)
            shortHold(cell)
        mousePress(cell)
        tryVerify(() => page.held >= 0, 10000, "the hold completes")
    }
    // Set on the held cell alone, just before its press (a reflow can replace
    // the delegate); other cells' taps keep the platform's interval
    readonly property int shortHoldMs: 200
    function shortHold(cell) {
        find(cell, "cellTap").pressAndHoldInterval = shortHoldMs
    }

    function test_editAndDone() {
        // B's dial shows its peak, so its gauge takes taps too
        const peaked = cellOf("B", "analog")
        peaked.options.peak = true
        const page = pageOf({}, docOf([cellOf("A"), peaked]))
        const bar = find(page, "dashEditBar")
        verify(!bar.visible, "no bar outside edit mode")
        verify(!findChild(find(page, "dashCell_0_0"), "cellRemove"), "no Remove outside edit mode")
        verify(!find(page, "dashAddCell_0").visible, "no + cell outside edit mode")
        click(page, "dashEdit")
        verify(page.editing)
        verify(bar.visible)
        verify(!find(page, "dashEdit").visible, "Edit hides while editing")
        verify(find(find(page, "dashCell_0_1"), "cellRemove").visible)
        verify(find(page, "dashAddCell_0").visible, "the page ends with a + cell")
        // A tap selects; another switches the panel to it
        press(find(page, "dashCell_0_1"))
        compare(page.selected, 1)
        compare(page.panel, "cell")
        verify(find(find(page, "dashCell_0_1"), "cellSelected").visible, "outlined")
        verify(!findChild(find(page, "dashCell_0_0"), "cellSelected"))
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        click(page, "panelClose")
        compare(page.panel, "")
        click(page, "dashDone")
        verify(!page.editing)
        verify(!bar.visible)
        compare(page.selected, -1)
        tryVerify(() => !findChild(find(page, "dashCell_0_1"), "cellRemove"), 3000,
                  "Remove goes with edit mode")
        compare(storedText(page), "", "no edit, nothing stored")
        // Outside edit mode a tap selects nothing
        press(find(page, "dashCell_0_0"))
        compare(page.selected, -1)
        compare(page.panel, "")
    }

    // Only the page shown is edited: another page's cells show no Remove
    // and no outline at the selected index
    function test_onlyTheShownPageIsEdited() {
        const two = docOf([cellOf("A")])
        two.pages.push({ name: "More", cells: [cellOf("B")] })
        const page = pageOf({}, two)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        const other = find(page, "dashCell_1_0")
        verify(find(find(page, "dashCell_0_0"), "cellRemove").visible)
        verify(!findChild(other, "cellRemove"), "no Remove off the page shown")
        verify(!findChild(other, "cellSelected"), "no outline off the page shown")
    }

    // Add: a kind with its still preview, then a signal of the session's;
    // the cell stores the signal's decoded unit and its default range
    // (DashboardBook.defaultRange) in any unit system, with its
    // counterpart's ref
    function test_addTakesTheDecodedUnitAndRange_data() {
        return [{ tag: "metric", units: 0, from: "0", to: "150" },
                { tag: "imperial", units: 1, from: "32", to: "302" }]
    }
    function test_addTakesTheDecodedUnitAndRange(data) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        Bridge.unitSystem = data.units
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        click(page, "dashEdit")
        click(page, "dashAdd")
        compare(page.panel, "add")
        const kinds = find(page, "dashAddPanel")
        for (const kind of page.drawnKinds) {
            find(kinds, "addKind_" + kind)
            const preview = find(kinds, "preview_" + kind)
            tryCompare(preview, "status", Loader.Ready)
            compare(preview.item.value, 60, kind + ": still at 60 percent")
            compare(preview.item.from, 0)
            compare(preview.item.to, 100)
            verify(!preview.item.sweeping, kind + ": no sweep")
        }
        click(page, "addKind_analog")
        compare(page.panel, "signal")
        const coolant = Bridge.gaugeSignals["coolant"]
        const panel = find(page, "dashSignalPanel")
        verify(!findChild(panel, "pickSignal_"), "no row without a ref")
        click(page, "pickSignal_" + coolant[0])

        const cells = page.doc.pages[0].cells
        compare(cells.length, 2)
        const made = cells[1]
        compare(made.kind, "analog")
        compare(made.w, 1)
        compare(made.h, 4)
        compare(made.label, "Coolant Temp")
        compare(made.unit, "°C", "the decoded unit")
        compare(JSON.stringify(made.refs), JSON.stringify(coolant), "with its counterpart")
        compare(made.options.from, 0, "the unit's default range")
        compare(made.options.to, 150)
        verify(made.options.decimals === undefined, "decimals follow the row")
        compare([made.options.coldBelow, made.options.warnFrom, made.options.dangerFrom],
                [60, 105, 115], "the coolant's table zones")
        compare(page.selected, 1, "the new cell is selected")
        compare(page.panel, "cell")
        compare(storedText(page), DashboardBook.serialize(page.doc), "stored as it's made")

        // The panel shows the range in the unit shown, and stores what's
        // typed in the decoded unit
        compare(find(page, "panelFrom").text, data.from)
        compare(find(page, "panelTo").text, data.to)
        type(find(page, "panelTo"), data.units === 1 ? "212" : "100")
        // Exactly: compare() passes numbers within 1e-5, and the inverse's
        // noise is under that
        verify(page.doc.pages[0].cells[1].options.to === 100, String(page.doc.pages[0].cells[1].options.to))
        compare(find(page, "panelTo").text, data.units === 1 ? "212" : "100", "re-bound")
        type(find(page, "panelWarnFrom"), data.units === 1 ? "194" : "90")
        verify(page.doc.pages[0].cells[1].options.warnFrom === 90, String(page.doc.pages[0].cells[1].options.warnFrom))
        tryCompare(find(page, "dashCell_0_1"), "cell", page.doc.pages[0].cells[1])
    }

    // A new gauge on a common signal starts with the table's zones, stored in
    // the base unit and shown through the unit system; a signal outside the
    // table starts with none
    function addGauge(page, kind, signalKey) {
        click(page, "dashAdd")
        click(page, "addKind_" + kind)
        click(page, "pickSignal_" + Bridge.gaugeSignals[signalKey][0])
        return page.doc.pages[0].cells[page.selected]
    }
    function test_aNewGaugeStartsWithItsSignalsZones_data() {
        return [{ tag: "metric", units: 0 }, { tag: "imperial", units: 1 }]
    }
    function test_aNewGaugeStartsWithItsSignalsZones(data) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        Bridge.unitSystem = data.units
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        for (const key in DashboardBook.zoneTable) {
            if (key === "rpm")
                continue
            const cell = addGauge(page, "analog", key)
            const zones = DashboardBook.zonesFor(key)
            for (const name in zones)
                compare(cell.options[name], zones[name], key + " stores " + name)
            for (const name of DashboardBook.zoneOptions)
                verify((cell.options[name] !== undefined) === (zones[name] !== undefined),
                       key + ": only the table's zones, " + name)
        }
        const cell = addGauge(page, "analog", "coolant")
        const shown = field => find(page, field).text
        compare([shown("panelColdBelow"), shown("panelWarnFrom"), shown("panelDangerFrom")],
                data.units === 1 ? ["140", "221", "239"] : ["60", "105", "115"],
                "the panel shows the unit system's values")
        compare(cell.options.dangerFrom, 115, "stored in the base unit")
        for (const key of ["speed", "maf", "throttle", "load", "lambda", "boost"]) {
            const plain = addGauge(page, "analog", key)
            for (const name of DashboardBook.zoneOptions)
                verify(plain.options[name] === undefined, key + " starts with no " + name)
        }
    }

    function test_aNewShiftLightStartsFromTheShiftBandOnRpm() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        Bridge.unitSystem = 0
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        let cell = addGauge(page, "shift-light", "rpm")
        compare([cell.options.from, cell.options.to], [4500, 6500], "rpm takes the band")
        verify(cell.options.dangerFrom === undefined, "and no zones")
        compare(find(page, "panelShiftHint").visible, true, "the hint shows")
        compare(find(page, "panelDangerFrom").visible, false, "the lights read no zones")
        compare([find(page, "panelFromCaption").text, find(page, "panelToCaption").text],
                ["First light at (RPM)", "Shift at (RPM)"], "the shift light's captions")
        cell = addGauge(page, "analog", "rpm")
        compare([find(page, "panelFromCaption").text, find(page, "panelToCaption").text],
                ["From (RPM)", "To (RPM)"], "another kind shows From and To")
        verify(!find(page, "panelShiftHint").visible, "and no hint")
        cell = addGauge(page, "shift-light", "speed")
        const speed = Bridge.profileSignals("mock://demo").find(
            r => r.ref === Bridge.gaugeSignals["speed"][0])
        const range = DashboardBook.defaultRange(speed.decodedUnit, speed.decodedLow, speed.decodedHigh)
        compare([cell.options.from, cell.options.to], [range.from, range.to],
                "another signal keeps its range")
    }

    // A file's gauge on engine speed doesn't change a shift light's band; a
    // gauge with a range and no levels takes the table's zones
    function test_aShiftLightOnRpmTakesTheBandWhateverTheFilesGauge() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        const row = { ref: Bridge.gaugeSignals["rpm"][0], decodedUnit: "RPM", decodedLow: 0,
                      decodedHigh: 8000, gauge: { min: 0, max: 8000 } }
        const light = page.addFlowPart.startOptions("shift-light", row)
        compare([light.from, light.to], [4500, 6500], "the band, not the file's 0-8000")
        const dial = page.addFlowPart.startOptions("analog", row)
        compare([dial.from, dial.to, dial.dangerFrom], [0, 8000, 6800],
                "a dial takes the file's range and, with no levels, the table's zones")
    }

    // The file's gauge is the signal's own: its range and levels win over
    // the table, and a file level it leaves out stays unset
    function test_aFilesGaugeWinsOverTheTable() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        const ref = Bridge.gaugeSignals["battery"][0]
        const row = { ref: ref, decodedUnit: "V", decodedLow: 0, decodedHigh: 20,
                      gauge: { min: 9, max: 17, warnLow: 11.5 } }
        const o = page.addFlowPart.startOptions("analog", row)
        compare([o.from, o.to, o.warnBelow], [9, 17, 11.5], "the file's range and low level")
        for (const name of ["dangerBelow", "warnFrom", "dangerFrom", "coldBelow"])
            verify(o[name] === undefined, "not the table's " + name)
        row.gauge = { min: 9, max: 17, warnHigh: 15 }
        const high = page.addFlowPart.startOptions("analog", row)
        compare(high.warnFrom, 15)
        verify(high.warnBelow === undefined, "no low level the file lacks")
        row.gauge = {}
        const table = page.addFlowPart.startOptions("analog", row)
        compare([table.dangerBelow, table.warnBelow, table.warnFrom, table.dangerFrom],
                [11.8, 12.4, 14.8, 15.2], "no gauge: the table")
    }

    // A signal file's text kept as a plugin of the user's own; profile: its one variant's
    function keptFile(text) {
        const r = Bridge.keepSignalText(text, "", "", "")
        return { ok: r.ok, words: r.words, sha256: r.sha256,
                 profile: r.ok ? r.profiles[0] : "" }
    }

    // End to end: a signal file's gauge, loaded as a profile, reaches the cell
    // picked on its row, and the levels it leaves out stay unset
    function test_aSignalFilesLevelsReachTheNewCell() {
        const file = { havne_signals: 1, name: "Gauged", source: { name: "t" },
                       ecus: [{ id: "pcm", rx: "7E8" }],
                       signals: [{ key: "a", name: "A", read: { service: "22", number: "D100" },
                                   field: { byte: 0, bits: 8 }, unit: "V",
                                   gauge: { min: 10, max: 16, warn_low: 11.5, warn_high: 15.25 } },
                                 { key: "b", name: "B", read: { service: "22", number: "D101" },
                                   field: { byte: 0, bits: 8 }, unit: "V",
                                   gauge: { min: 0, max: 50 } }] }
        const r = keptFile(JSON.stringify(file))
        verify(r.ok, "the file imports")
        compare(Bridge.connectSession("mock://demo?profile=" + r.profile), "", "the file's profile connects")
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_analog")
        click(page, "pickSignal_key:a")
        let cell = page.doc.pages[0].cells[page.selected]
        compare([cell.options.from, cell.options.to, cell.options.warnBelow, cell.options.warnFrom],
                [10, 16, 11.5, 15.25], "the file's range and levels")
        verify(cell.options.dangerBelow === undefined && cell.options.dangerFrom === undefined,
               "no danger level the file lacks")
        click(page, "dashAdd")
        click(page, "addKind_analog")
        click(page, "pickSignal_key:b")
        cell = page.doc.pages[0].cells[page.selected]
        compare([cell.options.from, cell.options.to], [0, 50], "a gauge with a range and no levels")
        for (const name of DashboardBook.zoneOptions)
            verify(cell.options[name] === undefined, "starts with no " + name + ": a key outside the table")
    }

    function test_changeSignalReplacesTheZonesWithTheNewSignalsTable() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        let cell = addGauge(page, "analog", "battery")
        compare([cell.options.dangerBelow, cell.options.warnBelow], [11.8, 12.4])
        click(page, "panelSignal")
        click(page, "pickSignal_" + Bridge.gaugeSignals["coolant"][0])
        cell = page.doc.pages[0].cells[page.selected]
        verify(cell.options.dangerBelow === undefined && cell.options.warnBelow === undefined,
               "the battery's low levels go")
        compare([cell.options.coldBelow, cell.options.warnFrom, cell.options.dangerFrom], [60, 105, 115])
        click(page, "panelSignal")
        click(page, "pickSignal_" + Bridge.gaugeSignals["speed"][0])
        cell = page.doc.pages[0].cells[page.selected]
        for (const name of DashboardBook.zoneOptions)
            verify(cell.options[name] === undefined, "a signal outside the table clears " + name)
    }

    // An enumeration only for a digital tile; text never
    function test_thePickerOffersWhatTheKindCanShow() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        page.addFlowPart.refreshRows()
        const rows = page.addFlowPart.profileRows
        verify(rows.length > 0, "the session's rows")
        const analog = page.addFlowPart.pickable("analog")
        const digital = page.addFlowPart.pickable("digital")
        verify(analog.every(r => r.ref !== "" && !r.isText && !r.isEnum))
        verify(digital.every(r => r.ref !== "" && !r.isText))
        compare(digital.length, rows.filter(r => r.ref !== "" && !r.isText).length)
        // The demo has no enumeration: one made up, beside a number and a
        // text
        page.addFlowPart.profileRows = [{ ref: "key:mode", isText: false, isEnum: true },
                            { ref: "key:rpm", isText: false, isEnum: false },
                            { ref: "key:vin", isText: true, isEnum: false }]
        compare(page.addFlowPart.pickable("bar").map(r => r.ref), ["key:rpm"])
        compare(page.addFlowPart.pickable("digital").map(r => r.ref), ["key:mode", "key:rpm"])
    }

    // No session: the rows of the last URI that connected; none, a line
    function test_withNoSessionThePickerReadsWhatSettingsChose() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        page.addFlowPart.pickKind("ring")
        find(page, "dashSignalPanel")
        verify(find(page, "signalNone").visible, "nothing chosen, nothing to pick")
        compare(find(page, "signalNone").text,
                "No signals: connect, or pick a plugin for your vehicle under Settings > Signals.",
                "names where a vehicle's signals come from")

        // While the list is open it follows Settings
        page.offlineUri = "mock://?profile=obd2"
        tryVerify(() => findChild(page, "pickSignal_" + Bridge.gaugeSignals["speed"][1]) !== null,
                  3000, "obd2's speed, offered")
        verify(!find(page, "signalNone").visible)

        page.offlineUri = "mock://demo"
        tryVerify(() => findChild(page, "pickSignal_" + Bridge.gaugeSignals["speed"][1]) === null,
                  3000, "and gone with obd2")
        click(page, "pickSignal_" + Bridge.gaugeSignals["speed"][0])
        compare(page.doc.pages[0].cells[1].kind, "ring")
        compare(page.doc.pages[0].cells[1].unit, "km/h")
        compare(page.doc.pages[0].cells[1].options.to, 260)
    }

    // The vehicle's picked sets come first, as sections, with or without a
    // session; the listed profile's rows follow by group, folded while a set leads
    function test_theSignalListLeadsWithThePickedSetsOwnSignals() {
        const profile = threeSignalFile()
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.pickedSets = [{ profile: profile, name: "Three", variant: "" }]
        page.offlineUri = "mock://?profile=obd2"
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_bar")
        const panel = find(page, "dashSignalPanel")
        tryVerify(() => panel.sections.length > 1, 3000, "the sections")
        compare(panel.sections[0].key, "set0", "the set's own first")
        compare(panel.sections[0].title, "Three")
        compare(panel.sections[0].rows.map(r => r.ref), ["key:a", "key:b", "key:c"])
        compare(find(page, "signalSection_set0").text, "Three (3)")
        verify(find(page, "pickSignal_key:a").visible, "its signals are shown")
        const groups = panel.sections.filter(sec => sec.key.startsWith("group"))
        verify(groups.length > 0, "obd2's groups follow")
        verify(groups.every(sec => !sec.open), "folded while a set leads")
        const speed = "pickSignal_" + Bridge.gaugeSignals["speed"][1]
        verify(findChild(page, speed) === null, "no wall of chips")
        // A word in the filter opens every group that has a match
        type(find(page, "filterText"), "speed")
        tryVerify(() => findChild(page, speed) !== null, 3000, "a typed word shows the match")
        type(find(page, "filterText"), "")
        tryVerify(() => findChild(page, speed) === null, 3000, "and the groups fold again")
        // A header opens its group
        click(page, "signalSection_" + groups[0].key)
        tryVerify(() => panel.sections.find(sec => sec.key === groups[0].key).open, 3000, "opened")
        tryVerify(() => findChild(page, "pickSignal_" + groups[0].rows[0].ref) !== null, 3000, "its chips show")
        // The set's own header folds and opens it
        click(page, "signalSection_set0")
        tryVerify(() => findChild(page, "pickSignal_key:a") === null, 3000, "folded by its header")
        click(page, "signalSection_set0")
        tryVerify(() => findChild(page, "pickSignal_key:a") !== null, 3000, "and opened again")
        // The set's signal makes the gauge
        click(page, "pickSignal_key:b")
        compare(page.doc.pages[0].cells[1].refs[0], "key:b")
    }

    // No session and no address: the picked set is still offered
    function test_thePickedSetIsOfferedWithNoSessionAndNoAddress() {
        const profile = threeSignalFile()
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.pickedSets = [{ profile: profile, name: "Three", variant: "" }]
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_digital")
        const panel = find(page, "dashSignalPanel")
        tryVerify(() => panel.sections.length === 1, 3000, "the set alone")
        verify(!find(page, "signalNone").visible, "not 'No signals'")
        click(page, "pickSignal_key:a")
        compare(page.doc.pages[0].cells[1].refs[0], "key:a")
    }

    // The session reads the set itself: its rows are the set's section alone, never listed twice
    function test_aSetTheSessionReadsIsListedOnce() {
        const profile = threeSignalFile()
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.pickedSets = [{ profile: profile, name: "Three", variant: "" }]
        page.offlineUri = "mock://?profile=" + profile
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_bar")
        const panel = find(page, "dashSignalPanel")
        tryVerify(() => panel.sections.length > 0, 3000)
        compare(panel.sections.map(sec => sec.key), ["set0"], "the set once, no group of its own rows")
        compare(panel.shownCount, 3)
    }

    // With no set picked the list is the profile's groups, all open
    function test_withNoPickedSetTheGroupsAreAllOpen() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.offlineUri = "mock://?profile=obd2"
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_bar")
        const panel = find(page, "dashSignalPanel")
        tryVerify(() => panel.sections.length > 0, 3000, "the groups")
        verify(panel.sections.every(sec => sec.key.startsWith("group") && sec.open))
        verify(findChild(page, "pickSignal_" + Bridge.gaugeSignals["speed"][1]) !== null)
    }

    // A group lists the signals the ECU doesn't send last, dim
    function test_theSignalsTheEcuDoesntSendComeLastInTheirGroup() {
        compare(Bridge.connectSession("mock://demo?profile=obd2"), "", "the demo connects")
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_bar")
        const panel = find(page, "dashSignalPanel")
        tryVerify(() => Bridge.signalModel.hiddenCount > 0, 10000, "the demo car lacks some PIDs")
        tryVerify(() => panel.sections.some(sec => sec.hiddenFrom >= 0), 3000, "a group with a dim tail")
        const g = panel.sections.find(sec => sec.hiddenFrom >= 0)
        const hub = page.signalHub
        verify(g.rows.slice(0, g.hiddenFrom).every(r => hub.lookup([r.ref]).hidden !== true), "sent ones first")
        verify(g.rows.slice(g.hiddenFrom).every(r => hub.lookup([r.ref]).hidden === true), "the rest last")
        compare(find(page, "pickSignal_" + g.rows[g.rows.length - 1].ref).opacity, 0.5, "and dim")
    }

    // Add's every signal of a set: the listed file's own signals the page
    // doesn't show, in Add's kind, as one undo step; then only what is
    // left, as many as the page has room for
    function threeSignalFile() {
        const sig = (key, did) => ({ key: key, name: key.toUpperCase(), unit: "V",
                                     read: { service: "22", number: did },
                                     field: { byte: 0, bits: 8 } })
        const file = { havne_signals: 1, name: "Three", source: { name: "t" },
                       ecus: [{ id: "pcm", rx: "7E8" }],
                       signals: [sig("a", "D100"), sig("b", "D101"), sig("c", "D102")] }
        const r = keptFile(JSON.stringify(file))
        verify(r.ok, "the file imports")
        return r.profile
    }
    function test_addAllAddsTheSetsSignalsThePageLacksAsOneStep() {
        const profile = threeSignalFile()
        const b = { kind: "digital", w: 2, h: 4, label: "B", refs: ["key:b"], unit: "V", options: {} }
        const page = storedPageOf({}, docOf([cellOf("A"), b]))
        page.offlineUri = "mock://?profile=" + profile
        page.profileNames = { [profile]: "Three" }
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_bar")
        const all = find(page, "addAllSignals")
        tryVerify(() => all.visible, 3000, "offered for a set of its own signals")
        compare(all.text, "Add 2 more of the 3 signals of Three", "B is on the page already")
        click(page, "addAllSignals")
        compare(page.panel, "", "the list closes")
        compare(labels(page), ["A", "B", "A", "C"], "the set's A and C, at the end")
        const added = page.doc.pages[0].cells.slice(2)
        compare(added.map(c => c.kind), ["bar", "bar"], "in Add's kind")
        compare(added.map(c => JSON.stringify(c.refs)), ['["key:a"]', '["key:c"]'])
        compare(added[0].unit, "V")
        compare(find(page, "dashUndo").Accessible.name, "Undo: added 2 signals of Three")
        click(page, "dashUndo")
        compare(labels(page), ["A", "B"], "one undo takes both")

        // Every signal on the page: no button
        click(page, "dashRedo")
        compare(labels(page), ["A", "B", "A", "C"])
        click(page, "dashAdd")
        click(page, "addKind_digital")
        find(page, "dashSignalPanel")
        verify(!find(page, "addAllSignals").visible, "every signal is on the page")
    }
    function test_addAllStopsAtThePagesRoom() {
        const profile = threeSignalFile()
        const cells = []
        for (let i = 0; i < DashboardBook.maxCells - 2; ++i)
            cells.push(cellOf("N" + i))
        const page = pageOf({}, docOf(cells))
        page.offlineUri = "mock://?profile=" + profile
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_digital")
        const all = find(page, "addAllSignals")
        tryVerify(() => all.visible, 3000, "offered")
        compare(all.text, "Add 2 of the 3 missing signals of " + profile + ": a page holds "
                + DashboardBook.maxCells + " cells",
                "two places left, said so; a set with no name known reads as core names it")
        click(page, "addAllSignals")
        compare(page.doc.pages[0].cells.length, DashboardBook.maxCells, "the page is full")
        compare(labels(page).slice(-2), ["A", "B"], "the set's first two")
    }
    // One signal left: the words and the undo step say it in the singular
    function test_addAllNamesOneSignalInTheSingular() {
        const profile = threeSignalFile()
        const own = key => ({ kind: "digital", w: 2, h: 4, label: key.toUpperCase(), refs: ["key:" + key],
                              unit: "V", options: {} })
        const page = storedPageOf({}, docOf([own("b"), own("c")]))
        page.offlineUri = "mock://?profile=" + profile
        page.profileNames = { [profile]: "Three" }
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_digital")
        const all = find(page, "addAllSignals")
        tryVerify(() => all.visible, 3000, "offered")
        compare(all.text, "Add 1 more of the 3 signals of Three")
        click(page, "addAllSignals")
        compare(labels(page), ["B", "C", "A"])
        compare(find(page, "dashUndo").Accessible.name, "Undo: added 1 signal of Three")
    }
    // A set of the built-in IDs alone has no signal of its own to add
    function test_addAllIsNotOfferedForABuiltInSet() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.offlineUri = "mock://?profile=obd2"
        page.startEditing()
        click(page, "dashAdd")
        click(page, "addKind_digital")
        tryVerify(() => findChild(page, "pickSignal_" + Bridge.gaugeSignals["speed"][1]) !== null,
                  3000, "obd2's rows are listed")
        verify(!find(page, "addAllSignals").visible)
    }

    // A battery cell made on either built-in set refers to both. The IDs
    // are signal_sets.h's, not Bridge.gaugeSignals', so a wrong one there
    // fails
    function test_aBatteryCellReadsOnEitherSet_data() {
        return [{ tag: "obd2", uri: "mock://?profile=obd2", refs: ["id:7008", "id:14001"] },
                { tag: "mock", uri: "mock://demo", refs: ["id:14001", "id:7008"] }]
    }
    function test_aBatteryCellReadsOnEitherSet(data) {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        page.addFlowPart.pickKind("digital")
        page.offlineUri = data.uri
        const panel = find(page, "dashSignalPanel")
        const findField = find(panel, "filterText")
        findField.text = "battery"
        findField.textEdited()
        tryVerify(() => findChild(panel, "pickSignal_" + data.refs[0]) !== null, 3000,
                  "the set's battery row, offered")
        click(page, "pickSignal_" + data.refs[0])
        const made = page.doc.pages[0].cells[1]
        compare(made.label, "Battery Voltage")
        compare(made.unit, "V")
        compare(JSON.stringify(made.refs), JSON.stringify(data.refs), "with its counterpart")
    }

    // Change signal: the bar narrows the rows; the cell takes the row's
    // name, decoded unit, default range and counterpart's ref, its kind,
    // size and peak hold stay, and its zones, scale, words and decimals
    // go with the old signal
    function test_anotherSignalDropsTheZones() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const zoned = cellOf("A", "analog")
        zoned.options.coldBelow = 500
        zoned.options.warnFrom = 6000
        zoned.options.dangerFrom = 7000
        zoned.options.decimals = 1
        zoned.options.majorStep = 1000
        zoned.options.minorStep = 250
        zoned.options.labelDivisor = 1000
        zoned.options.caption = "x1000 r/min"
        zoned.options.description = "Engine"
        zoned.options.peakLabel = "max"
        zoned.options.peak = true
        const page = pageOf({}, docOf([zoned]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelSignal")
        compare(page.panel, "signal")
        const coolant = Bridge.gaugeSignals["coolant"]
        const speed = Bridge.gaugeSignals["speed"][0]
        // The bar narrows the rows; a find with no match says so
        const panel = find(page, "dashSignalPanel")
        const findField = find(panel, "filterText")
        verify(!find(panel, "filterButton").visible, "no Filter button: its options are a Popup")
        verify(find(panel, "pickSignal_" + speed).visible)
        findField.text = "coolant"
        findField.textEdited()
        tryVerify(() => !findChild(panel, "pickSignal_" + speed), 3000, "speed filtered out")
        verify(find(panel, "pickSignal_" + coolant[0]).visible)
        verify(!find(panel, "signalNone").visible)
        findField.text = "no such signal"
        findField.textEdited()
        tryVerify(() => find(panel, "signalNone").visible, 3000, "nothing matches")
        compare(find(panel, "signalNone").text, "No signal matches.")
        findField.text = "coolant"
        findField.textEdited()
        click(page, "pickSignal_" + coolant[0])
        const c = page.doc.pages[0].cells[0]
        compare(c.kind, "analog")
        compare(c.w, 2)
        compare(c.label, "Coolant Temp")
        compare(c.unit, "°C")
        compare(JSON.stringify(c.refs), JSON.stringify(coolant))
        compare(c.options.from, 0)
        compare(c.options.to, 150)
        for (const gone of ["decimals", "majorStep", "minorStep", "labelDivisor", "caption",
                            "description", "peakLabel"])
            verify(c.options[gone] === undefined, gone + " goes with the old signal")
        compare(c.options.peak, true, "peak hold stays")
        compare([c.options.coldBelow, c.options.warnFrom, c.options.dangerFrom],
                [60, 105, 115], "the old zones are replaced by the new signal's")
        compare(page.panel, "cell")
        compare(page.selected, 0)
        compare(storedText(page), DashboardBook.serialize(page.doc))
    }

    // The cell's panel: each field writes its option, an emptied zone
    // goes, a refused write (from past to) stores nothing
    function test_theCellsPanelSetsItsOptions() {
        const page = pageOf({}, docOf([cellOf("A", "analog"), cellOf("D", "digital"),
                                       cellOf("S", "shift-light")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const at = () => page.doc.pages[0].cells[0]
        type(find(page, "panelLabel"), "Tach")
        compare(at().label, "Tach")
        type(find(page, "panelFrom"), "500")
        compare(at().options.from, 500)
        type(find(page, "panelDangerFrom"), "6500")
        compare(at().options.dangerFrom, 6500)
        type(find(page, "panelDangerFrom"), "")
        verify(at().options.dangerFrom === undefined, "emptied, removed")
        type(find(page, "panelFrom"), "")
        compare(at().options.from, 500, "an empty end puts the end back")
        compare(find(page, "panelFrom").text, "500")
        spin(find(page, "panelDecimals"), 2)
        compare(at().options.decimals, 2)
        const peak = find(page, "panelPeak")
        peak.toggle()
        peak.toggled()
        compare(at().options.peak, true)
        type(find(page, "panelMajorStep"), "1000")
        compare(at().options.majorStep, 1000)
        type(find(page, "panelCaption"), "x1000")
        compare(at().options.caption, "x1000")
        verify(!find(page, "panelDescription").visible, "a digital tile's own")

        const before = storedText(page)
        type(find(page, "panelFrom"), "9000")
        compare(at().options.from, 500, "refused: from past to")
        compare(storedText(page), before, "nothing stored")
        compare(find(page, "panelFrom").text, "500", "the field shows what's kept")
        type(find(page, "panelFrom"), "abc")
        compare(storedText(page), before)

        press(find(page, "dashCell_0_1"))
        verify(find(page, "panelDescription").visible)
        verify(!find(page, "panelMajorStep").visible)
        type(find(page, "panelDescription"), "Gear")
        compare(page.doc.pages[0].cells[1].options.description, "Gear")
        press(find(page, "dashCell_0_2"))
        spin(find(page, "panelCount"), 10)
        compare(page.doc.pages[0].cells[2].options.count, 10)
        compare(storedText(page), DashboardBook.serialize(page.doc))
    }
}
