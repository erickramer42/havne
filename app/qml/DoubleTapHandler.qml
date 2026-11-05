// File/layer: app/qml/DoubleTapHandler.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_phone.qml): two taps on Live's chart
//          a little apart within the interval toggle it, two far apart
//          or past the interval don't, two clicks a little apart do; the
//          viewer's chart and splitter and Live's splitter through it too.

import QtQuick

// A TapHandler whose double tap allows a thumb's drift. Qt's own
// doubleTapped takes two touches as a double only within about 11
// logical px of each other, closer than a thumb's two taps land; this
// counts a tap within the double-click interval and kDoubleTapSlop of
// the one before. The handler's tapped still fires for
// every tap, so a page can use both
TapHandler {
    id: handler

    signal doubleTap

    // Android's double-tap slop is 100 dp; this is less, so two taps
    // meant for different places on a chart stay two taps
    readonly property real kDoubleTapSlop: 40

    property double lastTapMs: -1
    property point lastTapAt: Qt.point(0, 0)

    onTapped: eventPoint => {
        const now = Date.now()
        const at = eventPoint.scenePosition
        if (lastTapMs >= 0 && now - lastTapMs <= Application.styleHints.mouseDoubleClickInterval
                && Math.hypot(at.x - lastTapAt.x, at.y - lastTapAt.y) <= kDoubleTapSlop) {
            // A third tap starts afresh
            lastTapMs = -1
            handler.doubleTap()
        } else {
            lastTapMs = now
            lastTapAt = at
        }
    }
}
