// File/layer: app/qml/Immersive.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.driveplatform (test_qml); BENCH for the bars themselves

pragma Singleton
import QtQuick

// Hiding the system bars for the drive view (ADR-0059 section 5): a
// full-screen window hides them on Android, and comes back by swipe. It
// remembers the window's visibility and puts that value back, since a
// phone's window isn't Window.Windowed. The phone form only.
QtObject {
    property var window: null
    property int restoreTo: Window.Hidden

    function enter(w, form) {
        if (form !== "phone" || window !== null)
            return
        window = w
        restoreTo = w.visibility
        w.visibility = Window.FullScreen
    }

    function leave() {
        if (window === null)
            return
        window.visibility = restoreTo
        window = null
    }
}
