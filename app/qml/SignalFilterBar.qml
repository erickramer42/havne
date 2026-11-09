// File/layer: app/qml/SignalFilterBar.qml | Qt6 Quick, UI layer
// Testing: BRIDGE: BridgeTest (test_bridge); QML: QmlTest.liveview,
//          QmlTest.recordings, QmlTest.dashedit, QmlTest.phone (test_qml)

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Finding a signal among many (ADR-0050): the one filter Live's list and
// the Recordings page's signal rows share, one row high so it fits a
// page's title row. A text field, and a Filter button whose popup holds
// Plotted only, a box for each group in codes (in code order) and Clear.
// It edits filter; the list it sits above applies filter's rule.
RowLayout {
    id: bar

    required property SignalFilter filter
    // The groups' codes among the list's rows
    property var codes: []
    // False leaves the text field alone, with no Filter button and so no
    // popup (a list the groups and Plotted only don't apply to)
    property bool filterButtonShown: true

    readonly property var choices: bar.filter.groupChoices(bar.codes)
    // The popup's content, which lives in the window's overlay
    readonly property alias options: options

    spacing: Theme.spacing

    ThemedTextField {
        objectName: "filterText"
        Layout.fillWidth: true
        Layout.minimumWidth: 80
        // The short word on a phone, where the field is squeezed between the
        // title and Filter; Accessible.name keeps the whole phrase
        placeholderText: Theme.compact ? qsTr("Find") : qsTr("Find a signal")
        text: bar.filter.text
        onTextEdited: bar.filter.text = text
        Accessible.name: qsTr("Find a signal by its name or unit")
    }

    ThemedButton {
        id: filterButton
        objectName: "filterButton"
        visible: bar.filterButtonShown
        text: qsTr("Filter")
        // Lit while the popup's choices narrow the list
        checkable: true
        checked: bar.filter.plottedOnly || bar.filter.groups.length > 0
        onClicked: options.opened ? options.close() : options.open()
        Accessible.name: qsTr("Filter the signals")

        Popup {
            id: options
            objectName: "filterOptions"
            y: filterButton.height + 4
            // Right-aligned under the button, and kept inside the window:
            // without margins a Popup may run past its edge (a phone)
            x: Math.min(0, filterButton.width - width)
            margins: Theme.spacing
            padding: 12

            ColumnLayout {
                spacing: 4

                Switch {
                    objectName: "plottedOnly"
                    text: qsTr("Plotted only")
                    checked: bar.filter.plottedOnly
                    onToggled: bar.filter.plottedOnly = checked
                }
                Repeater {
                    model: bar.choices.length > 1 ? bar.choices : []
                    delegate: ThemedCheckBox {
                        required property var modelData
                        objectName: "group_" + modelData.code
                        text: modelData.name
                        // filter.groups is the dependency, so a clear unticks it
                        checked: bar.filter.groups.indexOf(modelData.code) >= 0
                        onToggled: bar.filter.toggleGroup(modelData.code)
                    }
                }
                ThemedButton {
                    objectName: "filterClear"
                    text: qsTr("Clear")
                    enabled: bar.filter.active
                    onClicked: bar.filter.clear()
                }
            }
        }
    }
}
