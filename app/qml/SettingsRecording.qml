// File/layer: app/qml/SettingsRecording.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The recording switch and where recordings go: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // ---- Recording (ADR-0017) ----
    // Off until switched on, and kept between runs. It takes effect
    // at the next connect, so it's fixed while a session is up and
    // each recording covers a whole session.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing

        ThemedCheckBox {
            id: recordBox
            objectName: "recordBox"
            text: qsTr("Record sessions")
            checked: Bridge.recording
            enabled: root.host.idle
            // The bridge decides; the binding comes back either way
            onToggled: {
                Bridge.recording = checked
                checked = Qt.binding(() => Bridge.recording)
            }
            contentItem: Label {
                text: recordBox.text
                color: recordBox.enabled ? Theme.text : Theme.textDim
                leftPadding: recordBox.indicator.width + recordBox.spacing
                verticalAlignment: Text.AlignVCenter
            }
        }
        Label {
            objectName: "recordStatus"
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            color: Bridge.recordingActive ? Theme.success : Theme.textDim
            text: Bridge.recordingActive
                  ? qsTr("Recording to %1").arg(Bridge.recordingFolder)
                  : Bridge.recording && root.host.replayInField
                    ? qsTr("Replays aren't recorded")
                  : Bridge.recordingFolder !== ""
                    ? qsTr("Last: %1").arg(Bridge.recordingFolder)
                  : Bridge.recording
                    ? qsTr("Frames, values and events, from the next connect")
                  : ""
        }
        // A phone has no app that opens the app's own folders
        ThemedButton {
            text: qsTr("Open folder")
            visible: Bridge.recordingFolder !== "" && !Theme.mobile
            onClicked: Qt.openUrlExternally(Bridge.recordingFolderUrl)
        }
    }
}
