// File/layer: app/qml/SettingsSignalFiles.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The picked vehicle's signal files and the imports: one part of SettingsPage, which holds the state

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    objectName: "settingsSignalFilesPart"
    visible: root.host.pickedVehicle !== null
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    property alias obdbYearField: obdbYearField
    property alias saveDialog: saveDialog

    // ---- The picked vehicle's signal files (ADR-0051) ----
    Label {
        objectName: "signalFilesTitle"
        visible: root.host.pickedVehicle !== null
        text: qsTr("Signal files")
        color: Theme.text
        font.bold: true
    }

    Repeater {
        model: root.host.pickedVehicle !== null ? root.host.pickedVehicle.signals : []
        // The file's line at the page's width, its buttons under
        // it, so a phone's narrow page never squeezes the line
        ColumnLayout {
            id: fileRow
            required property var modelData
            required property int index
            objectName: "signalFile_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing / 2

            Label {
                objectName: "signalFileText"
                text: root.host.signalFileText(fileRow.modelData)
                color: root.host.signalOk(fileRow.modelData.sha256) ? Theme.text
                                                               : Theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.spacing
                ThemedButton {
                    objectName: "signalSave_" + fileRow.index
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Save a copy")
                    // Reads the copy alone, so a session may run
                    enabled: root.host.confirming === ""
                    onClicked: root.host.saveSignalFileAs(fileRow.modelData)
                }
                ThemedButton {
                    objectName: "signalRemove_" + fileRow.index
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Remove")
                    enabled: root.host.idle && root.host.confirming === ""
                    onClicked: root.host.removeSignalFile(fileRow.modelData.sha256)
                }
            }
        }
    }

    Label {
        objectName: "signalFilesNone"
        visible: root.host.pickedVehicle !== null && root.host.pickedVehicle.signals.length === 0
        text: qsTr("None yet. Import adds a file of your own signals, each "
                   + "then a profile under What to read")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Flow {
        visible: root.host.pickedVehicle !== null
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "signalImport"
            text: qsTr("Import a signal file")
            enabled: root.host.idle && root.host.confirming === ""
            onClicked: importDialog.open()
        }
        // An OBDb set (ADR-0051's appendix of 2026-11-06):
        // its report shows before the file is kept
        ThemedButton {
            objectName: "obdbImport"
            text: qsTr("Import an OBDb set")
            enabled: root.host.idle && root.host.confirming === ""
            onClicked: {
                if (obdbYearField.text.trim() === "") {
                    const y = root.host.obdbVinYear()
                    obdbYearField.text = y > 0 ? String(y) : ""
                }
                obdbDialog.open()
            }
        }
    }

    LabeledFlow {
        visible: root.host.pickedVehicle !== null
        label: qsTr("OBDb model year:")
        TextField {
            id: obdbYearField
            objectName: "obdbYearField"
            width: Math.min(120, parent.width)
            maximumLength: 4
            placeholderText: qsTr("from the VIN")
            inputMethodHints: Qt.ImhDigitsOnly
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

    // The OBDb import's report, before the file is kept: what
    // converted, each field that differs, each signal left out
    ColumnLayout {
        objectName: "obdbReport"
        visible: root.host.obdbShown
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
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "obdbKeep"
                text: root.host.obdbReplaces !== "" ? qsTr("Keep, replacing the earlier")
                                               : qsTr("Keep")
                highlighted: true
                enabled: root.host.idle && root.host.confirming === ""
                onClicked: root.host.keepObdb()
            }
            ThemedButton {
                objectName: "obdbDiscard"
                text: qsTr("Discard")
                onClicked: root.host.discardObdb()
            }
        }
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import a signal file")
        nameFilters: [qsTr("Signal files (*.json)"), qsTr("All files (*)")]
        onAccepted: root.host.importSignalFile(importDialog.selectedFile)
    }
    FileDialog {
        id: obdbDialog
        title: qsTr("Import an OBDb set")
        nameFilters: [qsTr("OBDb signal sets (*.json)"), qsTr("All files (*)")]
        onAccepted: root.host.previewObdb(obdbDialog.selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: qsTr("Save a copy of the signal file")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Signal files (*.json)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.host.saveSignalFileTo(root.host.savingSha, saveDialog.selectedFile)
    }
}
