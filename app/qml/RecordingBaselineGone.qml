// File/layer: app/qml/RecordingBaselineGone.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The note that a vehicle's baseline recording is gone, with Clear it, a part of RecordingsPage

RowLayout {
    id: root
    required property RecordingsPage host
    objectName: "baselineGoneRow"
    visible: root.host.baselineGone && !root.host.chartMaximized
    Layout.fillWidth: true
    spacing: Theme.spacing
    Label {
        objectName: "baselineGone"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.warning
        text: qsTr("%1's baseline recording is gone: its folder was moved or deleted.")
                  .arg(root.host.pickedVehicle !== null ? root.host.pickedVehicle.label : "")
    }
    ThemedButton {
        objectName: "clearBaseline"
        compact: true
        text: qsTr("Clear it")
        Accessible.name: qsTr("Clear %1's baseline").arg(root.host.pickedVehicle !== null
                                                         ? root.host.pickedVehicle.label : "")
        onClicked: root.host.openPart.clearBaseline()
    }
}
