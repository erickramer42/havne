// File/layer: tests/qml/tst_phone_viewer.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.phone_viewer (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

PhoneKit {
    id: testCase
    name: "Phone"

    // The Jump row stands under the chart on a phone, its buttons a touch's
    // size and inside the width at 384 and 320 px
    function test_theJumpRowFitsAPhone_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }
    function test_theJumpRowFitsAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 832
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=obd2", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "obd2" })
        tryVerify(() => find(page, "chartFrame").visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        const row = find(page, "jumpRow")
        verify(row.visible, "the Jump row is shown")
        const chartBottom = find(page, "chartFrame").mapToItem(main.contentItem, 0, find(page, "chartFrame").height).y
        const rowTop = row.mapToItem(main.contentItem, 0, 0).y
        verify(rowTop >= chartBottom - 0.5, "under the chart: " + rowTop + " from " + chartBottom)
        for (const name of ["jumpPrevious_pull", "jumpNext_pull", "jumpPrevious_cruise", "jumpStretches",
                            "jumpNext_cruise"]) {
            const item = find(page, name)
            const top = item.mapToItem(main.contentItem, 0, 0).y
            const bottom = item.mapToItem(main.contentItem, 0, item.height).y
            verify(bottom - top >= 47.5, name + " is " + (bottom - top) + " tall")
            const right = item.mapToItem(main.contentItem, item.width, 0).x
            verify(right <= main.contentItem.width + 0.5, name + " ends at " + right)
        }
        const status = find(page, "jumpStatus")
        const edge = status.mapToItem(main.contentItem, status.width, 0).x
        verify(edge <= main.contentItem.width + 0.5, "the status ends at " + edge)
    }

    // The Stretches panel in the chart's place on a phone: its buttons a
    // touch's size, every field inside the width at 384 and 320 px
    function test_theStretchesPanelFitsAPhone_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }
    function test_theStretchesPanelFitsAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 832
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=obd2", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "obd2" })
        tryVerify(() => find(page, "chartFrame").visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(main), "laid out")
        verify(find(page, "stretchesPanel").visible, "the panel is up")
        verify(!find(page, "chartFrame").visible, "in the chart's place")
        for (const name of ["stretchesApply", "stretchesReset", "stretchesClose", "rule_pullLevel",
                            "rule_pullRise", "pullEnd_lift", "pullEnd_topSpeed"]) {
            const item = find(page, name)
            const top = item.mapToItem(main.contentItem, 0, 0).y
            const bottom = item.mapToItem(main.contentItem, 0, item.height).y
            verify(bottom - top >= 47.5, name + " is " + (bottom - top) + " tall")
            const right = item.mapToItem(main.contentItem, item.width, 0).x
            verify(right <= main.contentItem.width + 0.5, name + " ends at " + right)
        }
        const panel = find(page, "stretchesPanel")
        const bottom = panel.mapToItem(main.contentItem, 0, panel.height).y
        verify(bottom <= main.contentItem.height + 0.5, "the panel ends at " + bottom)
    }

    // The Conditions panel in the chart's place on a phone: its rows and
    // buttons a touch's size inside the width at 384 and 320 px, and a row's
    // press gives the chart back centered on its entry
    function test_theConditionsPanelFitsAPhone_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }
    function test_theConditionsPanelFitsAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 832
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Conditions", uri: "replay://x?profile=obd2", source: "",
                    folder: Harness.recordingFixtures + "/conditions", profile: "obd2" })
        tryVerify(() => find(page, "chartFrame").visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        const fits = (item, name) => {
            const top = item.mapToItem(main.contentItem, 0, 0).y
            const bottom = item.mapToItem(main.contentItem, 0, item.height).y
            verify(bottom - top >= 47.5, name + " is " + (bottom - top) + " tall")
            const right = item.mapToItem(main.contentItem, item.width, 0).x
            verify(right <= main.contentItem.width + 0.5, name + " ends at " + right)
        }
        fits(find(page, "jumpConditions"), "jumpConditions")
        find(page, "jumpConditions").clicked()
        verify(waitForPolish(main), "laid out")
        verify(find(page, "conditionsPanel").visible, "the panel is up")
        verify(!find(page, "chartFrame").visible, "in the chart's place")
        fits(find(page, "conditionsClose"), "conditionsClose")
        const list = find(page, "conditionList")
        tryVerify(() => list.itemAtIndex(2) !== null && list.itemAtIndex(2).width > 0, 2000,
                  "the rows are laid out")
        for (let i = 0; i < 3; ++i) {
            const row = list.itemAtIndex(i)
            fits(find(row, "conditionRowArea"), "row " + i)
            for (const name of ["conditionTime", "conditionName", "conditionLength"]) {
                const label = find(row, name)
                const right = label.mapToItem(main.contentItem, label.width, 0).x
                verify(right <= main.contentItem.width + 0.5, name + " of row " + i + " ends at " + right)
            }
        }
        // A name as long as a name may be (40 bytes) takes a second line,
        // the level and the length under it
        const long = "Coolant over the limit at idle in summer"
        compare(ConditionBook.nameBytes(long), ConditionBook.maxName)
        const panel = find(page, "conditionsPanel")
        panel.spans = page.conditionList.map(s => Object.assign({}, s, { name: long }))
        tryVerify(() => list.itemAtIndex(0) !== null && find(list.itemAtIndex(0), "conditionName").text
                  === long, 2000, "the rows take the long name")
        verify(waitForPolish(main), "laid out")
        for (let i = 0; i < 3; ++i) {
            const row = list.itemAtIndex(i)
            for (const name of ["conditionName", "conditionLength"]) {
                const label = find(row, name)
                verify(!label.truncated, name + " of row " + i + " is whole: " + label.text)
                const right = label.mapToItem(main.contentItem, label.width, 0).x
                verify(right <= main.contentItem.width + 0.5, name + " of row " + i + " ends at " + right)
                verify(label.mapToItem(row, 0, label.height).y <= row.height + 0.5, name + " inside its row")
            }
        }
        panel.spans = Qt.binding(() => page.conditionList)
        page.cursorsPart.setView(0, 2000)
        mouseClick(find(list.itemAtIndex(0), "conditionRowArea"))
        verify(!page.conditionsOpen, "the panel gives the chart back")
        verify(waitForPolish(main), "laid out")
        verify(find(page, "chartFrame").visible)
        compare(page.viewT0, 11376)
        compare(page.cursorMs, 7251)
    }

    // A recording folder of number signals (twelve unless given), three
    // samples each
    function wideRecording(name, count) {
        const n = count || 12
        const folder = Harness.writeFramesRecording(name, 1, 100000)
        verify(folder !== "", "the folder is made")
        let header = "ecuio session recording, format 2 (ADR-0049)\n"
                     + "started  2026-11-03 10:00:00.000 (local time)\n"
                     + "utc  2026-11-03T16:00:00.000Z\ncore  0.3.6\n"
                     + "uri  socketcan://vcan0?profile=raw\nprofile  raw\n"
        let rows = "host_us,signal,value,unit\n"
        for (let s = 1; s <= n; ++s)
            header += "signal  Signal " + s + ",number,V,0,0\n"
        for (let t = 1; t <= 3; ++t) {
            for (let s = 1; s <= n; ++s)
                rows += (t * 100000) + ",Signal " + s + "," + s + ",V\n"
        }
        header += "\n10:00:01.000  +1.000  end  havne-cli: seconds: 1 frames, " + (3 * n) + " values, "
                  + "0 records dropped, 0 notes dropped\n"
        Harness.writeScratchFile(name + "/events.log", header)
        Harness.writeScratchFile(name + "/signals.csv", rows)
        return folder
    }

    // The scrollers the page shows that a swipe moves: visible, interactive
    // and holding more than their height
    function scrollersOf(page) {
        const found = []
        const walk = item => {
            for (const child of item.children) {
                if (!child.visible)
                    continue
                if (child.flickableDirection !== undefined && child.interactive
                        && child.contentHeight > child.height + 0.5)
                    found.push(child.objectName || String(child))
                walk(child)
            }
        }
        walk(page)
        return found
    }

    // A recording of three signals in three units, so the chart has panes
    // and seams between them
    function panedRecording(name) {
        const folder = Harness.writeFramesRecording(name, 1, 100000)
        verify(folder !== "", "the folder is made")
        const units = ["rpm", "%", "km/h"]
        let header = "ecuio session recording, format 2 (ADR-0049)\n"
                     + "started  2026-11-03 10:00:00.000 (local time)\n"
                     + "utc  2026-11-03T16:00:00.000Z\ncore  0.3.6\n"
                     + "uri  socketcan://vcan0?profile=raw\nprofile  raw\n"
        let rows = "host_us,signal,value,unit\n"
        for (let s = 0; s < 3; ++s)
            header += "signal  Signal " + s + ",number," + units[s] + ",0,0\n"
        for (let t = 1; t <= 3; ++t) {
            for (let s = 0; s < 3; ++s)
                rows += (t * 100000) + ",Signal " + s + "," + (t * (s + 1)) + "," + units[s] + "\n"
        }
        header += "\n10:00:01.000  +1.000  end  havne-cli: seconds: 1 frames, 9 values, "
                  + "0 records dropped, 0 notes dropped\n"
        Harness.writeScratchFile(name + "/events.log", header)
        Harness.writeScratchFile(name + "/signals.csv", rows)
        return folder
    }

    // The viewer's resize grips reach a touch's size on the glass: the
    // pane seam and the chart splitter draw small and take the press
    // from a margin around them
    function test_theViewersGripsReachATouchsSize() {
        const v = uprightViewer({ title: "Paned", uri: "socketcan://vcan0?profile=raw", source: "",
                                  folder: panedRecording("pv-grips"), profile: "raw" })
        const main = v.main
        const page = v.page
        verify(page.phoneShape, "the phone's form")
        const grips = [find(page, "chartSplitter"), find(page, "paneSeam_0")]
        for (const grip of grips) {
            tryVerify(() => grip && grip.visible, 5000, "a grip shows")
        }
        for (const grip of grips) {
            const handler = grip.data.filter(c => c.yAxis !== undefined && c.margin !== undefined)[0]
            verify(handler, grip.objectName + " has its drag handler")
            const top = grip.mapToItem(main.contentItem, 0, 0).y
            const foot = grip.mapToItem(main.contentItem, 0, grip.height).y
            const scale = (foot - top) / grip.height
            const reach = foot - top + 2 * handler.margin * scale
            verify(reach >= 48 - 0.5, grip.objectName + " reaches " + reach + " on the glass")
        }
    }

    // A press 16 px under the splitter is still its grip's: the Jump row's
    // buttons below it keep out of the grip's reach (the drag moves the
    // chart's stored height)
    function test_theSplittersGripWorksUnderItBesideTheJumpRow() {
        const v = uprightViewer({ title: "Pulls", uri: "replay://x?profile=obd2", source: "",
                                  folder: Harness.recordingFixtures + "/pull-twice", profile: "obd2" })
        const main = v.main
        const page = v.page
        const splitter = find(page, "chartSplitter")
        tryVerify(() => splitter.visible, 5000, "the splitter shows")
        verify(find(page, "jumpRow").visible, "the Jump row is under the chart")
        tryVerify(() => find(page, "jumpNext_pull").enabled, 10000, "the pulls are found")
        compare(page.chartHeight, -1, "the chart's height is its own")
        const root = main.contentItem
        const foot = splitter.mapToItem(root, 0, splitter.height).y
        // Over a Jump button, where a press under the grip would land
        const button = find(page, "jumpNext_pull")
        verify(button.visible, "a Jump button")
        const x = button.mapToItem(root, button.width / 2, 0).x
        const seq = touchEvent(root)
        seq.press(0, root, x, foot + 16).commit()
        verify(!button.pressed, "no Jump button takes the press meant for the grip")
        for (let d = 4; d <= 48; d += 4)
            seq.move(0, root, x, foot + 16 - d).commit()
        seq.release(0, root, x, foot + 16 - 48).commit()
        verify(page.chartHeight > 0, "the drag from the grip's lower reach set the height: " + page.chartHeight)
    }

    // A phone upright pins the title row, the chart and its splitter, and
    // under them one list scrolls in each view: Values' rows, the Frames
    // table and About's findings, each with its view's controls at its head
    function test_aPhoneUprightPinsTheChartAndScrollsOneList() {
        const v = uprightViewer({ title: "Wide", uri: "socketcan://vcan0?profile=raw", source: "",
                                  folder: wideRecording("pv-a-wide", 100), profile: "raw" })
        const main = v.main
        const page = v.page
        const scroll = find(page, "recordingsScroll")
        const rows = find(page, "signalRows")
        verify(page.pinnedViewer, "the viewer is pinned")
        verify(!scroll.interactive, "the page itself never scrolls")
        const glass = name => find(page, name).mapToItem(main.contentItem, 0, 0).y
        const pinned = ["openedTitle", "chartFrame", "chartSplitter"]
        const before = pinned.map(glass)
        const splitter = find(page, "chartSplitter")
        const pinnedFoot = splitter.mapToItem(main.contentItem, 0, splitter.height).y
        const listTop = rows.mapToItem(main.contentItem, 0, 0).y
        const listFoot = rows.mapToItem(main.contentItem, 0, rows.height).y
        const pageFoot = scroll.mapToItem(main.contentItem, 0, scroll.height).y
        const firstTop = rows.itemAtIndex(0).mapToItem(main.contentItem, 0, 0).y
        console.log("pinned view at 384 x 832: the pinned part ends at " + pinnedFoot
                    + ", the list runs " + listTop + " to " + listFoot + " of " + pageFoot
                    + ", rows at first sight " + ((listFoot - firstTop) / page.rowHeight).toFixed(1)
                    + ", with the head scrolled off " + ((listFoot - listTop) / page.rowHeight).toFixed(1))
        verify(listTop >= pinnedFoot, "the list under the splitter")
        verify(listFoot >= pageFoot - Theme.pageMargin - 1 && listFoot <= pageFoot + 0.5,
               "the list's foot is the page's: " + listFoot + " of " + pageFoot)
        verify(firstTop > glass("recordingFilter"), "the head over the rows")
        verify(!find(page, "recordingInfo").visible, "About waits for its view")
        compare(scrollersOf(page).join(", "), "signalRows", "Values: the rows alone scroll")

        // The splitter dragged past the page stops with two rows under it
        const kept = page.chartHeight
        page.chartHeight = 2000
        verify(waitForPolish(main), "a tall chart")
        const tallFoot = splitter.mapToItem(main.contentItem, 0, splitter.height).y
        verify(tallFoot <= pageFoot - 2 * page.rowHeight + 0.5, "the splitter ends at " + tallFoot)
        verify(rows.height >= 2 * page.rowHeight - 1, "two rows still show: " + rows.height)
        compare(scroll.contentY, 0, "the page stays")
        page.chartHeight = kept
        verify(waitForPolish(main), "the chart's height again")
        compare(splitter.mapToItem(main.contentItem, 0, splitter.height).y, pinnedFoot, "as it was")

        // A swipe on the rows moves the rows alone
        mouseDrag(rows, rows.width / 2, rows.height - 30, 0, -250)
        tryVerify(() => rows.contentY > rows.originY + 40, 3000, "the swipe scrolls the rows")
        tryVerify(() => !rows.moving, 5000, "the rows settle")
        compare(pinned.map(glass).join(", "), before.join(", "), "the title row and the chart stay")
        compare(scroll.contentY, 0, "the page stays")
        rows.positionViewAtEnd()
        verify(waitForPolish(main), "at the end")
        const last = rows.itemAtIndex(rows.count - 1)
        verify(last !== null && last.mapToItem(main.contentItem, 0, last.height).y <= listFoot + 0.5,
               "the last row is reached")
        compare(pinned.map(glass).join(", "), before.join(", "), "the title row and the chart stay at the end")
        rows.positionViewAtBeginning()
        verify(waitForPolish(main), "back at the start")

        // Filtered, the count closes the rows' list and the list keeps the page's foot
        const signalFilter = find(page, "recordingFilter").filter
        signalFilter.text = "Signal 1"
        const count = find(page, "filterCount")
        tryVerify(() => count.visible && rows.count > 0 && rows.count < 100, 2000, "the filter narrows the rows")
        verify(waitForPolish(main), "filtered")
        rows.positionViewAtEnd()
        verify(waitForPolish(main), "at the filtered end")
        const countTop = count.mapToItem(main.contentItem, 0, 0).y
        const lastShown = rows.itemAtIndex(rows.count - 1)
        verify(countTop >= lastShown.mapToItem(main.contentItem, 0, lastShown.height).y - 0.5
               && count.mapToItem(main.contentItem, 0, count.height).y <= listFoot + 0.5,
               "the count under the last row, inside the list: " + countTop)
        compare(rows.mapToItem(main.contentItem, 0, rows.height).y, listFoot, "the list keeps its foot")
        signalFilter.text = ""
        tryVerify(() => !count.visible && rows.count === 100, 2000, "the filter cleared")
        rows.positionViewAtBeginning()
        verify(waitForPolish(main), "back at the start again")

        find(page, "detailView_frames").clicked()
        const table = find(page, "viewerFrameTable")
        tryVerify(() => table.visible && find(table, "frameList").count > 0, 10000, "Frames: the table")
        verify(waitForPolish(main), "Frames laid out")
        verify(!rows.visible, "the rows give way to the table")
        verify(scrollersOf(page).length <= 1, "Frames: one list at most: " + scrollersOf(page).join(", "))
        const tableFoot = table.mapToItem(main.contentItem, 0, table.height).y
        verify(tableFoot >= pageFoot - Theme.pageMargin - 1 && tableFoot <= pageFoot + 0.5,
               "the table's foot is the page's: " + tableFoot + " of " + pageFoot)
        const names = find(table, "frameColumnNames")
        const firstFrame = find(table, "frameList").itemAtIndex(0)
        verify(names.mapToItem(main.contentItem, 0, 0).y > glass("framesFilterToggle"),
               "the column names under the head's tools")
        const namesGap = firstFrame.mapToItem(main.contentItem, 0, 0).y
                         - names.mapToItem(main.contentItem, 0, names.height).y
        verify(namesGap >= -0.5 && namesGap <= 4.5, "and right over the first frame: " + namesGap)
        const listInTable = find(table, "frameList").mapToItem(table, 0, 0).y
        verify(listInTable <= 4.5, "the names left no gap at the table's top: " + listInTable)

        find(page, "detailView_about").clicked()
        const findings = find(page, "findingList")
        tryVerify(() => findings.visible && findings.contentY <= findings.originY + 0.5, 2000, "About")
        verify(waitForPolish(main), "About laid out")
        verify(!rows.visible && !table.visible, "the rows and the table give way to About")
        const info = find(page, "recordingInfo")
        const infoTop = info.mapToItem(main.contentItem, 0, 0).y
        const infoFoot = info.mapToItem(main.contentItem, 0, info.height).y
        verify(info.visible && infoTop >= pinnedFoot && infoFoot <= pageFoot + 0.5,
               "the recording's line on screen with no scroll: " + infoTop + " to " + infoFoot)
        verify(!find(page, "aboutHeading").visible, "the switch names About, no heading does")
        verify(scrollersOf(page).length <= 1, "About: one list at most: " + scrollersOf(page).join(", "))
        compare(pinned.map(glass).join(", "), before.join(", "), "the chart stays in every view")
        find(page, "detailView_values").clicked()
        verify(waitForPolish(main), "Values again")
        verify(rows.visible && !info.visible, "Values: the rows, About gone")
    }

    // A head that grows keeps its list at the start when it was there, and
    // leaves a list scrolled on where it was
    function test_aGrowingHeadKeepsItsListsPlace() {
        const v = uprightViewer({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                                  folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const page = v.page
        find(page, "detailView_frames").clicked()
        const table = find(page, "viewerFrameTable")
        const list = find(table, "frameList")
        tryVerify(() => table.visible && list.count > 20, 10000, "the frames")
        verify(waitForPolish(v.main), "laid out")
        tryVerify(() => list.contentY <= list.originY + 0.5, 2000, "the table at its start")
        page.framesFilterOpen = true
        tryVerify(() => find(page, "filterPanel").visible && list.contentY <= list.originY + 0.5,
                  2000, "the grown head shows from its top")
        page.framesFilterOpen = false
        tryVerify(() => list.contentY <= list.originY + 0.5, 2000, "and when it shrinks")
        list.positionViewAtEnd()
        verify(waitForPolish(v.main), "scrolled to the end")
        const scrolled = list.contentY
        page.framesFilterOpen = true
        verify(waitForPolish(v.main), "grown")
        verify(waitForPolish(v.main), "and settled")
        verify(list.contentY > list.originY + 100, "a list scrolled on stays scrolled: " + list.contentY)
        compare(list.contentY, scrolled, "where it was")
        page.framesFilterOpen = false
    }

    // A recording of values alone has no Frames view: Values | About
    function test_aRecordingWithoutFramesShowsTwoViews() {
        const folder = Harness.importCsvFixture("logger-a", "20261105-220324 Logger A")
        verify(folder !== "", "the fixture imports")
        const v = uprightViewer({ title: "Logger A", uri: "", source: "", folder: folder, profile: "" })
        verify(v.page.pinnedViewer, "pinned")
        verify(!find(v.page, "detailView_frames").visible, "no Frames")
        verify(find(v.page, "detailView_values").visible && find(v.page, "detailView_about").visible,
               "Values and About")
        find(v.page, "detailView_about").clicked()
        verify(waitForPolish(v.main), "About laid out")
        verify(find(v.page, "recordingInfo").visible, "About shows the recording's line")
    }

    // Upright, a decode chosen in About reopens the recording and leaves the
    // viewer in About with the choice kept
    function test_aDecodeChosenInAboutStaysInAbout() {
        const v = uprightViewer({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                                  folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const page = v.page
        find(page, "detailView_about").clicked()
        verify(waitForPolish(v.main), "About laid out")
        const choice = find(page, "decodeChoice")
        verify(choice.visible, "the decode choice in About")
        const obd2 = choice.indexOfValue("obd2")
        verify(obd2 > 0, "obd2 is offered")
        choice.activated(obd2)
        const info = find(page, "recordingInfo")
        tryVerify(() => info.visible && info.text.indexOf("through obd2") > 0, 10000, "decoded through obd2")
        verify(page.aboutShown, "still About")
        compare(choice.currentIndex, obd2, "the choice kept")
        find(page, "detailView_values").clicked()
        verify(!choice.visible, "Values: no choice")
        find(page, "detailView_about").clicked()
        tryCompare(choice, "currentIndex", obd2, 2000, "the choice as decoded when About shows again")
        page.openPart.backToList()
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        tryVerify(() => find(page, "signalRows").visible, 10000, "a recording opened from the list")
        compare(page.detailView, "values", "opens at Values")
    }

    // On its side the viewer gives the page to the view the switch names:
    // Values the chart, Frames the table, and the switch leads the zoom
    // row in both, so either leads back
    function test_theSideViewFollowsTheSwitch() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 832
        main.height = 384
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const chart = find(page, "chartFrame")
        tryVerify(() => chart.visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        verify(Theme.shortLayout && page.chartMaximized, "on its side the chart has the page")
        const table = find(page, "viewerFrameTable")
        const frames = find(page, "detailView_frames")
        const values = find(page, "detailView_values")
        const scroll = find(page, "recordingsScroll")
        verify(!table.visible, "Values: no table")
        verify(frames.visible, "the switch shows on its side")
        verify(frames.mapToItem(scroll, 0, 0).y > chart.mapToItem(scroll, 0, 0).y + chart.height - 1,
               "in the zoom row, under the chart")
        tap(frames, frames.width / 2, frames.height / 2)
        tryVerify(() => page.framesShown && table.visible, 2000, "Frames: the table")
        verify(waitForPolish(main), "laid out")
        verify(!chart.visible, "and no chart")
        verify(page.chartMaximized, "still the side view")
        tryVerify(() => find(table, "frameList").count > 0, 10000, "the rows")
        const foot = table.mapToItem(scroll, 0, table.height).y
        verify(foot >= scroll.height - Theme.pageMargin - 1 && foot <= scroll.height + 0.5,
               "the table runs to the page's foot: " + foot + " of " + scroll.height)
        verify(table.height >= 3 * Theme.touchTarget, "the table is " + table.height)
        verify(values.visible && frames.mapToItem(scroll, 0, 0).y < table.mapToItem(scroll, 0, 0).y,
               "the switch above the table")
        tap(values, values.width / 2, values.height / 2)
        tryVerify(() => !page.framesShown && chart.visible, 2000, "Values: the chart again")
        verify(!table.visible, "no table")
    }

    // Upright on the narrowest phone, the maximized viewer's switch has a
    // row of its own above the zoom row, which would otherwise push the
    // page past the glass
    function test_aNarrowMaximizedViewerKeepsTheSwitchInARow() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 320
        main.height = 800
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const chart = find(page, "chartFrame")
        tryVerify(() => chart.visible, 10000, "the recording opens")
        if (!page.chartMaximized)
            page.refreshPart.toggleMaximized()
        try {
            verify(waitForPolish(main), "laid out")
            const frames = find(page, "detailView_frames")
            const zoom = find(page, "zoomAll")
            verify(frames.visible, "the switch shows maximized")
            verify(frames.mapToItem(main.contentItem, 0, frames.height).y
                   <= zoom.mapToItem(main.contentItem, 0, 0).y + 0.5, "in a row above the zoom row")
            const over = []
            pastTheEdge(page, main.contentItem, over)
            compare(over, [], "past the edge: " + over.join(", "))
            tap(frames, frames.width / 2, frames.height / 2)
            const table = find(page, "viewerFrameTable")
            tryVerify(() => page.framesShown && table.visible && !chart.visible, 2000,
                      "Frames: the table has the page")
            verify(waitForPolish(main), "laid out")
            pastTheEdge(page, main.contentItem, over)
            compare(over, [], "past the edge in Frames: " + over.join(", "))
        } finally {
            page.detailView = "values"
            if (page.chartMaximized)
                page.refreshPart.toggleMaximized()
        }
    }

    // The Import panel's preview on a phone: every control in it is a
    // touch's size (48 on the glass at any zoom, in the layout's units
    // Theme.touchTarget), and none is past the window's edge
    function test_theImportPreviewsTargetsAreATouchsSize() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        tryCompare(Theme, "compact", true)
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.openPart.chooseImportFile(Harness.writeScratchFile("targets.csv", guessFile))
        tryVerify(() => !page.importGuessing && !Bridge.recordingImport.busy, 10000,
                  "the guess ends")
        const preview = find(page, "guessPreview")
        verify(preview.visible)
        verify(waitForPolish(main), "laid out")
        const seen = []
        const small = []
        const walk = item => {
            for (const child of item.children) {
                if (child.visible && child.width > 0
                        && (child.pressed !== undefined || child.placeholderText !== undefined)) {
                    const top = child.mapToItem(main.contentItem, 0, 0).y
                    const bottom = child.mapToItem(main.contentItem, 0, child.height).y
                    seen.push(child.objectName || String(child))
                    if (bottom - top < Theme.touchTarget - 0.5)
                        small.push((child.objectName || String(child)) + " " + Math.round(bottom - top))
                }
                walk(child)
            }
        }
        walk(preview)
        verify(seen.length >= 10, "controls found: " + seen.join(", "))
        compare(small.length, 0, "under " + Theme.touchTarget + ": " + small.join(", "))
    }

    // The text fields are a touch's size on a phone without a page asking
    function test_theFieldsAreATouchsSizeOnAPhone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        tryCompare(Theme, "compact", true)
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        verify(waitForPolish(main), "laid out")
        const field = find(live, "filterText")
        verify(field && field.visible, "Find a signal shows")
        const h = field.mapToItem(main.contentItem, 0, field.height).y
                  - field.mapToItem(main.contentItem, 0, 0).y
        verify(h >= Theme.touchTarget - 0.5, "Find a signal is " + h + " tall")
        verify(field.width >= 80, "and not squeezed out: " + field.width)
        // A signal row's plot box has no text and is a touch's width, and
        // the row still fits the phone
        const box = find(live, "plotCheck")
        verify(box && box.visible, "a plot box shows")
        verify(box.width >= Theme.touchTarget - 0.5, "the plot box is " + box.width + " wide")
        const row = box.parent.parent
        const edge = row.mapToItem(main.contentItem, row.width, 0).x
        const names = []
        const walk = item => {
            for (const c of item.children) {
                if (c.visible && c.width > 0
                        && c.mapToItem(main.contentItem, c.width, 0).x > edge + 0.5)
                    names.push(c.objectName || String(c))
                walk(c)
            }
        }
        walk(row)
        compare(names.length, 0, "past the row's edge: " + names.join(", "))
    }

    // Live's hidden-signals list: Show is a touch's size and inside the
    // width, and each opened row's words end inside it
    function test_theHiddenListFitsAPhone_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }
    function test_theHiddenListFitsAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 832
        tryCompare(Theme, "compact", true)
        compare(Bridge.connectSession("mock://demo?profile=obd2"), "", "connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        verify(waitForPolish(main), "laid out")
        tryVerify(() => find(live, "hiddenNote").visible && Bridge.signalModel.hiddenCount > 1, 5000)
        const toggle = find(live, "hiddenToggle")
        const inside = item => item.mapToItem(main.contentItem, item.width, 0).x <= main.width + 0.5
                               && item.mapToItem(main.contentItem, 0, 0).x >= -0.5
        verify(waitForPolish(main), "laid out with the note")
        tryVerify(() => toggle.height >= Theme.touchTarget - 0.5, 2000,
                  "Show is a touch target tall")
        verify(inside(toggle), "Show is inside the width")
        mouseClick(toggle)
        const list = find(live, "hiddenList")
        tryVerify(() => list.visible, 2000, "the list opens")
        verify(waitForPolish(main), "laid out again")
        verify(inside(list), "the list is inside the width")
        let drawn = 0
        for (let r = 0; r < list.count; ++r) {
            list.positionViewAtIndex(r, ListView.Contain)
            const row = list.itemAtIndex(r)
            if (!row)
                continue
            ++drawn
            const name = find(row, "hiddenName")
            const why = find(row, "hiddenWhy")
            verify(inside(name) && inside(why), "the row is inside the width")
            // The reason wraps whole: its lines are all there, none cut
            verify(why.width > 100, "the reason keeps its width: " + why.width)
            verify(why.contentHeight <= why.height + 0.5, "the reason is whole: " + why.text)
            verify(why.contentWidth <= why.width + 0.5, "and fits its width: " + why.text)
            verify(!why.truncated, "not elided: " + why.text)
        }
        verify(drawn > 0, "rows are drawn")
        verify(inside(toggle), "Hide stays reachable")
        mouseClick(toggle)
        tryVerify(() => !list.visible, 2000, "the list closes")
    }

    // A phone's density: upright (narrower than 720) and on its side
    // (shorter than 480), and not in a desktop's window
    function test_densityFollowsThePhonesShape_data() {
        return [{ tag: "upright", w: 384, h: 832, dense: true },
                { tag: "on its side", w: 832, h: 384, dense: true },
                { tag: "a laptop", w: 1280, h: 650, dense: false }]
    }
    function test_densityFollowsThePhonesShape(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = data.h
        tryCompare(Theme, "dense", data.dense)
        compare(Theme.pageMargin, data.dense ? 10 : 16)
        const live = find(main.contentItem, "livePage")
        compare(live.rowHeight, data.dense ? 36 : 44, "Live's rows follow it")
    }

    // The Bluetooth permission is not asked for at launch: the app opens on
    // another page, so the ask waits until Settings is seen, and at 1400
    // wide it is then one ask, with Connection beside the list
    function test_theBluetoothAskWaitsForSettingsToBeSeen() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1400
        main.height = 800
        verify(waitForPolish(main), "laid out")
        const page = find(main.contentItem, "settingsPage")
        verify(main.currentPage !== main.settingsPage, "the app opens elsewhere")
        verify(!page.visible, "Settings is hidden")
        compare(page.bluetoothAsks, 0, "no ask at launch")
        main.gotoPage(main.settingsPage)
        tryCompare(page, "shownKey", "connection")   // wide: Connection beside the list
        compare(page.bluetoothAsks, 1, "seen: one ask")
    }

    // The phone's tab bar upright: every tab a touch's size on the glass
    // at any zoom that shows the bar (67 % is the least at 384; at 50 %
    // the layout passes 720 and the sidebar shows), the targets ending a
    // gap above the window's bottom, and with the system's bars over the
    // window (Android edge to edge) no tab inside the bottom inset, the
    // bar's color reaching the window's edge and the header's row below
    // the top inset
    function test_theTabBarsTargetsAreATouchsSize_data() {
        const none = { left: 0, top: 0, right: 0, bottom: 0 }
        return [{ tag: "100 %", zoom: 1.0, insets: none },
                { tag: "67 %", zoom: 0.67, insets: none },
                { tag: "150 %", zoom: 1.5, insets: none },
                { tag: "the system's bars", zoom: 1.0,
                  insets: { left: 0, top: 24, right: 0, bottom: 48 } },
                { tag: "the system's bars at 67 %", zoom: 0.67,
                  insets: { left: 0, top: 24, right: 0, bottom: 48 } }]
    }

    function test_theTabBarsTargetsAreATouchsSize(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        Theme.zoom = data.zoom
        main.testInsets = data.insets
        verify(waitForPolish(main), "laid out")
        const bar = find(main.contentItem, "tabBar")
        tryVerify(() => bar.visible, 2000, "the tab bar shows upright")
        const glass = main.contentItem.height
        const floor = glass - data.insets.bottom - 8
        compare(bar.count, 5, "the five tabs")
        for (let i = 0; i < bar.count; ++i) {
            const tab = bar.itemAt(i)
            const r = onGlass(tab, main)
            verify(r.height >= 48 - 0.5, tab.text + " is " + r.height + " tall on the glass")
            verify(r.width >= 48 - 0.5, tab.text + " is " + r.width + " wide on the glass")
            verify(r.y + r.height <= floor + 0.5,
                   tab.text + " ends at " + (r.y + r.height) + ", past " + floor)
        }
        const back = onGlass(find(bar, "tabBarBackground"), main)
        verify(back.y + back.height >= glass - 0.5,
               "the bar's color ends at " + (back.y + back.height) + " of " + glass)
        const head = onGlass(find(main.contentItem, "headerRow"), main)
        verify(head.y >= data.insets.top - 0.5,
               "the header's row starts at " + head.y + ", inside the top inset")
        const rowHeight = Theme.toolBarHeight * Theme.zoom
        verify(head.height >= rowHeight - 0.5,
               "the header's row is " + head.height + " tall, under " + rowHeight)
        const headBar = onGlass(find(main.contentItem, "headerBar"), main)
        verify(headBar.y <= 0.5, "the header's color starts at the window's top")
    }

    // At the sidebar's width (a phone on its side) there's no tab bar, and
    // the pages still keep off the bottom inset
    function test_thePagesKeepOffTheBottomInsetBesideTheSidebar() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 832
        main.height = 384
        main.testInsets = { left: 0, top: 24, right: 0, bottom: 30 }
        verify(waitForPolish(main), "laid out")
        verify(!find(main.contentItem, "tabBar").visible, "the sidebar instead")
        const home = find(main.contentItem, "homePage")
        main.gotoPage(Array.from(home.parent.children).indexOf(home))
        verify(waitForPolish(main), "laid out")
        const r = onGlass(home.parent, main)
        verify(r.y + r.height <= main.contentItem.height - 30 + 0.5,
               "the pages end at " + (r.y + r.height) + " of " + main.contentItem.height)
    }
}
