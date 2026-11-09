// File/layer: app/qml/SettingsVinLookup.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The VIN lookup: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    property alias vinLookupField: vinLookupField

    // ---- A VIN looked up (ADR-0043): decoded here, never kept ----
    LabeledFlow {
        label: qsTr("Look up a VIN:")

        ThemedTextField {
            id: vinLookupField
            objectName: "vinLookupField"
            width: Math.min(300, parent.width)
            maximumLength: 32
            placeholderText: qsTr("17 characters")
            inputMethodHints: Qt.ImhPreferUppercase | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
            // A typed VIN proposes plugins too (ADR-0052 section 5)
            onTextChanged: root.host.refreshMatches()
        }
    }

    Label {
        readonly property string summary: Bridge.describeVin(vinLookupField.text)
        objectName: "vinLookupResult"
        visible: summary !== ""
        text: summary.charAt(0).toUpperCase() + summary.slice(1)
        color: Theme.text
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Label {
        objectName: "vinLookupHint"
        text: qsTr("By 49 CFR 565 and NHTSA's vPIC list (%1), on this device; the VIN isn't saved. The connected car's goes in the activity log.")
                  .arg(Bridge.vinSource())
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
}
