// File/layer: app/qml/ConsentChanged.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.consent, QmlTest.phone_* (test_qml)
// What changed since the version the user allowed: the lines above Havne's
// own on a consent asked again (the consent words, section 2)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    // The consent screen's map (Bridge.consent.ask); the block shows when
    // its `changed` holds a record
    required property var ask
    readonly property var changed: ask.changed ?? ({})
    readonly property bool shown: changed.version !== undefined
    // The same version number over other contents has words of its own
    readonly property bool sameNumber: shown && changed.version === ask.version
    readonly property string given: shown
        ? new Date(changed.givenMs).toLocaleDateString(Qt.locale(), Locale.ShortFormat) : ""
    readonly property var lines: {
        if (!shown)
            return []
        const out = []
        out.push(sameNumber
                 ? qsTr("You allowed %1 %2 on %3. This file is not the same plugin: its contents differ, though it still says %2.")
                       .arg(ask.name).arg(changed.version).arg(given)
                 : qsTr("You allowed %1 %2 on %3. This is %4.")
                       .arg(ask.name).arg(changed.version).arg(given).arg(ask.version))
        out.push(changed.publisherSame
                 ? qsTr("Same publisher: %1.").arg(ask.publisher.name)
                 : qsTr("The publisher is different. Before: %1. Now: %2.")
                       .arg(changed.publisher).arg(ask.publisher.name))
        out.push(changed.termsSame
                 ? qsTr("The terms are the same as before.")
                 : qsTr("The terms have changed. Read them below."))
        out.push(qsTr("Havne can't tell you what else changed. The plugin may now send other requests."))
        return out
    }

    visible: shown
    Layout.fillWidth: true
    spacing: Theme.spacing

    Repeater {
        model: root.lines
        Label {
            required property string modelData
            required property int index
            objectName: "consentChanged_" + index
            textFormat: Text.PlainText
            text: modelData
            color: Theme.text
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }
    }
}
