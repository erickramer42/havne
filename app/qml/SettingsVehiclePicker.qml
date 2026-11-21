// File/layer: app/qml/SettingsVehiclePicker.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone_* (test_qml)
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
    // Whether the row of the less common actions shows
    property bool moreShown: false

    // Vehicle (ADR-0027): picked before a connect, its
    // connection filling the field; every change to the list is
    // made here. Locked while a session is up or in flight
    // A later Havne's vehicle list is never written over: nothing here
    // changes it
    enabled: !root.host.bookLater
    Label {
        objectName: "vehicleBookLater"
        visible: root.host.bookLater
        text: qsTr("This vehicle list was saved by a later Havne and is left as it is.")
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        objectName: "vehicleNote"
        text: qsTr("A vehicle keeps its picked plugins, its own signals, its last address and its plotted signals.")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        objectName: "vehicleLockedNote"
        visible: !root.host.ownPart.idle
        text: qsTr("Disconnect to add or change vehicles.")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    LabeledFlow {
        label: qsTr("Vehicle:")

        ThemedButton {
            objectName: "vehicle_none"
            text: qsTr("None")
            checkable: true
            checked: root.host.pickedVehicle === null
            enabled: root.host.ownPart.idle
            onClicked: root.host.ownPart.pickVehicle("")
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
                checkable: true
                checked: vehicle !== undefined && root.host.vehicles.picked === vehicle.id
                enabled: root.host.ownPart.idle
                onClicked: root.host.ownPart.pickVehicle(vehicle.id)
            }
        }
    }

    Label {
        textFormat: Text.PlainText
        objectName: "vehicleVinPickNote"
        visible: root.host.ownPart.vinPickNote !== ""
        text: root.host.ownPart.vinPickNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // Wide, the field and its three buttons share a row; on a phone the
    // field takes a row and the three split the next, so none wraps alone
    GridLayout {
        id: labelRow
        Layout.fillWidth: true
        columnSpacing: Theme.spacing
        rowSpacing: Theme.spacing
        visible: root.host.confirming === ""
        readonly property bool oneRow: root.width >= 300 + 3 * 120 + 3 * columnSpacing
        columns: oneRow ? 4 : 3

        ThemedTextField {
            id: vehicleLabelField
            objectName: "vehicleLabelField"
            Layout.preferredWidth: 300
            Layout.fillWidth: true
            Layout.maximumWidth: labelRow.oneRow ? 300 : Number.POSITIVE_INFINITY
            Layout.columnSpan: labelRow.oneRow ? 1 : 3
            enabled: root.host.ownPart.idle
            placeholderText: qsTr("Label")
        }
        ThemedButton {
            objectName: "vehicleNew"
            text: qsTr("New")
            Layout.fillWidth: true
            Layout.maximumWidth: labelRow.oneRow ? implicitWidth : Number.POSITIVE_INFINITY
            enabled: root.host.ownPart.idle && VehicleBook.labelProblem(root.host.vehicles, root.host.newLabel, "") === ""
            onClicked: root.host.ownPart.changeVehicles(VehicleBook.add(root.host.vehicles, root.host.newLabel))
        }
        ThemedButton {
            objectName: "vehicleDuplicate"
            text: qsTr("Duplicate")
            Layout.fillWidth: true
            Layout.maximumWidth: labelRow.oneRow ? implicitWidth : Number.POSITIVE_INFINITY
            enabled: root.host.ownPart.idle && root.host.pickedVehicle !== null
                     && VehicleBook.labelProblem(root.host.vehicles, root.host.copyLabel, "") === ""
            onClicked: root.host.ownPart.duplicateVehicle()
        }
        ThemedButton {
            objectName: "vehicleMore"
            text: qsTr("More")
            Layout.fillWidth: true
            Layout.maximumWidth: labelRow.oneRow ? implicitWidth : Number.POSITIVE_INFINITY
            enabled: root.host.ownPart.idle
            checkable: true
            checked: root.moreShown
            onClicked: root.moreShown = !root.moreShown
        }
    }

    // The minor and destructive actions apart from New, all quiet, each
    // destructive one confirming; on a phone two share each row
    GridLayout {
        id: moreRow
        objectName: "vehicleMoreRow"
        Layout.fillWidth: true
        columnSpacing: Theme.spacing
        rowSpacing: Theme.spacing
        visible: root.moreShown && root.host.confirming === ""
        readonly property bool oneRow: root.width >= 560
        columns: oneRow ? 4 : 2

        ThemedButton {
            objectName: "vehicleRename"
            flat: true
            Layout.fillWidth: true
            Layout.maximumWidth: moreRow.oneRow ? implicitWidth : Number.POSITIVE_INFINITY
            text: qsTr("Rename")
            enabled: root.host.ownPart.idle && root.host.pickedVehicle !== null && root.host.typedLabel !== ""
                     && root.host.typedLabel !== root.host.pickedVehicle.label
                     && VehicleBook.labelProblem(root.host.vehicles, root.host.typedLabel,
                                                 root.host.pickedVehicle.id) === ""
            onClicked: {
                root.moreShown = false
                root.host.ownPart.changeVehicles(VehicleBook.rename(root.host.vehicles, root.host.typedLabel))
            }
        }
        ThemedButton {
            objectName: "vehicleForgetVin"
            flat: true
            Layout.fillWidth: true
            Layout.maximumWidth: moreRow.oneRow ? implicitWidth : Number.POSITIVE_INFINITY
            text: qsTr("Forget the VIN")
            enabled: root.host.ownPart.idle && root.host.pickedVehicle !== null && root.host.pickedVehicle.vinKey !== ""
            onClicked: {
                root.moreShown = false
                root.host.ownPart.changeVehicles(VehicleBook.forgetVin(root.host.vehicles))
            }
        }
        ThemedButton {
            objectName: "vehicleForget"
            flat: true
            Layout.fillWidth: true
            Layout.maximumWidth: moreRow.oneRow ? implicitWidth : Number.POSITIVE_INFINITY
            text: qsTr("Forget")
            enabled: root.host.ownPart.idle && root.host.pickedVehicle !== null
            onClicked: {
                root.moreShown = false
                root.host.confirming = "forget"
            }
        }
        ThemedButton {
            objectName: "vehicleForgetAll"
            flat: true
            Layout.fillWidth: true
            Layout.maximumWidth: moreRow.oneRow ? implicitWidth : Number.POSITIVE_INFINITY
            text: qsTr("Forget all vehicles")
            enabled: root.host.ownPart.idle && root.host.vehicles.list.length > 0
            onClicked: {
                root.moreShown = false
                root.host.confirming = "forgetAll"
            }
        }
    }

    // With the field empty, the defaults New and Duplicate take
    Label {
        textFormat: Text.PlainText
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
        textFormat: Text.PlainText
        objectName: "vehicleLabelProblem"
        visible: text !== "" && root.host.confirming === ""
        text: root.host.typedLabel === "" ? ""
            : VehicleBook.labelProblem(root.host.vehicles, root.host.typedLabel,
                                       root.host.pickedVehicle !== null ? root.host.pickedVehicle.id : "")
        color: Theme.warning
    }

    // Forget asks once, in place
    Flow {
        visible: root.host.confirming === "forget" || root.host.confirming === "forgetAll"
        Layout.fillWidth: true
        spacing: Theme.spacing

        Label {
            textFormat: Text.PlainText
            objectName: "vehicleConfirmText"
            text: root.host.confirming === "forgetAll" ? qsTr("Forget all vehicles?")
                : root.host.pickedVehicle !== null ? qsTr("Forget %1?").arg(root.host.pickedVehicle.label)
                : ""
            color: Theme.text
            wrapMode: Text.Wrap
            width: Math.min(implicitWidth, parent.width)
        }
        ThemedButton {
            objectName: "vehicleConfirm"
            text: qsTr("Forget")
            highlighted: true
            enabled: root.host.ownPart.idle
            onClicked: root.host.ownPart.forgetVehicles(root.host.confirming === "forgetAll")
        }
        ThemedButton {
            objectName: "vehicleCancel"
            text: qsTr("Cancel")
            onClicked: root.host.confirming = ""
        }
    }
}
