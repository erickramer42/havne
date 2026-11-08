// File/layer: app/qml/SettingsDisplay.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// Zoom, units and developer mode: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // The whole UI's zoom; Main.qml applies and stores it
    LabeledFlow {
        label: qsTr("Display size:")
        ThemedButton {
            id: smaller
            text: "−"
            Accessible.name: qsTr("Smaller")
            enabled: Theme.zoom > Theme.zoomSteps[0] + 0.001
            onClicked: Theme.stepZoom(-1)
        }
        Label {
            width: Math.max(52, implicitWidth)
            height: smaller.height
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: qsTr("%1 %").arg(Math.round(Theme.zoom * 100))
            color: Theme.text
        }
        ThemedButton {
            text: "+"
            Accessible.name: qsTr("Larger")
            enabled: Theme.zoom < Theme.zoomSteps[Theme.zoomSteps.length - 1] - 0.001
            onClicked: Theme.stepZoom(1)
        }
        ThemedButton {
            text: qsTr("Reset")
            enabled: Math.abs(Theme.zoom - 1) > 0.001
            onClicked: Theme.zoom = 1
        }
    }
    // A phone has no keyboard to zoom with
    Label {
        visible: !Theme.mobile
        text: qsTr("or Ctrl + / Ctrl − / Ctrl 0")
        color: Theme.textDim
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // Units: what the pages show, converted in core (ADR-0022);
    // recordings keep the decoded values. Main.qml stores it.
    // Takes effect at once, connected or not.
    LabeledFlow {
        label: qsTr("Units:")
        Repeater {
            model: [
                { system: 0, name: "metric", label: qsTr("Metric (km/h, °C)") },
                { system: 1, name: "imperial", label: qsTr("Imperial (mph, °F)") }
            ]
            ThemedButton {
                required property var modelData
                objectName: "units_" + modelData.name
                text: modelData.label
                highlighted: Bridge.unitSystem === modelData.system
                onClicked: Bridge.unitSystem = modelData.system
            }
        }
    }

    // Developer mode (Theme.developerMode); Main.qml stores it
    CheckBox {
        id: developerBox
        objectName: "developerBox"
        text: qsTr("Developer mode: the mock:// preset, and the raw frames on the Log page")
        checked: Theme.developerMode
        Layout.fillWidth: true
        onToggled: {
            Theme.developerMode = checked
            checked = Qt.binding(() => Theme.developerMode)
        }
        contentItem: Label {
            text: developerBox.text
            color: Theme.text
            leftPadding: developerBox.indicator.width + developerBox.spacing
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
        }
    }
}
