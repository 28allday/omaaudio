pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

C.Popup {
    id: root
    objectName: "recordingPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool recording: service !== null && service.recording
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 40 : 560)
    height: Math.min(body.implicitHeight + padding * 2, parent ? parent.height - 40 : 850)
    padding: Theme.s4
    // The scrolling column reaches into this margin (ScrollPane).
    leftPadding: Theme.s4 - Theme.s2; rightPadding: Theme.s4 - Theme.s3
    modal: true
    focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onOpened: if (service) service.refreshRecordingInputs()
    onAboutToHide: if (service) service.cancelRecordingInputScan()
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle {
        color: Theme.panelBg; radius: Theme.rPanel
        border.width: Theme.hairline; border.color: Theme.border
    }
    contentItem: ScrollPane {
        id: recordingScroll
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
        id: body
        x: recordingScroll.reachLeft; width: recordingScroll.columnWidth
        spacing: Theme.s3
        RowLayout {
            Layout.fillWidth: true
            Text { text: qsTr("Recording"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                iconName: "x"; text: qsTr("Close recording panel"); onClicked: root.close()
                helpText: qsTr("Close this panel. Your recording settings are kept.")
                helpScope: qsTr("A recording in progress carries on; stop it with the transport.")
            }
        }
        Text {
            Layout.fillWidth: true
            text: root.service && root.service.recordArmedTrack >= 0
                ? qsTr("Armed: %1").arg(root.service.trackById(root.service.recordArmedTrack).name || qsTr("removed track"))
                : qsTr("Record to a new track")
            textFormat: Text.PlainText; elide: Text.ElideRight
            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        }
        RowLayout {
            Layout.fillWidth: true
            ComboField {
                objectName: "recordInputChoice"
                Layout.fillWidth: true
                caption: qsTr("Input")
                helpTitle: qsTr("Recording input")
                helpText: qsTr("Choose the PipeWire input to record from.")
                helpScope: qsTr("The primary input for the next take. Refresh the list after plugging in a device.")
                unavailableReason: qsTr("Stop the current recording before changing the input.")
                model: root.service ? root.service.recordingInputNames : []
                currentIndex: root.service ? root.service.recordingInputIds.indexOf(root.service.recordInput) : 0
                enabled: !root.recording
                onActivated: index => root.service.setRecordInput(root.service.recordingInputIds[index])
            }
            ToolButton {
                objectName: "refreshRecordInputs"; iconName: "refresh-cw"; text: qsTr("Refresh inputs")
                helpText: qsTr("Look again for PipeWire inputs, for example after connecting an interface.")
                helpScope: qsTr("Updates the input lists. Your chosen input is kept where it still exists.")
                unavailableReason: root.service === null ? qsTr("The audio engine is not available.")
                                                         : qsTr("A scan for inputs is already running.")
                enabled: root.service !== null && !root.service.scanningInputs
                onClicked: root.shell.runAction("refresh_recording_inputs")
            }
        }
        RowLayout {
            ComboField {
                objectName: "recordChannelChoice"; caption: qsTr("Channels")
                helpTitle: qsTr("Recording channels")
                helpText: qsTr("Record one channel (mono) or two (stereo) from the primary input.")
                helpScope: qsTr("Mono suits a single microphone. The new take uses this layout.")
                unavailableReason: qsTr("Stop the current recording before changing the channels.")
                Layout.fillWidth: true; model: [qsTr("Mono"), qsTr("Stereo")]
                currentIndex: root.service ? root.service.recordChannels - 1 : 0
                enabled: !root.recording
                onActivated: index => root.service.setRecordChannels(index + 1)
            }
            ToolButton {
                objectName: "recordNewTrack"; text: qsTr("Use new track"); showLabel: true
                helpText: qsTr("Record onto a new track instead of the armed track.")
                helpScope: qsTr("Clears the arming. The armed track's audio is left as it is.")
                unavailableReason: root.recording ? qsTr("Stop the current recording first.")
                                                  : qsTr("No track is armed, so recording already goes to a new track.")
                enabled: !root.recording && root.service !== null && root.service.recordArmedTrack >= 0
                onClicked: root.shell.runAction("record_new_track")
            }
        }
        RowLayout {
            Layout.fillWidth: true; enabled: !root.recording
            ComboField {
                id: extraInput; objectName: "additionalRecordInputChoice"; Layout.fillWidth: true
                caption: qsTr("Additional input")
                helpText: qsTr("Choose another PipeWire input to record at the same time as the primary input.")
                helpScope: qsTr("Press Add input to include it. Each additional input records to its own track.")
                unavailableReason: qsTr("Stop the current recording first.")
                model: root.service ? root.service.recordingInputNames : []
            }
            ComboField {
                id: extraChannels; objectName: "additionalRecordChannels"; Layout.preferredWidth: 92
                Accessible.name: qsTr("Additional input channels"); model: [qsTr("Mono"), qsTr("Stereo")]
                helpTitle: qsTr("Additional input channels")
                helpText: qsTr("Mono or stereo for the additional input chosen beside it.")
                unavailableReason: qsTr("Stop the current recording first.")
            }
            ToolButton {
                objectName: "addRecordInput"; text: qsTr("Add input"); iconName: "plus"; showLabel: true
                helpText: qsTr("Add the chosen additional input to the next recording.")
                helpScope: qsTr("It records to a separate new track in the same Undo step as the primary take.")
                unavailableReason: !root.service ? qsTr("The audio engine is not available.")
                    : root.service.additionalRecordInputs.length >= 7 ? qsTr("Up to seven additional inputs can be added.")
                    : qsTr("Choose an input other than the primary input.")
                enabled: root.service && root.service.additionalRecordInputs.length < 7
                    && root.service.recordingInputIds[extraInput.currentIndex] !== root.service.recordInput
                onClicked: root.service.addRecordInput(root.service.recordingInputIds[extraInput.currentIndex], extraChannels.currentIndex + 1)
            }
        }
        Repeater {
            model: root.service ? root.service.additionalRecordInputs : []
            RowLayout {
                id: inputRow
                required property var modelData
                required property int index
                Layout.fillWidth: true
                Text {
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    Layout.fillWidth: true; elide: Text.ElideMiddle; textFormat: Text.PlainText; color: Theme.textSecondary
                    text: (root.service.recordingInputNames[root.service.recordingInputIds.indexOf(inputRow.modelData.input)] || inputRow.modelData.input || qsTr("Default input"))
                        + " · " + (inputRow.modelData.channels === 1 ? qsTr("mono") : qsTr("stereo"))
                        + " · " + (inputRow.modelData.track < 0 ? qsTr("new track") : qsTr("track %1").arg(inputRow.modelData.track))
                }
                ToolButton {
                    objectName: "removeRecordInput" + inputRow.index; text: qsTr("Remove input"); iconName: "x"
                    helpText: qsTr("Leave this input out of the next recording.")
                    unavailableReason: qsTr("Stop the current recording first.")
                    enabled: !root.recording; onClicked: root.service.removeRecordInput(inputRow.index)
                }
            }
        }
        ToolButton {
            objectName: "recordPunchChoice"; text: qsTr("Punch into selected range"); showLabel: true
            helpText: qsTr("Record only inside the selected time range, replacing that part of the armed track.")
            helpScope: qsTr("Needs an armed track and a time selection. Audio outside the range is untouched.")
            helpTopic: qsTr("Punch-in recording starts and stops at the selection edges while playback runs before and after them, so you can perform a fix in context.")
            unavailableReason: qsTr("Stop the current recording first.")
            iconName: "replace"; checkable: true; checked: root.service !== null && root.service.recordPunch
            enabled: !root.recording
            onClicked: root.shell.runAction("toggle_record_punch")
        }
        ToolButton {
            objectName: "recordMonitorChoice"; text: qsTr("Monitor primary input — use headphones"); showLabel: true
            helpText: qsTr("Hear the main input through your headphones or speakers while recording. Input effects are not included.")
            helpScope: qsTr("Use headphones. Speakers would feed the output back into the microphone.")
            unavailableReason: qsTr("Stop the current recording first.")
            iconName: "headphones"; checkable: true; checked: root.service !== null && root.service.recordMonitor
            enabled: !root.recording
            onClicked: root.shell.runAction("toggle_record_monitor")
        }
        RowLayout {
            enabled: !root.recording
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Loop passes (0 = off)"); color: Theme.textSecondary }
            ValueField {
                objectName: "recordLoopTakes"; Layout.preferredWidth: 70; from: 0; to: 16; decimals: 0; step: 1
                helpTitle: qsTr("Loop passes")
                helpText: qsTr("Record the selected range repeatedly, one take per pass. 0 records a single take.")
                helpScope: qsTr("Range: 0 to 16. Earlier passes are kept on muted tracks; the last pass is the active one.")
                unavailableReason: qsTr("Stop the current recording first.")
                value: root.service ? root.service.recordLoopTakes : 0
                onEdited: value => root.service.setRecordLoopTakes(Math.round(value))
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; text: qsTr("Uses the selected range. Earlier passes are kept on muted tracks; the last pass is active.") }
        }
        RowLayout {
            enabled: !root.recording
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Pre-roll s"); color: Theme.textSecondary }
            ValueField {
                objectName: "recordPreRoll"; Layout.preferredWidth: 70; from: 0; to: 10; decimals: 2; step: 0.1
                helpTitle: qsTr("Pre-roll")
                helpText: qsTr("Seconds of playback before recording starts, so you hear the lead-in.")
                helpScope: qsTr("Range: 0 to 10 seconds. The take starts after the pre-roll; the lead-in stays only in the raw recovery file.")
                unavailableReason: qsTr("Stop the current recording first.")
                value: root.service ? root.service.recordPreRoll : 0
                onEdited: value => root.service.setRecordingLeadIn(value, root.service.recordCountIn, root.service.recordTempo)
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Count-in beats"); color: Theme.textSecondary }
            ValueField {
                objectName: "recordCountIn"; Layout.preferredWidth: 55; from: 0; to: 16; decimals: 0; step: 1
                helpTitle: qsTr("Count-in beats")
                helpText: qsTr("Number of click beats played before recording starts.")
                helpScope: qsTr("Range: 0 to 16, at the tempo beside it. The count-in comes before the pre-roll.")
                unavailableReason: qsTr("Stop the current recording first.")
                value: root.service ? root.service.recordCountIn : 0
                onEdited: value => root.service.setRecordingLeadIn(root.service.recordPreRoll, Math.round(value), root.service.recordTempo)
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("BPM"); color: Theme.textSecondary }
            ValueField {
                objectName: "recordTempo"; Layout.fillWidth: true; from: 20; to: 300; decimals: 1; step: 1
                helpTitle: qsTr("Count-in tempo")
                helpText: qsTr("Beats per minute (BPM) for the count-in clicks.")
                helpScope: qsTr("Range: 20 to 300. Only the count-in uses it.")
                unavailableReason: qsTr("Stop the current recording first.")
                value: root.service ? root.service.recordTempo : 120
                onEdited: value => root.service.setRecordingLeadIn(root.service.recordPreRoll, root.service.recordCountIn, value)
            }
        }
        RowLayout {
            enabled: !root.recording
            ToolButton {
                objectName: "recordAlign"; text: qsTr("Align overdubs to device timing"); showLabel: true
                helpText: qsTr("Shift new recordings earlier by the delay the audio device reports, so overdubs line up with what you heard.")
                helpScope: qsTr("Applies to takes recorded from now on. Add a correction if a take still lands late or early.")
                helpTopic: qsTr("Latency is the delay between sound reaching the input and the audio arriving here. To measure what remains, record a click played through the output and compare it with the original.")
                unavailableReason: qsTr("Stop the current recording first.")
                iconName: "timer"; checkable: true; checked: root.service && root.service.recordAlign
                onClicked: root.service.setRecordingAlignment(!root.service.recordAlign, root.service.recordOffsetMs)
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Correction ms"); color: Theme.textSecondary }
            ValueField {
                objectName: "recordOffset"; Layout.fillWidth: true; from: -2000; to: 2000; decimals: 2; step: 0.1
                helpTitle: qsTr("Alignment correction")
                helpText: qsTr("Extra milliseconds to move recorded audio. A positive value moves the input earlier.")
                helpScope: qsTr("Range: −2000 to +2000 ms, added to the device-reported alignment when that is on.")
                unavailableReason: qsTr("Stop the current recording first.")
                value: root.service ? root.service.recordOffsetMs : 0
                onEdited: value => root.service.setRecordingAlignment(root.service.recordAlign, value)
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "recordAlignmentStatus"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: root.service ? qsTr("Measured alignment: %1 ms (%2 samples). Positive correction moves input earlier.").arg((root.service.recordAlignmentFrames * 1000 / root.service.sampleRate).toFixed(2)).arg(root.service.recordAlignmentFrames) : ""
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            text: qsTr("Use a track's R button to record over it; otherwise a new track is added. Punch-in replaces only the selected range. Extra inputs get separate tracks. Monitoring plays the main input without effects. Undo removes the completed recording.")
            color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        }
        Rectangle {
            objectName: "recordInputMeter"
            Layout.fillWidth: true; implicitHeight: 8; color: Theme.meterBg
            Rectangle {
                height: parent.height
                width: parent.width * Math.max(0, Math.min(1, root.service && root.service.inputPeak > 0 ? (20*Math.log(root.service.inputPeak)/Math.LN10 + 60)/60 : 0))
                color: root.service && root.service.inputPeak >= 1 ? Theme.record : Theme.meterLow
            }
        }
        Text {
            objectName: "recordPanelStatus"
            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            text: root.service && root.service.lastError !== "" ? root.service.lastError
                : root.recording ? root.service.recordingStatus + " · " + (root.service.recordingFrames/root.service.sampleRate).toFixed(1) + " s"
                : qsTr("Ready · %1 kHz · monitoring %2").arg(root.service ? root.service.sampleRate/1000 : 48).arg(root.service && root.service.recordMonitor ? qsTr("on") : qsTr("off"))
            color: root.service && root.service.lastError !== "" ? Theme.danger : root.recording ? Theme.record : Theme.textSecondary
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        }
        ToolButton {
            objectName: "recordStartStop"; Layout.fillWidth: true
            iconName: root.recording ? "square" : "circle"; showLabel: true
            text: root.recording ? qsTr("Stop recording") : qsTr("Start recording")
            helpText: qsTr("Start recording from the playhead, or stop the take in progress.")
            helpScope: qsTr("A new take lands on the armed track, or on a new track. Stopping keeps what was recorded.")
            unavailableReason: root.service === null ? qsTr("The audio engine is not available.")
                                                     : qsTr("Wait for the file to finish loading.")
            shortcut: root.shell ? root.shell.bindingFor("record") : ""
            enabled: root.service !== null && (root.recording || !root.service.loading)
            onClicked: root.shell.runAction(root.recording ? "stop" : "start_recording")
        }
        Text {
            visible: root.service !== null && root.service.recoverableRecordings.length > 0
            text: qsTr("Recover unfinished takes")
            color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        }
        ScrollPane {
            id: recoveryScroll
            // Inside the panel's own scrolling column: its bar stays inside the column.
            Layout.rightMargin: 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(120, recoveryRows.implicitHeight)
            visible: root.service !== null && root.service.recoverableRecordings.length > 0
            clip: true
            Column {
                id: recoveryRows
                x: recoveryScroll.reachLeft; width: recoveryScroll.columnWidth
                Repeater {
                    model: root.service ? root.service.recoverableRecordings : []
                    RowLayout {
                        id: recovery
                        required property var modelData
                        width: recoveryRows.width
                        Text {
                            Layout.fillWidth: true; text: recovery.modelData.name; textFormat: Text.PlainText
                            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                        }
                        ToolButton {
                            text: qsTr("Recover"); showLabel: true
                            helpText: qsTr("Bring this unfinished take into the project as a new track.")
                            helpScope: qsTr("Reads the raw recording left in the recovery folder by an earlier run.")
                            unavailableReason: root.recording ? qsTr("Stop the current recording first.")
                                                              : qsTr("Wait for the file to finish loading.")
                            enabled: !root.recording && root.service !== null && !root.service.loading
                            onClicked: root.service.recoverRecording(recovery.modelData.path)
                        }
                    }
                }
            }
        }
    }
    }
}
