// File/layer: app/qml/SettingsSignals.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The one Import and the one list of plugins, each variant the picked vehicle's to pick (ADR-0064): one part of SettingsPage, which holds the state

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Shapes

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    objectName: "settingsSignalsPart"
    required property SettingsPage host
    readonly property bool vehicle: root.host.pickedVehicle !== null
    // Nothing changes the store or a vehicle's picks under a session, or
    // past a question that waits for its answer
    readonly property bool canChange: root.host.idle && root.host.confirming === ""
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    property alias obdbYearField: obdbYearField
    property alias importVariantField: importVariantField
    property alias saveDialog: saveDialog

    function replaceFrom(row) {
        root.host.startReplace(row)
        importDialog.open()
    }

    Label {
        objectName: "signalLockedNote"
        visible: !root.host.idle
        text: qsTr("Disconnect to change what a vehicle reads.")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "signalImport"
            text: qsTr("Import...")
            enabled: root.canChange
            onClicked: {
                root.host.replaceTarget = null
                importDialog.open()
            }
        }
    }

    Label {
        objectName: "signalProblem"
        visible: text !== ""
        text: root.host.signalProblem
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        objectName: "signalNote"
        visible: text !== ""
        text: root.host.signalNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // ---- A file read and not yet kept ----
    ColumnLayout {
        id: importPanel
        objectName: "importPanel"
        visible: root.host.pendingImport !== null
        readonly property bool isFile: root.host.pendingImport !== null
                                       && root.host.pendingImport.kind === "signals"
        readonly property bool isObdb: root.host.pendingImport !== null
                                       && root.host.pendingImport.kind === "obdb"
        spacing: Theme.spacing
        Layout.fillWidth: true

        // A signal file: a plugin of its own, or a variant of one of the
        // user's
        Label {
            objectName: "importText"
            visible: importPanel.isFile
            text: importPanel.isFile ? qsTr("%1: a signal file").arg(root.host.pendingImport.name) : ""
            color: Theme.text
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Flow {
            visible: importPanel.isFile
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "importAsNew"
                width: Math.min(implicitWidth, parent.width)
                text: qsTr("As a new plugin")
                highlighted: root.host.importInto === ""
                enabled: root.canChange
                onClicked: root.host.keepPending("", "")
            }
            Repeater {
                model: importPanel.isFile ? root.host.ownPlugins : []
                ThemedButton {
                    required property var modelData
                    required property int index
                    objectName: "importInto_" + index
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Into %1").arg(modelData.name)
                    checkable: true
                    checked: root.host.importInto === modelData.sha256
                    enabled: root.canChange
                    onClicked: root.host.importInto = modelData.sha256
                }
            }
        }
        LabeledFlow {
            visible: importPanel.isFile && root.host.importInto !== ""
            label: qsTr("Variant:")
            ThemedTextField {
                id: importVariantField
                objectName: "importVariantField"
                width: Math.min(200, parent.width)
                maximumLength: 24
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }
            Label {
                objectName: "importReplaces"
                visible: text !== ""
                text: root.host.importReplaces(importVariantField.text)
                color: Theme.textDim
            }
            ThemedButton {
                objectName: "importKeep"
                text: qsTr("Keep")
                highlighted: true
                enabled: root.canChange
                onClicked: root.host.keepPending(root.host.importInto, importVariantField.text.trim())
            }
        }

        // An OBDb set: its report first, converted again for another year
        LabeledFlow {
            visible: importPanel.isObdb
            label: qsTr("OBDb model year:")
            ThemedTextField {
                id: obdbYearField
                objectName: "obdbYearField"
                width: Math.min(120, parent.width)
                maximumLength: 4
                placeholderText: qsTr("none")
                inputMethodHints: Qt.ImhDigitsOnly
                // A year typed is the user's; the page marks one it fills
                // from a VIN after setting it
                onTextChanged: {
                    root.host.obdbYearFromVin = false
                    if (importPanel.isObdb)
                        root.host.previewObdb()
                }
            }
        }
        ColumnLayout {
            objectName: "obdbReport"
            visible: importPanel.isObdb && root.host.obdbShown
            spacing: Theme.spacing / 2
            Layout.fillWidth: true

            Label {
                objectName: "obdbSummary"
                text: root.host.obdbSummary(root.host.obdbReport)
                color: Theme.text
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                visible: root.host.obdbReport !== null && root.host.obdbReport.changed.length > 0
                text: qsTr("Differs from OBDb:")
                color: Theme.text
                font.bold: true
            }
            Repeater {
                objectName: "obdbChanged"
                model: root.host.obdbReport !== null
                       ? root.host.obdbReport.changed.slice(0, root.host.obdbRowsShown) : []
                Label {
                    required property var modelData
                    text: qsTr("%1, %2: %3")
                              .arg(modelData.key !== "" ? modelData.key : modelData.signal)
                              .arg(modelData.field).arg(modelData.text)
                    color: Theme.textDim
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
            Label {
                objectName: "obdbChangedMore"
                readonly property int more: root.host.obdbReport === null ? 0
                    : root.host.obdbReport.changed.length - root.host.obdbRowsShown
                visible: more > 0
                text: qsTr("and %1 more").arg(more)
                color: Theme.textDim
            }
            Label {
                visible: root.host.obdbReport !== null && root.host.obdbReport.skipped.length > 0
                text: qsTr("Left out:")
                color: Theme.text
                font.bold: true
            }
            Repeater {
                objectName: "obdbSkipped"
                model: root.host.obdbReport !== null
                       ? root.host.obdbReport.skipped.slice(0, root.host.obdbRowsShown) : []
                Label {
                    required property var modelData
                    text: qsTr("%1 (line %2): %3")
                              .arg(modelData.signal !== "" ? modelData.signal : qsTr("the set"))
                              .arg(modelData.line).arg(modelData.text)
                    color: Theme.textDim
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
            Label {
                objectName: "obdbSkippedMore"
                readonly property int more: root.host.obdbReport === null ? 0
                    : root.host.obdbReport.skipped.length - root.host.obdbRowsShown
                visible: more > 0
                text: qsTr("and %1 more").arg(more)
                color: Theme.textDim
            }
            Repeater {
                objectName: "obdbFileNotes"
                model: root.host.obdbReport !== null ? root.host.obdbReport.file : []
                Label {
                    required property var modelData
                    text: modelData.field !== ""
                          ? qsTr("The set's %1: %2").arg(modelData.field).arg(modelData.text)
                          : qsTr("The set: %1").arg(modelData.text)
                    color: Theme.textDim
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "obdbKeep"
                visible: importPanel.isObdb && root.host.obdbShown
                text: root.host.obdbReplaces ? qsTr("Keep, replacing the earlier") : qsTr("Keep")
                highlighted: true
                enabled: root.canChange
                onClicked: root.host.keepObdb()
            }
            ThemedButton {
                objectName: "importDiscard"
                text: qsTr("Discard")
                onClicked: root.host.discardImport()
            }
        }
    }

    // The removal of a plugin of the user's own asks once, in place
    Flow {
        objectName: "signalsConfirm"
        visible: root.host.confirmingPlugin() !== null
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            objectName: "signalsConfirmText"
            text: root.host.confirmingPlugin() !== null
                ? qsTr("Remove %1? It is yours, and no other copy exists")
                      .arg(root.host.confirmingPlugin().name)
                : ""
            color: Theme.text
            wrapMode: Text.Wrap
            width: Math.min(implicitWidth, parent.width)
        }
        ThemedButton {
            objectName: "signalsConfirmRemove"
            text: qsTr("Remove")
            highlighted: true
            enabled: root.host.idle
            onClicked: root.host.removePlugin(root.host.confirmingPlugin().sha256)
        }
        ThemedButton {
            objectName: "signalsConfirmCancel"
            text: qsTr("Cancel")
            onClicked: root.host.confirming = ""
        }
    }

    Label {
        objectName: "signalsNone"
        visible: root.host.pluginList.length === 0
        text: qsTr("None yet. Import takes a plugin (a .havne-plugin.json file), a signal file "
                   + "you keep yourself or an OBDb set (a community list of a model's signals); "
                   + "each is listed here as a plugin.")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // A pick no plugin here has, so it can be seen and undone
    Repeater {
        model: root.host.missingPicks
        RowLayout {
            id: missingRow
            required property var modelData
            required property int index
            objectName: "pickMissing_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing
            Label {
                objectName: "pickMissingText"
                text: missingRow.modelData.text
                color: Theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            ThemedButton {
                objectName: "pickMissingUnpick"
                flat: true
                text: qsTr("Unpick")
                enabled: root.canChange
                onClicked: root.host.togglePick(missingRow.modelData.profile)
            }
        }
    }

    // A kept copy that no longer loads, with the loader's words
    Repeater {
        model: root.host.refusedPlugins
        RowLayout {
            id: refusedRow
            required property var modelData
            required property int index
            objectName: "pluginRefused_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing
            Label {
                objectName: "pluginRefusedText"
                text: qsTr("A plugin that no longer loads: %1").arg(refusedRow.modelData.words)
                color: Theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            ThemedButton {
                objectName: "pluginRefusedRemove"
                flat: true
                text: qsTr("Remove")
                enabled: root.canChange
                onClicked: root.host.removePlugin(refusedRow.modelData.sha256)
            }
        }
    }

    // ---- The list: a row for each variant of each plugin. A press picks
    // it for the picked vehicle or undoes the pick; More opens the row's
    // actions in place ----
    Repeater {
        model: root.host.variantRows
        ColumnLayout {
            id: row
            required property var modelData
            required property int index
            readonly property bool open: root.host.openRow === modelData.profile
            readonly property string title: root.host.rowTitle(modelData)
            readonly property string reads: root.host.rowReads(modelData)
            objectName: "signalRow_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing / 2

            Label {
                objectName: "rowCaption"
                visible: text !== ""
                text: row.modelData.caption
                color: Theme.textDim
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                ThemedButton {
                    id: pick
                    objectName: "rowPick"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    checkable: true
                    checked: row.modelData.picked
                    enabled: root.vehicle && root.canChange
                    leftPadding: 12
                    rightPadding: 12
                    Accessible.name: row.modelData.picked
                        ? qsTr("%1, %2, picked").arg(row.title).arg(row.reads)
                        : qsTr("%1, %2").arg(row.title).arg(row.reads)
                    onClicked: root.host.togglePick(row.modelData.profile)
                    contentItem: RowLayout {
                        spacing: 10
                        // A mark, so the pick doesn't rest on the fill alone
                        Item {
                            implicitWidth: 18; implicitHeight: 18   // layout-managed
                            Shape {
                                objectName: "rowPickMark"
                                visible: row.modelData.picked
                                anchors.centerIn: parent
                                width: 24; height: 24
                                scale: 0.75
                                preferredRendererType: Shape.CurveRenderer
                                ShapePath {
                                    strokeColor: Theme.text
                                    strokeWidth: 2.5
                                    fillColor: "transparent"
                                    capStyle: ShapePath.SquareCap
                                    startX: 5; startY: 12
                                    PathLine { x: 10; y: 17 }
                                    PathLine { x: 19; y: 7 }
                                }
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                objectName: "rowTitle"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                wrapMode: Text.Wrap
                                color: Theme.text
                                text: row.title
                            }
                            Label {
                                objectName: "rowReads"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                wrapMode: Text.Wrap
                                font.pixelSize: Theme.fontCaption
                                color: Theme.textDim
                                text: row.reads
                            }
                        }
                    }
                }
                ThemedButton {
                    objectName: "rowMore"
                    flat: true
                    // One width open or closed, so the row beside it keeps its own
                    compact: true
                    Layout.minimumWidth: Theme.touchTarget
                    text: qsTr("More")
                    checkable: true
                    checked: row.open
                    Accessible.name: qsTr("More for %1").arg(row.title)
                    onClicked: root.host.openRow = row.open ? "" : row.modelData.profile
                }
            }

            Flow {
                objectName: "rowActions"
                visible: row.open
                Layout.fillWidth: true
                spacing: Theme.spacing
                ThemedButton {
                    objectName: "rowSave"
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Save as a signal file...")
                    // Reads the copy alone, so a session may run
                    enabled: root.host.confirming === ""
                    onClicked: root.host.saveVariantAs(row.modelData)
                }
                ThemedButton {
                    objectName: "rowExport"
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Export plugin...")
                    enabled: root.host.confirming === ""
                    onClicked: root.host.exportPluginAs(row.modelData.sha256)
                }
                ThemedButton {
                    objectName: "rowReplace"
                    visible: row.modelData.mine
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Replace...")
                    enabled: root.canChange
                    onClicked: root.replaceFrom(row.modelData)
                }
                ThemedButton {
                    objectName: "rowCopy"
                    visible: !row.modelData.mine
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Copy to my plugin")
                    checkable: true
                    checked: row.open && root.host.copyShown
                    enabled: root.canChange
                    onClicked: root.host.copyShown = !root.host.copyShown
                }
                ThemedButton {
                    objectName: "rowMine"
                    visible: !row.modelData.mine
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Mine")
                    enabled: root.canChange
                    onClicked: root.host.markMine(row.modelData.sha256)
                }
                ThemedButton {
                    objectName: "rowRemoveVariant"
                    visible: row.modelData.mine && row.modelData.several
                    flat: true
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Remove this variant")
                    enabled: root.canChange
                    onClicked: root.host.removeVariant(row.modelData)
                }
                ThemedButton {
                    objectName: "rowRemove"
                    flat: true
                    width: Math.min(implicitWidth, parent.width)
                    text: row.modelData.several ? qsTr("Remove the plugin") : qsTr("Remove")
                    enabled: root.canChange
                    onClicked: root.host.askRemovePlugin(row.modelData.sha256)
                }
            }
            // What Mine does, said before the press
            Label {
                objectName: "rowMineLine"
                visible: row.open && !row.modelData.mine
                text: qsTr("Mine marks it as yours, so its variants can be changed here; a newer "
                           + "export of the same plugin then updates it in place.")
                color: Theme.textDim
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            // Copy to my plugin: where the copy goes
            Flow {
                objectName: "rowCopyPanel"
                visible: row.open && root.host.copyShown
                Layout.fillWidth: true
                spacing: Theme.spacing
                ThemedButton {
                    objectName: "copyAsNew"
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Into a new plugin")
                    highlighted: true
                    enabled: root.canChange
                    onClicked: root.host.copyVariantTo(row.modelData, "")
                }
                Repeater {
                    model: row.open && root.host.copyShown ? root.host.ownPlugins : []
                    ThemedButton {
                        required property var modelData
                        required property int index
                        objectName: "copyInto_" + index
                        width: Math.min(implicitWidth, parent.width)
                        text: qsTr("Into %1").arg(modelData.name)
                        enabled: root.canChange
                        onClicked: root.host.copyVariantTo(row.modelData, modelData.sha256)
                    }
                }
            }
            // Export's share-alike note, before the save dialog
            ColumnLayout {
                objectName: "rowExportNote"
                visible: row.open && root.host.exportNote !== ""
                Layout.fillWidth: true
                spacing: Theme.spacing / 2
                Label {
                    objectName: "exportNoteText"
                    text: root.host.exportNote
                    color: Theme.text
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    ThemedButton {
                        objectName: "exportSave"
                        text: qsTr("Save...")
                        highlighted: true
                        onClicked: root.host.openExport(row.modelData.sha256)
                    }
                    ThemedButton {
                        objectName: "exportCancel"
                        text: qsTr("Cancel")
                        onClicked: root.host.exportNote = ""
                    }
                }
            }
            Label {
                objectName: "rowUnused"
                visible: row.modelData.unused
                text: root.host.pickedVehicle === null
                      ? qsTr("Not in use: add or pick a vehicle, then pick it here.")
                      : row.modelData.several
                        ? qsTr("Not in use: press a variant to pick it for %1.")
                              .arg(root.host.pickedVehicle.label)
                        : qsTr("Not in use: press it to pick it for %1.")
                              .arg(root.host.pickedVehicle.label)
                color: Theme.textDim
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
    }

    // ---- The signal files the move into the plugin store left behind ----
    Label {
        objectName: "unmigratedTitle"
        visible: root.host.unmigratedRows.length > 0
        text: qsTr("Not moved from the earlier version:")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Repeater {
        model: root.host.unmigratedRows
        ColumnLayout {
            id: leftRow
            required property var modelData
            required property int index
            objectName: "unmigrated_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing / 2
            Label {
                objectName: "unmigratedText"
                text: root.host.unmigratedText(leftRow.modelData)
                color: Theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.spacing
                ThemedButton {
                    objectName: "unmigratedSave"
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Save as a signal file...")
                    enabled: root.host.confirming === ""
                    onClicked: root.host.saveUnmigratedAs(leftRow.modelData)
                }
                ThemedButton {
                    objectName: "unmigratedRemove"
                    flat: true
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Remove")
                    enabled: root.canChange
                    onClicked: root.host.removeUnmigrated(leftRow.modelData)
                }
            }
        }
    }

    FileDialog {
        id: importDialog
        objectName: "signalImportDialog"
        title: root.host.replaceTarget !== null
               ? qsTr("Replace %1 with a signal file").arg(root.host.replaceTarget.name)
               : qsTr("Import a plugin, a signal file or an OBDb set")
        nameFilters: [qsTr("Plugins, signal files and OBDb sets (*.json)"), qsTr("All files (*)")]
        onAccepted: root.host.importFile(importDialog.selectedFile)
        onRejected: root.host.replaceTarget = null
    }
    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.host.saveTo(saveDialog.selectedFile)
        onRejected: root.host.saving = null
    }
}
