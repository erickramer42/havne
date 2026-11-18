// File/layer: tests/qml/tst_dashedit_color.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_color (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5), a gauge's color and the page names:
// each control's write and what's stored
TestCase {
    id: testCase
    name: "DashboardEditColor"
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
    // A scratch file a layout is written to and read back from
    function layoutUrl(name) {
        return Harness.writeScratchFile(name, "")
    }

    // ---- A gauge's color

    function relativeLuminance(c) {
        const lin = v => v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
    }
    function contrast(a, b) {
        const x = relativeLuminance(a), y = relativeLuminance(b)
        return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
    }
    // CIE L*a*b* distance (the 1976 formula): how far apart two colors look
    function lab(c) {
        const lin = v => v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        const r = lin(c.r), g = lin(c.g), b = lin(c.b)
        const x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047
        const y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        const z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883
        const f = t => t > 0.008856 ? Math.cbrt(t) : 7.787 * t + 16 / 116
        return [116 * f(y) - 16, 500 * (f(x) - f(y)), 200 * (f(y) - f(z))]
    }
    function apart(a, b) {
        const p = lab(a), q = lab(b)
        return Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2])
    }

    // The original seven keep ADR-0057's rule against the zones; the five
    // standard colors sit near them on purpose (readable only)
    readonly property var originalColors: ["teal", "green", "lime", "cyan", "pink", "magenta", "stone"]
    readonly property var standardColors: ["red", "blue", "yellow", "truegreen", "lightblue"]

    function test_everyNamedColorIsReadableAndTheOriginalsAreApartFromTheZones() {
        for (const look of ["dark", "daylight"]) {
            const p = Theme.palettes[look]
            const surface = Qt.color(p.surface)
            const zones = [p.gaugeWarn, p.gaugeDanger, p.info, p.gaugeNeedle].map(c => Qt.color(c))
            for (const name of Theme.gaugeColorNames) {
                const c = Qt.color(Theme.gaugePalettes[look][name])
                verify(contrast(c, surface) >= 3, name + " on " + look + ": " + contrast(c, surface))
                if (originalColors.indexOf(name) >= 0)
                    for (const z of zones)
                        verify(apart(c, z) >= 25, name + " on " + look + " is " + apart(c, z) + " from a zone color")
            }
        }
        compare(Theme.gaugeColorNames.slice(0, 7), originalColors, "the original seven keep their names and order")
        compare(Theme.gaugeColorNames.slice(7), standardColors)
        compare(Theme.gaugeColorNames.length, 12)
        for (const name of Object.keys(Theme.gaugePalettes.dark))
            verify(Theme.gaugeColorNames.indexOf(name) >= 0, name + " is listed")
    }

    function test_theStandardColorsResolveInBothLooksAndNightHasNone() {
        const expected = { red: ["#ff601f", "#fc5d1c"], blue: ["#34cafb", "#1f9ec6"], yellow: ["#e5fd15", "#946c00"],
                           truegreen: ["#39ff14", "#38a721"], lightblue: ["#60a5fa", "#2458b8"],
                           green: ["#4ade80", "#1e7d3f"] }
        const labels = { red: "Red", blue: "Blue", yellow: "Yellow", truegreen: "Green", lightblue: "Light blue",
                         green: "Light green" }
        Theme.night = "off"
        for (const name of Object.keys(expected)) {
            Theme.look = "dark"
            compare(Theme.gaugeColor(name), expected[name][0], name + " in Dark")
            Theme.look = "daylight"
            compare(Theme.gaugeColor(name), expected[name][1], name + " in Daylight")
            compare(Theme.gaugeColorLabel(name), labels[name])
        }
        Theme.night = "on"
        for (const name of Object.keys(expected))
            compare(Theme.gaugeColor(name), "", name + " at Night")
        Theme.night = "off"
        Theme.look = "dark"
    }

    function test_aCustomColorIsOnlyAWellFormedLowercaseHex() {
        Theme.night = "off"
        for (const look of ["dark", "daylight"]) {
            Theme.look = look
            compare(Theme.gaugeColor("#12ab9f"), "#12ab9f", look)
            compare(Theme.gaugeSwatch("#12ab9f"), "#12ab9f")
            for (const bad of ["#12345", "#1234567", "red ", "#ABCDEF", "12ab9f", "#12ab9g", " #12ab9f", "#12ab9f ", 256, null])
                compare(Theme.gaugeColor(bad), "", String(bad))
        }
        Theme.night = "on"
        compare(Theme.gaugeColor("#12ab9f"), "", "Night has none")
        compare(Theme.gaugeSwatch("#12ab9f"), "#12ab9f", "the picker still shows it")
        Theme.night = "off"
        Theme.look = "dark"
    }

    function test_thePickerOffersEveryNamedColorAndACustomEntry() {
        const page = storedPageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        for (const name of Theme.gaugeColorNames)
            verify(find(page, "panelColor_" + name), name + " is in the picker")
        verify(find(page, "panelColor_custom"))
        verify(!find(page, "panelColorCustom").visible, "the fields wait for Custom")
        click(page, "panelColor_truegreen")
        compare(page.doc.pages[0].cells[0].options.color, "truegreen")
    }

    function test_theCustomFieldsMakeAHexAndRefuseWhatIsNotAColor() {
        Theme.night = "off"
        Theme.look = "dark"
        const page = storedPageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        verify(find(page, "panelColorCustom").visible)
        const r = find(page, "panelColorR"), g = find(page, "panelColorG"), b = find(page, "panelColorB")
        type(r, "18"); type(g, "171"); type(b, "159")
        compare(page.doc.pages[0].cells[0].options.color, "#12ab9f")
        verify(find(page, "panelColor_custom").checked)
        verify(!find(page, "panelColor_default").checked)
        compare(find(page, "panelColorPreview").color.toString(), "#12ab9f")
        const why = find(page, "panelColorWhy")
        verify(!why.visible)
        for (const bad of ["256", "", "-1", "1.5", "ab", "1e2"]) {
            type(g, bad)
            compare(page.doc.pages[0].cells[0].options.color, "#12ab9f", "[" + bad + "] is refused")
            compare(g.text, bad, "[" + bad + "] stays typed")
            verify(why.visible, "[" + bad + "] says why")
            verify(why.text.indexOf("0 to 255") >= 0, why.text)
        }
        type(g, "171")
        verify(!why.visible, "a value in range takes the words away")
        compare(g.text, "171")
        type(r, "0"); type(g, "5"); type(b, "255")
        compare(page.doc.pages[0].cells[0].options.color, "#0005ff", "a channel under 16 keeps its zero")
        click(page, "panelColor_pink")
        compare(page.doc.pages[0].cells[0].options.color, "pink", "a named pick takes the place")
        verify(!find(page, "panelColorCustom").visible)
    }

    function test_aCustomColorHardToReadSaysSoAndIsStillStored() {
        Theme.night = "off"
        Theme.look = "dark"
        const page = storedPageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        const note = find(page, "panelColorHard")
        const rgb = (r, g, b) => { type(find(page, "panelColorR"), r); type(find(page, "panelColorG"), g); type(find(page, "panelColorB"), b) }
        rgb("128", "128", "128")
        verify(!note.visible, "a mid gray reads on both looks")
        // Both looks are judged, whichever one is shown, and the note names the one
        rgb("255", "255", "255")
        compare(page.doc.pages[0].cells[0].options.color, "#ffffff", "stored, not refused")
        verify(note.visible, "white shown on Dark is hard to read on Light")
        compare(note.text, "Hard to read in Light")
        rgb("20", "20", "20")
        compare(page.doc.pages[0].cells[0].options.color, "#141414")
        verify(note.visible, "near-black is hard to read on Dark")
        compare(note.text, "Hard to read in Dark")
        Theme.look = "daylight"
        verify(note.visible, "and says so under Daylight too")
        compare(note.text, "Hard to read in Dark")
        Theme.look = "dark"
    }

    // Qt sends editingFinished when a field loses focus with its text unchanged
    // (type() calls it by hand and can't see this): a pass-through stores nothing
    function test_tabbingThroughTheCustomFieldsStoresNothing() {
        Theme.night = "off"
        Theme.look = "dark"
        const page = storedPageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        const r = find(page, "panelColorR"), g = find(page, "panelColorG"), b = find(page, "panelColorB")
        r.forceActiveFocus()
        verify(r.activeFocus)
        g.forceActiveFocus()
        verify(g.activeFocus)
        b.forceActiveFocus()
        verify(b.activeFocus)
        find(page, "panelColor_custom").forceActiveFocus()
        // The window may not be active: the same pass-through by hand, with no textEdited
        r.editingFinished(); g.editingFinished(); b.editingFinished()
        compare(page.doc.pages[0].cells[0].options.color, undefined, "nothing stored by passing through")
        compare(find(page, "dashUndo").Accessible.name.indexOf("changed"), -1, "no undo step either")
        // A changed value stored, then passed through again, adds no second step
        type(r, "10"); type(g, "20"); type(b, "30")
        compare(page.doc.pages[0].cells[0].options.color, "#0a141e")
        r.forceActiveFocus(); g.forceActiveFocus(); b.forceActiveFocus()
        compare(page.doc.pages[0].cells[0].options.color, "#0a141e")
    }

    // Custom on a named pick, its own three numbers typed: the user keeps the color
    // as a custom one (a named pick's value changes with the look, a custom one doesn't)
    function test_aNamedColorsOwnNumbersTypedAreStoredAsACustomColor() {
        Theme.night = "off"
        Theme.look = "dark"
        const cell = cellOf("A", "analog")
        cell.options.color = "teal"
        const page = storedPageOf({}, docOf([cell]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        const r = find(page, "panelColorR"), g = find(page, "panelColorG"), b = find(page, "panelColorB")
        compare([r.text, g.text, b.text], ["63", "209", "192"], "the fields start at Dark's teal")
        r.forceActiveFocus(); g.forceActiveFocus(); b.forceActiveFocus()
        find(page, "panelColor_custom").forceActiveFocus()
        r.editingFinished(); g.editingFinished(); b.editingFinished()
        compare(page.doc.pages[0].cells[0].options.color, "teal", "a pass-through stores nothing")
        type(r, "63"); type(g, "209"); type(b, "192")
        compare(page.doc.pages[0].cells[0].options.color, "#3fd1c0", "the same numbers, typed, are stored")
        Theme.look = "daylight"
        compare(Theme.gaugeColor(page.doc.pages[0].cells[0].options.color), "#3fd1c0", "and stay as they are in Daylight")
        Theme.look = "dark"
    }

    function test_aValueLeftTypedIsGoneWhenAnotherGaugeIsShown() {
        Theme.night = "off"
        Theme.look = "dark"
        const page = storedPageOf({}, docOf([cellOf("A", "analog"), cellOf("B", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        type(find(page, "panelColorG"), "256")
        compare(find(page, "panelColorG").text, "256")
        press(find(page, "dashCell_0_1"))
        click(page, "panelColor_custom")
        verify(find(page, "panelColorG").text !== "256", "B's fields show B's color, not A's leftover")
        verify(!find(page, "panelColorWhy").visible)
    }

    function test_theCustomRowIsClosedForAnotherGauge() {
        Theme.night = "off"
        Theme.look = "dark"
        const page = storedPageOf({}, docOf([cellOf("A", "analog"), cellOf("B", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        verify(find(page, "panelColorCustom").visible)
        press(find(page, "dashCell_0_1"))
        verify(!find(page, "panelColorCustom").visible, "B has no custom color and shows none")
        verify(!find(page, "panelColor_custom").checked)
        press(find(page, "dashCell_0_0"))
        verify(!find(page, "panelColorCustom").visible, "A is closed again too")
    }

    function test_theCustomButtonStaysCheckedWithItsRowShown() {
        Theme.night = "off"
        const cell = cellOf("A", "analog")
        cell.options.color = "#12ab9f"
        const page = storedPageOf({}, docOf([cell]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        verify(find(page, "panelColorCustom").visible)
        click(page, "panelColor_custom")
        verify(find(page, "panelColorCustom").visible, "a stored custom color keeps its row")
        verify(find(page, "panelColor_custom").checked, "and the button says so")
    }

    function test_theContrastNoteJudgesAgainstDarkAtNight() {
        const gray = Qt.color("#5a5a62")
        Theme.look = "daylight"
        Theme.night = "on"
        const against = Qt.color(Theme.palettes.dark.surface)
        verify(Math.abs(Theme.gaugeContrast("#5a5a62") - contrast(gray, against)) < 0.001,
               "Night's color isn't drawn, so the note uses Dark's surface: " + Theme.gaugeContrast("#5a5a62"))
        Theme.night = "off"
        verify(Math.abs(Theme.gaugeContrast("#5a5a62") - contrast(gray, Qt.color(Theme.palettes.daylight.surface))) < 0.001)
        Theme.look = "dark"
    }

    function test_theHardToReadNoteHasARowOfItsOwnOnAPhone() {
        Theme.night = "off"
        Theme.look = "dark"
        const page = storedPageOf({ form: "phone", width: 384, height: 832 }, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        type(find(page, "panelColorR"), "20"); type(find(page, "panelColorG"), "20"); type(find(page, "panelColorB"), "20")
        const note = find(page, "panelColorHard")
        verify(note.visible)
        const row = find(page, "panelColorCustom")
        const panel = find(page, "dashCellPanel")
        const at = row.mapToItem(panel, 0, 0)
        verify(at.x >= -0.5 && at.x + row.width <= panel.width + 0.5, "the fields fit the card: " + at.x + " + " + row.width + " of " + panel.width)
        verify(note.width > 100, "the note is not a sliver: " + note.width)
        tryVerify(() => note.mapToItem(panel, 0, 0).y >= row.mapToItem(panel, 0, 0).y + row.height - 1, 3000,
                  "the note sits under the fields")
    }

    function test_theWhyLineHasARowOfItsOwnOnAPhone() {
        Theme.night = "off"
        Theme.look = "dark"
        const page = storedPageOf({ form: "phone", width: 384, height: 832 }, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelColor_custom")
        type(find(page, "panelColorG"), "256")
        const why = find(page, "panelColorWhy")
        verify(why.visible)
        const row = find(page, "panelColorCustom")
        const panel = find(page, "dashCellPanel")
        const at = row.mapToItem(panel, 0, 0)
        verify(at.x >= -0.5 && at.x + row.width <= panel.width + 0.5, "the fields fit the card: " + row.width + " of " + panel.width)
        tryVerify(() => why.width > 100 && why.mapToItem(panel, 0, 0).y >= row.mapToItem(panel, 0, 0).y + row.height - 1,
                  3000, "the words are no sliver and sit under the fields: " + why.width)
        verify(why.mapToItem(panel, 0, 0).x + why.width <= panel.width + 0.5, "inside the card")
    }

    function test_aCustomColorRoundTripsThroughAFileAndNightDrawsNoneOfIt() {
        const cell = cellOf("A", "digital")
        cell.options.color = "#12ab9f"
        const url = layoutUrl("layout-custom-color.json")
        let page = storedPageOf({}, docOf([cell]))
        verify(page.layoutFilePart.exportLayoutTo(url), page.layoutFilePart.layoutNote)
        page.destroy()
        page = storedPageOf({}, docOf([cellOf("X")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        compare(page.doc.pages[0].cells[0].options.color, "#12ab9f")
        page.destroy()
        const g = colored("digital", "dark", "#12ab9f")
        verify(Qt.colorEqual(find(g, "readout").color, "#12ab9f"), "drawn")
        Theme.night = "on"
        verify(Qt.colorEqual(find(g, "readout").color, Theme.gaugeReadout), "Night draws its own")
        Theme.night = "off"
    }

    function test_aStoredNameIsOnlyAFixedOne() {
        for (const name of ["constructor", "toString", "__proto__", "hasOwnProperty", "", "Teal", 5, null])
            compare(Theme.gaugeColor(name), "", String(name))
        compare(Theme.gaugeColor("teal"), Theme.gaugePalettes.dark.teal)
        Theme.look = "daylight"
        compare(Theme.gaugeColor("teal"), Theme.gaugePalettes.daylight.teal)
        Theme.night = "on"
        compare(Theme.gaugeColor("teal"), "", "Night has none")
        compare(Theme.gaugeSwatch("constructor"), "transparent")
    }

    function colored(kind, look, color, extra) {
        Theme.night = "off"
        Theme.look = look
        const cell = cellOf("A", kind)
        cell.options.color = color
        Object.assign(cell.options, extra || {})
        const page = pageOf({}, docOf([cell]))
        const gauge = find(find(page, "dashCell_0_0"), "gauge")
        tryVerify(() => gauge.item !== null, 5000, kind + " loads")
        const g = gauge.item
        // A value in no zone, set on the drawn gauge
        g.hasValue = true
        g.value = 10
        return g
    }

    // What each kind draws in the picked color, read off the drawn items:
    // the readout's text (and the hub pin, the number) for a dial, the
    // trace's stroke for a sparkline
    function drawnColors(kind, g) {
        const found = {}
        if (kind === "analog") {
            found.hubPin = find(g, "hubPin").color
            found.readout = find(g, "readout").color
        } else if (kind === "sparkline") {
            const paths = find(g, "trace").data.filter(d => d.strokeWidth === 1.5)
            verify(paths.length === 1, "the trace's path")
            found.trace = paths[0].strokeColor
            found.readout = find(g, "readout").color
        } else if (kind === "seven-segment") {
            found.digits = g.digitColor
        } else {
            found.readout = find(g, "readout").color
        }
        return found
    }

    function test_aPickedColorPaintsTheValueMarks_data() {
        const rows = []
        for (const look of ["dark", "daylight"])
            for (const kind of ["analog", "ring", "bar", "digital", "segments", "seven-segment", "sparkline"])
                rows.push({ tag: kind + " " + look, kind: kind, look: look })
        return rows
    }
    function test_aPickedColorPaintsTheValueMarks(data) {
        const g = colored(data.kind, data.look, "teal")
        const teal = Theme.gaugePalettes[data.look].teal
        const plain = colored(data.kind, data.look, "")
        const wasPlain = drawnColors(data.kind, plain)
        const drawn = drawnColors(data.kind, g)
        for (const part in drawn) {
            verify(Qt.colorEqual(drawn[part], teal), data.kind + "'s " + part + " is " + drawn[part])
            verify(!Qt.colorEqual(wasPlain[part], teal), "and the default's " + part + " is not")
        }
        Theme.look = "dark"
    }

    function test_aZoneBeatsThePickedColor() {
        const g = colored("analog", "dark", "teal", { warnFrom: 6000, dangerFrom: 7000 })
        g.value = 6500
        tryVerify(() => Qt.colorEqual(find(g, "readout").color, Theme.gaugeWarn), 3000, "warn")
        g.value = 7500
        tryVerify(() => Qt.colorEqual(find(g, "readout").color, Theme.gaugeDanger), 3000, "danger")
        g.value = 10
        tryVerify(() => Qt.colorEqual(find(g, "readout").color, Theme.gaugePalettes.dark.teal), 3000,
                  "and back to the pick")
    }

    function test_aLowZoneBeatsThePickedColor_data() {
        return ["analog", "bar", "digital", "ring"].map(k => ({ tag: k, kind: k }))
    }
    function test_aLowZoneBeatsThePickedColor(data) {
        const g = colored(data.kind, "dark", "teal", { warnBelow: 600, dangerBelow: 300 })
        g.value = 500
        tryVerify(() => Qt.colorEqual(find(g, "readout").color, Theme.gaugeWarn), 3000, "warn")
        g.value = 200
        tryVerify(() => Qt.colorEqual(find(g, "readout").color, Theme.gaugeDanger), 3000, "danger")
        g.value = 1000
        tryVerify(() => Qt.colorEqual(find(g, "readout").color, Theme.gaugePalettes.dark.teal), 3000,
                  "and back to the pick")
    }

    function test_nightIgnoresThePickedColor() {
        const g = colored("analog", "dark", "teal")
        Theme.night = "on"
        tryVerify(() => Qt.colorEqual(find(g, "readout").color, Theme.gaugeReadout), 3000, "Night's own tint")
        verify(Qt.colorEqual(find(g, "hubPin").color, Theme.gaugeNeedle))
    }

    function test_anUnknownColorNameDrawsAsDefault() {
        const g = colored("digital", "dark", "chartreuse9")
        verify(Qt.colorEqual(find(g, "readout").color, Theme.gaugeReadout))
        const odd = colored("digital", "dark", "constructor")
        verify(Qt.colorEqual(find(odd, "readout").color, Theme.gaugeReadout), "an inherited name too")
    }

    function test_thePanelPicksAColorOneUndoStepAndChangeKindKeepsIt() {
        const page = storedPageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        verify(find(page, "panelColor_default").checked)
        click(page, "panelColor_teal")
        compare(page.doc.pages[0].cells[0].options.color, "teal")
        verify(find(page, "panelColor_teal").checked)
        verify(!find(page, "panelColor_default").checked)
        compare(find(page, "dashUndo").Accessible.name, "Undo: changed A's color")
        click(page, "panelKind")
        click(page, "addKind_bar")
        compare(page.doc.pages[0].cells[0].options.color, "teal", "kept across Change kind")
        click(page, "dashUndo")
        click(page, "dashUndo")
        compare(page.doc.pages[0].cells[0].options.color, undefined, "undo restores")
        click(page, "panelColor_pink")
        click(page, "panelColor_default")
        compare(page.doc.pages[0].cells[0].options.color, undefined, "Default removes it")
        verify(find(page, "panelColor_default").checked)
        click(page, "panelColor_default")
        verify(find(page, "panelColor_default").checked, "a second tap on Default leaves it chosen")
        compare(page.doc.pages[0].cells[0].options.color, undefined)
        click(page, "panelColor_pink")
        click(page, "panelColor_pink")
        verify(find(page, "panelColor_pink").checked, "a second tap on a color leaves it chosen")
    }

    function test_theColorRowReadsTheStoredNameAtNightAndForAnUnknownOne() {
        const own = cellOf("A", "analog")
        own.options.color = "teal"
        const page = pageOf({}, docOf([own, cellOf("B")]))
        page.startEditing()
        Theme.night = "on"
        press(find(page, "dashCell_0_0"))
        verify(find(page, "panelColor_teal").checked, "the stored color is marked at Night")
        verify(!find(page, "panelColor_default").checked)
        Theme.night = "off"
        const later = pageOf({}, docOf([Object.assign(cellOf("C"), { options: { from: 0, to: 100, color: "chartreuse9" } })]))
        later.startEditing()
        press(find(later, "dashCell_0_0"))
        verify(find(later, "panelColor_default").checked, "an unknown name reads as Default")
    }

    function test_atNightTheColorRowDimsAndSaysWhy() {
        const own = cellOf("A", "analog")
        own.options.color = "teal"
        const page = pageOf({}, docOf([own]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const row = find(page, "panelColors")
        const note = find(page, "panelColorNight")
        compare(row.opacity, 1, "not dimmed by day")
        verify(!note.visible, "and no note")
        Theme.night = "on"
        tryCompare(row, "opacity", 0.45, 3000, "dimmed at Night")
        verify(note.visible, "the note shows")
        compare(note.text, "Night draws every gauge in its own tint")
        verify(find(page, "panelColor_teal").checked, "the stored name stays marked")
        click(page, "panelColor_pink")
        compare(page.doc.pages[0].cells[0].options.color, "pink", "and the row stays choosable")
        Theme.night = "off"
        tryCompare(row, "opacity", 1, 3000)
        verify(!note.visible)
    }

    // A zone's color wins over the pick, and the panel says so
    function test_aZonedCellWithAPickedColorSaysTheZoneWins() {
        const own = cellOf("A", "analog")
        own.options.color = "teal"
        const page = pageOf({}, docOf([own]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const note = find(page, "panelColorZones")
        verify(!note.visible, "a color and no zone: no note")
        page.editsPart.setOptions({ warnFrom: 90 })
        verify(note.visible, "a color and a zone level")
        compare(note.text, "A zone's color replaces this one while its level is reached")
        click(page, "panelColor_default")
        verify(!note.visible, "no picked color: no note")
        click(page, "panelColor_teal")
        verify(note.visible)
        Theme.night = "on"
        verify(!note.visible, "Night draws no pick, and has its own note")
        Theme.night = "off"
    }

    // Load replaces every page, and its button says so
    function test_loadLayoutSaysItReplacesEveryPage() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        page.moreShown = true
        const load = find(page, "moreLoad")
        compare(load.text, "Load layout (all pages)")
        compare(load.Accessible.name, "Load a layout from a file: it replaces every page")
    }

    // The "+" tab says what it adds, and a new page asks for its name at once
    function test_aNewPageAsksForItsNameAndTheAddTabSaysPage() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        compare(find(page, "dashPageAdd").text, "+ Page")
        click(page, "dashPageAdd")
        click(page, "preset_blank")
        const field = find(page, "moreRename")
        tryVerify(() => field.activeFocus, 3000, "the name field has the focus")
        compare(field.selectedText, "Page 2", "and its text is selected")
        keyClick("T")
        keyClick("o")
        keyClick(Qt.Key_Return)
        compare(page.doc.pages[1].name, "To", "typing replaced the default name")
        // A preset page asks too
        page.moreShown = false
        click(page, "dashPageAdd")
        click(page, "preset_standard")
        tryVerify(() => field.activeFocus, 3000, "a preset's page, its name field focused")
        verify(field.selectedText !== "" && field.selectedText === page.doc.pages[2].name, "and its name selected")
    }

    // A double click on a tab renames it in place; Escape and an empty name keep the old one
    function test_aDoubleClickRenamesATabInPlace() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        click(page, "dashPageAdd")
        click(page, "preset_blank")
        keyClick(Qt.Key_Escape)
        const tab0 = find(page, "dashPageTab_0")
        const field = find(page, "dashPageRename_0")
        verify(!field.visible, "no field before the double click")
        tryVerify(() => waitForPolish(tab0.Window.window), 3000)
        mouseDoubleClickSequence(tab0)
        tryVerify(() => field.visible && field.activeFocus, 3000, "the tab's own field")
        compare(field.selectedText, "Test")
        compare(page.pageIndex, 0, "the page is the tab's")
        keyClick("Z")
        keyClick(Qt.Key_Return)
        compare(page.doc.pages[0].name, "Z")
        verify(!field.visible, "the field is gone")
        // Escape keeps the old name
        mouseDoubleClickSequence(tab0)
        tryVerify(() => field.visible && field.activeFocus, 3000)
        keyClick("Q")
        keyClick(Qt.Key_Escape)
        compare(page.doc.pages[0].name, "Z")
        verify(!field.visible)
        // An empty name is refused by the page's rule
        mouseDoubleClickSequence(tab0)
        tryVerify(() => field.visible && field.activeFocus, 3000)
        keyClick(Qt.Key_Backspace)
        keyClick(Qt.Key_Return)
        compare(page.doc.pages[0].name, "Z")
        // Outside edit mode a double click renames nothing
        page.stopEditing()
        tryVerify(() => waitForPolish(tab0.Window.window), 3000)
        mouseDoubleClickSequence(tab0)
        wait(50)
        verify(!field.visible, "no rename field outside edit mode")
    }

    // A name left as it was makes no undo step; a rename in place gives way to Done and leaves room
    function test_aRenameThatChangesNothingIsNoUndoStep() {
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        const depth = page.historyPart.undoStack.length
        click(page, "dashMore")
        const name = find(page, "moreRename")
        name.forceActiveFocus()
        name.editingFinished()
        compare(page.historyPart.undoStack.length, depth, "the Page row's field")
        const tab0 = find(page, "dashPageTab_0")
        const field = find(page, "dashPageRename_0")
        tryVerify(() => waitForPolish(tab0.Window.window), 3000)
        mouseDoubleClickSequence(tab0)
        tryVerify(() => field.visible && field.activeFocus, 3000)
        verify(tab0.width >= 120, "a short name has room: " + tab0.width)
        keyClick(Qt.Key_Return)
        compare(page.historyPart.undoStack.length, depth, "the tab's field")
    }

    function test_leavingEditModeDropsAnOpenRename() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        const tab0 = find(page, "dashPageTab_0")
        tryVerify(() => waitForPolish(tab0.Window.window), 3000)
        mouseDoubleClickSequence(tab0)
        const field = find(page, "dashPageRename_0")
        tryVerify(() => field.visible, 3000)
        page.stopEditing()
        compare(find(page, "dashPageStrip").renaming, -1)
        page.startEditing()
        verify(!field.visible, "editing again shows no field")
    }

    function test_aBlankCellHasNoColorRow() {
        const page = pageOf({}, docOf([{ kind: "blank", w: 2, h: 4, refs: [] }]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        verify(!find(page, "panelColors").visible)
    }

    function test_shiftLightsHaveNoColorRow() {
        const page = pageOf({}, docOf([cellOf("A", "shift-light"), cellOf("B")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        verify(!find(page, "panelColors").visible, "their own green, amber and red")
        press(find(page, "dashCell_0_1"))
        verify(find(page, "panelColors").visible)
    }

    // The cell's panel on a phone: the Change signal and Change kind row and the
    // color row stay inside the panel, each target a touch's size, none on another
    function test_thePhonesPanelFitsItsColorRowAndChangeButtons_data() {
        return [{ tag: "320", w: 320, h: 760 }, { tag: "384", w: 384, h: 832 }]
    }
    function test_thePhonesPanelFitsItsColorRowAndChangeButtons(data) {
        const page = pageOf({ form: "phone", width: data.w, height: data.h }, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const panel = find(page, "dashPanel")
        verify(waitForPolish(panel.Window.window), "laid out")
        const inside = (item, what) => {
            const at = item.mapToItem(panel, 0, 0)
            verify(at.x >= -0.5 && at.x + item.width <= panel.width + 0.5, what + " inside the panel: "
                   + at.x + " + " + item.width + " of " + panel.width)
            return at
        }
        const boxes = []
        for (const name of ["panelSignal", "panelKind", "panelColor_default", "panelColor_teal", "panelColor_stone"]) {
            const item = find(page, name)
            const at = inside(item, name)
            verify(item.height >= Theme.touchTarget - 0.5, name + " is " + item.height + " high")
            boxes.push({ name: name, x: at.x, y: at.y, r: at.x + item.width, b: at.y + item.height })
        }
        for (let i = 0; i < boxes.length; ++i)
            for (let j = i + 1; j < boxes.length; ++j) {
                const a = boxes[i], b = boxes[j]
                verify(a.r <= b.x + 0.5 || b.r <= a.x + 0.5 || a.b <= b.y + 0.5 || b.b <= a.y + 0.5,
                       a.name + " overlaps " + b.name)
            }
        const flow = find(page, "panelColors")
        verify(flow.height > Theme.touchTarget * 1.5, "the swatches wrap onto more than one row")
        for (const name of Theme.gaugeColorNames)
            inside(find(page, "panelColor_" + name), name)
    }
}
