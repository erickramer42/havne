// File/layer: app/qml/SettingsDisplay.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// Zoom and units: one part of SettingsPage, which holds the state

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
    // One row of four: a button never wraps alone onto a second line
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            text: qsTr("Display size:")
            color: Theme.textDim
        }
        RowLayout {
            spacing: Theme.spacing
            ThemedButton {
                id: smaller
                compact: true
                implicitWidth: Math.max(40, Theme.touchTarget)
                text: "−"
                Accessible.name: qsTr("Smaller")
                enabled: Theme.zoom > Theme.zoomSteps[0] + 0.001
                onClicked: Theme.stepZoom(-1)
            }
            Label {
                Layout.preferredWidth: Math.max(52, implicitWidth)
                Layout.preferredHeight: smaller.height
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: qsTr("%1 %").arg(Math.round(Theme.zoom * 100))
                color: Theme.text
            }
            ThemedButton {
                compact: true
                implicitWidth: Math.max(40, Theme.touchTarget)
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
                // A long label wraps rather than run past a phone's edge
                width: Math.min(implicitWidth, parent.width)
                objectName: "units_" + modelData.name
                text: modelData.label
                highlighted: Bridge.unitSystem === modelData.system
                onClicked: Bridge.unitSystem = modelData.system
            }
        }
    }
}
