// File/layer: app/qml/SettingsPlugins.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The app's plugins and the VIN's variant matches: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // ---- The app's plugins (ADR-0052): every vehicle's, the
    // picked one's variant proposed by its VIN ----
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

    // The variants the VIN proposes for the picked vehicle; the
    // user picks one (ADR-0052 section 5)
    Label {
        objectName: "pluginMatchesText"
        visible: root.host.pickedVehicle !== null && root.host.pluginList.length > 0
        text: root.host.pluginMatches.length > 0
            ? qsTr("For this car, by its VIN:")
            : qsTr("No match yet: plugins are proposed once the car's VIN is "
                   + "read, or one is typed in the VIN lookup")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Flow {
        visible: root.host.pickedVehicle !== null
        Layout.fillWidth: true
        spacing: Theme.spacing
        Repeater {
            model: root.host.pickedVehicle !== null ? root.host.pluginMatches : []
            ThemedButton {
                required property var modelData
                required property int index
                objectName: "pluginMatch_" + index
                width: Math.min(implicitWidth, parent.width)
                text: qsTr("%1: %2").arg(modelData.name, modelData.variant)
                highlighted: root.host.pickedVehicle !== null
                             && root.host.pickedVehicle.plugin === modelData.profile
                enabled: root.host.idle
                onClicked: root.host.pickPluginVariant(modelData.profile)
            }
        }
    }

    FileDialog {
        id: pluginDialog
        title: qsTr("Import a plugin")
        nameFilters: [qsTr("Plugins (*.havne-plugin.json)"), qsTr("All files (*)")]
        onAccepted: root.host.importPlugin(pluginDialog.selectedFile)
    }
}
