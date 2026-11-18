// File/layer: app/qml/DashPages.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit and its parts (QmlTest.dashedit_*), QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls

pragma ComponentBehavior: Bound

// The Dashboard's pages: one SwipeView of grids, and the edit panels they open, a part of DashboardPage

SwipeView {
    id: swipe
    required property DashboardPage host

    Component {
        id: cellPanel
        DashCellPanel { host: swipe.host }
    }

    // Add's kinds, each with a still preview
    Component {
        id: addPanel
        DashAddPanel { host: swipe.host }
    }

    // A signal for Add's kind, or the edited cell's new one: the session's
    // rows with references, or those of the profile Settings has chosen
    Component {
        id: signalPanel
        DashSignalPanel { host: swipe.host }
    }
    Component {
        id: connectPanel
        ConnectList {
            objectName: "dashConnectList"
            recents: swipe.host.connectUris
            vehicleUri: swipe.host.connectVehicleUri
            adapters: swipe.host.connectAdapters
            profileWords: swipe.host.connectProfileWords
            tapped: swipe.host.connectTapped
            onPicked: uri => swipe.host.connectTapped = uri
            onConnectTo: uri => swipe.host.connectToUri(uri)
            onOtherAddress: swipe.host.otherAddressRequested()
            onShowLog: swipe.host.logRequested()
            onShown: swipe.host.connectListShown()
        }
    }

    objectName: "dashPages"
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.top: swipe.host.drivePart.driving ? swipe.host.driveStatusItem.bottom : swipe.host.topBarItem.bottom
    anchors.bottom: swipe.host.indicatorItem.visible ? swipe.host.indicatorItem.top : parent.bottom
    anchors.bottomMargin: swipe.host.drivePart.driving ? swipe.host.drivePart.driveInsetBottom : 0
    clip: true
    interactive: swipe.host.form === "phone" && swipe.host.held < 0
    // The page shown goes in once every page is in: the first page
    // added makes the index 0, which would override a stored one
    onCountChanged: {
        if (count === swipe.host.pages.length && currentIndex !== swipe.host.pageIndex)
            setCurrentIndex(swipe.host.pageIndex)
    }
    onCurrentIndexChanged: {
        if (count === swipe.host.pages.length && currentIndex >= 0) {
            if (currentIndex !== swipe.host.pageIndex)
                swipe.host.commitTyping()
            swipe.host.pageIndex = currentIndex
        }
    }

    Repeater {
        model: swipe.host.pages.length

        Flickable {
            id: pageView
            required property int index
            readonly property var page: swipe.host.pages[index]
            readonly property var cells: page && Array.isArray(page.cells) ? page.cells : []
            objectName: "dashPage_" + index
            clip: true
            Connections {
                target: swipe.host
                function onRevealRequested(cell) {
                    if (!pageView.shown)
                        return
                    const item = cellRepeater.itemAt(cell)
                    if (!item)
                        return
                    const top = grid.y + item.y
                    const bottom = top + item.height
                    const max = Math.max(0, pageView.contentHeight - pageView.height)
                    if (top < pageView.contentY || bottom > pageView.contentY + pageView.height)
                        pageView.contentY = Math.max(0, Math.min(max, top - swipe.host.panelMargin))
                }
            }
            contentWidth: width
            contentHeight: grid.y + grid.height + swipe.host.panelMargin
            interactive: swipe.host.held < 0
            // Shown whenever there's more below, not only while
            // scrolling, so a short window says it scrolls
            ScrollBar.vertical: ScrollBar {
                policy: pageView.contentHeight > pageView.height ? ScrollBar.AlwaysOn
                                                                 : ScrollBar.AsNeeded
            }

            // The grid (section 4): the phone's 6 columns upright and
            // 12 on its side, the desktop's one per 220 px from 2 to 12
            readonly property real fullGridWidth: Math.max(0, width - 2 * swipe.host.panelMargin)
            // Counted on the full width, so opening a panel narrows the cells and moves none
            readonly property int columns: swipe.host.form === "phone"
                ? (swipe.host.width > swipe.host.height ? 12 : 6)
                : Math.max(2, Math.min(12, Math.round(fullGridWidth / 220)))
            readonly property real sideGridWidth:
                Math.max(0, fullGridWidth - swipe.host.sideColumnWidth - swipe.host.panelMargin)
            readonly property bool sideColumn: !swipe.host.phoneForm && !swipe.host.drivePart.driving && panelHost.active
                && (sideGridWidth - (columns - 1) * swipe.host.cellGap) / columns >= swipe.host.sideCellMinWidth
            readonly property real gridWidth: sideColumn ? sideGridWidth : fullGridWidth
            readonly property real columnWidth: Math.max(0, (gridWidth - (columns - 1)
                                                             * swipe.host.cellGap) / columns)
            readonly property bool shown: index === swipe.host.pageIndex
            // In edit mode the page ends with a "+" cell while it has room
            readonly property bool addCell: swipe.host.editing && swipe.host.shown
                                            && DashboardBook.canAddCell(swipe.host.doc, index)
            // While a held cell moves, the order its drop would make
            readonly property bool reflowing: swipe.host.shown && swipe.host.dropAt >= 0
            readonly property var ordered: reflowing
                ? DashboardBook.reordered(cells, swipe.host.held, swipe.host.dropAt) : cells
            readonly property var placed: addCell
                ? ordered.concat([{ w: swipe.host.spanScale, h: 2 * swipe.host.spanScale }]) : ordered
            // Edit mode and a held cell show the true sizes
            readonly property var boxes: {
                const b = DashboardBook.place(placed, columns)
                return swipe.host.joined && !swipe.host.editing && swipe.host.held < 0
                    ? DashboardBook.stretched(placed, b, columns, swipe.host.builtInShown) : b
            }
            readonly property var seams: swipe.host.joined
                ? DashboardBook.seams(placed, boxes, columns) : []
            readonly property int tallest: placed.reduce((m, c) => Math.max(m, c.h), 1)
            // The row unit is a quarter of a column, so h = 4w is
            // square; never so tall that the tallest cell overflows
            // what the page shows, down to a floor of 160 px, below
            // which the page scrolls instead
            readonly property real squarePitch: (columnWidth + swipe.host.cellGap) / 4
            readonly property real pagePitch: Math.max(1, Math.min(squarePitch,
                (Math.max(160, height - swipe.host.gridTop - swipe.host.panelMargin) + swipe.host.cellGap) / tallest))
            // The drive view's panel reaches the screen's foot, its rows
            // up to twice the square; a page that already scrolls keeps its pitch
            readonly property bool fills: swipe.host.drivePart.driving && swipe.host.phoneForm && rows > 0
            readonly property real pitch: fills
                ? Math.max(pagePitch, Math.min(2 * squarePitch,
                                               (height - swipe.host.gridTop + swipe.host.cellGap) / rows))
                : pagePitch
            readonly property int rows: boxes.reduce((m, b) => Math.max(m, b.y + b.h), 0)
            // The drop's places are measured on this grid
            onColumnsChanged: swipe.host.cancelHold()
            onPitchChanged: swipe.host.cancelHold()

            function boxLeft(b) { return b.x * (columnWidth + swipe.host.cellGap) }
            function boxTop(b) { return b.y * pitch }
            function boxWidth(b) { return b.w * columnWidth + (b.w - 1) * swipe.host.cellGap }
            function boxHeight(b) { return Math.max(0, b.h * pitch - swipe.host.cellGap) }

            // ---- The held cell (section 4)

            property var dropCenters: []     // the held cell's center at each place
            property size heldSize: Qt.size(0, 0)
            property point grab: Qt.point(0, 0)      // the finger within the cell
            property point pressedAt: Qt.point(0, 0)
            property point finger: Qt.point(0, 0)
            property real dragX: 0
            property real dragY: 0
            property bool leftHome: false    // the drop place went past the cell's own

            function lift(i, scene) {
                swipe.host.commitTyping()
                const at = grid.mapFromItem(null, scene.x, scene.y)
                const b = DashboardBook.place(placed, columns)[i]
                heldSize = Qt.size(boxWidth(b), boxHeight(b))
                // A press on the stretched part lifts the cell under
                // the finger at its true size, not past its edge
                grab = Qt.point(Math.max(0, Math.min(at.x - boxLeft(b), heldSize.width)),
                                Math.max(0, Math.min(at.y - boxTop(b), heldSize.height)))
                pressedAt = scene
                finger = scene
                leftHome = false
                dropCenters = DashboardBook.dropBoxes(cells, i, columns).map(
                    d => ({ x: boxLeft(d) + boxWidth(d) / 2, y: boxTop(d) + boxHeight(d) / 2 }))
                swipe.host.held = i
            }
            function follow(scene) {
                finger = scene
                if (swipe.host.dropAt < 0 && Math.hypot(scene.x - pressedAt.x, scene.y - pressedAt.y)
                        <= Application.styleHints.startDragDistance)
                    return
                const at = grid.mapFromItem(null, scene.x, scene.y)
                dragX = at.x - grab.x
                dragY = at.y - grab.y
                swipe.host.dropAt = DashboardBook.nearest(dropCenters, dragX + heldSize.width / 2,
                                                    dragY + heldSize.height / 2)
                if (swipe.host.dropAt !== swipe.host.held)
                    leftHome = true
            }
            function letGo(canceled) {
                const from = swipe.host.held, to = swipe.host.dropAt, strayed = leftHome
                swipe.host.cancelHold()
                if (canceled)
                    return
                // A tremble past the slop that never left its own place: the
                // menu; a drag elsewhere and back changes nothing
                if (to < 0 || (to === from && !strayed))
                    swipe.host.select(from)
                else if (swipe.host.edit(DashboardBook.moveCell(swipe.host.doc, swipe.host.pageIndex, from, to),
                                   swipe.host.historyPart.stepOf("", qsTr("moved %1").arg(swipe.host.cellName(pageView.cells[from])),
                                               from, to))
                         && swipe.host.selected >= 0)
                    swipe.host.selected = DashboardBook.movedIndex(swipe.host.selected, from, to)
            }
            // Near the view's top or bottom edge the page scrolls, at
            // up to a quarter of the edge's width per tick
            function scrollAtTheEdge() {
                const y = mapFromItem(null, finger.x, finger.y).y
                const edge = Theme.touchTarget
                const by = y < edge ? y - edge : y > height - edge ? y - (height - edge) : 0
                if (by === 0)
                    return
                const bottom = Math.max(0, contentHeight - height)
                contentY = Math.max(0, Math.min(bottom,
                    contentY + Math.max(-edge, Math.min(edge, by)) / 4))
                follow(finger)
            }
            Timer {
                interval: 16
                repeat: true
                running: pageView.reflowing
                onTriggered: pageView.scrollAtTheEdge()
            }

            // A tap between the cells reveals the drive view's bars too
            MouseArea {
                objectName: "driveTapArea"
                enabled: swipe.host.drivePart.driving
                width: pageView.width
                height: Math.max(pageView.height, pageView.contentHeight)
                onClicked: swipe.host.drivePart.driveTap()
            }

            // The edit panel, in the page above its grid (never a Popup)
            // The side column: the panel in a column of its own, as high as
            // the page's view and held there while the grid scrolls
            Flickable {
                id: sideHost
                objectName: "dashSidePanel"
                visible: pageView.sideColumn
                x: pageView.width - swipe.host.panelMargin - swipe.host.sideColumnWidth
                y: pageView.contentY
                z: 1
                width: swipe.host.sideColumnWidth
                height: pageView.height
                clip: true
                contentWidth: width
                contentHeight: panelHost.item
                    ? swipe.host.gridTop + (panelHost.item as Item).height + swipe.host.panelMargin : 0
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            }

            Loader {
                id: panelHost
                objectName: "dashPanel"
                parent: pageView.sideColumn ? sideHost.contentItem : pageView.contentItem
                x: pageView.sideColumn ? 0 : (pageView.width - width) / 2
                y: swipe.host.gridTop
                width: pageView.sideColumn ? swipe.host.sideColumnWidth : pageView.fullGridWidth
                active: (pageView.shown && swipe.host.panel !== "") || swipe.host.connectShown
                sourceComponent: swipe.host.panel === "cell" ? cellPanel
                               : swipe.host.panel === "add" ? addPanel
                               : swipe.host.panel === "signal" ? signalPanel
                               : swipe.host.connectShown ? connectPanel : null
                onLoaded: {
                    pageView.contentY = 0
                    sideHost.contentY = 0
                }
            }

            Label {
                objectName: "dashEmptyPage"
                visible: pageView.cells.length === 0 && !swipe.host.editing && !swipe.host.connectShown
                x: swipe.host.margin
                y: swipe.host.topGap
                width: pageView.gridWidth
                wrapMode: Text.WordWrap
                color: Theme.textDim
                text: qsTr("No gauges on this page. Edit adds them.")
            }

            Item {
                id: grid
                // On whole pixels of the display, so the seams snapped inside it are
                x: swipe.host.snap(pageView.sideColumn ? swipe.host.panelMargin : (pageView.width - width) / 2)
                y: swipe.host.snap(swipe.y + swipe.host.gridTop + (panelHost.item && !pageView.sideColumn
                    ? (panelHost.item as Item).height + swipe.host.gap : 0)) - swipe.y
                width: pageView.gridWidth
                height: Math.max(0, pageView.rows * pageView.pitch - swipe.host.cellGap)

                // The joined panel, under its cells: the flat surface
                // with its edge, and the hairlines between neighbors
                Rectangle {
                    objectName: "dashPanelSurface"
                    anchors.fill: parent
                    visible: swipe.host.joined && pageView.rows > 0
                    color: Theme.surface
                    radius: swipe.host.drivePart.driving ? 0 : Theme.radiusCard
                    // Under the first tab while it's shown: its sheet runs straight in
                    topLeftRadius: swipe.host.stripMeetsPanel && swipe.host.pageIndex === 0 ? 0 : radius
                    border.color: Theme.hairline
                    border.width: swipe.host.drivePart.driving ? 0 : swipe.host.hairlineWidth
                }
                Repeater {
                    model: pageView.seams
                    Rectangle {
                        required property var modelData
                        readonly property real along: modelData.vertical
                            ? modelData.y * pageView.pitch : modelData.x * pageView.columnWidth
                        readonly property real across: swipe.host.snap((modelData.vertical
                            ? modelData.x * pageView.columnWidth : modelData.y * pageView.pitch)
                            - swipe.host.hairlineWidth / 2)
                        readonly property real run: modelData.length
                            * (modelData.vertical ? pageView.pitch : pageView.columnWidth)
                        objectName: "dashSeam"
                        x: modelData.vertical ? across : along
                        y: modelData.vertical ? along : across
                        width: modelData.vertical ? swipe.host.hairlineWidth : run
                        height: modelData.vertical ? run : swipe.host.hairlineWidth
                        color: Theme.hairline
                    }
                }

                Repeater {
                    id: cellRepeater
                    model: pageView.cells.length

                    DashCell {
                        id: placedCell
                        host: swipe.host
                        required property int index
                        readonly property var box: pageView.boxes[pageView.reflowing
                            ? DashboardBook.movedIndex(index, swipe.host.held, swipe.host.dropAt) : index]
                        readonly property bool carried: pageView.reflowing && swipe.host.held === index
                        objectName: "dashCell_" + pageView.index + "_" + index
                        cell: pageView.cells[index]
                        pageNo: pageView.index
                        cellIndex: index
                        later: pageView !== null && Math.abs(pageView.index - swipe.host.pageIndex) > 1
                        // qmllint disable missing-property
                        shown: pageView !== null && pageView.index === swipe.host.pageIndex
                               && Qt.application.state !== Qt.ApplicationSuspended
                               && Qt.application.state !== Qt.ApplicationHidden
                        // qmllint enable missing-property
                        x: carried ? pageView.dragX : box ? pageView.boxLeft(box) : 0
                        y: carried ? pageView.dragY : box ? pageView.boxTop(box) : 0
                        width: box ? pageView.boxWidth(box) : 0
                        height: box ? pageView.boxHeight(box) : 0
                        Behavior on x {
                            enabled: pageView.reflowing && !placedCell.carried
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                        Behavior on y {
                            enabled: pageView.reflowing && !placedCell.carried
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                        onHoldStarted: (scene) => pageView.lift(index, scene)
                        onHoldMoved: (scene) => pageView.follow(scene)
                        onHoldEnded: (canceled) => pageView.letGo(canceled)
                    }
                }

                // The "+" cell, split with Paste while a copy exists
                Item {
                    id: addBox
                    readonly property var box: pageView.boxes[pageView.cells.length]
                    // Off the joined panel's hairlines
                    readonly property real inset: swipe.host.joined ? 4 : 0
                    readonly property bool split: swipe.host.copied !== null
                    readonly property real half: (width - 8) / 2
                    objectName: "dashAddCellBox_" + pageView.index
                    visible: pageView.addCell
                    x: box ? pageView.boxLeft(box) + inset : 0
                    y: box ? pageView.boxTop(box) + inset : 0
                    width: box ? pageView.boxWidth(box) - 2 * inset : 0
                    height: box ? Math.max(Theme.touchTarget,
                                           pageView.boxHeight(box) - 2 * inset) : 0
                    ThemedButton {
                        objectName: "dashAddCell_" + pageView.index
                        width: addBox.split ? addBox.half : addBox.width
                        height: addBox.height
                        text: "+"
                        font.pixelSize: Theme.fontHero
                        Accessible.name: qsTr("Add a gauge")
                        onClicked: swipe.host.addFlowPart.startAdd()
                    }
                    ThemedButton {
                        objectName: "dashPasteCell_" + pageView.index
                        visible: addBox.split
                        x: addBox.width - width
                        width: addBox.half
                        height: addBox.height
                        enabled: swipe.host.canPaste
                        text: qsTr("Paste")
                        Accessible.name: qsTr("Paste the copied gauge")
                        onClicked: swipe.host.pasteAfter(swipe.host.shownCells.length - 1)
                    }
                }
            }
        }
    }
}
