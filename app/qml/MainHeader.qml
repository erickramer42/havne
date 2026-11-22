// File/layer: app/qml/MainHeader.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.phone_*, QmlTest.linkhealth, QmlTest.driveview{,_sheet,_platform} (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The window's header bar: the name, the status pill, the frame rate, and
// Mark, Start log, the armed recorder and Connect. Main makes it once and
// reads headerBar.row's width for the log readout.
ToolBar {
    id: headerBar
    objectName: "headerBar"
    required property Main host
    property alias row: headerRow
    width: parent.width
    height: Theme.toolBarHeight + headerBar.host.insetTop
    visible: !headerBar.host.driving
    // The row below pads the inset itself; the style would pad it too
    topPadding: 0
    background: Rectangle {
        color: Theme.surface
        // The bar's rule: a hairline, as every other divider
        Rectangle {
            objectName: "headerRule"
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1; color: Theme.hairline
        }
    }
    RowLayout {
        id: headerRow
        objectName: "headerRow"
        readonly property real touchMin: headerBar.host.isCompact ? Theme.touchTarget : 0
        anchors.fill: parent
        anchors.topMargin: headerBar.host.insetTop
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 12

        // The name gives way to Mark or the log's controls on a
        // phone's width
        Label {
            textFormat: Text.PlainText
            id: headerTitle
            objectName: "headerTitle"
            visible: !(markButton.visible || startLogButton.visible
                       || armedHost.visible || Bridge.logActive)
                     || headerRow.width >= 420
            text: qsTr("Havne")
            font.pixelSize: Theme.fontEmphasis
            font.weight: Font.DemiBold
            color: Theme.text
        }

        // The link state and address: the address elides first, the
        // word only at a narrow phone's top zoom. With the tab bar a
        // tap opens the Log it omits
        AbstractButton {
            id: pill
            objectName: "linkPill"
            readonly property color tone: (Bridge.connecting || Bridge.disconnecting)
                                        ? Theme.textDim
                                        : Theme.linkColor(Bridge.linkState)
            readonly property string stateWord:
                Bridge.connecting ? qsTr("Connecting…")
              : Bridge.disconnecting ? qsTr("Disconnecting…")
              : headerBar.host.isCompact || !headerTitle.visible
                ? headerBar.host.shortStatusWord(Bridge.linkState)
                : Theme.linkLabel(Bridge.linkState)
            readonly property string address: Bridge.connected ? SessionLabel.address(Bridge.uriLabel) : ""
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: implicitWidth
            Layout.maximumWidth: implicitWidth
            Layout.minimumWidth: 32
            implicitWidth: implicitContentWidth + leftPadding + rightPadding
            implicitHeight: implicitContentHeight
            leftPadding: headerBar.host.isCompact ? 0 : 12
            rightPadding: headerBar.host.isCompact ? 0 : 12
            topPadding: 0
            bottomPadding: 0
            enabled: headerBar.host.isCompact
            text: address !== "" ? qsTr("%1 · %2").arg(stateWord).arg(address) : stateWord
            // What the pill shows, the recording note included
            readonly property string spoken: recordingNote.visible
                                             ? text + " " + recordingNote.text : text
            Accessible.name: enabled ? qsTr("%1: show the activity").arg(spoken) : spoken
            onClicked: headerBar.host.gotoPage(headerBar.host.logPage)
            // A dot and a word at rest, no capsule
            background: Rectangle {
                color: pill.down ? Theme.elevated : "transparent"
                radius: Theme.radius
                border.width: pill.visualFocus ? 2 : 0
                border.color: Theme.text
            }
            contentItem: RowLayout {
                spacing: 8

                Rectangle {
                    implicitWidth: 8; implicitHeight: 8   // layout-managed
                    radius: 4
                    color: pill.tone
                }
                // The address gives way first; the word only alone in a
                // row under 240 units (a narrow phone at the top zoom)
                Label {
                    textFormat: Text.PlainText
                    objectName: "linkPillText"
                    Layout.fillWidth: true
                    Layout.minimumWidth: pill.address !== "" || headerRow.width >= 240
                                         ? implicitWidth : 0
                    Layout.maximumWidth: implicitWidth
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontCaption
                    color: Theme.text
                    text: pill.stateWord
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "linkPillAddress"
                    visible: pill.address !== ""
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: qsTr("· %1").arg(pill.address)
                }
                // Record sessions on: the session is being recorded
                // already, which is why Mark stands where Start log
                // would; the address gives way to the word
                Label {
                    textFormat: Text.PlainText
                    id: recordingNote
                    objectName: "linkPillRecording"
                    visible: Bridge.connected && Bridge.recordingActive && !Bridge.logActive
                             && !Bridge.armedActive
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: qsTr("· recording")
                }
                // Marks the status as a button where it opens the Log
                Label {
                    textFormat: Text.PlainText
                    objectName: "linkPillChevron"
                    // Not where it would squeeze a connected phone's address
                    visible: pill.enabled && (pill.address === "" || headerRow.width >= 420)
                    Accessible.ignored: true
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: "›"   // a right angle quotation mark
                }
            }
        }

        // Developer mode: the window's frames drawn a second, in
        // the bar so it covers no page (headerBar.host.framesPerSecond); not
        // in a row too narrow to hold it beside the pill's word
        Label {
            textFormat: Text.PlainText
            id: frameRate
            objectName: "frameRate"
            visible: Theme.developerMode && headerRow.width >= 340
            font.pixelSize: Theme.fontCaption
            font.features: Theme.valueFeatures
            color: Theme.textDim
            text: qsTr("%1 frames/s").arg(headerBar.host.framesPerSecond)
        }

        Item { Layout.fillWidth: true }  // Spacer

        // A marker in the recording under way (a log is one: beside
        // Stop log from 720 px of header), from any page
        // (ADR-0049 section 4): "Mark N" at the tap, which the
        // Recordings page aligns two runs on
        HeaderButton {
            id: markButton
            objectName: "markButton"
            visible: Bridge.recordingActive && (!Bridge.logActive || headerBar.host.headerHasRoom)
                     && (!Bridge.armedActive || Bridge.armedRecording)
            text: Bridge.markCount > 0 ? qsTr("Mark (%1)").arg(Bridge.markCount)
                                       : qsTr("Mark")
            Accessible.name: qsTr("Mark the recording")
            onClicked: Bridge.mark()
        }

        // The analysis log (ADR-0056 section 1): Live's ticked rows,
        // recorded from now on and asked alone until Stop log. A
        // replay plays a recording already; Record sessions' own
        // recorder is the session's one
        HeaderButton {
            id: startLogButton
            objectName: "startLogButton"
            highlighted: true
            visible: Bridge.connected && !Bridge.recordingActive
                     && !Bridge.currentUri.toLowerCase().startsWith("replay://")
            enabled: !Bridge.disconnecting && Bridge.signalModel.tickedCount > 0
            text: qsTr("Start log")
            Accessible.name: qsTr("Start a log of the ticked signals")
            onClicked: Bridge.startLog()
        }
        // The armed recorder (ADR-0063 section 7): writes nothing until
        // a condition that records fires, then a window of its own per
        // firing. Mark stands beside it only inside a window. Stands
        // where Start log would, the session's one recorder; disabled
        // with its reason (armedWhy) while no condition records. A
        // disabled control sees no pointer, so an enabled item around
        // it gives the reason: a tooltip on hover, and a tap says it
        // in the Dashboard's notice (what a refused press says)
        Item {
            id: armedHost
            objectName: "armedHost"
            visible: Bridge.connected && !Bridge.logActive
                     && (Bridge.armedActive || !Bridge.recordingActive)
                     && !Bridge.currentUri.toLowerCase().startsWith("replay://")
            implicitWidth: armedButton.implicitWidth
            implicitHeight: armedButton.implicitHeight
            Layout.minimumWidth: Theme.compact ? Theme.touchTarget : 0
            readonly property bool explaining: !armedButton.enabled && Bridge.armedWhy !== ""
            ToolTip {
                id: armedTip
                visible: armedHover.hovered && armedHost.explaining
                text: Bridge.armedWhy
                // Basic's own content, drawn as plain text
                contentItem: Text {
                    text: armedTip.text
                    textFormat: Text.PlainText
                    font: armedTip.font
                    color: armedTip.palette.toolTipText
                    wrapMode: Text.Wrap
                }
            }
            HoverHandler { id: armedHover }
            TapHandler {
                objectName: "armedExplain"
                enabled: armedHost.explaining
                onTapped: Bridge.startArmed()
            }
        HeaderButton {
            id: armedButton
            objectName: "armedButton"
            anchors.fill: parent
            enabled: !Bridge.disconnecting
                     && (Bridge.armedActive || Bridge.armedWhy === "")
            checkable: true
            checked: Bridge.armedActive
            // "Recording (window N)" leaves Mark and Disconnect no room
            // under 420 px of header row, and "Armed" beside Start log leaves
            // the status its address: the words shorten ("Window N", "Arm"
            // until it is on), the name doesn't
            text: Bridge.armedRecording
                  ? (headerRow.width < 420 ? qsTr("Window %1")
                                           : qsTr("Recording (window %1)")).arg(Bridge.armedWindows)
                  : Bridge.armedActive ? qsTr("Armed") : qsTr("Arm")
            Accessible.name: Bridge.armedRecording
                             ? qsTr("Stop the armed recorder, recording window %1").arg(Bridge.armedWindows)
                             : Bridge.armedActive ? qsTr("Stop the armed recorder")
                                                  : qsTr("Arm a recorder for the conditions that record")
            Accessible.description: Bridge.armedActive ? "" : Bridge.armedWhy
            onToggled: toggle()
            onClicked: Bridge.armedActive ? Bridge.stopArmed() : Bridge.startArmed()
        }
        }
        // While a log runs: its time, and its folder's size once
        // there is one to read; one line where the row has room,
        // else the size under the time, so a ten-hour log's
        // readout fits beside the name at 420 px
        Label {
            textFormat: Text.PlainText
            objectName: "logReadout"
            visible: Bridge.logActive
            text: headerBar.host.headerHasRoom
                  ? qsTr("Logging %1").arg(headerBar.host.logReadoutText(
                        Bridge.logElapsedMs, Bridge.logBytes, true))
                  : headerBar.host.logReadoutText(Bridge.logElapsedMs, Bridge.logBytes,
                                        headerBar.host.logReadoutOneLine)
            horizontalAlignment: Text.AlignRight
            font.pixelSize: Theme.fontBody
            color: Theme.text
        }
        HeaderButton {
            objectName: "stopLogButton"
            highlighted: true
            visible: Bridge.logActive
            enabled: !Bridge.disconnecting
            text: qsTr("Stop log")
            onClicked: Bridge.stopLog()
        }

        // Settings owns connecting: this button opens it, or ends the
        // session from any page. On Settings itself, its Connect
        // button is the one to use.
        // A log's Stop log takes its place while the log runs
        HeaderButton {
            objectName: "disconnectButton"
            // Ending a session is quiet; Connect... is primary, unless
            // Dash or Home shows its connect list, whose Connect is the primary
            flat: Bridge.connected || Bridge.disconnecting
            highlighted: !flat && !(headerBar.host.currentPage === headerBar.host.dashboardPage
                                    && headerBar.host.dashboardItem.connectShown)
                         && !(headerBar.host.currentPage === headerBar.host.homePage
                              && !Bridge.connected)
            visible: !Bridge.logActive
                     && (Bridge.connected || headerBar.host.currentPage !== headerBar.host.settingsPage)
            // A teardown can take seconds with a wedged adapter; no
            // connect starts until it ends
            enabled: !Bridge.connecting && !Bridge.disconnecting
            text: Bridge.connected ? qsTr("Disconnect")
                : Bridge.disconnecting ? qsTr("Disconnecting…")
                : qsTr("Connect…")
            onClicked: Bridge.connected ? Bridge.disconnectSession()
                                        : headerBar.host.openConnect()
        }
    }
}
