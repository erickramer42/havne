// File/layer: app/qml/ConsentDialog.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.consent (test_qml)
// The consent screen on a wide window: a card over the window, the title by
// the plugin's class, the facts in a scroll, Decline and Allow pinned under
// them (the consent words, section 1). Drawn in place, not as a Popup,
// which would not scale with the zoom

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

FocusScope {
    id: root
    // The consent screen's map; shown while one waits for an answer
    property var screen: ({})
    property bool shown: false
    signal allowed()
    signal declined()

    visible: shown
    // The card's body, for a test
    readonly property var card: cardLoader.item

    // Escape is Decline, from any control in the card
    Keys.onEscapePressed: event => {
        root.declined()
        event.accepted = true
    }

    // The window behind takes no press or wheel
    Rectangle {
        anchors.fill: parent
        color: "#b0000000"
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onWheel: wheel => wheel.accepted = true
        }
    }

    Loader {
        id: cardLoader
        anchors.fill: parent
        active: root.shown
        sourceComponent: Rectangle {
            id: card
            objectName: "consentCard"
            readonly property real margin: Theme.pageMargin
            x: Math.round((parent.width - width) / 2)
            y: Math.round((parent.height - height) / 2)
            width: Math.min(760, parent.width - 2 * margin)
            height: Math.min(parent.height - 2 * margin, content.implicitHeight + 2 * margin)
            radius: Theme.radiusCard
            // The buttons are Theme.elevated: a card of that color would hide them
            color: Theme.surface
            border.width: 1
            border.color: Theme.outline
            readonly property var body: facts
            readonly property var buttons: choices

            Component.onCompleted: choices.declineButton.forceActiveFocus()

            ColumnLayout {
                id: content
                anchors.fill: parent
                anchors.margins: card.margin
                spacing: Theme.spacing * 2

                Label {
                    textFormat: Text.PlainText
                    objectName: "consentTitle"
                    text: facts.title
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Accessible.role: Accessible.Heading
                }
                Flickable {
                    id: flick
                    objectName: "consentScroll"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: facts.implicitHeight
                    Layout.minimumHeight: Math.min(facts.implicitHeight, 120)
                    clip: true
                    contentWidth: width
                    contentHeight: facts.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {
                        policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
                    }
                    ConsentBody {
                        id: facts
                        width: flick.width - 14
                        ask: root.screen
                        compact: false
                    }
                }
                ConsentButtons {
                    id: choices
                    ask: root.screen
                    onAllowChosen: root.allowed()
                    onDeclineChosen: root.declined()
                }
            }
        }
    }
}
