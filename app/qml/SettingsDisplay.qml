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

    // Night (ADR-0058): off, on, or on a schedule; and its style, amber or
    // red on near black, for a cabin after dark. Each takes effect at once;
    // Theme.qml stores it. The Dashboard's switch is the same control
    LabeledFlow {
        label: qsTr("Night:")
        Repeater {
            model: [
                { mode: "off", label: qsTr("Off") },
                { mode: "on", label: qsTr("On") },
                { mode: "schedule", label: qsTr("Scheduled") }
            ]
            ThemedButton {
                required property var modelData
                objectName: "night_" + modelData.mode
                text: modelData.label
                checkable: true
                checked: Theme.clampNight(Theme.night) === modelData.mode
                onClicked: Theme.night = modelData.mode
            }
        }
    }
    // Chosen ahead of the evening too; the Dashboard's switch shows the result
    LabeledFlow {
        label: qsTr("Night style:")
        Repeater {
            model: [
                { style: "amber", label: qsTr("Amber") },
                { style: "red", label: qsTr("Red") }
            ]
            ThemedButton {
                required property var modelData
                objectName: "nightStyle_" + modelData.style
                text: modelData.label
                checkable: true
                checked: Theme.clampStyle(Theme.nightStyle) === modelData.style
                onClicked: Theme.nightStyle = modelData.style
            }
        }
    }
    // The schedule's window: a masked field takes a phone's numeric
    // keyboard and the desktop's alike (a popup picker would draw outside
    // the zoom). A time is stored when it is valid and the two differ
    ColumnLayout {
        id: schedule
        objectName: "nightSchedule"
        Layout.fillWidth: true
        visible: Theme.clampNight(Theme.night) === "schedule"
        spacing: Theme.spacing
        property string problem: ""
        // A field's text on leaving it: stored, or said why not
        function commit(isFrom, text) {
            const other = isFrom ? Theme.nightUntil : Theme.nightFrom
            if (Theme.minutesOf(text) < 0) {
                problem = qsTr("Not a time")
            } else if (text === other) {
                problem = qsTr("From and Until are the same time")
            } else {
                problem = ""
                if (isFrom)
                    Theme.nightFrom = text
                else
                    Theme.nightUntil = text
            }
        }
        RowLayout {
            spacing: Theme.spacing
            Label { text: qsTr("From"); color: Theme.textDim }
            TextField {
                id: fromField
                objectName: "nightFrom"
                implicitWidth: 80
                inputMask: "99:99"
                inputMethodHints: Qt.ImhDigitsOnly
                color: Theme.text
                text: Theme.nightFrom
                onEditingFinished: schedule.commit(true, text)
            }
            Label { text: qsTr("Until"); color: Theme.textDim }
            TextField {
                id: untilField
                objectName: "nightUntil"
                implicitWidth: 80
                inputMask: "99:99"
                inputMethodHints: Qt.ImhDigitsOnly
                color: Theme.text
                text: Theme.nightUntil
                onEditingFinished: schedule.commit(false, text)
            }
        }
        Label {
            objectName: "nightProblem"
            visible: schedule.problem !== ""
            text: schedule.problem
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "nightWords"
            text: Theme.scheduleWords()
            color: Theme.textDim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
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
                checkable: true
                checked: Bridge.unitSystem === modelData.system
                onClicked: Bridge.unitSystem = modelData.system
            }
        }
    }
}
