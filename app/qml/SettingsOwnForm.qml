// File/layer: app/qml/SettingsOwnForm.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone_* (test_qml)
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
    property alias computedPart: computedPart
    readonly property bool computed: root.host.ownPart.formKind === "computed"

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
            ThemedTextField {
                id: formName
                objectName: "formName"
                placeholderText: qsTr("Name")
                width: Math.min(formNameRow.width, Theme.controlHeight * 8)
            }
        }
        Captioned {
            visible: !root.computed
            caption: qsTr("Reply ID")
            ThemedTextField {
                id: formRx
                objectName: "formRx"
                text: "7E8"
                placeholderText: qsTr("Reply ID")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            visible: !root.computed
            caption: qsTr("Request ID")
            ThemedTextField {
                id: formTx
                objectName: "formTx"
                placeholderText: qsTr("Request ID")
                width: Theme.controlHeight * 3
            }
        }
    }

    LabeledFlow {
        objectName: "formReadBy"
        visible: !root.computed
        label: qsTr("Read by:")
        Repeater {
            model: [{ name: "01", label: qsTr("01 PID") }, { name: "22", label: qsTr("22 DID") }]
            ThemedButton {
                required property var modelData
                objectName: "formService_" + modelData.name
                text: modelData.label
                checkable: true
                checked: root.host.ownPart.formService === modelData.name
                onClicked: root.host.ownPart.formService = modelData.name
            }
        }
        Captioned {
            caption: root.host.ownPart.formService === "22" ? qsTr("DID") : qsTr("PID")
            ThemedTextField {
                id: formNumber
                objectName: "formNumber"
                placeholderText: root.host.ownPart.formService === "22" ? qsTr("DID, 4 hex digits")
                                                           : qsTr("PID, 2 hex digits")
                width: Theme.controlHeight * 5
            }
        }
    }

    LabeledFlow {
        objectName: "formType"
        label: qsTr("Type:")
        Repeater {
            model: SignalForm.kinds
            ThemedButton {
                required property string modelData
                objectName: "formKind_" + modelData
                text: modelData
                checkable: true
                checked: root.host.ownPart.formKind === modelData
                onClicked: root.host.ownPart.formKind = modelData
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: !root.computed
        Captioned {
            caption: qsTr("Byte")
            ThemedTextField {
                id: formByte
                objectName: "formByte"
                text: "0"
                placeholderText: qsTr("Byte")
                width: Theme.controlHeight * 2.5
            }
        }
        Captioned {
            caption: qsTr("Bits")
            visible: root.host.ownPart.formKind === "number" || root.host.ownPart.formKind === "enum"
            ThemedTextField {
                id: formBits
                objectName: "formBits"
                text: "8"
                placeholderText: qsTr("Bits")
                width: Theme.controlHeight * 2.5
            }
        }
        Captioned {
            caption: qsTr("Start bit")
            visible: root.host.ownPart.formKind === "number" || root.host.ownPart.formKind === "enum"
            ThemedTextField {
                id: formBit
                objectName: "formBit"
                placeholderText: qsTr("Bit")
                width: Theme.controlHeight * 2.5
            }
        }
        Captioned {
            caption: qsTr("Bytes")
            visible: !(root.host.ownPart.formKind === "number" || root.host.ownPart.formKind === "enum")
            ThemedTextField {
                id: formBytes
                objectName: "formBytes"
                placeholderText: qsTr("Bytes (all)")
                width: Theme.controlHeight * 3
            }
        }
        Label {
            textFormat: Text.PlainText
            objectName: "formOrderLabel"
            visible: root.host.ownPart.formKind === "number" || root.host.ownPart.formKind === "enum"
            text: qsTr("Byte order:")
            color: Theme.textDim
            topPadding: 8
        }
        Repeater {
            model: root.host.ownPart.formKind === "number" || root.host.ownPart.formKind === "enum" ? ["be", "le"] : []
            ThemedButton {
                required property string modelData
                objectName: "formOrder_" + modelData
                text: modelData === "be" ? qsTr("big") : qsTr("little")
                checkable: true
                checked: root.host.ownPart.formOrder === modelData
                onClicked: root.host.ownPart.formOrder = modelData
            }
        }
        ThemedCheckBox {
            id: formSigned
            objectName: "formSigned"
            visible: root.host.ownPart.formKind === "number"
            text: qsTr("Signed")
        }
    }

    SettingsOwnComputed {
        id: computedPart
        host: root.host
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: root.host.ownPart.formKind === "number" || root.computed
        Captioned {
            visible: !root.computed
            caption: qsTr("Scale")
            ThemedTextField {
                id: formScale
                objectName: "formScale"
                placeholderText: qsTr("Scale (1)")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            visible: !root.computed
            caption: qsTr("Offset")
            ThemedTextField {
                id: formOffset
                objectName: "formOffset"
                placeholderText: qsTr("Offset (0)")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            caption: qsTr("Unit")
            ThemedTextField {
                id: formUnit
                objectName: "formUnit"
                placeholderText: qsTr("Unit")
                width: Theme.controlHeight * 3
            }
        }
        Captioned {
            caption: qsTr("Decimals")
            ThemedTextField {
                id: formDecimals
                objectName: "formDecimals"
                placeholderText: qsTr("Decimals")
                width: Theme.controlHeight * 3
            }
        }
    }

    ThemedTextField {
        id: formValues
        objectName: "formValues"
        visible: root.host.ownPart.formKind === "enum"
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
                checked: root.host.ownPart.formGroup === modelData
                onClicked: root.host.ownPart.formGroup = modelData
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "formReadOnce"
            text: Bridge.readingOnce ? qsTr("Reading...") : qsTr("Read once")
            visible: !root.computed
            enabled: root.host.ownPart.idle && !Bridge.readingOnce && root.host.confirming === ""
            onClicked: root.host.ownPart.readOnceDraft()
        }
        // The form's one action
        ThemedButton {
            objectName: "formSave"
            text: qsTr("Save")
            highlighted: true
            enabled: root.host.ownPart.idle && root.host.confirming === ""
            onClicked: {
                if (root.host.ownPart.saveDraft()) {
                    // Saved: the form empties for the next, and Signals shows the note
                    const note = root.host.ownPart.formNote
                    root.host.ownPart.clearForm()
                    root.host.ownPart.formNote = note
                    root.host.back()
                }
            }
        }
        ThemedButton {
            objectName: "formClear"
            flat: true
            text: qsTr("Clear")
            onClicked: root.host.ownPart.clearForm()
        }
    }

    Label {
        textFormat: Text.PlainText
        objectName: "formReadResult"
        visible: text !== ""
        text: root.host.ownPart.readResultText(root.host.ownPart.readResult)
        color: root.host.ownPart.readResult !== null && root.host.ownPart.readResult.ok ? Theme.text : Theme.warning
        wrapMode: Text.WrapAnywhere
        Layout.fillWidth: true
    }
    Label {
        textFormat: Text.PlainText
        objectName: "formProblem"
        visible: text !== ""
        text: root.host.ownPart.formProblem
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        textFormat: Text.PlainText
        objectName: "formNote"
        visible: text !== ""
        text: root.host.ownPart.formNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
}
