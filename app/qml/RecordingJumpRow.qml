// File/layer: app/qml/RecordingJumpRow.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The viewer's Jump row (ADR-0056 section 6): previous and next for the
// recording's pulls and its cruises, a line saying where the view is, and
// the switch for the stretch bands on the chart. The page owns the list,
// the view and the cursor; this row only asks for a jump.
Flow {
    id: row
    // Rows and groups a hair apart on a phone, where the chart wants the height
    spacing: Theme.compact ? 4 : Theme.spacing

    // "Pull 2 of 5 · 12.9 s", "No cruise found", "Looking for stretches..."
    property string status: ""
    property bool canPreviousPull: false
    property bool canNextPull: false
    property bool canPreviousCruise: false
    property bool canNextCruise: false
    property bool bandsShown: true
    property bool stretchesOpen: false
    signal jumpRequested(string kind, int direction)
    signal bandsToggled()
    signal stretchesToggled()

    component Group: RowLayout {
        id: group
        required property string kind
        required property string word
        required property bool canPrevious
        required property bool canNext
        spacing: 4
        ThemedButton {
            objectName: "jumpPrevious_" + group.kind
            compact: true
            flat: true
            text: "←"
            Accessible.name: qsTr("Previous %1").arg(group.word)
            enabled: group.canPrevious
            Layout.preferredWidth: Theme.touchTarget
            Layout.minimumHeight: Theme.touchTarget
            onClicked: row.jumpRequested(group.kind, -1)
        }
        Label {
            text: group.word.charAt(0).toUpperCase() + group.word.slice(1)
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        ThemedButton {
            objectName: "jumpNext_" + group.kind
            compact: true
            flat: true
            text: "→"
            Accessible.name: qsTr("Next %1").arg(group.word)
            enabled: group.canNext
            Layout.preferredWidth: Theme.touchTarget
            Layout.minimumHeight: Theme.touchTarget
            onClicked: row.jumpRequested(group.kind, 1)
        }
    }

    Group {
        kind: "pull"
        word: qsTr("pull")
        canPrevious: row.canPreviousPull
        canNext: row.canNextPull
    }
    Group {
        kind: "cruise"
        word: qsTr("cruise")
        canPrevious: row.canPreviousCruise
        canNext: row.canNextCruise
    }
    // On a phone the Bands switch is in the Stretches panel, so the row
    // keeps to two lines
    ThemedCheckBox {
        objectName: "jumpBands"
        visible: !Theme.compact
        text: qsTr("Bands")
        checked: row.bandsShown
        onToggled: row.bandsToggled()
    }
    // Selected while the panel of levels is open
    ThemedButton {
        objectName: "jumpStretches"
        compact: Theme.compact
        checkable: true
        checked: row.stretchesOpen
        height: Theme.touchTarget
        text: qsTr("Stretches…")
        onClicked: row.stretchesToggled()
    }
    Label {
        objectName: "jumpStatus"
        // The status takes a line of its own when the groups fill the row
        width: Math.min(implicitWidth, row.width)
        height: Theme.touchTarget
        verticalAlignment: Text.AlignVCenter
        text: row.status
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        elide: Text.ElideRight
    }
}
