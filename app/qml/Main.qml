// File/layer: app/qml/Main.qml | Qt6 Quick, UI layer
// Testing: manual smoke — navigation, sidebar↔TabBar at 720px, header
//          pill elides at 400 px, keyboard focus rings

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

    // Every role the Basic style reads, so stock controls (CheckBox,
    // TextField, ScrollBar) draw from Theme whatever the OS colour
    // scheme is
    palette.window: Theme.bg
    palette.windowText: Theme.text
    palette.base: Theme.elevated
    palette.alternateBase: Theme.surface
    palette.text: Theme.text
    palette.button: Theme.elevated
    palette.buttonText: Theme.text
    palette.brightText: Theme.text
    palette.highlight: Theme.primary
    palette.highlightedText: Theme.text
    palette.placeholderText: Theme.textDim
    palette.toolTipBase: Theme.elevatedLight
    palette.toolTipText: Theme.text
    palette.light: Theme.elevatedLight
    palette.midlight: Theme.elevatedLight
    palette.mid: Theme.outline
    palette.dark: Theme.textDim
    palette.shadow: "#000000"
    palette.link: Theme.info
    palette.linkVisited: Theme.info
    palette.disabled.windowText: Theme.textDim
    palette.disabled.text: Theme.textDim
    palette.disabled.buttonText: Theme.textDim

    readonly property bool isCompact: width < 720
    readonly property int currentPage: stack.currentIndex

    // Page indices, in the order of the sidebar, stack and tab bar
    readonly property int settingsPage: 4

    function gotoPage(index) { stack.currentIndex = index }

    header: ToolBar {
        height: 48
        background: Rectangle {
            color: Theme.surface
            Rectangle { // 2px accent underline
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 2; color: Theme.primary
            }
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 12

            Label {
                text: qsTr("QtTune")
                font.pixelSize: 16
                font.weight: Font.DemiBold
                color: Theme.text
            }

            // Connection pill: link state and URI, from Theme like every
            // other surface. It shrinks and elides before anything else
            // moves, so the header fits a phone.
            Rectangle {
                id: pill
                readonly property color tone: !QtTune.coreInitialized ? Theme.danger
                                            : QtTune.connecting ? Theme.textDim
                                            : Theme.linkColor(QtTune.linkState)
                Layout.fillWidth: true
                Layout.preferredWidth: pillRow.implicitWidth + 24
                Layout.maximumWidth: pillRow.implicitWidth + 24
                Layout.minimumWidth: 32
                implicitHeight: 28
                radius: height / 2
                color: Qt.alpha(pill.tone, 0.14)

                RowLayout {
                    id: pillRow
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8

                    Rectangle {
                        implicitWidth: 8; implicitHeight: 8   // layout-managed
                        radius: 4
                        color: pill.tone
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: pill.tone
                        text: !QtTune.coreInitialized ? qsTr("Core init failed")
                            : QtTune.connecting ? qsTr("Connecting…")
                            : QtTune.connected
                              ? qsTr("%1 · %2").arg(Theme.linkLabel(QtTune.linkState))
                                                .arg(QtTune.currentUri)
                              : Theme.linkLabel(QtTune.linkState)
                    }
                }
            }

            Item { Layout.fillWidth: true }  // Spacer

            // Settings owns connecting: this button opens it, or ends the
            // session from any page. On Settings itself, its Connect
            // button is the one to use.
            ThemedButton {
                visible: QtTune.connected || root.currentPage !== root.settingsPage
                enabled: QtTune.coreInitialized && !QtTune.connecting
                text: QtTune.connected ? qsTr("Disconnect") : qsTr("Connect…")
                onClicked: QtTune.connected ? QtTune.disconnectSession()
                                            : root.gotoPage(root.settingsPage)
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Sidebar (≥720px): one full-height panel ----
        Rectangle {
            visible: !root.isCompact
            Layout.fillHeight: true
            Layout.preferredWidth: 220
            color: Theme.surface

            Rectangle {   // hairline against the page
                anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
                width: 1
                color: Theme.elevated
            }

            ColumnLayout {
                anchors { left: parent.left; right: parent.right; top: parent.top }
                anchors.topMargin: 8
                anchors.rightMargin: 1
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
            }
        }

        StackLayout {
            id: stack
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            HomePage { onConnectRequested: root.gotoPage(root.settingsPage) }
            DashboardPage {}
            LiveViewPage { onConnectRequested: root.gotoPage(root.settingsPage) }
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
                font.weight: btn.checked ? Font.DemiBold : Font.Normal
                color: btn.checked ? Theme.text : Theme.textDim
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            // The selected tab is underlined, so selection doesn't rest
            // on colour alone
            background: Rectangle {
                color: Theme.surface
                border.width: btn.visualFocus ? 2 : 0
                border.color: Theme.text
                Rectangle {
                    visible: btn.checked
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 6
                    width: Math.min(parent.width - 16, btn.contentItem.implicitWidth + 8)
                    height: 3
                    radius: 1.5
                    color: Theme.primary
                }
            }
        }

        ThemedTabButton { text: qsTr("Home") }
        ThemedTabButton { text: qsTr("Dash") }
        ThemedTabButton { text: qsTr("Live") }
        ThemedTabButton { text: qsTr("Log") }
        ThemedTabButton { text: qsTr("Settings") }
    }
}
