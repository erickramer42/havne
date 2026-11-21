// File/layer: app/qml/DashPluginPages.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashedit_plugpages (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The pages the loaded plugins carry for the shown form, listed under the
// Add page list (ADR-0052's appendix on pages), a part of DashTopBar

ColumnLayout {
    id: part
    required property DashboardPage host
    // The bar's own width: a Flow takes its unwrapped content as its least
    readonly property real barWidth: host.topBarItem.width
    readonly property var list: host.editsPart.pluginPageList

    objectName: "dashPluginPages"
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    Layout.preferredWidth: 1
    Layout.maximumWidth: barWidth
    visible: host.editing && host.presetList === "add" && list.length > 0
    spacing: 4

    Label {
        objectName: "plugpage_heading"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: qsTr("From a plugin")
    }
    Repeater {
        model: part.list
        Flow {
            id: group
            required property var modelData
            required property int index
            objectName: "plugpage_group_" + index
            readonly property real touchMin: Theme.touchTarget
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            Layout.maximumWidth: part.barWidth
            spacing: 8
            Label {
                textFormat: Text.PlainText
                objectName: "plugpage_name_" + group.index
                // A long name stays inside the bar: a Flow lets a child run past its own width
                width: Math.min(implicitWidth, group.width)
                elide: Text.ElideRight
                height: Theme.touchTarget
                verticalAlignment: Text.AlignVCenter
                color: group.modelData.picked ? Theme.text : Theme.textDim
                font.pixelSize: Theme.fontCaption
                text: group.modelData.name
            }
            Repeater {
                model: group.modelData.pages
                ThemedButton {
                    id: pick
                    required property var modelData
                    objectName: "plugpage_" + group.index + "_" + modelData.index
                    compact: part.host.form === "phone"
                    enabled: DashboardEdit.canAddPage(part.host.doc)
                    text: modelData.name
                    Accessible.name: qsTr("Add the page %1 from the plugin %2")
                                         .arg(modelData.name).arg(group.modelData.name)
                    onClicked: part.host.editsPart.addPluginPage(group.modelData.id, modelData.index)
                }
            }
        }
    }
}
