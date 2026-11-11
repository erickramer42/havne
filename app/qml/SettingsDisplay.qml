// File/layer: app/qml/SettingsDisplay.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone, QmlTest.driveview (test_qml)
// Zoom, the look, Night, units and the drive view's settings: one part of
// SettingsPage, which holds the zoom's and the units' state; the drive
// view's is stored here

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // The drive view's settings (ADR-0059 section 9): a phone's, and the
    // orientation only where the phone can hold one
    property bool driveShown: Theme.mobile
    property bool orientationShown: PhoneWindow.orientationLockAvailable
    Settings {
        id: driveSettings
        objectName: "driveSettings"
        category: "ui"
        property int driveHideMs: 4000
        property string driveOrientation: "follow"
    }
    readonly property int driveHideMs: driveSettings.driveHideMs
    readonly property string driveOrientation: driveSettings.driveOrientation

    // A closed choice: a caption, then a segment row under it. On a phone
    // the segments take equal shares of the width and their words wrap, so
    // no word can widen the page
    component ChoiceRow: ColumnLayout {
        id: choice
        property string caption
        property string prefix
        property var choices: []
        property var current
        signal picked(var key)
        readonly property bool shares: width < 560
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            text: choice.caption
            color: Theme.textDim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        SegmentRow {
            Layout.fillWidth: choice.shares
            Repeater {
                model: choice.choices
                SegmentButton {
                    required property var modelData
                    objectName: choice.prefix + modelData.key
                    share: choice.shares
                    labelSize: Theme.fontBody
                    wrapMode: choice.shares ? Text.Wrap : Text.NoWrap
                    tapHeight: Theme.touchTarget
                    Layout.preferredHeight: Math.max(Theme.touchTarget, implicitContentHeight)
                    text: modelData.label
                    selected: choice.current === modelData.key
                    onClicked: choice.picked(modelData.key)
                }
            }
        }
    }

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

    // The look (ADR-0058): Dark, or Daylight for a screen read in sun.
    // Night shows over either; Theme.qml stores it. Shown while there is
    // more than one look to choose
    ChoiceRow {
        visible: Theme.lookNames.length > 1
        caption: qsTr("Look:")
        prefix: "look_"
        choices: [
            { key: "dark", label: qsTr("Dark") },
            { key: "daylight", label: qsTr("Daylight") }
        ]
        current: Theme.clampLook(Theme.look)
        onPicked: key => Theme.look = key
    }

    // Night (ADR-0058): off, on, or on a schedule; and its style, amber or
    // red on near black, for a cabin after dark. Each takes effect at once;
    // Theme.qml stores it. The Dashboard's switch is the same control
    ChoiceRow {
        caption: qsTr("Night:")
        prefix: "night_"
        choices: [
            { key: "off", label: qsTr("Off") },
            { key: "on", label: qsTr("On") },
            { key: "schedule", label: qsTr("Scheduled") }
        ]
        current: Theme.clampNight(Theme.night)
        onPicked: key => Theme.night = key
    }
    // Chosen ahead of the evening too; the Dashboard's switch shows the result
    ChoiceRow {
        caption: qsTr("Night style:")
        prefix: "nightStyle_"
        choices: [
            { key: "amber", label: qsTr("Amber") },
            { key: "red", label: qsTr("Red") }
        ]
        current: Theme.clampStyle(Theme.nightStyle)
        onPicked: key => Theme.nightStyle = key
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
        // Its words go with it: they're about a time typed here
        onVisibleChanged: if (!visible) problem = ""
        // A field's text on leaving it: stored, or said why not; either way
        // the field shows the stored time again, bound to it
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
            if (isFrom)
                fromField.text = Qt.binding(() => Theme.nightFrom)
            else
                untilField.text = Qt.binding(() => Theme.nightUntil)
        }
        RowLayout {
            spacing: Theme.spacing
            Label { text: qsTr("From"); color: Theme.textDim }
            ThemedTextField {
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
            ThemedTextField {
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
    ChoiceRow {
        caption: qsTr("Units:")
        prefix: "units_"
        choices: [
            { key: "metric", label: qsTr("Metric (km/h, °C)") },
            { key: "imperial", label: qsTr("Imperial (mph, °F)") }
        ]
        current: Bridge.unitSystem === 0 ? "metric" : "imperial"
        onPicked: key => Bridge.unitSystem = key === "metric" ? 0 : 1
    }

    ChoiceRow {
        objectName: "driveHideRow"
        visible: root.driveShown
        caption: qsTr("Hide the drive view's bars after:")
        prefix: "driveHide_"
        choices: [
            { key: 2000, label: qsTr("2 s") },
            { key: 4000, label: qsTr("4 s") },
            { key: 8000, label: qsTr("8 s") },
            { key: 0, label: qsTr("Never") }
        ]
        current: driveSettings.driveHideMs
        onPicked: key => driveSettings.driveHideMs = key
    }
    ChoiceRow {
        objectName: "driveOrientationRow"
        visible: root.driveShown && root.orientationShown
        caption: qsTr("Drive view orientation:")
        prefix: "driveOrientation_"
        choices: [
            { key: "follow", label: qsTr("Follow the phone") },
            { key: "upright", label: qsTr("Upright") },
            { key: "side", label: qsTr("On the side") }
        ]
        current: driveSettings.driveOrientation
        onPicked: key => driveSettings.driveOrientation = key
    }
}
