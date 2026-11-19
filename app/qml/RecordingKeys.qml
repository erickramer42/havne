// File/layer: app/qml/RecordingKeys.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

// The Recordings page's keys: F, the arrows, Home, End and B, a part of RecordingsPage

Item {
    id: root
    required property RecordingsPage host
    visible: false
    // F on a keyboard; a text field takes its own letters first
    Shortcut {
        sequence: "F"
        enabled: root.host.visible && root.host.recording.state === RecordingView.Open && !Theme.mobile
                 && !root.host.boxFocused
        onActivated: root.host.refreshPart.toggleMaximized()
    }
    Shortcut {
        sequence: "Left"
        enabled: root.host.cursorKeys
        onActivated: root.host.cursorsPart.stepCursor(false)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.host.cursorKeys
        onActivated: root.host.cursorsPart.stepCursor(true)
    }
    Shortcut {
        sequence: "Home"
        enabled: root.host.editKeys
        onActivated: root.host.cursorsPart.cursorToEnd(false)
    }
    Shortcut {
        sequence: "End"
        enabled: root.host.editKeys
        onActivated: root.host.cursorsPart.cursorToEnd(true)
    }
    Shortcut {
        sequence: "B"
        enabled: root.host.editKeys
        onActivated: root.host.arrowsMoveB = root.host.hasCursorB && !root.host.arrowsMoveB
    }
}
