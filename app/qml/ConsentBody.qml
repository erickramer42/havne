// File/layer: app/qml/ConsentBody.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.consent, QmlTest.phone_* (test_qml)
// What a consent shows, wide and phone alike: what changed, Havne's lines,
// the publisher, the terms whole and the fingerprint (the consent words,
// sections 1-3). Plain text throughout; the terms are the publisher's

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    // The consent screen's map (Bridge.consent.ask)
    required property var ask
    // The phone's form: labels over their values, the terms one block of
    // the page's scroll
    property bool compact: Theme.compact
    // The terms box's height where it scrolls on its own
    property real termsHeight: 220

    readonly property bool again: (ask.changed ?? ({})).version !== undefined
    readonly property var classes: ask.classes ?? []
    // The title by the class of the plugin's operations; a version change
    // has one of its own
    readonly property string title: again ? qsTr("This plugin has changed. Allow it again?")
        : classes.indexOf("write") >= 0
            ? qsTr("Allow this plugin to unlock and write to your car's modules?")
        : classes.indexOf("read") >= 0
            ? qsTr("Allow this plugin to unlock and read your car's modules?")
        : qsTr("Allow this plugin to talk to your car?")
    readonly property string shortTitle: again ? qsTr("This plugin has changed") : qsTr("Allow this plugin?")
    readonly property string termsText: ask.terms ?? ""
    readonly property int termsKb: {
        // The terms' bytes in UTF-8
        const bytes = encodeURIComponent(termsText).replace(/%[0-9A-Fa-f]{2}/g, "x").length
        return Math.ceil(bytes / 1024)
    }
    property bool fullShown: false

    // The contact on the clipboard: QML has no clipboard of its own
    function copyContact() {
        clipboardBuffer.text = ask.publisher.contact
        clipboardBuffer.selectAll()
        clipboardBuffer.copy()
    }

    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    TextEdit {
        id: clipboardBuffer
        visible: false
        Layout.preferredWidth: 0
        Layout.preferredHeight: 0
    }

    ConsentChanged {
        ask: root.ask
    }

    // Havne's lines: core's constants, the same for every plugin
    ColumnLayout {
        objectName: "consentLines"
        Layout.fillWidth: true
        spacing: Theme.spacing
        Repeater {
            model: root.ask.lines ?? []
            ColumnLayout {
                id: line
                required property var modelData
                required property int index
                objectName: "consentLine_" + modelData.key
                Layout.fillWidth: true
                spacing: 0
                Label {
                    textFormat: Text.PlainText
                    visible: line.modelData.heading !== ""
                    text: line.modelData.heading
                    color: Theme.text
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "consentLineText"
                    text: line.modelData.text
                    color: Theme.text
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
            }
        }
    }

    // The publisher's block, each value as text: never a link, never cut
    GridLayout {
        objectName: "consentPublisher"
        columns: root.compact ? 1 : 2
        columnSpacing: Theme.spacing * 2
        rowSpacing: root.compact ? 2 : Theme.spacing
        Layout.fillWidth: true

        Label {
            textFormat: Text.PlainText
            text: qsTr("Publisher")
            color: Theme.textDim
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: root.compact ? Theme.spacing : 0
        }
        Label {
            textFormat: Text.PlainText
            objectName: "consentPublisherName"
            text: root.ask.publisher.name
            color: Theme.text
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.preferredWidth: 0
        }

        Label {
            textFormat: Text.PlainText
            text: qsTr("Contact")
            color: Theme.textDim
            Layout.alignment: root.compact ? Qt.AlignTop : Qt.AlignVCenter
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing
            Label {
                textFormat: Text.PlainText
                objectName: "consentContact"
                text: root.ask.publisher.contact
                color: Theme.text
                wrapMode: Text.Wrap
                // As wide as its text, so Copy sits beside it, and shorter when the row is
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: Math.ceil(implicitWidth)
            }
            ThemedButton {
                objectName: "consentCopyContact"
                flat: true
                compact: true
                Layout.minimumWidth: Theme.touchTarget
                text: qsTr("Copy")
                Accessible.name: qsTr("Copy the contact")
                onClicked: root.copyContact()
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Label {
            textFormat: Text.PlainText
            text: qsTr("License")
            color: Theme.textDim
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: root.compact ? Theme.spacing : 0
        }
        Label {
            textFormat: Text.PlainText
            objectName: "consentLicense"
            text: root.ask.publisher.license
            color: Theme.text
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.preferredWidth: 0
        }

        Label {
            textFormat: Text.PlainText
            text: qsTr("Plugin")
            color: Theme.textDim
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: root.compact ? Theme.spacing : 0
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                textFormat: Text.PlainText
                objectName: "consentPlugin"
                text: root.ask.id + " " + root.ask.version
                color: Theme.text
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.preferredWidth: 0
            }
            Label {
                textFormat: Text.PlainText
                objectName: "consentFingerprint"
                text: qsTr("Fingerprint %1").arg(root.ask.fingerprintShort.toUpperCase())
                color: Theme.text
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.preferredWidth: 0
            }
            ThemedButton {
                objectName: "consentShowFull"
                flat: true
                compact: true
                Layout.minimumWidth: Theme.touchTarget
                text: root.fullShown ? qsTr("Hide full fingerprint") : qsTr("Show full fingerprint")
                onClicked: root.fullShown = !root.fullShown
            }
            // Selectable, to compare with the digits the CLI prints
            TextEdit {
                objectName: "consentFingerprintFull"
                visible: root.fullShown
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
                text: root.ask.fingerprint
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                font.family: Theme.monoFamily
                Accessible.name: qsTr("Full fingerprint")
                Layout.fillWidth: true
                Layout.preferredWidth: 0
            }
        }
    }

    // The terms, whole. A box that scrolls on its own where there is room;
    // a phone's is one block of the page's scroll, since a scroll inside a
    // scroll fights the thumb. Both are read-only and take the focus
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing / 2
        RowLayout {
            Layout.fillWidth: true
            Label {
                textFormat: Text.PlainText
                text: qsTr("Terms")
                color: Theme.textDim
                Layout.fillWidth: true
            }
            Label {
                textFormat: Text.PlainText
                objectName: "consentTermsSize"
                visible: root.termsKb > 2
                text: qsTr("%1 KB").arg(root.termsKb)
                color: Theme.textDim
            }
        }
        ScrollView {
            id: termsBox
            visible: !root.compact
            Layout.fillWidth: true
            Layout.preferredHeight: root.termsHeight
            clip: true
            TextArea {
                objectName: "pluginTerms"
                readOnly: true
                activeFocusOnTab: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                text: root.compact ? "" : root.termsText
                color: Theme.text
                font.pixelSize: Theme.fontBody
                Accessible.name: qsTr("Plugin terms")
                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.bg
                    border.width: parent.activeFocus ? 2 : 0
                    border.color: Theme.primary
                }
            }
        }
        TextEdit {
            objectName: "pluginTermsBlock"
            visible: root.compact
            readOnly: true
            activeFocusOnTab: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            text: root.compact ? root.termsText : ""
            color: Theme.text
            font.pixelSize: Theme.fontBody
            Accessible.name: qsTr("Plugin terms")
            Layout.fillWidth: true
            Layout.preferredWidth: 0
        }
    }
}
