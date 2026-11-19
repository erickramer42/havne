// File/layer: app/qml/RecordingWatchers.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

// The settle and refill timers, the part in view's frames and the frame test's animation, a part of RecordingsPage

Item {
    id: root
    required property RecordingsPage host
    visible: false
    property alias settle: settle
    property alias framesRefill: framesRefill
    property alias viewFrames: viewFrames
    Timer {
        id: settle
        interval: root.host.settleMs
        onTriggered: root.host.chart.moving = false
    }

    // The part in view's frames (ADR-0049's appendix "frames read back")
    RecordingFrames { id: viewFrames }
    Timer {
        id: framesRefill
        interval: root.host.settleMs
        onTriggered: root.host.cursorsPart.refillFrames()
    }

    FrameAnimation {
        running: root.host.frameTesting
        onTriggered: {
            const elapsed = (Date.now() - root.host.frameTest.begun) / 1000
            if (elapsed >= root.host.frameTestSeconds)
                root.host.frameTestPart.finishFrameTest()
            else
                root.host.frameTestPart.frameStep(elapsed)
        }
    }
    Connections {
        target: root.host.Window.window
        enabled: root.host.frameTesting
        function onFrameSwapped() {
            const zooming = (Date.now() - root.host.frameTest.begun) / 1000 >= root.host.frameTestSeconds / 2
            root.host.frameTest.frames[zooming ? 1 : 0] += 1
        }
    }

}
