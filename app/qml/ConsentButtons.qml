// File/layer: app/qml/ConsentButtons.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.consent, QmlTest.phone_* (test_qml)
// The consent's last words and its two buttons: what Allow does, what
// Decline leaves, Decline and Allow side by side (the consent words,
// sections 1 and 3)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    // The consent screen's map (Bridge.consent.ask)
    required property var ask
    readonly property bool again: (ask.changed ?? ({})).version !== undefined
    // A short window drops the two notes the buttons stand without
    readonly property bool notes: !Theme.shortLayout
    readonly property alias declineButton: decline
    signal allowChosen()
    signal declineChosen()

    Layout.fillWidth: true
    spacing: Theme.spacing

    Label {
        textFormat: Text.PlainText
        objectName: "consentAllowLine"
        visible: root.notes
        text: qsTr("Allow %1 %2 to talk to your car's modules.").arg(root.ask.name).arg(root.ask.version)
        color: Theme.text
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.minimumWidth: 0
    }
    Label {
        textFormat: Text.PlainText
        objectName: "consentDeclineLine"
        text: qsTr("If you decline, Havne still reads with this plugin's signals. It sends nothing else.")
        color: Theme.text
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.minimumWidth: 0
    }
    // Decline is the default: it holds the focus, and Escape is Decline.
    // Neither is the page's primary, so the screen doesn't steer
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            id: decline
            objectName: "consentDecline"
            text: qsTr("Decline")
            focus: true
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.minimumHeight: Theme.touchTarget
            onClicked: root.declineChosen()
        }
        ThemedButton {
            objectName: "consentAllow"
            text: root.again ? qsTr("Allow this version") : qsTr("Allow")
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.minimumHeight: Theme.touchTarget
            onClicked: root.allowChosen()
        }
    }
    Label {
        textFormat: Text.PlainText
        objectName: "consentWithdrawLine"
        visible: root.notes
        text: qsTr("You can withdraw this in Settings, under Plugins.")
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.minimumWidth: 0
    }
}
