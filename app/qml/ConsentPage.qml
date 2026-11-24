// File/layer: app/qml/ConsentPage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.consent, QmlTest.phone_* (test_qml)
// The consent screen on a phone: a page over the window with Settings'
// subpage's pinned title row and back arrow (Decline), one scrolling column
// and the two buttons pinned under it (the consent words, section 3)

import QtQuick
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
    readonly property var card: pageLoader.item

    Keys.onEscapePressed: event => {
        root.declined()
        event.accepted = true
    }

    // Opaque: the window behind takes no press
    Rectangle {
        anchors.fill: parent
        color: Theme.bg
        MouseArea {
            anchors.fill: parent
        }
    }

    Loader {
        id: pageLoader
        anchors.fill: parent
        active: root.shown
        sourceComponent: ColumnLayout {
            id: page
            objectName: "consentPageBody"
            readonly property var body: facts
            readonly property var buttons: choices
            spacing: 0
            Component.onCompleted: choices.declineButton.forceActiveFocus()

            SettingsSubpage {
                id: sub
                objectName: "consentPage"
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: facts.shortTitle
                backShown: true
                backName: qsTr("Decline and go back")
                // A reader hears the whole title
                Accessible.name: facts.title
                onBackRequested: root.declined()

                ConsentBody {
                    id: facts
                    ask: root.screen
                    compact: true
                    Layout.fillWidth: true
                }
            }
            // Pinned, above the gesture area: Decline left, Allow right
            Rectangle {
                objectName: "consentBar"
                Layout.fillWidth: true
                color: Theme.bg
                implicitHeight: choices.implicitHeight + 2 * Theme.pageMargin
                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.outline
                }
                ConsentButtons {
                    id: choices
                    x: Theme.pageMargin
                    y: Theme.pageMargin
                    width: parent.width - 2 * Theme.pageMargin
                    ask: root.screen
                    onAllowChosen: root.allowed()
                    onDeclineChosen: root.declined()
                }
            }
        }
    }
}
