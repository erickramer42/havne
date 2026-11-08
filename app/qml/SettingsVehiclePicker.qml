// File/layer: app/qml/SettingsVehiclePicker.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The vehicle picker, its label and the list edits: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    property alias vehicleLabelField: vehicleLabelField

    // ---- Vehicle (ADR-0027): picked before a connect, its
    // connection filling the field; every change to the list is
    // made here. Locked while a session is up or in flight ----
    LabeledFlow {
        label: qsTr("Vehicle:")

        ThemedButton {
            objectName: "vehicle_none"
            text: qsTr("None")
            highlighted: root.host.pickedVehicle === null
            enabled: root.host.idle
            onClicked: root.host.pickVehicle("")
        }
        // Keyed by the count, not the list: each change makes a
        // new list, and a list model would rebuild every
        // button (losing focus) for a pick or a rename
        Repeater {
            model: root.host.vehicles.list.length
            ThemedButton {
                required property int index
                // Undefined for a moment when the list shrinks,
                // before the button goes
                readonly property var vehicle: root.host.vehicles.list[index]
                objectName: "vehicle_" + (vehicle?.id ?? "")
                text: vehicle?.label ?? ""
                // A long label wraps rather than run past a phone's edge
                width: Math.min(implicitWidth, parent.width)
                highlighted: vehicle !== undefined && root.host.vehicles.picked === vehicle.id
                enabled: root.host.idle
                onClicked: root.host.pickVehicle(vehicle.id)
            }
        }
    }

    Label {
        objectName: "vehicleVinPickNote"
        visible: root.host.vinPickNote !== ""
        text: root.host.vinPickNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: root.host.confirming === ""

        TextField {
            id: vehicleLabelField
            objectName: "vehicleLabelField"
            width: Math.min(300, parent.width)
            enabled: root.host.idle
            placeholderText: qsTr("Label")
        }
        ThemedButton {
            objectName: "vehicleNew"
            text: qsTr("New")
            enabled: root.host.idle && VehicleBook.labelProblem(root.host.vehicles, root.host.newLabel, "") === ""
            onClicked: root.host.changeVehicles(VehicleBook.add(root.host.vehicles, root.host.newLabel))
        }
        ThemedButton {
            objectName: "vehicleDuplicate"
            text: qsTr("Duplicate")
            enabled: root.host.idle && root.host.pickedVehicle !== null
                     && VehicleBook.labelProblem(root.host.vehicles, root.host.copyLabel, "") === ""
            onClicked: root.host.duplicateVehicle()
        }
        ThemedButton {
            objectName: "vehicleRename"
            text: qsTr("Rename")
            enabled: root.host.idle && root.host.pickedVehicle !== null && root.host.typedLabel !== ""
                     && root.host.typedLabel !== root.host.pickedVehicle.label
                     && VehicleBook.labelProblem(root.host.vehicles, root.host.typedLabel,
                                                 root.host.pickedVehicle.id) === ""
            onClicked: root.host.changeVehicles(VehicleBook.rename(root.host.vehicles, root.host.typedLabel))
        }
        ThemedButton {
            objectName: "vehicleForgetVin"
            text: qsTr("Forget the VIN")
            enabled: root.host.idle && root.host.pickedVehicle !== null && root.host.pickedVehicle.vinKey !== ""
            onClicked: root.host.changeVehicles(VehicleBook.forgetVin(root.host.vehicles))
        }
        ThemedButton {
            objectName: "vehicleForget"
            text: qsTr("Forget")
            enabled: root.host.idle && root.host.pickedVehicle !== null
            onClicked: root.host.confirming = "forget"
        }
        ThemedButton {
            objectName: "vehicleForgetAll"
            text: qsTr("Forget all vehicles")
            enabled: root.host.idle && root.host.vehicles.list.length > 0
            onClicked: root.host.confirming = "forgetAll"
        }
    }

    // With the field empty, the defaults New and Duplicate take
    Label {
        objectName: "vehicleLabelHint"
        visible: root.host.typedLabel === "" && root.host.confirming === ""
        text: root.host.pickedVehicle === null
            ? qsTr("Left empty, New makes “%1”").arg(VehicleBook.defaultNewLabel(root.host.vehicles))
            : qsTr("Left empty, New makes “%1” and Duplicate “%2”")
                  .arg(VehicleBook.defaultNewLabel(root.host.vehicles),
                       VehicleBook.defaultCopyLabel(root.host.vehicles, root.host.pickedVehicle.label))
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // Why a typed label can't be used
    Label {
        objectName: "vehicleLabelProblem"
        visible: text !== "" && root.host.confirming === ""
        text: root.host.typedLabel === "" ? ""
            : VehicleBook.labelProblem(root.host.vehicles, root.host.typedLabel,
                                       root.host.pickedVehicle !== null ? root.host.pickedVehicle.id : "")
        color: Theme.warning
    }

    // A removal asks once, in place
    Flow {
        visible: root.host.confirming !== ""
        Layout.fillWidth: true
        spacing: Theme.spacing

        Label {
            objectName: "vehicleConfirmText"
            text: root.host.confirming === "forgetAll" ? qsTr("Forget all vehicles?")
                : root.host.confirmingFile() !== null
                  ? qsTr("Remove %1? It was written here, and no other copy exists")
                        .arg(root.host.confirmingFile().name)
                : root.host.pickedVehicle !== null ? qsTr("Forget %1?").arg(root.host.pickedVehicle.label)
                : ""
            color: Theme.text
            wrapMode: Text.Wrap
            width: Math.min(implicitWidth, parent.width)
        }
        ThemedButton {
            objectName: "vehicleConfirm"
            text: root.host.confirmingFile() !== null ? qsTr("Remove") : qsTr("Forget")
            highlighted: true
            enabled: root.host.idle
            onClicked: {
                const file = root.host.confirmingFile()
                if (file !== null)
                    root.host.dropSignalFile(file.sha256)
                else
                    root.host.forgetVehicles(root.host.confirming === "forgetAll")
            }
        }
        ThemedButton {
            objectName: "vehicleCancel"
            text: qsTr("Cancel")
            onClicked: root.host.confirming = ""
        }
    }
}
