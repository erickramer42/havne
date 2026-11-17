// File/layer: tests/qml/tst_dashedit_side.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_side (test_qml)

import QtQuick
import QtTest
import App
import AppTest

// The edit panel's side column on a wide desktop page: where it stands, the
// width it takes from the cells, and its own scrolling
TestCase {
    id: testCase
    name: "DashboardEditSide"
    when: windowShown
    width: 1280
    height: 800

    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }
    function cellOf(label) {
        return { kind: "digital", w: 2, h: 4, label: label, refs: ["id:1001"],
                 unit: "RPM", options: { from: 0, to: 8000 } }
    }
    // A page of four cells at this width, the first one's panel open
    function opened(width) {
        const d = DashboardBook.builtIn("desktop")
        d.pages = [{ name: "Test", cells: [cellOf("A"), cellOf("B"), cellOf("C"), cellOf("D")] }]
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem,
                                           { width: width, height: 800 })
        page.doc = d
        page.startEditing()
        verify(waitForPolish(page.Window.window))
        page.select(0)
        verify(waitForPolish(page.Window.window), "laid out")
        return page
    }
    function at(page, name) { return find(page, name).mapToItem(page, 0, 0) }

    function test_theCellPanelStandsBesideTheGridOnAWideDesktop() {
        const page = opened(1280)
        const view = find(page, "dashPage_0")
        const panel = find(page, "dashPanel")
        const cell = find(page, "dashCell_0_0")
        verify(view.sideColumn)
        verify(panel.width <= page.sideColumnWidth, "the panel is " + panel.width)
        verify(at(page, "dashPanel").x >= at(page, "dashCell_0_0").x + cell.width, "right of the first cell")
        verify(Math.abs(at(page, "dashCell_0_0").y - at(page, "dashPanel").y) < 1, "the grid starts level with the panel")
        verify(view.gridWidth + page.sideColumnWidth <= view.fullGridWidth, "the grid gives the column its width")
        for (const name of ["panelClose", "panelPaste"]) {
            const item = find(page, name)
            verify(at(page, name).x + item.width <= page.width, name + " inside the page")
        }
    }

    function test_theColumnsStayWhateverThePanelDoes() {
        const page = opened(1280)
        const view = find(page, "dashPage_0")
        const open = view.columns
        page.closePanel()
        compare(view.gridWidth, view.fullGridWidth, "a closed panel gives the grid its width")
        compare(view.columns, open, "and the columns are the same")
    }

    function test_theCellPanelStaysAboveTheGridOnANarrowDesktop() {
        const page = opened(800)
        const panel = find(page, "dashPanel")
        verify(!find(page, "dashPage_0").sideColumn)
        verify(at(page, "dashCell_0_0").y >= at(page, "dashPanel").y + panel.height, "grid under the panel")
    }

    function test_thePhoneKeepsItsPanelAboveTheGrid() {
        const page = opened(1280)
        page.form = "phone"
        verify(!find(page, "dashPage_0").sideColumn)
    }

    // The column opens where the cells it leaves are at least sideCellMinWidth, on both sides of the equality
    function test_theColumnNeedsTheCellsItLeavesToBeWideEnough() {
        const page = opened(1280)
        const view = find(page, "dashPage_0")
        const cell = (view.sideGridWidth - (view.columns - 1) * page.cellGap) / view.columns
        page.sideCellMinWidth = cell
        verify(view.sideColumn, "cells of exactly the minimum are enough")
        page.sideCellMinWidth = cell + 0.01
        verify(!view.sideColumn, "a little less is not")
        const narrow = opened(900)
        verify(!find(narrow, "dashPage_0").sideColumn, "900 px leaves cells under the minimum")
    }

    function test_aResizeMovesThePanelAndGivesTheGridItsWidthBack() {
        const page = opened(1280)
        const view = find(page, "dashPage_0")
        verify(view.sideColumn)
        page.width = 800
        verify(waitForPolish(page.Window.window))
        verify(!view.sideColumn)
        compare(view.gridWidth, view.fullGridWidth, "the grid has its width again")
        verify(at(page, "dashCell_0_0").y >= at(page, "dashPanel").y + find(page, "dashPanel").height,
               "the panel is above the grid")
        page.width = 1280
        verify(waitForPolish(page.Window.window))
        verify(view.sideColumn, "and beside it again")
    }

    // A tall panel scrolls in its column and leaves the grid where it was
    function test_theColumnScrollsByItselfAndTheGridStays() {
        const page = opened(1280)
        const side = find(page, "dashSidePanel")
        const view = find(page, "dashPage_0")
        verify(side.contentHeight > side.height, "the panel is taller than the page: " + side.contentHeight)
        const y = at(page, "dashCell_0_0").y
        const colors = find(page, "panelColor_default")
        side.contentY = Math.min(side.contentHeight - side.height, colors.mapToItem(side.contentItem, 0, 0).y - 20)
        const inView = colors.mapToItem(side, 0, 0).y
        verify(inView >= 0 && inView + colors.height <= side.height, "the Color row is in the column's view: " + inView)
        compare(at(page, "dashCell_0_0").y, y, "the selected cell stays")
        compare(view.contentY, 0, "the grid did not scroll")
    }
}
