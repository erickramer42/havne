// File/layer: tests/qml/tst_dashboard_panel.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboard_panel (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

DashboardKit {
    id: testCase
    name: "Dashboard"

    // A danger alert on the demo's RPM, saved as the app's conditions
    // (cleanup() empties them)
    function saveRpmDanger() {
        const saved = Bridge.saveConditions(
            '{"havne_conditions":1,"conditions":[{"key":"rpm","name":"Engine on",' +
            '"when":{"signal":["id:1001"],"unit":"RPM","above":-1},' +
            '"alert":{"level":"danger"}}]}')
        verify(saved.ok, saved.words)
    }

    // ---- The Night switch (ADR-0058 section 6)

    // The switch: one per page, at the end of the desktop's link state or
    // of the phone's tool row
    function nightSwitchOf(page) {
        return findChild(page, "nightSwitch")
    }

    function test_theSwitchFlipsTheModeInOnAndOff() {
        const page = pageOf({ form: "desktop" }, null, true)
        const sw = nightSwitchOf(page)
        verify(sw, "the switch is on the page")
        compare(sw.text, "Night")
        compare(sw.checked, false, "off")
        sw.clicked()
        compare(Theme.night, "on")
        compare(Theme.nightActive, true)
        compare(sw.checked, true, "checked while night is on")
        sw.clicked()
        compare(Theme.night, "off")
        compare(sw.checked, false)
    }

    function test_theSwitchHoldsAnOverrideUnderASchedule() {
        const page = pageOf({ form: "desktop" }, null, true)
        const sw = nightSwitchOf(page)
        Theme.now = () => new Date(2026, 10, 10, 18, 0, 0, 0)
        Theme.night = "schedule"
        Theme.refreshNight()
        compare(sw.checked, false, "the schedule says off at 18:00")
        sw.clicked()
        compare(Theme.night, "schedule", "the mode is kept")
        compare(Theme.nightOverride, "on")
        compare(sw.checked, true, "checked follows nightActive")
        compare(sw.Accessible.name, "Night is on until 07:00. Turn it off")
        sw.clicked()
        compare(Theme.nightOverride, "")
        compare(sw.checked, false)
        compare(sw.Accessible.name, "Night is off until 19:00. Turn it on")
    }

    // The desktop's switch ends its link state; the phone's the tool row
    function test_theSwitchSitsAtTheEndOfItsRow_data() {
        return [
            { tag: "desktop", form: "desktop", short: false, shown: "linkInRow",
              empty: "toolRowNight" },
            { tag: "phone upright", form: "phone", short: false, shown: "toolRowNight",
              empty: "linkInRow" },
            { tag: "phone on its side", form: "phone", short: true, shown: "toolRowNight",
              empty: "linkInRow" }
        ]
    }

    function test_theSwitchSitsAtTheEndOfItsRow(data) {
        Theme.shortLayout = data.short
        const page = pageOf({ form: data.form }, null, true)
        connect("mock://demo")
        const loader = findChild(page, data.shown)
        tryVerify(() => loader.active && loader.item, 3000, data.shown + " is shown")
        const sw = nightSwitchOf(page)
        verify(sw, "the switch exists")
        if (data.shown === "linkInRow") {
            compare(sw.parent, loader.item, "in the link state's row")
            compare(Array.from(loader.item.children).indexOf(sw), loader.item.children.length - 1,
                    "at the link state's end")
        } else {
            compare(loader.item, sw, "the tool row's own")
            const row = loader.parent
            const shown = Array.from(row.children).filter(c => c.visible)
            compare(shown[shown.length - 1], loader, "at the row's end")
        }
        verify(!findChild(page, data.empty).active, "the other place is empty")
    }

    // ---- The phone's tool row and joined panel (ADR-0057's appendix,
    // section 3)

    function test_thePhonesToolRow_data() {
        return [{ tag: "upright", short: false }, { tag: "on its side", short: true }]
    }

    function test_thePhonesToolRow(data) {
        Theme.shortLayout = data.short
        const page = pageOf({ form: "phone", width: 384, height: 832 }, null, true)
        connect("mock://demo")
        tryVerify(() => findChild(page, "toolRowNight").item, 3000, "the row is laid out")
        verify(waitForPolish(page), "laid out")
        const reset = findChild(page, "resetPeaks"), edit = findChild(page, "dashEdit")
        const night = nightSwitchOf(page), start = findChild(page, "toolRowStart")
        const row = reset.parent
        // The session's shorter Drive words lay the row out again
        settleToolRow(row)
        verify(!row.children[0].visible, "no title: the tab bar names the page")
        verify(!findChild(page, "linkLabel"), "no link state: the top bar has it")
        verify(!findChild(page, "linkInRow").active, "no link state in the row")
        verify(start.x + start.width <= reset.x + 0.5, "the start of the row is free")
        const end = night.mapToItem(row, 0, 0).x + night.width
        fuzzyCompare(row.x + end, row.parent.width, 0.5, "the tools sit at the page's right")
        fuzzyCompare(edit.x - (reset.x + reset.width), 8, 0.5, "8 between the tools")
        verify(reset.x + reset.width <= edit.x + 0.5, "Reset peaks, then Edit")
        verify(edit.x + edit.width <= night.mapToItem(row, 0, 0).x + 0.5, "Edit, then Night")
        verify(reset.flat, "Reset peaks is quiet")
        verify(!edit.flat && !edit.highlighted, "Edit is secondary")
        verify(night.checkable, "Night is a state")
        fuzzyCompare(row.height, Theme.touchTarget, 0.5, "one touch target tall")
        // Editing replaces Edit with its bar; the rest of the row stays
        page.startEditing()
        verify(!edit.visible, "Edit hides while editing")
        verify(reset.visible && night.visible, "the rest stay")
        page.stopEditing()
    }

    function test_theDesktopKeepsItsTitleAndLinkState() {
        const page = pageOf({ form: "desktop" }, null, true)
        connect("mock://demo")
        const row = findChild(page, "resetPeaks").parent
        verify(row.children[0].visible, "the title")
        compare(row.children[0].text, qsTr("Dashboard"))
        tryVerify(() => findChild(page, "linkInRow").item, 3000, "the link state")
        verify(!findChild(page, "toolRowStart").visible, "no empty start")
    }

    function jointDoc() {
        const cell = (w, h, kind, label) => ({
            kind: kind || "digital", w: w, h: h, label: label, refs: ["id:1001"], unit: "RPM",
            options: { from: 0, to: 8000 }
        })
        return { format: 2, pages: [{ name: "P", cells: [
            cell(3, 12, "digital", "A"), cell(3, 12, "digital", "B"),
            cell(6, 6, "digital", "C"), cell(3, 12, "blank", "")
        ] }] }
    }

    function seamsOf(page) {
        const grid = findChild(page, "dashPanelSurface").parent
        return Array.from(grid.children).filter(c => c.objectName === "dashSeam")
    }

    function test_thePhoneIsOnePanel() {
        checkOnePanel(pageOf({ form: "phone", width: 384, height: 832 }, jointDoc(), true))
    }

    // A blank cell draws nothing, so the panel's surface shows there; the
    // panel ends at the last row used
    function test_aJointPanelEndsWithItsLastRow() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, jointDoc(), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        const surface = findChild(page, "dashPanelSurface")
        fuzzyCompare(surface.height, view.rows * view.pitch, 1e-6, "the height of its rows")
    }

    // The cell held up covers what it crosses; a blank has no fill to show
    function test_aLiftedCellIsOpaqueOnThePanel() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, jointDoc(), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        const c = findChild(page, "dashCell_0_0"), blank = findChild(page, "dashCell_0_3")
        page.held = 0
        tryVerify(() => c.lifted, 1000, "lifted")
        compare(c.color, Theme.surface, "the held cell is opaque")
        compare(c.radius, Theme.radiusCard, "a card while held")
        page.held = 3
        tryVerify(() => blank.lifted, 1000, "lifted")
        compare(Qt.colorEqual(blank.color, "transparent"), true, "a blank stays empty")
        page.cancelHold()
        compare(Qt.colorEqual(c.color, "transparent"), true, "put back")
    }

    // The phone's panel's checks, for either form
    function checkOnePanel(page) {
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(page.cellGap, 0, "the cells meet")
        const surface = findChild(page, "dashPanelSurface")
        verify(surface.visible, "one surface")
        compare(surface.radius, Theme.radiusCard)
        compare(surface.border.width, page.hairlineWidth, "a hairline edge")
        compare(surface.border.color, Theme.hairline)
        compare(surface.color, Theme.surface)
        verify(!surface.gradient, "no gradient")
        for (const i of [0, 1, 2]) {
            const c = findChild(page, "dashCell_0_" + i)
            compare(c.radius, 0, "cell " + i + " has no radius of its own")
            compare(c.border.width, 0, "cell " + i + " has no border of its own")
            compare(Qt.colorEqual(c.color, "transparent"), true, "cell " + i + " has no fill")
            verify(!c.gradient, "cell " + i + " has no gradient")
        }
        const a = findChild(page, "dashCell_0_0"), b = findChild(page, "dashCell_0_1")
        fuzzyCompare(a.x + a.width, b.x, 1e-6, "neighbors meet across")
        const c2 = findChild(page, "dashCell_0_2")
        fuzzyCompare(a.y + a.height, c2.y, 1e-6, "neighbors meet down")
        // One line per seam, the book's, on the boundary between the cells
        const lines = seamsOf(page)
        const seams = DashboardBook.seams(view.placed, view.boxes, view.columns)
        compare(lines.length, seams.length, "a line for each seam")
        verify(lines.length > 0, "there are lines")
        for (let i = 0; i < lines.length; ++i) {
            const s = seams[i], l = lines[i]
            compare(Qt.colorEqual(l.color, Theme.hairline), true, "hairline color")
            const at = (s.vertical ? s.x * view.columnWidth : s.y * view.pitch)
            const center = (s.vertical ? l.x + l.width / 2 : l.y + l.height / 2)
            verify(Math.abs(center - at) <= page.hairlineWidth, "line " + i + " on its boundary")
            const run = s.length * (s.vertical ? view.pitch : view.columnWidth)
            fuzzyCompare(s.vertical ? l.height : l.width, run, 1e-6, "line " + i + " spans its run")
        }
    }

    // The desktop's gauges are one panel too, as the phone's
    function test_theDesktopIsOnePanelToo() {
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, jointDoc(), true)
        checkOnePanel(page)
        const edit = findChild(page, "dashEdit")
        verify(!edit.compact, "a wide window's Edit isn't compact")
        compare(edit.horizontalPadding, 8, "Edit keeps a button's padding")
    }

    // Unequal heights in a row: outside edit mode the shorter cell fills
    // the empty squares under it, and the stored layout stays
    function fitDoc(columns, blankUnder) {
        const cell = (w, h, kind, label) => ({
            kind: kind, w: w, h: h, label: label, refs: ["id:1001"], unit: "RPM",
            options: { from: 0, to: 8000 }
        })
        const head = [cell(2, 6, "digital", "short"), cell(columns - 2, 7, "digital", "tall")]
        const under = cell(columns, 3, "digital", "under")
        return { format: 2, pages: [{ name: "P", cells: blankUnder
            ? head.concat([cell(2, 1, "blank", ""), under])
            : head.concat([under, cell(2, 3, "blank", "")]) }] }
    }

    function test_aShortCellFillsTheEmptySquaresUnderIt_data() {
        return [{ tag: "6 columns", w: 384, h: 832, columns: 6, blankUnder: false },
                { tag: "12 columns", w: 832, h: 384, columns: 12, blankUnder: false },
                { tag: "6 columns, a blank under it", w: 384, h: 832, columns: 6, blankUnder: true },
                { tag: "12 columns, a blank under it", w: 832, h: 384, columns: 12, blankUnder: true }]
    }

    function test_aShortCellFillsTheEmptySquaresUnderIt(data) {
        const page = pageOf({ form: "phone", width: data.w, height: data.h },
                            fitDoc(data.columns, data.blankUnder), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(view.columns, data.columns)
        const short = findChild(page, "dashCell_0_0")
        const blank = findChild(page, data.blankUnder ? "dashCell_0_2" : "dashCell_0_3")
        const drawn = data.blankUnder ? 6 : 7
        fuzzyCompare(short.height, drawn * view.pitch, 1e-6, "down to the next cell, never a blank")
        fuzzyCompare(blank.height, (data.blankUnder ? 1 : 3) * view.pitch, 1e-6, "a blank stays its size")
        compare(DashboardBook.seams(view.placed, view.boxes, view.columns).length, seamsOf(page).length)
        if (!data.blankUnder)
            verify(!seamsOf(page).some(l => l.width > l.height && Math.abs(l.y - 6 * view.pitch) < 1
                                            && l.x < view.columnWidth),
                   "no line closes off a strip under it")
        page.held = 0
        tryVerify(() => Math.abs(short.height - 6 * view.pitch) < 1e-6, 1000, "a held cell's true size")
        page.cancelHold()
        fuzzyCompare(short.height, drawn * view.pitch, 1e-6, "back after the hold")
        page.startEditing()
        fuzzyCompare(short.height, 6 * view.pitch, 1e-6, "edit mode's true size")
        compare(page.doc.pages[0].cells[0].h, 6, "the layout is unchanged")
    }

    // The squares a page's boxes cover and a dial or ring drawn past its own
    function emptySquaresOf(view) {
        const covered = {}
        let rows = 0
        for (const b of view.boxes) {
            rows = Math.max(rows, b.y + b.h)
            for (let y = b.y; y < b.y + b.h; ++y)
                for (let x = b.x; x < b.x + b.w; ++x)
                    covered[x + "," + y] = true
        }
        let empty = 0
        for (let y = 0; y < rows; ++y)
            for (let x = 0; x < view.columns; ++x)
                empty += covered[x + "," + y] ? 0 : 1
        return empty
    }

    function stretchedRound(view) {
        const own = DashboardBook.place(view.placed, view.columns)
        return view.placed.filter((c, i) => (c.kind === "analog" || c.kind === "ring")
                                           && (view.boxes[i].w !== own[i].w
                                               || view.boxes[i].h !== own[i].h)).length
    }

    // The built-in layout leaves no empty square at any desktop width from 5
    // to 8 columns, and its dials and rings keep their own boxes; the same
    // cells saved are drawn as before, gaps and all
    function test_theBuiltInLayoutIsDrawnWithNoEmptySquare_data() {
        return [{ tag: "5 columns", w: 1100, columns: 5 },
                { tag: "6 columns", w: 1280, columns: 6 },
                { tag: "7 columns", w: 1580, columns: 7 },
                { tag: "8 columns", w: 1850, columns: 8 }]
    }

    function test_theBuiltInLayoutIsDrawnWithNoEmptySquare(data) {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { form: "desktop", width: data.w, height: 1400 })
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(view.columns, data.columns)
        verify(page.builtInShown, "nothing is stored")
        compare(emptySquaresOf(view), 0, "no empty square")
        compare(stretchedRound(view), 0, "no dial or ring stretched")

        findChild(page, "dashStore").desktop = DashboardBook.serialize(DashboardBook.builtIn("desktop"))
        tryVerify(() => !page.builtInShown, 1000, "the same cells, saved")
        const cells = page.pages[0].cells
        compare(view.boxes, DashboardBook.stretched(cells, DashboardBook.place(cells, data.columns),
                                                    data.columns), "a saved layout is drawn as before")
    }

    // After Reset the built-in layout is drawn as at first run: no empty
    // squares, no stretched dial
    function test_theBuiltInLayoutAfterResetIsDrawnAsAtFirstRun() {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { form: "desktop", width: 1100, height: 1400 })
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        const changed = DashboardBook.builtIn("desktop")
        changed.pages[0].cells[0].label = "Changed"
        findChild(page, "dashStore").desktop = DashboardBook.serialize(changed)
        tryVerify(() => !page.builtInShown && page.pages[0].cells[0].label === "Changed", 1000, "a saved layout")
        page.layoutFilePart.resetForm()
        tryVerify(() => page.builtInShown, 1000, "the built-in layout after Reset")
        compare(findChild(page, "dashStore").desktop, "", "nothing stored")
        compare(emptySquaresOf(view), 0, "no empty square after Reset")
        compare(stretchedRound(view), 0, "no dial or ring stretched after Reset")
    }

    // The desktop's short cell fills the empty squares under it too, and
    // the stored layout stays
    function test_theDesktopStretchesAShortCellToo() {
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, fitDoc(6, false), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(view.columns, 6)
        const short = findChild(page, "dashCell_0_0")
        fuzzyCompare(short.height, 7 * view.pitch, 1e-6, "down to the next cell")
        page.held = 0
        tryVerify(() => Math.abs(short.height - 6 * view.pitch) < 1e-6, 1000, "a held cell's true size")
        page.cancelHold()
        page.startEditing()
        fuzzyCompare(short.height, 6 * view.pitch, 1e-6, "edit mode's true size")
        compare(page.doc.pages[0].cells[0].h, 6, "the layout is unchanged")
        page.stopEditing()
    }

    // The strip's shown tab meets the panel: the panel's top at the
    // strip's foot, the tab's sheet a hairline over it, the strip painted
    // over the pages; an edit panel open sits a gap lower
    function test_thePanelMeetsTheStrip() {
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, pagesDoc(2), true)
        const surface = findChild(page, "dashPanelSurface")
        const host = findChild(page, "dashPageStripHost")
        tryVerify(() => surface.height > 0 && host.height > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        verify(page.stripMeetsPanel)
        compare(page.gridTop, 0)
        const foot = host.mapToItem(null, 0, host.height).y
        verify(Math.abs(surface.mapToItem(null, 0, 0).y - foot) <= page.hairlineWidth,
               "the panel starts at the strip's foot: " + surface.mapToItem(null, 0, 0).y + " and " + foot)
        const sheet = findChild(findChild(page, "dashPageTab_0"), "tabSheet")
        verify(sheet.visible, "the shown tab's sheet")
        fuzzyCompare(sheet.mapToItem(null, 0, sheet.height).y, foot + page.hairlineWidth, 1e-6,
                     "a hairline past the strip, over the panel's top line")
        verify(!findChild(findChild(page, "dashPageTab_1"), "tabSheet").visible, "only the shown one")
        verify(findChild(page, "dashPageStripHost").parent.z > findChild(page, "dashPages").z,
               "the strip paints over the pages")
        compare(surface.topLeftRadius, 0, "the first tab runs straight into the panel")
        page.goTo(1)
        tryVerify(() => findChild(page, "dashPageTab_1").checked, 1000, "the second page")
        compare(findChild(page, "dashPanelSurface").topLeftRadius, Theme.radiusCard,
                "the first page's corner keeps its radius")
        const second = findChild(findChild(page, "dashPage_1"), "dashPanelSurface")
        verify(second, "the second page's surface")
        compare(second.topLeftRadius, Theme.radiusCard, "under another tab the corner is round")

        page.goTo(0)
        page.select(0)
        compare(page.panel, "cell")
        verify(!page.stripMeetsPanel, "an edit panel doesn't meet the strip")
        compare(page.gridTop, page.topGap)
        const panel = findChild(page, "dashCellPanel")
        tryVerify(() => panel && panel.height > 0, 3000, "the panel")
        fuzzyCompare(panel.mapToItem(null, 0, 0).y - foot, page.topGap, 1, "a gap under the strip")
        page.closePanel()

        page.doc = pagesDoc(1)
        verify(!host.visible)
        compare(page.gridTop, page.topGap, "one page: the header's gap")
    }

    // A row of tabs wider than the strip scrolls to the shown one, and the
    // wheel moves it
    function test_aLongStripScrollsToTheShownTab() {
        const page = pageOf({ form: "desktop", width: 560, height: 760 }, pagesDoc(10), true)
        const view = findChild(page, "dashPageStripView")
        tryVerify(() => view.width > 0, 3000, "laid out")
        verify(view.contentWidth > view.width, "ten tabs are wider than the strip")
        compare(view.contentX, 0)
        page.goTo(9)
        const last = findChild(page, "dashPageTab_9")
        tryVerify(() => last.x >= view.contentX - 0.5 && last.x + last.width <= view.contentX + view.width + 0.5,
                  2000, "the shown tab scrolled into view")
        page.goTo(0)
        tryCompare(view, "contentX", 0, 2000, "and back")
        // Near the strip's start: the test window is narrower than the page
        mouseWheel(view, 20, view.height / 2, 0, -120)
        tryVerify(() => view.contentX > 0, 1000, "the wheel moves the row")

        // Tabs at their widest keep the row's width when the shown one turns
        // bold, so the page change alone brings the tab into view
        const d = pagesDoc(10)
        for (let i = 0; i < d.pages.length; ++i)
            d.pages[i].name = "A page name long enough to be cut " + i
        page.doc = d
        page.goTo(0)
        tryCompare(view, "contentX", 0, 2000, "at the start")
        const width = view.contentWidth
        page.goTo(8)
        const eighth = findChild(page, "dashPageTab_8")
        tryVerify(() => eighth.x >= view.contentX - 0.5
                        && eighth.x + eighth.width <= view.contentX + view.width + 0.5,
                  2000, "the shown tab scrolled into view")
        compare(view.contentWidth, width, "the row kept its width")
    }

    // A long name is cut at the tab's widest and read whole
    function test_aLongNameElidesAndIsReadWhole() {
        const d = pagesDoc(2)
        const name = "A page name thirty-four chars long"
        d.pages[1].name = name
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, d, true)
        const t = findChild(page, "dashPageTab_1")
        tryVerify(() => t.width > 0, 3000, "laid out")
        compare(t.width, 220, "at the widest")
        verify(t.contentItem.truncated, "the name is cut")
        compare(t.Accessible.name, name, "and read whole")
        verify(findChild(page, "dashPageTab_0").width >= 64, "a short name keeps the least width")
    }

    // A Digits card counts from its range; its own option wins
    function test_aDigitsCardsCountComesFromItsRange_data() {
        return [{ tag: "from the range", options: { from: 0, to: 100 }, count: 3 },
                { tag: "the option wins", options: { from: 0, to: 100, digits: 6 }, count: 6 }]
    }

    function test_aDigitsCardsCountComesFromItsRange(data) {
        const doc = { format: 2, pages: [{ name: "P", cells: [
            { kind: "seven-segment", w: 3, h: 6, label: "D", refs: ["id:1001"], unit: "RPM",
              options: data.options }] }] }
        const page = pageOf({ form: "phone", width: 384, height: 832 }, doc, true)
        tryVerify(() => allLoaded(page) && loaders(page)[0].item, 3000, "its gauge loads")
        compare(loaders(page)[0].item.count, data.count)
    }

    // Each hairline is a whole number of the display's pixels, and sits on them
    function test_aHairlineIsWholePixels_data() {
        const rows = []
        for (const form of ["phone", "desktop"])
            for (const z of [{ tag: "100 %", zoom: 1 }, { tag: "125 %", zoom: 1.25 },
                             { tag: "75 %", zoom: 0.75 }, { tag: "110 %", zoom: 1.1 },
                             { tag: "67 %", zoom: 0.67 }])
                rows.push({ tag: form + ", " + z.tag, zoom: z.zoom, form: form })
        return rows
    }

    function test_aHairlineIsWholePixels(data) {
        Theme.zoom = data.zoom
        const page = pageOf(data.form === "phone" ? { form: "phone", width: 384, height: 832 }
                                                  : { form: "desktop", width: 1280, height: 800 },
                            jointDoc(), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        const ratio = (Screen.devicePixelRatio || 1) * Theme.zoom
        const onGlass = page.hairlineWidth * ratio
        fuzzyCompare(onGlass, Math.round(onGlass), 1e-9, "whole pixels on the glass: " + onGlass)
        verify(onGlass >= 1, "at least one")
        compare(findChild(page, "dashPanelSurface").border.width, page.hairlineWidth, "the edge matches")
        const lines = seamsOf(page)
        verify(lines.length > 0, "there are lines")
        for (const l of lines) {
            const p = l.mapToItem(null, 0, 0)
            const at = (l.width < l.height ? p.x : p.y) * ratio
            fuzzyCompare(at, Math.round(at), 1e-6, "a line on a whole pixel: " + at)
        }
        Theme.zoom = 1
    }

    Component { id: spyComponent; SignalSpy {} }

    Component { id: metricsComponent; TextMetrics {} }

    // The desktop's link state drops the address where its row leaves it
    // no room (a narrow window): the header carries the address. Under 60
    // px the dot and the label fade rather than read "L...e"
    function test_theLinkStateDropsTheAddressInANarrowRow() {
        const page = pageOf({ form: "desktop", width: 1000, height: 600 }, null, true)
        connect("mock://demo?profile=mock")
        const loader = findChild(page, "linkInRow")
        tryVerify(() => loader.item, 3000, "the link state")
        const label = findChild(page, "linkLabel")
        tryCompare(label, "text", "Live · mock://demo", 5000)
        verify(label.roomForAddress)
        // The label takes what the title and the tools leave: widen from
        // 380 until it holds a word but not the address
        let w = 360
        do {
            page.width = (w += 20)
            wait(50)
        } while (w < 800 && !(label.width >= 60 && label.width < 150))
        verify(label.width >= 60 && label.width < 150, "a narrow label with room for a word at " + w
               + ": " + label.width)
        compare(label.text, "Live")
        compare(label.opacity, 1, "shown")
        page.width = 300
        tryVerify(() => label.width < 60, 3000, "no room for a word")
        compare(label.opacity, 0, "faded")
        compare(findChild(page, "linkDot").opacity, 0, "the dot with it")
        page.width = 1000
        tryCompare(label, "text", "Live · mock://demo", 3000)
        compare(label.opacity, 1)
    }

    // A plugin's session adds the vehicle's and plugin's names after the
    // address, eliding them before the address; no room for the address
    // whole, none added
    function test_theLinkStateNamesTheVehicleAndPluginAfterTheAddress() {
        const page = pageOf({ form: "desktop", width: 1000, height: 600,
                              profileNames: { "mock": "Daily driver extras" } }, null, true)
        Bridge.vehicleLabel = "Project car"
        connect("mock://demo?profile=mock")
        tryVerify(() => findChild(page, "linkInRow").item, 3000, "the link state")
        const label = findChild(page, "linkLabel")
        tryCompare(label, "text", "Live · mock://demo · Project car · Daily driver extras", 5000)
        compare(label.elide, Text.ElideRight)
        // Narrower: the names elide, the address stays
        let w = 1000
        do {
            page.width = (w -= 20)
            wait(50)
        } while (w > 400 && !label.truncated)
        verify(label.truncated, "the names run out of room at " + w)
        verify(label.text.startsWith("Live · mock://demo · "), label.text)
        // Wide again: whole; without a vehicle, the plugin alone
        page.width = 1000
        tryCompare(label, "text", "Live · mock://demo · Project car · Daily driver extras", 3000)
        Bridge.vehicleLabel = ""
        tryCompare(label, "text", "Live · mock://demo · Daily driver extras", 3000)
    }

    // An address that doesn't fit whole in a row past the room for one
    // takes no names
    function test_theLinkStateAddsNoNamesWhereTheAddressIsCut() {
        const page = pageOf({ form: "desktop", width: 1000, height: 600,
                              profileNames: { "mock": "Daily driver extras" } }, null, true)
        Bridge.vehicleLabel = "Project car"
        connect("mock://averyveryveryverylongdemohostnameforthistest?profile=mock")
        tryVerify(() => findChild(page, "linkInRow").item, 3000, "the link state")
        const label = findChild(page, "linkLabel")
        tryVerify(() => label.text.endsWith("Daily driver extras"), 5000, label.text)
        const metrics = createTemporaryObject(metricsComponent, testCase,
                                              { font: label.font, text: label.withAddress })
        let w = 1000, cut = false
        do {
            page.width = (w -= 10)
            wait(50)
            cut = label.roomForAddress && label.width < metrics.advanceWidth
        } while (w > 400 && !cut)
        verify(cut, "the address is cut at " + w)
        verify(!label.text.includes("Project car"), "no names beside a cut address: " + label.text)
        page.width = 1000
        Bridge.vehicleLabel = ""
    }

    function settleToolRow(row) {
        tryVerify(() => !isPolishScheduled(row) && !isPolishScheduled(row.parent), 3000,
                  "the tool row laid out")
    }

    // At every width around the one where Reset peaks gives way, the row
    // ends inside its bar and keeps Reset peaks while it fits, whatever
    // the font's widths
    function test_theToolRowFitsItsBarAtEveryWidth_data() {
        return [{ tag: "no session", uri: "" }, { tag: "a session", uri: "mock://demo" }]
    }

    function test_theToolRowFitsItsBarAtEveryWidth(data) {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, null, true)
        if (data.uri !== "")
            connect(data.uri)
        tryVerify(() => findChild(page, "toolRowNight").item, 3000, "the Night switch")
        const reset = findChild(page, "resetPeaks"), row = reset.parent, bar = row.parent
        settleToolRow(row)
        const around = Math.round(row.toolsWidth + page.width - bar.width)
        let shown = 0, dropped = 0
        for (let w = around - 16; w <= around + 16; ++w) {
            page.width = w
            settleToolRow(row)
            const ends = Array.from(row.children).filter(c => c.visible).map(c => c.x + c.width)
            const end = row.x + Math.max(...ends)
            verify(end <= bar.width + 0.5, "at " + w + " the row ends at " + end + " of " + bar.width
                   + (reset.visible ? ", Reset peaks shown" : ""))
            if (reset.visible) {
                ++shown
            } else {
                // and gives way only when it wouldn't fit
                const need = row.implicitWidth + row.spacing + Math.ceil(reset.implicitWidth)
                verify(need > bar.width + 0.5, "at " + w + " Reset peaks fits: " + need + " of " + bar.width)
                ++dropped
            }
        }
        verify(shown > 0 && dropped > 0, "the sweep crosses the width: " + shown + " shown, " + dropped + " dropped")
    }

    // A row too narrow for every tool drops Reset peaks, and edit mode's
    // Pages row offers it at its end, whole and inside the page
    function test_aNarrowToolRowMovesResetPeaksIntoThePagesRow() {
        // The phone form: the desktop's Pages flow keeps its unwrapped
        // width in a page made alone this narrow
        const page = pageOf({ form: "phone", width: 1000, height: 600 }, null, true)
        const reset = findChild(page, "resetPeaks"), inRow = findChild(page, "moreResetPeaks")
        const bar = reset.parent.parent
        verify(waitForPolish(page), "laid out")
        verify(bar.width >= page.toolRowFull, "a wide bar: " + bar.width)
        verify(reset.visible, "Reset peaks in the row")
        page.startEditing()
        page.moreShown = true
        verify(!inRow.visible, "and not in the Pages row")
        page.stopEditing()
        page.width = 240
        tryVerify(() => bar.width < page.toolRowFull, 3000, "a narrow bar")
        verify(!reset.visible, "Reset peaks gives way")
        page.startEditing()
        page.moreShown = true
        verify(waitForPolish(page), "the Pages row laid out")
        verify(inRow.visible, "the Pages row offers it")
        verify(inRow.flat, "quiet, as in the row")
        compare(inRow.text, "Reset peaks")
        const shown = Array.from(inRow.parent.children).filter(c => c.visible)
        compare(shown[shown.length - 1], inRow, "at the row's end")
        verify(inRow.width >= inRow.implicitWidth - 0.5, "whole: " + inRow.width + " of " + inRow.implicitWidth)
        verify(inRow.mapToItem(page, inRow.width, 0).x <= page.width + 0.5,
               "inside the page's width: " + inRow.mapToItem(page, inRow.width, 0).x)
        verify(inRow.mapToItem(page, 0, inRow.height).y <= page.height + 0.5,
               "inside the page's height: " + inRow.mapToItem(page, 0, inRow.height).y)
        const spy = createTemporaryObject(spyComponent, testCase,
                                          { target: page, signalName: "peaksResetRequested" })
        inRow.clicked()
        compare(spy.count, 1, "and it resets the peaks")
    }

    // A stored accent of "info" (an older layout's blue) draws as the
    // value's own color; "success" keeps its green. No document changes
    function test_aStoredInfoAccentDrawsAsTheValueColor() {
        const cell = (label, accent) => ({ kind: "digital", w: 2, h: 4, label: label,
                                           refs: refsOf("Engine speed"), unit: "RPM",
                                           options: { from: 0, to: 8000, accent: accent } })
        const page = pageOf({}, docOf([cell("Blue", "info"), cell("Green", "success")]), true)
        tryVerify(() => allLoaded(page), 5000, "the gauges load")
        const blue = gaugeOf(findChild(page, "dashCell_0_0"))
        const green = gaugeOf(findChild(page, "dashCell_0_1"))
        verify(blue && green, "both load")
        verify(Qt.colorEqual(blue.accent, Theme.gaugeValue), "info draws as the value: " + blue.accent)
        verify(Qt.colorEqual(green.accent, Theme.success), "success stays: " + green.accent)
        compare(page.doc.pages[0].cells[0].options.accent, "info", "the stored word stays")
    }

    // A dial with no caption names its signal and says the unit once; a typed
    // caption wins (the hub's label, never the unit, is the default)
    function test_aCaptionlessDialShowsItsSignalsName() {
        const cell = (caption) => ({ kind: "analog", w: 4, h: 4, label: "Engine speed",
                                     refs: refsOf("Engine speed"), unit: "RPM",
                                     options: caption === null ? { from: 0, to: 8000 }
                                                               : { from: 0, to: 8000, caption: caption } })
        const page = pageOf({}, docOf([cell(null), cell("x1000 r/min")]), true)
        tryVerify(() => allLoaded(page), 5000, "the gauges load")
        const bare = gaugeOf(findChild(page, "dashCell_0_0"))
        const typed = gaugeOf(findChild(page, "dashCell_0_1"))
        compare(findChild(bare, "gaugeCaption").text, "Engine speed", "the name, not the unit")
        compare(bare.unit, "RPM", "the unit stays under the value")
        compare(findChild(typed, "gaugeCaption").text, "x1000 r/min", "a typed caption wins")
    }

    // With no session a picked color tints the title, so a page's grouping
    // shows while it is built; Night draws no pick
    function test_aPickedColorTintsTheTitleBeforeASession() {
        const page = pageOf({}, docOf([{ kind: "digital", w: 2, h: 4, label: "Engine speed",
                                         refs: refsOf("Engine speed"), unit: "RPM",
                                         options: { from: 0, to: 8000, color: "stone" } }]), true)
        tryVerify(() => allLoaded(page), 5000, "the gauge loads")
        const g = gaugeOf(findChild(page, "dashCell_0_0"))
        verify(!g.hasValue, "no session")
        verify(Qt.colorEqual(g.titleColor, Theme.gaugeColor("stone")), "the pick: " + g.titleColor)
        Theme.night = "on"
        verify(Qt.colorEqual(g.titleColor, Theme.textDim), "Night: no pick: " + g.titleColor)
        Theme.night = "off"
    }

    // An alert ON on a cell's signal colors that gauge's value in the
    // level's role; a cell no alert reads keeps its own
    function test_anAlertColorsTheGaugeItsSignalIs_data() {
        return Theme.paletteNames.map(p => ({ tag: p, palette: p }))
    }

    function test_anAlertColorsTheGaugeItsSignalIs(data) {
        const night = Theme.nightPalettes.indexOf(data.palette) >= 0
        Theme.night = night ? "on" : "off"
        if (night)
            Theme.nightStyle = data.palette === "nightRed" ? "red" : "amber"
        else
            Theme.look = data.palette
        const page = createTemporaryObject(pageComponent, testCase)
        const found = gauges(page)
        connect("mock://demo")
        const tach = found["Engine speed"]
        const coolant = found["Coolant"]
        tryVerify(() => tach.hasValue && coolant.hasValue, 5000, "both read")
        const own = coolant.valueColor
        page.alertLevels = { "1001": 2 }
        compare(tach.alertLevel, 2, "the RPM cell takes the danger")
        verify(Qt.colorEqual(tach.valueColor, Theme.alertDangerFill), "in the danger's role")
        compare(coolant.alertLevel, 0, "a cell no alert reads")
        verify(Qt.colorEqual(coolant.valueColor, own), "keeps its color")
        page.alertLevels = { "1001": 1 }
        verify(Qt.colorEqual(tach.valueColor, Theme.alertWarnFill), "in the warning's role")
    }

    // The page's levels follow the bridge's: a saved alert on the demo's RPM
    // reaches the tach through Bridge.alertLevels, not a level set by hand
    function test_theBridgesAlertReachesTheGaugeItReads() {
        saveRpmDanger()
        const page = createTemporaryObject(pageComponent, testCase)
        const found = gauges(page)
        connect("mock://demo")
        const tach = found["Engine speed"]
        tryVerify(() => tach.alertLevel === 2, 10000, "the RPM's danger reaches the tach")
        compare(found["Coolant"].alertLevel, 0, "a gauge no alert reads")
        Bridge.disconnectSession()
        tryVerify(() => tach.alertLevel === 0, 10000, "and leaves at the disconnect")
    }
}
