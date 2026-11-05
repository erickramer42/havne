// File/layer: app/qml/DoubleTapHandler.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_phone.qml): two taps on Live's chart
//          a little apart within the interval toggle it, two far apart
//          or past the interval don't, two clicks a little apart do; the
//          viewer's chart and splitter and Live's splitter through it too;
//          two clicks stamped close but handled a frame's time apart on
//          the maximized viewer's chart still toggle it.

import QtQuick

// A TapHandler whose double tap allows a thumb's drift. Qt's own
// doubleTapped takes two touches as a double only within about 11
// logical px of each other, closer than a thumb's two taps land; this
// counts a tap within the double-click interval and kDoubleTapSlop of
// the one before. The handler's tapped still fires for
// every tap, so a page can use both.
//
// The gap is the events' own: their timestamps, from the platform's
// input. A tap is handled only once the frame before it is done, and a
// slow frame (the maximized viewer's chart rasterized on the GUI thread,
// about 370 ms on the test phone) would otherwise put the second of two
// taps lifted 30 ms apart past the interval
TapHandler {
    id: handler

    signal doubleTap

    // Android's double-tap slop is 100 dp; this is less, so two taps
    // meant for different places on a chart stay two taps
    readonly property real kDoubleTapSlop: 40

    // The last tap's time in its event's clock (ms), -1 for none
    property double lastTapMs: -1
    property point lastTapAt: Qt.point(0, 0)

    onTapped: eventPoint => {
        // A platform that stamps nothing gives 0: the clock then
        const now = eventPoint.timestamp > 0 ? eventPoint.timestamp : Date.now()
        const at = eventPoint.scenePosition
        // Below 0 when the two came from different clocks: not a double
        const gap = now - lastTapMs
        if (lastTapMs >= 0 && gap >= 0 && gap <= Application.styleHints.mouseDoubleClickInterval
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
