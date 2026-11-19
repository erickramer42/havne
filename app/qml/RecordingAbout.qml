// File/layer: app/qml/RecordingAbout.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)
// What applies to a recording as a whole: its info, the frames check, Save
// report, the decode, the baseline, Compare and the alignment; one part of
// RecordingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property RecordingsPage host
    property alias aboutActions: aboutActions
    property alias alignChoice: alignChoice
    parent: root.host.currentHead && root.host.aboutShown ? root.host.currentHead.toolsSlot : root.host.aboutHome
    Layout.fillWidth: true
    spacing: 10
    // ---- About this recording: what applies to it as a whole ----
    Heading {
        objectName: "aboutHeading"
        visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized && !root.host.pinnedViewer
        text: qsTr("About this recording")
    }
    Label {
        objectName: "recordingInfo"
        visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: root.host.recording.state === RecordingView.Open ? root.host.infoText() : ""
    }
    RowLayout {
        id: aboutActions
        visible: Theme.compact && root.host.recording.state === RecordingView.Open && !root.host.chartMaximized
        Layout.fillWidth: true
    }
    ColumnLayout {
        id: aboutNote
        visible: Theme.compact && root.host.reportNote !== "" && !root.host.chartMaximized
        Layout.fillWidth: true
    }
    Label {
        objectName: "reportNote"
        // Under Save report...: the title row's on the desktop, About's on a phone
        parent: Theme.compact ? aboutNote : root.host.topNote
        visible: root.host.reportNote !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.host.reportNote
        color: root.host.reportFailed ? Theme.danger : Theme.textDim
    }

    // Where the values come from: offered once it opened, or failed
    // (a profile that can't decode it, a folder with frames alone)
    ComboBox {
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                 implicitContentHeight + topPadding + bottomPadding,
                                 implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
        id: decodeChoice
        objectName: "decodeChoice"
        // Values alone have no frames to decode through a profile
        readonly property bool shown: (root.host.recording.state === RecordingView.Open
                                       || root.host.recording.state === RecordingView.Failed)
                                      && !root.host.chartMaximized && root.host.recording.hasFrames
        // Its own rule, not its place's: About's head hides and moves it
        visible: shown
        Layout.fillWidth: true
        textRole: "text"
        valueRole: "value"
        // The model stays while the box is hidden: a model made again
        // resets the index, which the binding below wouldn't set back
        model: root.host.decodeChoices()
        currentIndex: Math.max(0, indexOfValue(root.host.recording.decodedWith))
        Accessible.name: qsTr("Where the values come from")
        onActivated: index => root.host.decodeWith(valueAt(index))
    }

    // The picked vehicle's baseline: this recording made it, or the
    // baseline overlaid on it, lined up on their engine starts
    Flow {
        id: baselineFlow
        objectName: "baselineRow"
        visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized
                 && root.host.pickedVehicle !== null
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "useAsBaseline"
            visible: !root.host.isBaseline
            width: Math.min(implicitWidth, baselineFlow.width)
            text: qsTr("Use as %1's baseline").arg(root.host.pickedVehicle !== null
                                                    ? root.host.pickedVehicle.label : "")
            onClicked: root.host.openPart.useAsBaseline()
        }
        Label {
            objectName: "isBaseline"
            visible: root.host.isBaseline
            width: Math.min(implicitWidth, baselineFlow.width)
            height: root.host.phoneShape ? Theme.touchTarget : Theme.controlHeight
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: qsTr("%1's baseline").arg(root.host.pickedVehicle !== null
                                             ? root.host.pickedVehicle.label : "")
        }
        ThemedButton {
            objectName: "compareBaseline"
            visible: root.host.baselineRow !== null && !root.host.isBaseline
            width: Math.min(implicitWidth, baselineFlow.width)
            text: qsTr("Compare with the baseline")
            Accessible.name: qsTr("Compare with %1's baseline, aligned on the engine starts")
                                 .arg(root.host.pickedVehicle !== null ? root.host.pickedVehicle.label : "")
            onClicked: root.host.openPart.compareWithBaseline()
        }
    }

    // Another recording to overlay, and how the two line up
    RowLayout {
        parent: root.host.compareSlot
        visible: root.host.recording.state === RecordingView.Open
        Layout.fillWidth: true
        spacing: Theme.spacing
        ComboBox {
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
            id: compareChoice
            objectName: "compareChoice"
            readonly property var others: root.host.recordings.filter(r => r.folder !== root.host.recording.folder)
            Layout.fillWidth: true
            model: [root.host.comparing ? root.host.compareTitle : qsTr("Compare with…")]
                   .concat(others.map(r => r.title))
            enabled: others.length > 0
            Accessible.name: qsTr("Compare with another recording")
            onActivated: index => {
                if (index > 0)
                    root.host.compareWith(others[index - 1])
                currentIndex = 0
            }
        }
        ThemedButton {
            objectName: "stopCompare"
            visible: root.host.comparing
            text: qsTr("Stop")
            Accessible.name: qsTr("Stop comparing")
            onClicked: root.host.stopComparing()
        }
    }
    Flow {
        parent: root.host.compareSlot
        visible: root.host.comparing && root.host.recording.state === RecordingView.Open
        Layout.fillWidth: true
        spacing: 6
        ComboBox {
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
            id: alignChoice
            objectName: "alignChoice"
            width: 170
            textRole: "text"
            valueRole: "value"
            model: [{ text: qsTr("Align the starts"), value: "start" },
                    { text: qsTr("Align UTC starts"), value: "utc" },
                    { text: qsTr("Align a marker"), value: "marker" },
                    { text: qsTr("Align a rise"), value: "rise" },
                    { text: qsTr("Align a fall"), value: "fall" },
                    { text: qsTr("Align engine starts"), value: "engine" },
                    { text: qsTr("Align a pull"), value: "pull" }]
            onActivated: root.host.setAlignOn(currentValue)
        }
        ComboBox {
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
            objectName: "alignPullHere"
            visible: root.host.alignOn === "pull"
            width: 220
            enabled: count > 0
            model: root.host.pullRows(root.host.pullsHere, true)
            currentIndex: root.host.alignPullHere
            Accessible.name: qsTr("This recording's pull to align on")
            // A pick ends the binding: put it back, so the box
            // follows the page's choice
            onActivated: index => {
                root.host.alignPullHere = index
                currentIndex = Qt.binding(() => root.host.alignPullHere)
            }
        }
        ComboBox {
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
            objectName: "alignPullThere"
            visible: root.host.alignOn === "pull"
            width: 220
            enabled: count > 0
            model: root.host.pullRows(root.host.pullsThere, false)
            currentIndex: root.host.alignPullThere
            Accessible.name: qsTr("The compared recording's pull to align on")
            onActivated: index => {
                root.host.alignPullThere = index
                currentIndex = Qt.binding(() => root.host.alignPullThere)
            }
        }
        ComboBox {
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
            objectName: "alignMarker"
            visible: root.host.alignOn === "marker"
            width: 150
            model: root.host.recording.state === RecordingView.Open ? root.host.markerLabels() : []
            onActivated: root.host.alignLabel = currentText
        }
        ComboBox {
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
            objectName: "alignSignal"
            visible: root.host.alignOn === "rise" || root.host.alignOn === "fall"
            width: 150
            model: root.host.recording.state === RecordingView.Open
                   ? ComparePairs.numbers(root.host.recording.signalList) : []
            textRole: "name"
            onActivated: index => root.host.chooseAlignSignal(model[index])
        }
        ThemedTextField {
            objectName: "alignLevel"
            visible: root.host.alignOn === "rise" || root.host.alignOn === "fall"
            width: 100
            placeholderText: qsTr("Level")
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            onEditingFinished: root.host.alignLevel = root.host.numberOf(text)
        }
        ThemedTextField {
            objectName: "alignHysteresis"
            visible: root.host.alignOn === "rise" || root.host.alignOn === "fall"
            width: 100
            placeholderText: qsTr("Hysteresis")
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            onEditingFinished: {
                const v = root.host.numberOf(text)
                root.host.alignHysteresis = v >= 0 ? v : 0
            }
        }
        ThemedCheckBox {
            objectName: "compareDifference"
            text: qsTr("Difference")
            enabled: root.host.compareAligned
            checked: root.host.differenceShown
            Accessible.name: qsTr("Plot the compared recording's difference from this one")
            onToggled: {
                root.host.setDifferenceShown(!root.host.differenceShown)
                checked = Qt.binding(() => root.host.differenceShown)
            }
        }
    }
    Label {
        objectName: "compareStatus"
        parent: root.host.compareSlot
        visible: root.host.comparing && root.host.recording.state === RecordingView.Open
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: root.host.compareStatus
    }

    // The frames check: what it found, each row centering the view on
    // its finding
    Label {
        objectName: "checkSummary"
        visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: root.host.recording.state === RecordingView.Open ? root.host.checkText() : ""
    }
}
