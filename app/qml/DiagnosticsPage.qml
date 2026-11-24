// File/layer: app/qml/DiagnosticsPage.qml | Qt6 Quick, UI layer
// Testing: BRIDGE: BridgeTest (test_bridge); QML: QmlTest.diagnostics,
//          QmlTest.phone_* (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// What each ECU reports of its emissions diagnostics (ADR-0037): the
// check-engine light, the stored, pending and permanent trouble codes,
// the readiness monitors and the freeze frame. Bridge.diagnostics holds
// one entry per ECU, its codes already named by core; the page formats
// nothing but the values' decimals, as Live does. Clearing the codes
// (ADR-0038) asks first, in place: the confirmation says what a clear
// erases, and the bridge saves the report before anything is sent. Save
// report writes the car's health as one scrubbed file to share (ADR-0054's
// appendix "the health report as built"), text or JSON.
Page {
    id: root
    background: Rectangle { color: Theme.bg }

    // Settings owns connecting; Main opens it
    signal connectRequested()

    readonly property var ecus: Bridge.diagnostics
    // The vehicles, Settings' VehicleBook document (Main binds it): the
    // report names the picked vehicle's baseline recording by its start,
    // never its folder (ADR-0049's appendix "a vehicle's baseline")
    property var vehicles: VehicleBook.empty()
    readonly property var pickedVehicle: VehicleBook.picked(vehicles)
    readonly property string baselineFolder: pickedVehicle !== null ? pickedVehicle.baseline : ""
    // Read again needs a session and a read that has settled
    readonly property bool canReadAgain: Bridge.connected &&
        ecus.some(e => e.asked && e.complete)

    // The clear (ADR-0038): its confirmation open; the file the last one
    // saved the report to; a clear that sent nothing because the report
    // couldn't be saved
    property bool confirming: false
    // The confirmation opens at the top of the scrolling area
    onConfirmingChanged: if (confirming) flick.contentY = 0
    property string savedTo: ""
    property bool clearFailed: false
    // The last Save report's outcome in words, "" before one
    property string reportNote: ""
    property bool reportFailed: false
    readonly property bool clearing: ecus.some(e => e.clearing)
    // A clear needs a session, an ECU the bridge would clear, none under
    // way, and no moving vehicle's question open
    readonly property bool canClear: Bridge.connected &&
        Bridge.clearTargets.length > 0 && !clearing &&
        Bridge.clearMoving.length === 0

    Connections {
        target: Bridge
        // The moving vehicle's question opens at the top of the scrolling
        // area, as the first does
        function onClearMovingChanged() {
            if (Bridge.clearMoving.length > 0)
                flick.contentY = 0
        }
        function onConnectedChanged() {
            root.confirming = false
            if (Bridge.connected) {
                root.savedTo = ""
                root.clearFailed = false
                root.reportNote = ""
                root.reportFailed = false
            }
        }
    }

    // The modules a plugin talks to (ADR-0066): the bridge's entries copied
    // once into plain JS, which each card reads (a test sets its own list)
    property var conversations: JSON.parse(JSON.stringify(Bridge.consent.conversations))
    // The unlock's question open on this channel (-1 for none), its word
    // sent and not yet answered, and what each channel's last answer said
    property int unlocking: -1
    property bool unlockAsking: false
    property string unlockNote: ""
    property var unlockWords: ({})
    property real nowMs: Theme.now().getTime()
    readonly property var unlockEntry: conversations.find(c => c.channel === unlocking) ?? null
    function conversationOf(e) {
        const rx = parseInt(e.ecu, 16)
        return conversations.find(c => c.rx === rx) ?? null
    }
    // The question's Unlock... shows for a module the plugin talks to that
    // isn't unlocked and has no unlock under way
    function canUnlock(c) {
        return c !== null && c.state === "talking" && (c.levels ?? []).length > 0
               && ["none", "failed", "delayed", "busy", "refused"].indexOf(c.unlock) >= 0
    }
    function delayLeftS(c) {
        return Math.max(0, Math.ceil((c.delayEndsMs - nowMs) / 1000))
    }
    // The row's words on its unlock, "" while there is nothing to say
    function unlockText(c) {
        if (c === null)
            return ""
        const nrc = "0x" + c.nrc.toString(16).toUpperCase().padStart(2, "0")
        switch (c.unlock) {
        case "waiting":
        case "seed":
        case "key":
            return qsTr("Unlocking...")
        case "unlocked":
        case "alreadyUnlocked":
            return qsTr("Unlocked, level 0x%1").arg(c.level.toString(16).toUpperCase().padStart(2, "0"))
        case "failed":
            return delayLeftS(c) > 0
                   ? qsTr("The unlock failed (module code %1). Try again in %2 s").arg(nrc).arg(delayLeftS(c))
                   : qsTr("The unlock failed (module code %1)").arg(nrc)
        case "delayed":
            return qsTr("The module asks for a wait: %1 s").arg(delayLeftS(c))
        }
        return unlockWords[String(c.channel)] ?? ""
    }
    function unlockOpen(c) {
        unlocking = c.channel
        unlockNote = ""
        unlockAsking = false
        flick.contentY = 0
    }
    function unlockClose() {
        unlocking = -1
        unlockAsking = false
    }
    // The user's word: the bridge refuses at once or asks core, and the
    // answer comes by unlockAnswered
    function unlockSend(level) {
        const r = Bridge.consent.unlock(unlocking, level)
        unlockNote = r.ok ? "" : r.words
        unlockAsking = r.ok
    }
    // The question closes when its module stops talking or the session ends
    readonly property bool unlockShown: unlocking >= 0 && unlockEntry !== null && unlockEntry.state === "talking"
    // A handler runs before the bindings on its property: it reads the new list itself
    onConversationsChanged: {
        const e = conversations.find(c => c.channel === unlocking)
        if (unlocking >= 0 && (e === undefined || e.state !== "talking"))
            unlockClose()
    }
    Connections {
        target: Bridge.consent
        function onUnlockAnswered(channel, ok, words) {
            const kept = Object.assign({}, root.unlockWords)
            kept[String(channel)] = words
            root.unlockWords = kept
            if (channel === root.unlocking)
                root.unlockClose()
        }
    }
    // A running delay counts down on the rows that show it
    Timer {
        interval: 1000
        repeat: true
        running: root.conversations.some(c => c.delayEndsMs > root.nowMs)
        onTriggered: root.nowMs = Theme.now().getTime()
    }

    function clearCodes() {
        confirming = false
        savedTo = Bridge.clearDiagnostics()
        clearFailed = savedTo === ""
    }

    // The health report to url, its format by the dialog's filter or the
    // file's name, naming the picked vehicle's baseline; true when written
    function saveReportTo(url, filterIndex) {
        const r = Bridge.saveHealthReport(url, filterIndex, baselineFolder)
        reportFailed = !r.ok
        reportNote = r.ok ? qsTr("Report saved: %1").arg(r.summary)
                          : qsTr("Report not saved: %1").arg(r.words)
        return r.ok
    }

    function statusText() {
        if (Bridge.connecting) return qsTr("Connecting…")
        if (Bridge.disconnecting) return qsTr("Disconnecting…")
        if (!Bridge.connected)
            return ecus.length > 0 ? qsTr("Disconnected: the last read is shown")
                                   : qsTr("Connect a session to read trouble codes")
        if (ecus.length === 0)
            return qsTr("This session asks the vehicle nothing, so it reads no codes")
        if (clearing)
            return qsTr("Clearing: reading the engine and vehicle speeds, then clearing")
        if (clearFailed)
            return qsTr("Nothing was cleared: the report couldn't be saved first (the Activity page says why)")
        if (savedTo !== "")
            return qsTr("The report was saved before the clear: %1").arg(savedTo)
        return qsTr("Read at connect · Clear codes… asks first and saves the report")
    }

    function ecuState(e) {
        if (!e.asked) return qsTr("Not read: diagnose=0, poll= or a profile that asks nothing")
        if (e.clearing) return qsTr("Clearing…")
        if (!e.complete) return qsTr("Reading…")
        return qsTr("Read")
    }

    // What became of the ECU's last clear; "" when it had none
    function clearText(e) {
        switch (e.clear) {
        case "cleared":
            return qsTr("Cleared. What follows is the ECU's read after the clear")
        case "refused":
            // J1979: an ECU that can't clear with the engine running
            // answers conditionsNotCorrect
            if (e.clearRunning && e.clearNrc === "7F 04 22")
                return qsTr("This ECU won't clear with the engine running (%1): stop and turn it off, then try again. Nothing was erased").arg(e.clearNrc)
            return qsTr("The ECU refused the clear (%1): nothing was erased").arg(e.clearNrc)
        case "unanswered":
            return qsTr("No answer to the clear. What follows is the ECU's read after it")
        case "engineRunning":
            return qsTr("Not sent: the engine is running (%1 rpm). Stop it, leave the ignition on, and clear again").arg(e.clearRpm)
        case "noEngineSpeed":
            return e.clearNrc !== ""
                ? qsTr("Not sent: the ECU didn't give its engine speed (%1)").arg(e.clearNrc)
                : qsTr("Not sent: the ECU didn't give its engine speed")
        case "reportChanged":
            return qsTr("Not sent: the report changed after it was saved. Clear again to save this one")
        case "moving":
            return qsTr("Not sent: the vehicle was moving (%1)").arg(e.clearSpeed)
        case "noVehicleSpeed":
            return e.clearNrc !== ""
                ? qsTr("Not sent: the ECU didn't give the vehicle speed (%1)").arg(e.clearNrc)
                : qsTr("Not sent: the ECU didn't give the vehicle speed")
        case "notEmissions":
            return qsTr("Not sent: this ECU gave no emissions answer (01 00 or 03), so it isn't cleared with them")
        case "notOpened":
            return qsTr("Not sent: no room to open this ECU for the clear: live data holds every filter the adapter has")
        default:
            return ""
        }
    }

    function milText(e) {
        switch (e.readiness) {
        case "answered":
            if (!e.mil) return qsTr("Check-engine light off")
            return e.dtcCount === 1 ? qsTr("Check-engine light on · 1 confirmed code")
                                    : qsTr("Check-engine light on · %1 confirmed codes").arg(e.dtcCount)
        case "refused":    return qsTr("Readiness refused (%1)").arg(e.readinessNrc)
        case "unanswered": return qsTr("Readiness: no answer")
        default:           return qsTr("Reading…")
        }
    }

    function milColor(e) {
        if (e.readiness !== "answered") return Theme.textDim
        return e.mil ? Theme.danger : Theme.success
    }

    function listTitle(key) {
        switch (key) {
        case "stored":  return qsTr("Stored")
        case "pending": return qsTr("Pending")
        default:        return qsTr("Permanent")
        }
    }

    function listHint(key) {
        switch (key) {
        case "stored":  return qsTr("confirmed faults")
        case "pending": return qsTr("seen once, not yet confirmed")
        default:        return qsTr("cleared only by the ECU, once its monitor passes")
        }
    }

    // Each titled code of an answered list on a line of its own (ADR-0044);
    // a code with no title is in the codes line alone
    function titlesText(list) {
        if (list.outcome !== "answered") return ""
        return list.codes.map((c, i) => list.titles[i] !== "" ? c + "  " + list.titles[i] : "")
                         .filter(line => line !== "").join("\n")
    }

    // A list's codes, or what became of its request
    function codesText(list) {
        switch (list.outcome) {
        case "answered":
            if (list.codes.length === 0) return qsTr("None")
            return list.more > 0 ? list.codes.join("  ") + "  " + qsTr("and %1 more").arg(list.more)
                                 : list.codes.join("  ")
        case "refused":    return qsTr("Refused (%1)").arg(list.nrc)
        case "unanswered": return qsTr("No answer")
        default:           return qsTr("Not asked yet")
        }
    }

    function freezeText(f) {
        switch (f.outcome) {
        case "answered":
            if (!f.stored) return qsTr("None stored")
            return f.title !== "" ? qsTr("Stored by %1 (%2)").arg(f.code, f.title)
                                  : qsTr("Stored by %1").arg(f.code)
        case "refused":    return qsTr("Refused (%1)").arg(f.nrc)
        case "unanswered": return qsTr("No answer")
        default:           return qsTr("Not asked yet")
        }
    }

    function readinessSummary(e) {
        const done = e.monitors.filter(m => m.complete).length
        return qsTr("%1 of %2 complete").arg(done).arg(e.monitors.length)
    }

    // A monitor this drive cycle (PID 41), under the "This drive" head:
    // complete, enabled and not complete yet, or off for the rest of
    // the cycle
    function cycleText(m) {
        if (!m.cycleEnabled)
            return qsTr("Off")
        return m.cycleComplete ? qsTr("Done") : qsTr("Not yet")
    }

    // The monitors' and values' rows stop here, so a state or a value
    // stays near its name on a wide window
    readonly property int rowsMaxWidth: 480

    // Value plus unit at the def's decimal places, as Live formats them
    function valueText(v, decimals, unit) {
        const n = Number(v).toLocaleString(Qt.locale(), 'f', decimals)
        return unit !== "" ? n + " " + unit : n
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 12

        // Wide, the title and the actions share a row, Clear codes last; on a
        // phone the title stands alone and the actions take two rows under it
        GridLayout {
            id: headerGrid
            readonly property bool twoRows: root.width < 560
            Layout.fillWidth: true
            columns: twoRows ? 1 : 3
            columnSpacing: 10
            rowSpacing: Theme.spacing
            PageHeader {
                objectName: "diagnosticsHeader"
                title: Theme.compact ? qsTr("Codes") : qsTr("Diagnostics")
            }
            // The row's width: a title row alone reports its own width as
            // its most, and the grid would stop there
            Item {
                visible: !headerGrid.twoRows
                Layout.fillWidth: true
            }
            // Each cell is placed, since the order differs between the forms
            GridLayout {
                id: actions
                objectName: "diagnosticsActions"
                readonly property bool twoRows: headerGrid.twoRows
                Layout.fillWidth: twoRows
                columns: twoRows ? 2 : 3
                columnSpacing: Theme.spacing
                rowSpacing: Theme.spacing
                // Whatever the read's state: a report still being read says so
                ThemedButton {
                    id: saveReport
                    objectName: "saveReport"
                    visible: Bridge.connected
                    enabled: !root.confirming
                    text: qsTr("Save report…")
                    Layout.row: actions.twoRows ? 1 : 0
                    Layout.column: 0
                    Layout.columnSpan: actions.twoRows ? 2 : 1
                    Layout.fillWidth: actions.twoRows
                    onClicked: reportDialog.open()
                }
                ThemedButton {
                    id: readAgain
                    objectName: "readAgain"
                    visible: Bridge.connected
                    enabled: root.canReadAgain && !root.clearing && !root.confirming
                    text: qsTr("Read again")
                    Layout.row: 0
                    Layout.column: actions.twoRows ? 0 : 1
                    Layout.fillWidth: actions.twoRows
                    onClicked: Bridge.readDiagnostics()
                }
                ThemedButton {
                    id: clearCodes
                    objectName: "clearCodes"
                    visible: Bridge.connected
                    enabled: root.canClear && !root.confirming
                    text: qsTr("Clear codes…")
                    Layout.row: 0
                    Layout.column: actions.twoRows ? 1 : 2
                    Layout.fillWidth: actions.twoRows
                    onClicked: root.confirming = true
                }
            }
        }

        DocumentSaveDialog {
            id: reportDialog
            objectName: "reportDialog"
            title: qsTr("Save the health report")
            defaultSuffix: "txt"
            nameFilters: [qsTr("Text (*.txt)"), qsTr("JSON (*.json)")]
            currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
            currentFile: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
                         + "/health-report.txt"
            onAccepted: root.saveReportTo(reportDialog.selectedFile,
                                          reportDialog.selectedNameFilter.index)
            onFailed: words => { root.reportFailed = true; root.reportNote = qsTr("Report not saved: %1").arg(words) }
        }

        // Status line; disconnected, the empty state with a way to connect
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                textFormat: Text.PlainText
                objectName: "diagnosticsStatus"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.statusText()
                color: Theme.textDim
            }
            // Secondary: the header's Connect... is the window's primary
            ThemedButton {
                objectName: "diagnosticsConnect"
                visible: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
                text: qsTr("Connect…")
                onClicked: root.connectRequested()
            }
        }

        Label {
            textFormat: Text.PlainText
            objectName: "reportNote"
            visible: root.reportNote !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.reportNote
            color: root.reportFailed ? Theme.danger : Theme.textDim
        }

        Flickable {
            id: flick
            objectName: "diagnosticsFlick"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: cards.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            }

            ColumnLayout {
                id: cards
                width: flick.width - 12
                spacing: 12

                // The clear's confirmation, in place (a Popup doesn't scale with
                // the zoom): what a clear erases, what it keeps, what it needs.
                // It scrolls with the cards, so its buttons can be reached
                // however tall its text runs on a short page
                Rectangle {
                    objectName: "clearConfirmPanel"
                    visible: root.confirming && Bridge.connected
                    Layout.fillWidth: true
                    implicitHeight: confirmBody.implicitHeight + 2 * Theme.pageMargin
                    radius: Theme.radius
                    color: Theme.elevated
                    border.width: 1
                    border.color: Theme.warning

                    ColumnLayout {
                        id: confirmBody
                        anchors.fill: parent
                        anchors.margins: Theme.pageMargin
                        spacing: 10

                        Label {
                            textFormat: Text.PlainText
                            objectName: "clearConfirmTitle"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            // The bridge's list, the one clearDiagnostics sends to
                            text: qsTr("Clear the trouble codes of ECU %1?").arg(Bridge.clearTargets.join(", "))
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Label {
                            textFormat: Text.PlainText
                            objectName: "clearConfirmText"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: Theme.text
                            // J1979: "Other manufacturer specific
                            // clearing/resetting actions may also occur"
                            text: (Bridge.clearFindsNothing
                                   ? qsTr("No codes are stored and the check-engine light is off: a clear now only sets the readiness monitors back to incomplete and resets any values the manufacturer chose.") + "\n\n"
                                   : "") +
                                  qsTr("Each ECU that obeys erases its stored and pending codes and its freeze frame, turns the check-engine light off, and sets its readiness monitors back to incomplete. Permanent codes stay: only the ECU clears them, once its monitor passes. Until the monitors complete again, which takes some driving, the car can fail an emissions inspection.") +
                                  "\n\n" +
                                  qsTr("A clear works best with the engine off and the ignition on; some ECUs refuse it while the engine runs. Havne asks each ECU its engine and vehicle speeds first, and asks once more before clearing a moving vehicle. The report is saved to the recordings folder first; if it can't be saved, nothing is sent.")
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.spacing
                            ThemedButton {
                                objectName: "clearConfirm"
                                highlighted: true
                                text: qsTr("Clear codes")
                                baseColor: Theme.danger
                                textColor: Theme.textOnDanger
                                hoverColor: Qt.lighter(Theme.danger, 1.15)
                                pressedColor: Qt.darker(Theme.danger, 1.2)
                                enabled: root.canClear
                                onClicked: root.clearCodes()
                            }
                            ThemedButton {
                                objectName: "clearCancel"
                                text: qsTr("Cancel")
                                onClicked: root.confirming = false
                            }
                        }
                    }
                }

                // The moving vehicle's question (ADR-0038's appendix of
                // 2026-11-01): the ECUs whose clear stopped at the vehicle
                // speed, once every clear has ended; the speed named
                Rectangle {
                    objectName: "clearMovingPanel"
                    visible: Bridge.connected && Bridge.clearMoving.length > 0
                    Layout.fillWidth: true
                    implicitHeight: movingBody.implicitHeight + 2 * Theme.pageMargin
                    radius: Theme.radius
                    color: Theme.elevated
                    border.width: 1
                    border.color: Theme.danger

                    ColumnLayout {
                        id: movingBody
                        anchors.fill: parent
                        anchors.margins: Theme.pageMargin
                        spacing: 10

                        Label {
                            textFormat: Text.PlainText
                            objectName: "clearMovingTitle"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: Bridge.clearMovingSpeed !== ""
                                ? qsTr("The vehicle reads %1. Clear ECU %2 anyway?")
                                      .arg(Bridge.clearMovingSpeed)
                                      .arg(Bridge.clearMoving.join(", "))
                                : qsTr("ECU %1 didn't give the vehicle speed. Clear it anyway?")
                                      .arg(Bridge.clearMoving.join(", "))
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Label {
                            textFormat: Text.PlainText
                            objectName: "clearMovingText"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: Theme.text
                            // J1979: "Other manufacturer specific
                            // clearing/resetting actions may also occur"
                            text: qsTr("A clear can also reset other values the manufacturer chose, so the engine may run differently for a while. The report saved before the clear holds these codes.")
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.spacing
                            ThemedButton {
                                objectName: "clearMovingConfirm"
                                text: qsTr("Clear while moving")
                                baseColor: Theme.danger
                                textColor: Theme.textOnDanger
                                hoverColor: Qt.lighter(Theme.danger, 1.15)
                                pressedColor: Qt.darker(Theme.danger, 1.2)
                                onClicked: Bridge.clearDiagnosticsMoving()
                            }
                            ThemedButton {
                                objectName: "clearMovingCancel"
                                text: qsTr("Cancel")
                                onClicked: Bridge.cancelClearMoving()
                            }
                        }
                    }
                }

                // The unlock's question, in place and scrolling with the
                // cards: core's words in full, then the word itself
                UnlockDialog {
                    objectName: "unlockDialog"
                    visible: root.unlockShown
                    Layout.fillWidth: true
                    conversation: root.unlockEntry ?? ({})
                    lines: visible ? Bridge.consent.unlockLines() : []
                    asking: root.unlockAsking
                    note: root.unlockNote
                    onUnlockChosen: level => root.unlockSend(level)
                    onCancelChosen: root.unlockClose()
                }

                // The car's readiness by the federal inspection rule
                // (ADR-0045), every ECU judged together, once read
                Rectangle {
                    id: inspectionCard
                    readonly property var entry: Bridge.inspection
                    objectName: "inspectionCard"
                    visible: entry.verdict !== ""
                    Layout.fillWidth: true
                    implicitHeight: inspectionBody.implicitHeight + 2 * Theme.pageMargin
                    radius: Theme.radius
                    color: Theme.surface

                    ColumnLayout {
                        id: inspectionBody
                        anchors.fill: parent
                        anchors.margins: Theme.pageMargin
                        spacing: 6

                        SectionLabel { text: qsTr("Inspection readiness") }
                        Label {
                            textFormat: Text.PlainText
                            objectName: "inspectionText"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: inspectionCard.entry.text
                            color: inspectionCard.entry.verdict === "ready" ? Theme.success
                                 : inspectionCard.entry.verdict === "mil_on" ? Theme.danger
                                 : Theme.warning
                        }
                        Label {
                            textFormat: Text.PlainText
                            objectName: "inspectionRule"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: inspectionCard.entry.rule
                            color: Theme.textDim
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                }

                Repeater {
                    model: root.ecus
                    delegate: EcuCard {}
                }

                // A typed code's title (ADR-0044): with or without a session
                Rectangle {
                    objectName: "dtcLookup"
                    Layout.fillWidth: true
                    implicitHeight: lookupBody.implicitHeight + 2 * Theme.pageMargin
                    radius: Theme.radius
                    color: Theme.surface

                    ColumnLayout {
                        id: lookupBody
                        anchors.fill: parent
                        anchors.margins: Theme.pageMargin
                        spacing: 6

                        SectionLabel { text: qsTr("Look up a code") }
                        ThemedTextField {
                            id: dtcLookupField
                            objectName: "dtcLookupField"
                            Layout.fillWidth: true
                            Layout.maximumWidth: 300
                            maximumLength: 16
                            placeholderText: qsTr("P0301")
                            inputMethodHints: Qt.ImhPreferUppercase | Qt.ImhNoPredictiveText
                        }
                        Label {
                            textFormat: Text.PlainText
                            readonly property string summary: Bridge.describeDtc(dtcLookupField.text)
                            objectName: "dtcLookupResult"
                            visible: summary !== ""
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: summary.charAt(0).toUpperCase() + summary.slice(1)
                            color: Theme.text
                        }
                    }
                }
            }
        }
    }

    // One ECU's report
    component EcuCard: Rectangle {
        id: card
        required property var modelData
        readonly property var ecu: modelData
        // The module's conversation, null where no plugin talks to it
        readonly property var conversation: root.conversationOf(modelData)

        objectName: "ecu_" + ecu.ecu
        Layout.fillWidth: true
        implicitHeight: body.implicitHeight + 2 * Theme.pageMargin
        radius: Theme.radius
        color: Theme.surface

        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                Label {
                    textFormat: Text.PlainText
                    text: qsTr("ECU %1").arg(card.ecu.ecu)
                    font.pixelSize: Theme.fontEmphasis
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "ecuState"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.ecuState(card.ecu)
                    color: Theme.textDim
                }
                // A plugin that talks to this module: the question's way in
                ThemedButton {
                    objectName: "unlockOpen"
                    visible: root.canUnlock(card.conversation) && root.unlocking !== card.conversation.channel
                    enabled: root.unlocking < 0
                    flat: true
                    compact: true
                    Layout.minimumWidth: Theme.touchTarget
                    text: qsTr("Unlock…")
                    Accessible.name: qsTr("Unlock ECU %1").arg(card.ecu.ecu)
                    onClicked: root.unlockOpen(card.conversation)
                }
            }
            // What the plugin's unlock of this module did, and why not
            Label {
                textFormat: Text.PlainText
                objectName: "unlockState"
                visible: text !== ""
                text: root.unlockText(card.conversation)
                color: card.conversation !== null && card.conversation.unlock === "unlocked" ? Theme.success : Theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            // The last clear's outcome, above the read that followed it
            Label {
                textFormat: Text.PlainText
                objectName: "clearState"
                visible: text !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.clearText(card.ecu)
                color: card.ecu.clear === "cleared" ? Theme.success : Theme.warning
            }

            ColumnLayout {
                visible: card.ecu.asked
                Layout.fillWidth: true
                spacing: 10

                // The check-engine light (the MIL), from readiness
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    Rectangle {
                        implicitWidth: 12
                        implicitHeight: 12
                        radius: 6
                        color: root.milColor(card.ecu)
                    }
                    Label {
                        textFormat: Text.PlainText
                        objectName: "milLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.milText(card.ecu)
                        color: Theme.text
                    }
                }

                SectionLabel { text: qsTr("Trouble codes") }
                Repeater {
                    model: card.ecu.lists
                    delegate: ColumnLayout {
                        id: listRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.listTitle(listRow.modelData.key) + "  ·  " +
                                  root.listHint(listRow.modelData.key)
                            color: Theme.textDim
                            font.pixelSize: Theme.fontCaption
                        }
                        Label {
                            textFormat: Text.PlainText
                            objectName: "codes_" + listRow.modelData.key
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.codesText(listRow.modelData)
                            // An empty family is a bitmap face on Windows
                            font.family: listRow.modelData.codes.length > 0 ? Theme.monoFamily
                                                                            : root.font.family
                            font.pixelSize: listRow.modelData.codes.length > 0 ? Theme.fontEmphasis : Theme.fontBody
                            color: listRow.modelData.codes.length > 0 ? Theme.text : Theme.textDim
                        }
                        Label {
                            textFormat: Text.PlainText
                            objectName: "titles_" + listRow.modelData.key
                            visible: text !== ""
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.titlesText(listRow.modelData)
                            color: Theme.textDim
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                }

                SectionLabel {
                    text: card.ecu.readiness === "answered"
                          ? qsTr("Readiness monitors · %1").arg(root.readinessSummary(card.ecu))
                          : qsTr("Readiness monitors")
                }
                Label {
                    textFormat: Text.PlainText
                    visible: card.ecu.readiness === "answered" && card.ecu.monitors.length === 0
                    text: qsTr("The ECU reports none")
                    color: Theme.textDim
                }
                // One row a monitor: its name, this drive cycle's state
                // (PID 41, once answered: a column with its head) and
                // its state since the last clear (PID 01). A grid, so
                // the states line up under their heads; a hidden cell
                // takes no place, so without PID 41 a row is two cells.
                GridLayout {
                    id: monitorGrid
                    objectName: "monitors"
                    readonly property bool cycleShown: card.ecu.cycle === "answered"
                    Layout.fillWidth: true
                    Layout.maximumWidth: root.rowsMaxWidth
                    visible: card.ecu.monitors.length > 0
                    columns: cycleShown ? 3 : 2
                    columnSpacing: Theme.spacing
                    rowSpacing: 2
                    Item {
                        visible: monitorGrid.cycleShown
                        Layout.fillWidth: true
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: monitorGrid.cycleShown
                        text: qsTr("This drive")
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: monitorGrid.cycleShown
                        text: qsTr("Since clear")
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                    }
                    Repeater {
                        // three cells a monitor: name, this drive, since the clear
                        model: card.ecu.monitors.length * 3
                        delegate: Label {
                            textFormat: Text.PlainText
                            id: monitorCell
                            required property int index
                            readonly property var monitor: card.ecu.monitors[Math.floor(index / 3)]
                            readonly property int column: index % 3
                            objectName: column === 1 ? "monitorCycle"
                                      : column === 2 ? "monitorState" : "monitorName"
                            visible: column !== 1 || monitorGrid.cycleShown
                            Layout.fillWidth: column === 0
                            elide: column === 0 ? Text.ElideRight : Text.ElideNone
                            text: column === 0 ? monitor.name
                                  : column === 1 ? root.cycleText(monitor)
                                  : monitor.complete ? qsTr("Complete") : qsTr("Incomplete")
                            color: column === 0 ? Theme.text
                                   : column === 1 ? (!monitor.cycleEnabled ? Theme.textDim
                                                     : monitor.cycleComplete ? Theme.success
                                                     : Theme.warning)
                                   : monitor.complete ? Theme.success : Theme.warning
                        }
                    }
                }

                SectionLabel { text: qsTr("Freeze frame") }
                Label {
                    textFormat: Text.PlainText
                    objectName: "freezeLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.freezeText(card.ecu.freeze)
                    color: card.ecu.freeze.stored ? Theme.text : Theme.textDim
                }
                ColumnLayout {
                    objectName: "freezeValues"
                    Layout.fillWidth: true
                    Layout.maximumWidth: root.rowsMaxWidth
                    spacing: 2
                    Repeater {
                        model: card.ecu.freeze.values
                        delegate: RowLayout {
                            id: valueRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Theme.spacing
                            Label {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: valueRow.modelData.name
                                color: Theme.text
                            }
                            Label {
                                textFormat: Text.PlainText
                                objectName: "freezeValue"
                                text: root.valueText(valueRow.modelData.value,
                                                     valueRow.modelData.decimals,
                                                     valueRow.modelData.unit)
                                font.family: Theme.monoFamily
                                color: Theme.text
                            }
                        }
                    }
                }
            }
        }
    }

    component SectionLabel: Label {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        Layout.topMargin: 4
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
        color: Theme.textDim
    }
}
