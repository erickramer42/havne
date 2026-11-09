// File/layer: app/qml/SettingsOwnSignals.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The vehicle's own signals and Add a signal: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    objectName: "settingsOwnSignalsPart"
    visible: root.host.pickedVehicle !== null
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2


    // ---- The vehicle's own signals (ADR-0051's form) ----
    Label {
        objectName: "ownSignalsTitle"
        visible: root.host.pickedVehicle !== null
        text: qsTr("Your own signals")
        color: Theme.text
        font.bold: true
    }

    Repeater {
        model: root.host.ownSignals
        RowLayout {
            id: ownRow
            required property var modelData
            required property int index
            objectName: "ownSignal_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                objectName: "ownSignalText"
                text: qsTr("%1: %2").arg(ownRow.modelData.name,
                                         SignalForm.describe(root.host.ownFile, ownRow.modelData))
                color: Theme.text
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            ThemedButton {
                objectName: "ownEdit_" + ownRow.index
                text: qsTr("Edit")
                enabled: root.host.confirming === ""
                onClicked: {
                    root.host.editOwnSignal(ownRow.modelData.key)
                    root.host.openOwnForm()
                }
            }
            ThemedButton {
                objectName: "ownRemove_" + ownRow.index
                flat: true
                text: qsTr("Remove")
                enabled: root.host.idle && root.host.confirming === ""
                onClicked: root.host.removeOwnSignal(ownRow.modelData.key)
            }
        }
    }

    Label {
        objectName: "ownSignalsNote"
        visible: text !== ""
        text: root.host.formNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    ThemedButton {
        objectName: "ownAdd"
        text: qsTr("Add a signal")
        enabled: root.host.confirming === ""
        onClicked: {
            // A new signal starts from an empty form, not the last one edited
            root.host.clearForm()
            root.host.openOwnForm()
        }
    }
}
