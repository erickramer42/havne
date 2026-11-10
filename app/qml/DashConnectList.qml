// File/layer: app/qml/DashConnectList.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

pragma ComponentBehavior: Bound

// Dash with no session, on the phone: the addresses that connected, one
// chosen, and the one Connect. Connecting stays Settings' (the page signals)
Rectangle {
    id: list
    objectName: "dashConnectList"

    property var uris: []
    property int selected: 0
    property string errorText: ""

    signal picked(int index)
    signal connectTo(string uri)
    signal otherAddress()

    // Every row and button a touch's size (ThemedButton reads this)
    readonly property real touchMin: Theme.touchTarget
    readonly property bool busy: Bridge.connecting || Bridge.disconnecting
    readonly property string chosen: uris.length > 0
        ? uris[Math.min(selected, uris.length - 1)] : ""

    // What a row's address is, in words a driver knows
    function kindOf(uri) {
        return uri.startsWith("mock://") ? qsTr("Demo data")
             : uri.startsWith("replay://") ? qsTr("Recording")
             : uri.startsWith("j2534://") ? qsTr("J2534 adapter")
             : uri.startsWith("elm327://") ? qsTr("ELM327 adapter")
             : uri.startsWith("socketcan://") ? qsTr("CAN interface")
             : ""
    }

    implicitHeight: column.implicitHeight + 24
    color: Theme.surface
    radius: Theme.radiusCard
    border.width: 1
    border.color: Theme.hairline

    ColumnLayout {
        id: column
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
        spacing: 10

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: qsTr("No session")
                font.pixelSize: Theme.fontEmphasis
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Label {
                objectName: "connectHint"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
                text: list.uris.length > 0
                      ? qsTr("The gauges fill when a session runs. Pick a source, then connect.")
                      : qsTr("The gauges fill when a session runs. Choose what to connect to in Settings.")
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: list.uris.length > 0

            Repeater {
                model: list.uris
                delegate: ThemedButton {
                    id: row
                    required property string modelData
                    required property int index
                    readonly property bool isChosen: list.selected === index
                    objectName: "connectRow_" + index
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    enabled: !list.busy
                    checkable: true
                    checked: isChosen
                    leftPadding: 12
                    rightPadding: 12
                    Accessible.name: qsTr("%1, %2").arg(address.text).arg(list.kindOf(modelData))
                    onClicked: list.picked(index)
                    contentItem: RowLayout {
                        spacing: 10
                        // A mark, so the choice doesn't rest on the fill alone
                        Item {
                            implicitWidth: 18; implicitHeight: 18   // layout-managed
                            Shape {
                                objectName: "connectRowMark"
                                visible: row.isChosen
                                anchors.centerIn: parent
                                width: 24; height: 24
                                scale: 0.75
                                preferredRendererType: Shape.CurveRenderer
                                ShapePath {
                                    strokeColor: Theme.text
                                    strokeWidth: 2.5
                                    fillColor: "transparent"
                                    capStyle: ShapePath.SquareCap
                                    startX: 5; startY: 12
                                    PathLine { x: 10; y: 17 }
                                    PathLine { x: 19; y: 7 }
                                }
                            }
                        }
                        Label {
                            id: address
                            objectName: "connectRowAddress"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontEmphasis
                            color: Theme.text
                            text: Bridge.uriLabelFor(row.modelData)
                        }
                        Label {
                            objectName: "connectRowKind"
                            visible: text !== ""
                            font.pixelSize: Theme.fontCaption
                            color: Theme.textDim
                            text: list.kindOf(row.modelData)
                        }
                    }
                }
            }
        }

        // The bridge's words for a connect that failed; the Log has them too
        Label {
            objectName: "connectError"
            visible: list.errorText !== ""
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            font.pixelSize: Theme.fontCaption
            color: Theme.danger
            text: list.errorText
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            ThemedButton {
                objectName: "connectOther"
                flat: true
                visible: list.uris.length > 0
                enabled: !list.busy
                text: qsTr("Other address…")
                onClicked: list.otherAddress()
            }
            // Primary with a row to connect; with none it opens Settings,
            // a secondary step
            ThemedButton {
                objectName: "connectGo"
                highlighted: list.chosen !== ""
                // A relay holds the adapter (ADR-0047); Settings' page doesn't connect
                enabled: !list.busy && (list.chosen === "" || !Relay.running)
                text: Bridge.connecting ? qsTr("Connecting…")
                    : Bridge.disconnecting ? qsTr("Disconnecting…")
                    : list.chosen !== "" ? qsTr("Connect")
                    : qsTr("Connect…")
                onClicked: list.chosen !== "" ? list.connectTo(list.chosen)
                                              : list.otherAddress()
            }
        }
    }
}
