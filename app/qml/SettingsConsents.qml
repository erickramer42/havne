// File/layer: app/qml/SettingsConsents.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.consent, QmlTest.phone_* (test_qml)
// The plugins whose code talks to the car, each with its consent, and the
// consents given, each with Withdraw (ADR-0066): one part of SettingsPage's
// Signals page. Nothing shows until a plugin talks or a consent is kept

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    objectName: "settingsConsents"
    required property SettingsPage host

    // The bridge's lists, copied once into plain JS: a row reads them often
    readonly property var records: JSON.parse(JSON.stringify(Bridge.consent.records))
    readonly property var talking: host.picksPart.pluginList.filter(p => p.ok && p.conversation === true)
    // What the last Review... or Withdraw said
    property string note: ""

    // The words a plugin's row says of its consent
    function allowedRecord(plugin) {
        return root.records.find(r => r.fingerprint === plugin.fingerprint) ?? null
    }
    function stateWords(plugin) {
        const record = allowedRecord(plugin)
        return record !== null ? qsTr("Allowed for %1").arg(record.version)
                               : qsTr("Not allowed. Reads signals only.")
    }
    function givenOn(record) {
        return new Date(record.givenMs).toLocaleDateString(Qt.locale(), Locale.ShortFormat)
    }

    visible: talking.length > 0 || records.length > 0
    Layout.fillWidth: true
    spacing: Theme.spacing

    onVisibleChanged: if (visible) Bridge.consent.refresh()
    Component.onCompleted: Bridge.consent.refresh()

    Connections {
        target: Bridge.consent
        function onGiven(ok, words) {
            root.note = ok ? qsTr("Consent saved.") : qsTr("Consent not saved: %1").arg(words)
        }
        function onWithdrawn(ok, words) {
            root.note = ok ? qsTr("Consent withdrawn.") : qsTr("Consent not withdrawn: %1").arg(words)
        }
    }

    Label {
        textFormat: Text.PlainText
        text: qsTr("Plugins")
        font.pixelSize: Theme.fontEmphasis
        font.weight: Font.DemiBold
        color: Theme.text
        Layout.fillWidth: true
    }
    Label {
        textFormat: Text.PlainText
        objectName: "consentsNote"
        visible: text !== ""
        text: root.note
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // Plugins that talk to the car: their tier and consent
    Repeater {
        model: root.talking
        RowLayout {
            id: pluginRow
            required property var modelData
            required property int index
            objectName: "consentPlugin_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing
            readonly property bool allowed: root.allowedRecord(modelData) !== null

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 2
                Label {
                    textFormat: Text.PlainText
                    text: pluginRow.modelData.name + " " + pluginRow.modelData.version
                    color: Theme.text
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "consentPluginState"
                    text: root.stateWords(pluginRow.modelData)
                    color: pluginRow.allowed ? Theme.text : Theme.textDim
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
            }
            ThemedButton {
                objectName: "consentReview"
                // A plugin with no publisher asks no consent: nothing to review
                visible: !pluginRow.allowed && pluginRow.modelData.publisher !== ""
                compact: true
                Layout.minimumWidth: Theme.touchTarget
                text: qsTr("Review…")
                enabled: !Bridge.consent.busy && !Bridge.connecting
                Accessible.name: qsTr("Review %1").arg(pluginRow.modelData.name)
                onClicked: root.host.reviewRequested(pluginRow.modelData.sha256)
            }
        }
    }

    // The consents kept, newest first
    Repeater {
        model: root.records
        RowLayout {
            id: recordRow
            required property var modelData
            required property int index
            objectName: "consentRecord_" + index
            Layout.fillWidth: true
            spacing: Theme.spacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 2
                Label {
                    textFormat: Text.PlainText
                    text: recordRow.modelData.id + " " + recordRow.modelData.version
                    color: Theme.text
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "consentRecordWords"
                    text: qsTr("%1, allowed %2").arg(recordRow.modelData.publisher).arg(root.givenOn(recordRow.modelData))
                    color: Theme.textDim
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
            }
            ThemedButton {
                objectName: "consentWithdraw"
                flat: true
                compact: true
                Layout.minimumWidth: Theme.touchTarget
                text: qsTr("Withdraw")
                enabled: !Bridge.consent.busy
                Accessible.name: qsTr("Withdraw consent for %1 %2").arg(recordRow.modelData.id).arg(recordRow.modelData.version)
                onClicked: Bridge.consent.withdraw(recordRow.modelData.id, recordRow.modelData.fingerprint)
            }
        }
    }
}
