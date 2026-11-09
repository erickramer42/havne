// File/layer: app/qml/SettingsOwnForm.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The form for a vehicle's own signal (Add a signal, Edit): a page of its own in Settings

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    property alias formName: formName
    property alias formRx: formRx
    property alias formTx: formTx
    property alias formNumber: formNumber
    property alias formByte: formByte
    property alias formBits: formBits
    property alias formBit: formBit
    property alias formBytes: formBytes
    property alias formSigned: formSigned
    property alias formScale: formScale
    property alias formOffset: formOffset
    property alias formUnit: formUnit
    property alias formDecimals: formDecimals
    property alias formValues: formValues

    objectName: "signalForm"
    visible: root.host.pickedVehicle !== null
    Layout.fillWidth: true
    spacing: Theme.spacing

    Flow {
        id: formNameRow
        Layout.fillWidth: true
        spacing: Theme.spacing
        Captioned {
            caption: qsTr("Name")
            TextField {
                id: formName
                objectName: "formName"
                placeholderText: qsTr("Name")
                width: Math.min(formNameRow.width, Theme.controlHeight * 8)
            }
        }
        Captioned {
            caption: qsTr("Reply ID")
            TextField {
                id: formRx
                objectName: "formRx"
                text: "7E8"
                placeholderText: qsTr("Reply ID")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            caption: qsTr("Request ID")
            TextField {
                id: formTx
                objectName: "formTx"
                placeholderText: qsTr("Request ID")
                width: Theme.controlHeight * 3
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Repeater {
            model: [{ name: "01", label: qsTr("01 PID") }, { name: "22", label: qsTr("22 DID") }]
            ThemedButton {
                required property var modelData
                objectName: "formService_" + modelData.name
                text: modelData.label
                checkable: true
                checked: root.host.formService === modelData.name
                onClicked: root.host.formService = modelData.name
            }
        }
        Captioned {
            caption: root.host.formService === "22" ? qsTr("DID") : qsTr("PID")
            TextField {
                id: formNumber
                objectName: "formNumber"
                placeholderText: root.host.formService === "22" ? qsTr("DID, 4 hex digits")
                                                           : qsTr("PID, 2 hex digits")
                width: Theme.controlHeight * 5
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Repeater {
            model: SignalForm.kinds
            ThemedButton {
                required property string modelData
                objectName: "formKind_" + modelData
                text: modelData
                checkable: true
                checked: root.host.formKind === modelData
                onClicked: root.host.formKind = modelData
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Captioned {
            caption: qsTr("Byte")
            TextField {
                id: formByte
                objectName: "formByte"
                text: "0"
                placeholderText: qsTr("Byte")
                width: Theme.controlHeight * 2.5
            }
        }
        Captioned {
            caption: qsTr("Bits")
            visible: root.host.formKind === "number" || root.host.formKind === "enum"
            TextField {
                id: formBits
                objectName: "formBits"
                text: "8"
                placeholderText: qsTr("Bits")
                width: Theme.controlHeight * 2.5
            }
        }
        Captioned {
            caption: qsTr("Start bit")
            visible: root.host.formKind === "number" || root.host.formKind === "enum"
            TextField {
                id: formBit
                objectName: "formBit"
                placeholderText: qsTr("Bit")
                width: Theme.controlHeight * 2.5
            }
        }
        Captioned {
            caption: qsTr("Bytes")
            visible: !(root.host.formKind === "number" || root.host.formKind === "enum")
            TextField {
                id: formBytes
                objectName: "formBytes"
                placeholderText: qsTr("Bytes (all)")
                width: Theme.controlHeight * 3
            }
        }
        Repeater {
            model: root.host.formKind === "number" || root.host.formKind === "enum" ? ["be", "le"] : []
            ThemedButton {
                required property string modelData
                objectName: "formOrder_" + modelData
                text: modelData
                checkable: true
                checked: root.host.formOrder === modelData
                onClicked: root.host.formOrder = modelData
            }
        }
        ThemedCheckBox {
            id: formSigned
            objectName: "formSigned"
            visible: root.host.formKind === "number"
            text: qsTr("Signed")
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: root.host.formKind === "number"
        Captioned {
            caption: qsTr("Scale")
            TextField {
                id: formScale
                objectName: "formScale"
                placeholderText: qsTr("Scale (1)")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            caption: qsTr("Offset")
            TextField {
                id: formOffset
                objectName: "formOffset"
                placeholderText: qsTr("Offset (0)")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            caption: qsTr("Unit")
            TextField {
                id: formUnit
                objectName: "formUnit"
                placeholderText: qsTr("Unit")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            caption: qsTr("Decimals")
            TextField {
                id: formDecimals
                objectName: "formDecimals"
                placeholderText: qsTr("Decimals")
                width: Theme.controlHeight * 3
            }
        }
    }

    TextField {
        id: formValues
        objectName: "formValues"
        visible: root.host.formKind === "enum"
        placeholderText: qsTr("Values: 0=Park, 1=Reverse, 2=Neutral")
        Layout.fillWidth: true
    }

    LabeledFlow {
        label: qsTr("Group:")
        Repeater {
            model: SignalForm.groups
            ThemedButton {
                required property string modelData
                objectName: "formGroup_" + (modelData === "" ? "other" : modelData)
                text: modelData === "" ? qsTr("other") : modelData
                checkable: true
                checked: root.host.formGroup === modelData
                onClicked: root.host.formGroup = modelData
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "formReadOnce"
            text: Bridge.readingOnce ? qsTr("Reading...") : qsTr("Read once")
            enabled: root.host.idle && !Bridge.readingOnce && root.host.confirming === ""
            onClicked: root.host.readOnceDraft()
        }
        ThemedButton {
            objectName: "formSave"
            text: qsTr("Save")
            enabled: root.host.idle && root.host.confirming === ""
            onClicked: {
                if (root.host.saveDraft()) {
                    // Saved: the form empties for the next, and Signals shows the note
                    const note = root.host.formNote
                    root.host.clearForm()
                    root.host.formNote = note
                    root.host.back()
                }
            }
        }
        ThemedButton {
            objectName: "formClear"
            flat: true
            text: qsTr("Clear")
            onClicked: root.host.clearForm()
        }
    }

    Label {
        objectName: "formReadResult"
        visible: text !== ""
        text: root.host.readResultText(root.host.readResult)
        color: root.host.readResult !== null && root.host.readResult.ok ? Theme.text : Theme.warning
        wrapMode: Text.WrapAnywhere
        Layout.fillWidth: true
    }
    Label {
        objectName: "formProblem"
        visible: text !== ""
        text: root.host.formProblem
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        objectName: "formNote"
        visible: text !== ""
        text: root.host.formNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
}
