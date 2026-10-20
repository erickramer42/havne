// File/layer: app/qml/Main.qml | Qt6 Quick, UI layer
// Testing: manual smoke — navigation, sidebar↔TabBar at 720px, header bindings

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ApplicationWindow {
    id: root
    width: 1280; height: 800
    visible: true
    title: qsTr("QtTune")
    color: Theme.bg

    palette.text: Theme.text
    palette.windowText: Theme.text
    palette.buttonText: Theme.text

    readonly property bool isCompact: width < 720
    readonly property int currentPage: stack.currentIndex

    function gotoPage(index) { stack.currentIndex = index }

    readonly property bool bridgeConnected: QtTune.connected
    readonly property bool isConnecting: false  // unused; QtTune.connecting has the real state

    header: ToolBar {
        height: 48
        background: Rectangle {
            color: Theme.surface
            Rectangle { // 1px accent underline
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 2; color: Theme.primary
            }
        }
        RowLayout {
            anchors.fill: parent
            spacing: 12
            
            Label {
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                text: qsTr("QtTune — core %1 [%2]")
                    .arg(QtTune.coreVersion)
                    .arg(QtTune.coreInitialized ? qsTr("ready") : qsTr("init failed"))
                color: QtTune.coreInitialized ? Theme.text : Theme.danger
            }
            
            Item { Layout.fillWidth: true }  // Spacer
            
            Button {
                id: connBtn
                text: QtTune.connected ? qsTr("Disconnect") : qsTr("Connect Mock")
                enabled: QtTune.coreInitialized
                background: Rectangle {
                    radius: Theme.radius
                    color: QtTune.connected ? Theme.elevated : Theme.primary
                }
                onClicked: QtTune.connected ? QtTune.disconnectSession()
                                             : QtTune.connectSession("mock://demo")
            }
            Label {
                text: qsTr("frames:%1 notify:%2 batch:%3 drop:%4")
                    .arg(QtTune.frameCount)
                    .arg(QtTune.guiNotifyCount)
                    .arg(QtTune.signalBatchCount)
                    .arg(QtTune.droppedFrames)
                color: Theme.textDim
                visible: QtTune.frameCount > 0
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Sidebar (≥720px) ----
        ColumnLayout {
            visible: !root.isCompact
            Layout.preferredWidth: 220
            Layout.maximumWidth: 220
            Layout.minimumWidth: 200
            spacing: 2

            Repeater {
                model: [
                    { label: qsTr("Home"), icon: "" },
                    { label: qsTr("Dashboard"), icon: "" },
                    { label: qsTr("Live"),   icon: "" },
                    { label: qsTr("Log"),       icon: "" },
                    { label: qsTr("Settings"),  icon: "" }
                ]
                delegate: NavButton {
                    id: navBtn
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    label: modelData.label
                    active: stack.currentIndex === index
                    onClicked: stack.currentIndex = index
                }
            }
            Item { Layout.fillHeight: true }
        }

        StackLayout {
            id: stack
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            HomePage {}
            DashboardPage {}
            LiveViewPage {} 
            LogPage {}
            SettingsPage {}
        }
    }

    footer: TabBar {
        visible: root.isCompact
        width: parent.width
        currentIndex: stack.currentIndex
        onCurrentIndexChanged: stack.currentIndex = currentIndex

        background: Rectangle { color: Theme.surface }

        // per-button styling since TabBar doesn't propagate backgrounds
        component ThemedTabButton: TabButton {
            id: btn
            contentItem: Label {
                text: btn.text
                // 13px keeps "Settings" inside a fifth of a 375pt phone
                font.pixelSize: 13
                color: btn.checked ? Theme.primary : Theme.textDim
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: Theme.surface }   // selected highlight below
        }

        ThemedTabButton { text: qsTr("Home") }
        ThemedTabButton { text: qsTr("Dash") }
        ThemedTabButton { text: qsTr("Live") }
        ThemedTabButton { text: qsTr("Log") }
        ThemedTabButton { text: qsTr("Settings") }
    }
}
