// File/layer: app/qml/NightSwitch.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.phone (test_qml)

import QtQuick

// The one-tap Night switch (ADR-0058 section 6): a pill with a crescent
// and the word "Night", at the end of the Dashboard's link state. A tap is
// Theme.toggleNight(): in On and Off it flips the mode, under a schedule
// it holds the opposite until the next boundary and a second tap hands
// back to the schedule. Its hit area is a phone's touch target each way;
// the pill draws inside it.
ThemedButton {
    id: control
    objectName: "nightSwitch"

    readonly property bool shown: Theme.nightActive
    readonly property real pillHeight: 32

    compact: true
    highlighted: shown
    implicitHeight: Math.max(Theme.touchTarget, pillHeight)
    implicitWidth: Math.max(Theme.touchTarget, words.implicitWidth + crescent.width + 28)
    text: qsTr("Night")
    Accessible.name: Theme.nightSwitchName()
    onClicked: Theme.toggleNight()

    background: Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        height: control.pillHeight
        radius: height / 2
        color: control.shown ? Theme.primary : Theme.elevated
        border.width: control.shown ? 0 : 1
        border.color: Theme.outline
    }

    contentItem: Row {
        spacing: 6
        // Two circles, no font glyph: a filled one with another in the
        // pill's color cut out of it
        Item {
            id: crescent
            width: 14
            height: 14
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                width: 14; height: 14; radius: 7
                color: control.shown ? Theme.text : Theme.textDim
            }
            Rectangle {
                x: 5; y: -2
                width: 11; height: 11; radius: 5.5
                color: control.shown ? Theme.primary : Theme.elevated
            }
        }
        Text {
            id: words
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.pixelSize: 13
            color: control.shown ? Theme.text : Theme.textDim
        }
    }
}
