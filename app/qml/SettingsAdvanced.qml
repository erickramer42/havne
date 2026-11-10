// File/layer: app/qml/SettingsAdvanced.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// Developer mode and a log's minimum time between sweeps: one part of
// SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // Developer mode (Theme.developerMode); Main.qml stores it
    ThemedCheckBox {
        id: developerBox
        objectName: "developerBox"
        text: qsTr("Developer mode: the mock:// preset, and the raw frames on the Log page")
        checked: Theme.developerMode
        Layout.fillWidth: true
        onToggled: {
            Theme.developerMode = checked
            checked = Qt.binding(() => Theme.developerMode)
        }
        contentItem: Label {
            text: developerBox.text
            color: Theme.text
            leftPadding: developerBox.indicator.width + developerBox.spacing
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
        }
    }

    // A log's minimum time between sweeps (ADR-0056 section 4), read at
    // each Start log; Main.qml stores it
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            text: qsTr("Minimum time between sweeps")
            color: Theme.text
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        ComboBox {
            id: minSweepChoice
            objectName: "minSweepChoice"
            // A touch's height, as Settings' buttons take (SettingsSubpage's touchMin)
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, Theme.touchTarget)
            Accessible.name: qsTr("Minimum time between a log's sweeps")
            model: Bridge.minSweepChoices.map(ms => ms === 0 ? qsTr("Off") : qsTr("%1 ms").arg(ms))
            currentIndex: Bridge.minSweepChoices.indexOf(Bridge.minSweepMs)
            onActivated: index => {
                Bridge.minSweepMs = Bridge.minSweepChoices[index]
                currentIndex = Qt.binding(() => Bridge.minSweepChoices.indexOf(Bridge.minSweepMs))
            }
        }
    }
    Label {
        objectName: "minSweepNote"
        text: qsTr("For a slow adapter or a busy ECU: a log asks its signals no more often than this. It isn't for file size, which the export sets.")
        color: Theme.textDim
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
}
