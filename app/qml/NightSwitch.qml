// File/layer: app/qml/NightSwitch.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.phone (test_qml)

import QtQuick

// The one-tap Night switch (ADR-0058 section 6): a dot and the word
// "Night", in the Dashboard's header, no capsule: the dot
// is filled and the word full text while Night shows, a ring and dim words
// when it doesn't. A tap is Theme.toggleNight(): in On and Off it flips
// the mode, under a schedule it holds the opposite until the next boundary
// and a second tap hands back to the schedule. Its hit area is a phone's
// touch target each way; the dot and word draw inside it.
ThemedButton {
    id: control
    objectName: "nightSwitch"

    readonly property bool shown: Theme.nightActive
    readonly property real rowHeight: 32
    readonly property real dotSize: 10

    compact: true
    checkable: true
    checked: shown
    implicitHeight: Math.max(Theme.touchTarget, rowHeight)
    implicitWidth: Math.max(Theme.touchTarget, words.implicitWidth + dotSize + 18)
    text: qsTr("Night")
    Accessible.name: Theme.nightSwitchName()
    onClicked: Theme.toggleNight()

    background: Rectangle {
        color: "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.text
    }

    contentItem: Row {
        spacing: 6
        Rectangle {
            id: dot
            width: control.dotSize
            height: control.dotSize
            radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            color: control.shown ? Theme.text : "transparent"
            border.width: 1.5
            border.color: control.shown ? Theme.text : Theme.textDim
        }
        Text {
            id: words
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.pixelSize: Theme.fontCaption
            color: control.shown ? Theme.text : Theme.textDim
        }
    }
}
