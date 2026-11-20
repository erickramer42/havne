// File/layer: app/qml/RecordingViewerTitle.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The viewer's title row: back, the title, Replay, Save report, Compare and Export, a part of RecordingsPage

RowLayout {
    id: root
    required property RecordingsPage host
    objectName: "viewerTitleRow"
    visible: root.host.viewing && !root.host.chartMaximized
    Layout.fillWidth: true
    spacing: Theme.spacing
    // On a phone a quiet back arrow at the left edge, where a back
    // control is looked for
    ThemedButton {
        objectName: "backToList"
        flat: Theme.compact
        text: Theme.compact ? "←" : qsTr("All recordings")   // a left arrow
        Accessible.name: qsTr("All recordings")
        Layout.preferredWidth: Theme.compact ? Theme.touchTarget : -1
        onClicked: root.host.openPart.backToList()
    }
    Label {
        objectName: "openedTitle"
        text: root.host.openedTitle
        color: Theme.text
        font.pixelSize: Theme.fontEmphasis
        // The folder's date is at the end
        elide: Text.ElideMiddle
        Layout.fillWidth: true
    }
    // Replay and Save report...: here on the desktop, in About on a
    // phone, side by side
    RowLayout {
        id: titleActions
        visible: !Theme.compact
        spacing: Theme.spacing
        RowLayout {
            parent: Theme.compact ? root.host.aboutActions : titleActions
            visible: root.host.recording.state === RecordingView.Open
            Layout.fillWidth: Theme.compact
            spacing: Theme.spacing
            // The part in view replayed as a session in one tap, as
            // the list's Replay does; off while a session runs, as
            // the list's is
            ThemedButton {
                objectName: "replayView"
                visible: root.host.recording.hasFrames
                compact: Theme.compact
                Layout.fillWidth: Theme.compact
                Layout.preferredWidth: Theme.compact ? 1 : -1
                enabled: root.host.sessionIdle
                text: root.host.replayText
                Accessible.name: root.host.refreshPart.replayName(qsTr("Replay the part in view as a session"))
                onClicked: root.host.replayRequested(root.host.openPart.stretchUri())
            }
            ThemedButton {
                objectName: "saveReport"
                compact: Theme.compact
                Layout.fillWidth: Theme.compact
                Layout.preferredWidth: Theme.compact ? 1 : -1
                enabled: !root.host.recording.reporting
                text: qsTr("Save report…")
                onClicked: root.host.reportDialog.open()
            }
            // Selected while its controls are open above the chart
            ThemedButton {
                objectName: "compareButton"
                compact: Theme.compact
                Layout.fillWidth: Theme.compact
                Layout.preferredWidth: Theme.compact ? 1 : -1
                checkable: true
                checked: root.host.compareOpen
                text: qsTr("Compare…")
                onClicked: root.host.compareOpen = !root.host.compareOpen
            }
        }
    }
    ThemedButton {
        objectName: "exportButton"
        visible: root.host.recording.state === RecordingView.Open
        compact: Theme.compact
        text: qsTr("Export…")
        // Selected while its panel is open: Save as... is the primary there
        checkable: true
        checked: root.host.exportOpen
        onClicked: {
            if (root.host.exportOpen)
                root.host.reportPart.closeExport()
            else
                root.host.reportPart.openExport()
        }
    }
}
