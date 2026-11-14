// File/layer: app/qml/SettingsPlugins.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The app's plugins and the picked vehicle's variants, the VIN's matches first: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // ---- The app's plugins (ADR-0052): every vehicle's, each
    // variant the picked one's to pick ----
    Label {
        objectName: "pluginsTitle"
        text: qsTr("Plugins")
        color: Theme.text
        font.bold: true
    }

    Repeater {
        model: root.host.pluginList
        RowLayout {
            id: pluginRow
            required property var modelData
            required property int index
            objectName: "plugin_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                objectName: "pluginText"
                text: root.host.pluginText(pluginRow.modelData)
                color: pluginRow.modelData.ok ? Theme.text : Theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            ThemedButton {
                objectName: "pluginRemove_" + pluginRow.index
                flat: true
                text: qsTr("Remove")
                enabled: root.host.idle && root.host.confirming === ""
                onClicked: root.host.removePlugin(pluginRow.modelData.sha256)
            }
        }
    }

    Label {
        objectName: "pluginsNone"
        visible: root.host.pluginList.length === 0
        text: qsTr("None yet. A plugin (a .havne-plugin.json file) holds signals "
                   + "for the cars its manufacturer codes and model years name")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "pluginImport"
            text: qsTr("Import a plugin")
            enabled: root.host.idle && root.host.confirming === ""
            onClicked: pluginDialog.open()
        }
    }

    Label {
        objectName: "pluginProblem"
        visible: text !== ""
        text: root.host.pluginProblem
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        objectName: "pluginNote"
        visible: text !== ""
        text: root.host.pluginNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // Every loaded plugin's variants for the picked vehicle, those a VIN
    // matched first; the user picks one, VIN or none (ADR-0052 section 5
    // and its appendix of 2026-11-13)
    Label {
        objectName: "pluginMatchesText"
        visible: root.host.pluginVariants.matched.length > 0
        text: qsTr("For this car, by its VIN:")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Flow {
        visible: root.host.pluginVariants.matched.length > 0
        Layout.fillWidth: true
        spacing: Theme.spacing
        Repeater {
            model: root.host.pluginVariants.matched
            ThemedButton {
                required property var modelData
                required property int index
                objectName: "pluginMatch_" + index
                width: Math.min(implicitWidth, parent.width)
                text: modelData.label
                checkable: true
                checked: root.host.pickedVehicle !== null
                             && root.host.pickedVehicle.plugin === modelData.profile
                enabled: root.host.idle
                onClicked: root.host.pickPluginVariant(modelData.profile)
            }
        }
    }
    Label {
        objectName: "pluginOthersText"
        visible: root.host.pluginVariants.others.length > 0
        text: root.host.pluginVariants.matched.length > 0
            ? qsTr("Other plugins:")
            : qsTr("Pick a variant for this vehicle:")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Flow {
        visible: root.host.pluginVariants.others.length > 0
        Layout.fillWidth: true
        spacing: Theme.spacing
        Repeater {
            model: root.host.pluginVariants.others
            ThemedButton {
                required property var modelData
                required property int index
                objectName: "pluginOther_" + index
                width: Math.min(implicitWidth, parent.width)
                text: modelData.label
                checkable: true
                checked: root.host.pickedVehicle !== null
                             && root.host.pickedVehicle.plugin === modelData.profile
                enabled: root.host.idle
                onClicked: root.host.pickPluginVariant(modelData.profile)
            }
        }
    }

    DocumentOpenDialog {
        id: pluginDialog
        objectName: "pluginDialog"
        title: qsTr("Import a plugin")
        nameFilters: [qsTr("Plugins (*.havne-plugin.json)"), qsTr("All files (*)")]
        onAccepted: root.host.importPlugin(pluginDialog.selectedFile)
        onFailed: words => {
            root.host.pluginNote = ""
            root.host.pluginProblem = words
        }
    }
}
