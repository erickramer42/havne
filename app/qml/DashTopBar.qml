// File/layer: app/qml/DashTopBar.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The Dashboard's header, edit mode's bars and questions, and the
// desktop's page strip, a part of DashboardPage

ColumnLayout {
    id: topBar
    required property DashboardPage host
    // The page strip's Loader, for the page
    readonly property Loader stripLoader: stripHost
    visible: !topBar.host.driving
    // Over the pages, so the shown tab covers the panel's top line
    z: 1
    x: (topBar.host.width - width) / 2
    y: topBar.host.topGap
    width: Math.max(0, topBar.host.width - 2 * topBar.host.margin)
    spacing: topBar.host.gap / 2

    PageHeader {
        id: header
        Layout.fillWidth: true
        title: topBar.host.phoneForm ? "" : qsTr("Dashboard")
        spacing: topBar.host.phoneForm ? 8 : 10
        // At a narrow width the row's buttons are compact, the link
        // state gives way first and the title last, so the row fits
        // whatever the font's widths
        titleShrinks: true
        // Reset peaks and Edit at a phone's touch size
        readonly property real touchMin: Theme.touchTarget

        // The desktop's link state follows the title. No width of
        // its own: it takes what the title and buttons leave
        Loader {
            objectName: "linkInRow"
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            active: !topBar.host.phoneForm
            visible: active
            sourceComponent: linkState
        }
        // Drive, at the start of the phone's row. Its tools sit at the
        // right. With no session it stays, and says what it waits for
        ThemedButton {
            id: driveButton
            objectName: "dashDrive"
            flat: true
            compact: true
            Layout.minimumWidth: Theme.touchTarget
            visible: topBar.host.phoneForm
            enabled: Bridge.connected && !topBar.host.editing
            text: !Bridge.connected ? qsTr("Connect first")
                : topBar.host.driving ? qsTr("Exit drive") : qsTr("Drive")
            Accessible.name: !Bridge.connected ? qsTr("Connect first: a session shows the gauges full screen")
                           : topBar.host.driving ? qsTr("Exit drive: leave the full-screen gauges")
                                          : qsTr("Drive: show the gauges full screen")
            onClicked: topBar.host.driving ? topBar.host.leaveDrive(true) : topBar.host.enterDrive()
        }
        Item {
            objectName: "toolRowStart"
            Layout.fillWidth: true
            visible: topBar.host.phoneForm
        }
        // A row too narrow for every tool (a small window, a phone zoomed
        // far in, or no session, whose Drive words are longer) drops Reset
        // peaks, which edit mode's Pages row then offers (moreResetPeaks).
        // Judged against the bar's width (the row can't shrink under its
        // tools) and the tools' own widths, never the row's: no loop.
        // The layout rounds each width up to a whole pixel, and each item
        // shown ahead of the tools (the fills too) takes a spacing
        readonly property real toolsWidth: {
            let ahead = 0
            for (const item of children) {
                if (item === resetPeaksButton)
                    break
                if (item.visible)
                    ahead += spacing + (item === driveButton ? Math.ceil(item.implicitWidth) : 0)
            }
            return ahead + Math.ceil(resetPeaksButton.implicitWidth) + spacing
                + Math.ceil(editButton.implicitWidth)
                + (nightLoader.active ? spacing + Math.ceil(nightLoader.implicitWidth) : 0)
        }
        readonly property bool holdsResetPeaks: topBar.width >= Math.max(topBar.host.toolRowFull, toolsWidth)
        ThemedButton {
            id: resetPeaksButton
            objectName: "resetPeaks"
            flat: true
            compact: topBar.host.compact || topBar.host.phoneForm
            visible: header.holdsResetPeaks
            text: qsTr("Reset peaks")
            onClicked: topBar.host.resetPeaks()
        }
        ThemedButton {
            id: editButton
            objectName: "dashEdit"
            compact: topBar.host.compact || topBar.host.phoneForm
            horizontalPadding: topBar.host.phoneForm ? 18 : (compact ? 6 : 8)
            visible: !topBar.host.editing && !topBar.host.driving
            text: qsTr("Edit")
            onClicked: topBar.host.startEditing()
        }
        Loader {
            id: nightLoader
            objectName: "toolRowNight"
            active: topBar.host.phoneForm
            visible: active
            sourceComponent: topBar.host.nightSwitch
        }
    }

    LogNotice { name: "dashLogNotice" }

    // Edit mode's bar: Undo and Redo where Drive stands outside edit
    // mode, Add and Page, and Done in Edit's place at the end. Compact
    // on the phone, and Add's words shortened on a narrow page, so the
    // row holds them at a 320 px width and a large font
    RowLayout {
        objectName: "dashEditBar"
        readonly property real touchMin: Theme.touchTarget
        Layout.fillWidth: true
        visible: topBar.host.editing
        // Tighter on a phone, where a wider face and a large font need the room
        spacing: topBar.host.form === "phone" ? 4 : 8
        // Disabled rather than hidden with nothing to take back, so the row never moves
        ThemedButton {
            objectName: "dashUndo"
            flat: true
            enabled: topBar.host.canUndo
            text: qsTr("Undo")
            Accessible.name: topBar.host.canUndo ? qsTr("Undo: %1").arg(topBar.host.undoWords) : qsTr("Undo")
            onClicked: topBar.host.undo()
        }
        ThemedButton {
            objectName: "dashRedo"
            flat: true
            enabled: topBar.host.canRedo
            text: qsTr("Redo")
            Accessible.name: topBar.host.canRedo ? qsTr("Redo: %1").arg(topBar.host.redoWords) : qsTr("Redo")
            onClicked: topBar.host.redo()
        }
        Item { Layout.fillWidth: true }
        ThemedButton {
            objectName: "dashAdd"
            compact: topBar.host.form === "phone"
            enabled: DashboardBook.canAddCell(topBar.host.doc, topBar.host.pageIndex)
            text: topBar.host.compact ? qsTr("Add") : qsTr("Add gauge")
            Accessible.name: qsTr("Add a gauge")
            onClicked: topBar.host.startAdd()
        }
        ThemedButton {
            objectName: "dashMore"
            compact: topBar.host.form === "phone"
            checkable: true
            checked: topBar.host.moreShown
            text: qsTr("Page")
            onClicked: {
                topBar.host.moreShown = !topBar.host.moreShown
                topBar.host.confirmReset = false
                topBar.host.confirmRemovePage = false
            }
        }
        ThemedButton {
            objectName: "dashDone"
            compact: topBar.host.form === "phone"
            highlighted: true
            text: qsTr("Done")
            onClicked: topBar.host.stopEditing()
        }
    }

    // The desktop's edit mode in the phone form's frame (B1)
    RowLayout {
        objectName: "dashPhoneRow"
        readonly property real touchMin: Theme.touchTarget
        Layout.fillWidth: true
        visible: topBar.host.phoneSwitchShown
        spacing: 8
        ThemedButton {
            objectName: "dashPhoneSwitch"
            compact: topBar.host.phoneForm
            checkable: true
            checked: topBar.host.phoneEdit
            text: qsTr("Phone layout")
            Accessible.name: qsTr("Edit the phone's layout in a phone-sized frame")
            Accessible.description: qsTr("Switching drops the undo history")
            onClicked: topBar.host.setPhoneEdit(!topBar.host.phoneEdit)
        }
        Label {
            objectName: "dashPhoneNote"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 0
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
            text: topBar.host.phoneEdit
                  ? qsTr("The phone's layout, %1 px wide. A phone may be narrower or wider: the columns hold either way.")
                        .arg(topBar.host.phoneFrameWidth)
                  : qsTr("Edit the phone's layout here, then Export layout writes a file for the phone")
        }
    }

    // How a gauge is reached, while no panel is open
    Label {
        objectName: "dashEditHint"
        visible: topBar.host.editing && topBar.host.panel === ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: topBar.host.form === "phone" ? qsTr("Tap a gauge to change it; hold it to move it")
                                    : qsTr("Click a gauge to change it; hold it to move it")
    }

    // The pages' controls, then a gap, then Reset (it replaces every page,
    // so it stands apart) and Reset peaks where the tool row has no room
    Flow {
        id: moreRow
        objectName: "dashMoreRow"
        readonly property real touchMin: Theme.touchTarget
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: 1
        visible: topBar.host.editing && topBar.host.moreShown && !topBar.host.confirmReset && !topBar.host.confirmRemovePage && topBar.host.confirmReplaceKey === ""
                 && topBar.host.presetList === ""
        spacing: 8
        ThemedTextField {
            id: pageName
            objectName: "moreRename"
            width: Math.min(220, topBar.width)
            height: Theme.touchTarget
            maximumLength: DashboardBook.maxLabel
            text: topBar.host.pages[topBar.host.pageIndex] ? topBar.host.pages[topBar.host.pageIndex].name : ""
            placeholderText: qsTr("Page name")
            Accessible.name: qsTr("This page's name")
            onEditingFinished: {
                topBar.host.renamePage(text)
                text = Qt.binding(() => topBar.host.pages[topBar.host.pageIndex]
                                        ? topBar.host.pages[topBar.host.pageIndex].name : "")
            }
        }
        // The phone's only way to a new page; the desktop's is the strip's "+"
        ThemedButton {
            id: moreAddPage
            objectName: "moreAddPage"
            visible: topBar.host.phoneForm
            enabled: DashboardBook.canAddPage(topBar.host.doc)
            text: qsTr("Add page")
            Accessible.name: qsTr("Add a page: choose a blank page or a preset")
            onClicked: topBar.host.openPresets("add")
        }
        ThemedButton {
            id: moreReplace
            objectName: "moreReplace"
            text: qsTr("Replace this page with...")
            onClicked: topBar.host.openPresets("replace")
        }
        // Nothing to move with one page
        ThemedButton {
            id: moreLeft
            objectName: "moreLeft"
            visible: topBar.host.pages.length > 1
            enabled: topBar.host.pageIndex > 0
            text: qsTr("Move page left")
            onClicked: topBar.host.movePage(-1)
        }
        ThemedButton {
            id: moreRight
            objectName: "moreRight"
            visible: topBar.host.pages.length > 1
            enabled: topBar.host.pageIndex < topBar.host.pages.length - 1
            text: qsTr("Move page right")
            onClicked: topBar.host.movePage(1)
        }
        ThemedButton {
            id: moreRemovePage
            objectName: "moreRemovePage"
            enabled: DashboardBook.canRemovePage(topBar.host.doc)
            text: qsTr("Remove page")
            onClicked: topBar.host.confirmRemovePage = true
        }
        ThemedButton {
            id: moreExport
            objectName: "moreExport"
            text: qsTr("Export layout")
            onClicked: {
                topBar.host.noteLayout("")
                layoutSaveDialog.open()
            }
        }
        ThemedButton {
            id: moreLoad
            objectName: "moreLoad"
            text: qsTr("Load layout")
            onClicked: {
                topBar.host.noteLayout("")
                topBar.host.fileAdds = false
                layoutOpenDialog.open()
            }
        }
        Item {
            objectName: "moreResetGap"
            // The bar's width and the controls' own, never the flow's: a
            // hidden flow sizes itself from its children, and would loop
            readonly property real used: pageName.width + moreRemovePage.width + moreReplace.width
                + moreRow.spacing * 2
                + moreExport.width + moreLoad.width + 2 * moreRow.spacing
                + (moreAddPage.visible ? moreAddPage.width + moreRow.spacing : 0)
                + (moreLeft.visible ? moreLeft.width + moreRight.width + 2 * moreRow.spacing : 0)
            readonly property real tail: moreReset.width
                + (moreResetPeaks.visible ? moreResetPeaks.width + moreRow.spacing : 0)
            width: Math.max(16, topBar.width - used - tail - 2 * moreRow.spacing)
            height: 1
        }
        ThemedButton {
            id: moreReset
            objectName: "moreReset"
            flat: true
            text: qsTr("Reset all pages")
            Accessible.name: qsTr("Reset all pages: replace this form's pages with the built-in layout")
            onClicked: topBar.host.confirmReset = true
        }
        // Reset peaks, when the tool row has no room for it
        ThemedButton {
            id: moreResetPeaks
            objectName: "moreResetPeaks"
            flat: true
            visible: !header.holdsResetPeaks
            text: qsTr("Reset peaks")
            onClicked: topBar.host.resetPeaks()
        }
    }
    Label {
        objectName: "dashLayoutNote"
        visible: topBar.host.editing && topBar.host.layoutNote !== ""
                 && (topBar.host.moreShown || topBar.host.layoutNoteBad)
        Layout.fillWidth: true
        Layout.preferredWidth: 0
        wrapMode: Text.WordWrap
        color: topBar.host.layoutNoteBad ? Theme.danger : Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: topBar.host.layoutNote
    }
    DocumentSaveDialog {
        id: layoutSaveDialog
        objectName: "layoutSaveDialog"
        title: qsTr("Export this layout")
        defaultSuffix: "json"
        nameFilters: [qsTr("Layout files (*.json)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        currentFile: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
                     + "/" + topBar.host.layoutFileName()
        onAccepted: topBar.host.exportLayoutTo(layoutSaveDialog.selectedFile)
        onFailed: words => topBar.host.noteLayout(qsTr("Not saved: %1").arg(words), true)
    }
    DocumentOpenDialog {
        id: layoutOpenDialog
        objectName: "layoutOpenDialog"
        title: qsTr("Load a layout")
        nameFilters: [qsTr("Layout files (*.json)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: {
            const adds = topBar.host.fileAdds
            topBar.host.fileAdds = false
            topBar.host.loadLayoutFrom(layoutOpenDialog.selectedFile, adds)
        }
        onRejected: topBar.host.fileAdds = false
        onFailed: words => {
            topBar.host.fileAdds = false
            topBar.host.noteLayout(words, true)
        }
    }
    RowLayout {
        objectName: "dashRemovePageConfirm"
        readonly property real touchMin: Theme.touchTarget
        Layout.fillWidth: true
        visible: topBar.host.editing && topBar.host.confirmRemovePage
        spacing: 8
        Label {
            objectName: "removePageQuestion"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            color: Theme.warning
            font.pixelSize: Theme.fontCaption
            text: qsTr("Remove the page \"%1\" and its gauges?")
                  .arg(topBar.host.pages[topBar.host.pageIndex] ? topBar.host.pages[topBar.host.pageIndex].name : "")
        }
        ThemedButton {
            objectName: "removePageConfirm"
            compact: topBar.host.form === "phone"
            text: qsTr("Remove")
            onClicked: topBar.host.removePage()
        }
        ThemedButton {
            objectName: "removePageKeep"
            compact: topBar.host.form === "phone"
            text: qsTr("Keep")
            onClicked: topBar.host.confirmRemovePage = false
        }
    }
    // The presets: a new page after the shown one, or the shown page's
    // gauges replaced. Blank page and From a file only add
    Flow {
        id: presetFlow
        objectName: "dashPresetList"
        readonly property real touchMin: Theme.touchTarget
        readonly property bool adding: topBar.host.presetList === "add"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: 1
        // The bar's own width: a column narrower than its widest row's
        // minimum still lays its children out at that minimum, and the
        // list would follow it past a narrow page's edge
        Layout.maximumWidth: topBar.width
        visible: topBar.host.editing && topBar.host.presetList !== ""
        spacing: 8
        ThemedButton {
            objectName: "preset_blank"
            compact: topBar.host.form === "phone"
            visible: presetFlow.adding
            enabled: DashboardBook.canAddPage(topBar.host.doc)
            text: qsTr("Blank page")
            Accessible.name: qsTr("Add a blank page")
            onClicked: topBar.host.addBlankPage()
        }
        Repeater {
            model: DashboardBook.presets
            ThemedButton {
                required property var modelData
                objectName: "preset_" + modelData.key
                compact: topBar.host.form === "phone"
                enabled: !presetFlow.adding || DashboardBook.canAddPage(topBar.host.doc)
                text: modelData.label
                Accessible.name: presetFlow.adding
                    ? qsTr("Add the %1 preset as a new page").arg(modelData.label)
                    : qsTr("Replace this page with the %1 preset").arg(modelData.label)
                onClicked: presetFlow.adding ? topBar.host.addPresetPage(modelData.key)
                                             : topBar.host.replaceChosen(modelData.key)
            }
        }
        ThemedButton {
            objectName: "preset_file"
            compact: topBar.host.form === "phone"
            visible: presetFlow.adding
            enabled: DashboardBook.canAddPage(topBar.host.doc)
            text: qsTr("From a file...")
            onClicked: {
                topBar.host.noteLayout("")
                topBar.host.fileAdds = true
                layoutOpenDialog.open()
            }
        }
        ThemedButton {
            objectName: "preset_cancel"
            compact: topBar.host.form === "phone"
            flat: true
            text: qsTr("Cancel")
            onClicked: topBar.host.closePresets()
        }
    }
    RowLayout {
        objectName: "dashReplaceConfirm"
        readonly property real touchMin: Theme.touchTarget
        Layout.fillWidth: true
        visible: topBar.host.editing && topBar.host.confirmReplaceKey !== ""
        spacing: 8
        Label {
            objectName: "replaceQuestion"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            color: Theme.warning
            font.pixelSize: Theme.fontCaption
            text: qsTr("Replace the gauges of the page \"%1\" with the %2 preset? They aren't kept.")
                  .arg(topBar.host.pages[topBar.host.pageIndex] ? topBar.host.pages[topBar.host.pageIndex].name : "")
                  .arg(topBar.host.presetLabel(topBar.host.confirmReplaceKey))
        }
        ThemedButton {
            objectName: "replaceConfirm"
            compact: topBar.host.form === "phone"
            text: qsTr("Replace")
            onClicked: topBar.host.replaceWithPreset(topBar.host.confirmReplaceKey)
        }
        ThemedButton {
            objectName: "replaceKeep"
            compact: topBar.host.form === "phone"
            text: qsTr("Keep")
            onClicked: topBar.host.confirmReplaceKey = ""
        }
    }
    RowLayout {
        objectName: "dashResetConfirm"
        readonly property real touchMin: Theme.touchTarget
        Layout.fillWidth: true
        visible: topBar.host.editing && topBar.host.confirmReset
        spacing: 8
        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            color: Theme.warning
            font.pixelSize: Theme.fontCaption
            text: qsTr("Replace all of this form's pages with the built-in layout?")
        }
        ThemedButton {
            objectName: "resetConfirm"
            compact: topBar.host.form === "phone"
            text: qsTr("Reset all")
            onClicked: topBar.host.resetForm()
        }
        ThemedButton {
            objectName: "resetKeep"
            compact: topBar.host.form === "phone"
            text: qsTr("Keep")
            onClicked: topBar.host.confirmReset = false
        }
    }

    RowLayout {
        objectName: "dashProblem"
        Layout.fillWidth: true
        visible: (topBar.host.problem === "damaged" || topBar.host.problem === "newer"
                  || topBar.host.problem === "dropped") && !topBar.host.problemDismissed
        Label {
            objectName: "dashProblemText"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.warning
            font.pixelSize: Theme.fontCaption
            text: topBar.host.problem === "newer"
                  ? qsTr("The saved dashboard was made by a newer version of Havne. "
                         + "This shows the built-in one; the saved one is kept.")
                  : topBar.host.problem === "dropped"
                  ? qsTr("Part of the saved dashboard couldn't be read: %1 left out. "
                         + "The saved text is kept.").arg(topBar.host.droppedWords)
                  : qsTr("The saved dashboard couldn't be read. "
                         + "This shows the built-in one; the saved text is kept.")
        }
        ThemedButton {
            objectName: "dashProblemDismiss"
            compact: true
            text: qsTr("Dismiss")
            onClicked: topBar.host.problemDismissed = true
        }
    }

    // The desktop's pages as tabs, with two or more or in edit mode;
    // last in the column, so the panel starts at its foot
    Loader {
        id: stripHost
        objectName: "dashPageStripHost"
        Layout.fillWidth: true
        Layout.preferredHeight: item ? (item as DashPageStrip).implicitHeight : 0
        active: !topBar.host.phoneForm
        visible: topBar.host.stripShown
        sourceComponent: Component {
            DashPageStrip {
                pages: topBar.host.pages
                current: topBar.host.pageIndex
                editing: topBar.host.editing
                canAddPage: DashboardBook.canAddPage(topBar.host.doc)
                hairlineWidth: topBar.host.hairlineWidth
                onPageChosen: (i) => topBar.host.goTo(i)
                onAddRequested: topBar.host.openPresets("add")
            }
        }
    }

    // The desktop's link state: the dot and words, and the Night switch at
    // their end (ADR-0058 section 6: a status with a tap)
    Component {
        id: linkState
        RowLayout {
            spacing: 10
            // Under 60 px the label would read "N...n": the dot and the label
            // fade (opacity, so the layout still holds them) and the header's
            // pill carries the state alone
            Rectangle {
                objectName: "linkDot"
                implicitWidth: 8; implicitHeight: 8   // layout-managed
                radius: 4
                color: Theme.linkColor(Bridge.linkState)
                opacity: linkLabel.opacity
            }
            Label {
                id: linkLabel
                objectName: "linkLabel"
                Layout.fillWidth: true
                opacity: width >= 60 ? 1 : 0
                elide: Text.ElideMiddle
                // Short statuses come from Theme.linkLabel; "ok" adds the
                // address where the row leaves room for it (the header
                // shows it anyway). Long-form detail (episode counts,
                // error codes) stays in LiveView's banner.
                readonly property bool roomForAddress: width >= 150
                text: Bridge.linkState === "ok"
                      ? (roomForAddress ? qsTr("Live · %1").arg(Bridge.uriLabel) : qsTr("Live"))
                      : Theme.linkLabel(Bridge.linkState)
                font.pixelSize: Theme.fontCaption
                color: Theme.linkColor(Bridge.linkState)
            }
            NightSwitch {}
        }
    }
}
