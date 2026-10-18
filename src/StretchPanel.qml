pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "stretchPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var targetClip: ({})
    property var selectedTarget: ({})
    readonly property bool selectedScope: useSelection.checked && selectedTarget.count > 1
    readonly property var editTarget: selectedScope ? selectedTarget : targetClip
    property string message: ""
    property bool submitted: false
    property bool cancelled: false
    property bool submittedSelection: false
    property bool pendingLiveRefresh: false
    readonly property bool live: elasticMode.checked
    readonly property real basis: selectedScope ? (selectedTarget.length || 0)
        : live ? (targetClip.originalFrames || 0) : (targetClip.length || 0)
    readonly property real minimum: (selectedScope ? selectedTarget.minimum : live ? targetClip.liveMinimum : targetClip.minimum) || 0
    readonly property real maximum: (selectedScope ? selectedTarget.maximum : live ? targetClip.liveMaximum : targetClip.maximum) || 0
    readonly property bool targetValid: selectedScope && live ? !!selectedTarget.liveValid : !!editTarget.valid
    readonly property string targetReason: selectedScope ? (live ? selectedTarget.liveReason : selectedTarget.reason) || "" : ""
    readonly property real pitch: Numeric.parse(pitchInput.text)
    readonly property real duration: frameInput.value
    readonly property bool valid: targetValid && frameInput.valid
        && duration >= minimum && duration <= maximum
        && (!live || (pitchInput.acceptableInput && pitch >= (selectedScope ? -24 : -12) && pitch <= (selectedScope ? 24 : 12)))
    function resetDraft() {
        pitchInput.text = selectedScope ? "0" : String(targetClip.semitones || 0)
        frameInput.show(editTarget.length || 0)
        percentInput.text = basis ? (duration * 100 / basis).toFixed(3) : "100"
    }
    function reload(resetScope, resetMode) {
        targetClip = service ? service.stretchTarget() : {}
        selectedTarget = service ? service.selectedStretchTarget() : {}
        if (resetScope !== false) useSelection.checked = selectedTarget.count > 1
        if (!(selectedTarget.count > 1)) useSelection.checked = false
        if (resetMode !== false) elasticMode.checked = selectedScope ? !!selectedTarget.anyElastic : !!targetClip.elastic
        resetDraft()
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 32 : 560)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: {
        reload(); message = ""; submitted = false; cancelled = false; pendingLiveRefresh = false
    }
    onClosed: if (shell) shell.restoreFocus()
    Connections {
        target: root.service
        function onSampleEditChanged() {
            if (root.submitted && !root.service.sampleEditing) {
                root.submitted = false
                root.message = root.service.lastError || (root.cancelled ? qsTr("Stretch cancelled.")
                    : root.submittedSelection ? qsTr("Stretch processing ended. Undo restores any applied change.")
                    : qsTr("Stretch finished. Undo restores the original clip."))
                if (!root.service.lastError) root.reload(false, false)
            }
        }
        function onTimelineChanged() {
            if (root.pendingLiveRefresh) {
                root.pendingLiveRefresh = false
                Qt.callLater(function() { root.reload(false, false) })
            }
        }
        function onLastErrorChanged() {
            if ((root.pendingLiveRefresh || root.submitted) && root.service.lastError) {
                root.pendingLiveRefresh = false
                if (!root.service.sampleEditing) root.submitted = false
                root.message = root.service.lastError
            }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Pitch and duration"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                iconName: "x"; text: qsTr("Close")
                helpText: qsTr("Close this panel.")
                helpScope: qsTr("Length changes already being processed continue after you close this panel. You can undo the result.")
                onClicked: root.close()
            }
        }
        Toggle {
            id: useSelection; objectName: "stretchSelectedScope"; Layout.fillWidth: true
            visible: root.selectedTarget.count > 1; enabled: !root.submitted
            label: qsTr("Apply to all %1 selected clips").arg(root.selectedTarget.count || 0)
            helpText: qsTr("Change the duration and spacing of the selected whole clips together, including linked group members.")
            helpScope: qsTr("Timing scales from the earliest selected start. One Undo step. Turn off to edit only the reference clip below.")
            onClicked: { root.resetDraft(); root.message = ""; root.pendingLiveRefresh = false }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "stretchTargetName"
            Layout.fillWidth: true; text: root.selectedScope ? qsTr("%1 selected clips; duration includes the spaces between them.").arg(root.selectedTarget.count)
                : root.targetClip.name || qsTr("Place the playhead or selection start inside a clip.")
            elide: Text.ElideRight; textFormat: Text.PlainText; color: Theme.textPrimary
        }
        Toggle {
            id: elasticMode; objectName: "elasticMode"
            Layout.fillWidth: true
            label: qsTr("Realtime pitch and duration"); enabled: !root.submitted

            helpTitle: qsTr("Realtime pitch and duration")
            helpText: qsTr("On: change pitch and length during playback, and adjust them again later. Off: create a processed copy for a wider range of lengths; the pitch stays the same.")
            helpScope: qsTr("In live mode each clip must stay within half to twice its original length and one octave of its original pitch. With live mode off, each clip can be one quarter to four times its current length.")
            onToggled: percentInput.text = root.basis ? (root.duration * 100 / root.basis).toFixed(3) : "100"
        }
        RowLayout {
            visible: root.live
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.selectedScope ? qsTr("Pitch change (semitones)") : qsTr("Pitch (semitones)"); color: Theme.textSecondary }
            TextEntry {
                id: pitchInput; objectName: "elasticPitch"; Layout.preferredWidth: 110
                text: "0"; selectByMouse: true; enabled: !root.submitted

                helpTitle: qsTr("Pitch shift")
                helpText: root.selectedScope ? qsTr("Add this pitch change to each selected clip, preserving their differences. 0 keeps every clip's current pitch.")
                    : qsTr("How far to move the pitch, in semitones: 12 is an octave up, −12 an octave down, 0 leaves it.")
                helpScope: qsTr("Live stretching only. Every resulting pitch must be between −12 and +12 semitones. Applied with the Apply button.")
                validator: DoubleValidator { bottom: root.selectedScope ? -24 : -12; top: root.selectedScope ? 24 : 12; decimals: 3; notation: DoubleValidator.StandardNotation }
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.selectedScope ? qsTr("0 keeps the current pitches") : qsTr("−12 to +12; 0 keeps the pitch"); color: Theme.textMuted; Layout.fillWidth: true }
        }
        RowLayout {
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Duration %"); color: Theme.textSecondary }
            TextEntry {
                id: percentInput; objectName: "stretchPercent"; Layout.preferredWidth: 110; selectByMouse: true
                enabled: !root.submitted

                helpTitle: qsTr("New duration, percent")
                helpText: root.selectedScope ? qsTr("The selected clips' new overall span as a percentage of their current span, including spaces. 200 doubles clip lengths and spacing; 50 halves them.")
                    : root.live ? qsTr("The clip's new length as a percentage of its original audio: 200 is twice as long, 50 half. The length field follows it.")
                    : qsTr("The clip's new length as a percentage of its current one: 200 is twice as long, 50 half. The length field follows it.")
                helpScope: qsTr("Applied with the Apply button as one Undo step. Each clip must remain within its own duration limits.")
                validator: DoubleValidator { bottom: root.live && !root.selectedScope ? 50 : 25; top: root.live && !root.selectedScope ? 200 : 400; decimals: 3; notation: DoubleValidator.StandardNotation }
                onTextEdited: if (acceptableInput) frameInput.show(Math.round(root.basis * Numeric.parse(text) / 100))
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.selectedScope ? qsTr("New span") : qsTr("New length"); color: Theme.textSecondary }
            TimeField {
                duration: true
                id: frameInput; objectName: "stretchFrames"; Layout.fillWidth: true; selectByMouse: true
                service: root.service; minimum: 1
                enabled: !root.submitted

                helpTitle: root.selectedScope ? qsTr("New span") : qsTr("New length")
                helpText: root.selectedScope ? qsTr("The time from the earliest selected start to the latest selected end, including spaces. All selected boundaries scale together; the earliest start stays fixed.")
                    : qsTr("How long the clip should become. The percent field follows it. Applied with the Apply button as one Undo step.")
                onTextEdited: if (valid && root.basis) percentInput.text = (value * 100 / root.basis).toFixed(3)
                onEdited: samples => { if (root.basis) percentInput.text = (samples * 100 / root.basis).toFixed(3) }
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: root.targetValid
                ? root.selectedScope ? qsTr("Span range: %1 to %2. Every clip must also fit its duration limits and avoid unselected clips.").arg(root.shell.formatDuration(root.minimum)).arg(root.shell.formatDuration(root.maximum))
                    : qsTr("The new length can be from %1 to %2.").arg(root.shell.formatDuration(root.minimum)).arg(root.shell.formatDuration(root.maximum))
                : root.targetReason || qsTr("Choose a mono or stereo clip at least 50 ms long.")
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.selectedScope ? qsTr("Duration is relative to the current selection in both modes. Clip lengths, spacing and fades scale together; unselected clips, automation and markers stay in place. Original audio is retained. Undo reverses the whole change.")
                : root.live
                ? qsTr("Duration is relative to the original audio. Changes take effect during playback and can be reset. Fades scale; later clips, automation and markers keep their positions.")
                : qsTr("Pitch and start time stay the same. Fades follow the new length; later clips, automation and markers stay in place. Large changes can soften sharp sounds or introduce unwanted sound.")
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "stretchMessage"; Layout.fillWidth: true; visible: text !== ""; wrapMode: Text.WordWrap
            text: root.submitted ? qsTr("Stretching… You can close this panel and keep working.") : root.message
            textFormat: Text.PlainText; color: Theme.textPrimary
        }
        RowLayout {
            ToolButton {
                objectName: "reloadStretch"; text: qsTr("Reload"); showLabel: true; enabled: !root.submitted
                helpText: qsTr("Read the selected clips and reference clip again, discarding edits here.")
                onClicked: { root.pendingLiveRefresh = false; root.reload(); root.message = "" }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "resetElastic"; text: qsTr("Reset pitch / duration"); showLabel: true
                helpText: qsTr("Return this clip to its original pitch and duration, removing the realtime setting.")
                helpScope: qsTr("Acts on the clip named above. One Undo step; the source audio was never changed.")
                unavailableReason: !root.targetClip.elastic ? qsTr("This clip has no realtime pitch or duration setting to reset.")
                                   : qsTr("Wait for the current edit to finish.")
                visible: root.live && !root.selectedScope; enabled: !!root.targetClip.elastic && !root.submitted
                onClicked: {
                    root.service.setFocusedClipElastic(root.targetClip.originalFrames, 0, root.targetClip.stamp, root.targetClip.track, root.targetClip.clip)
                    root.message = root.service.lastError || qsTr("Original pitch and duration restored.")
                    if (!root.service.lastError) {
                        root.reload(false, false)
                    }
                }
            }
            ToolButton {
                objectName: "cancelStretch"; text: qsTr("Cancel processing"); showLabel: true
                helpText: qsTr("Stop the length change being processed.")
                helpScope: qsTr("A cancelled stretch writes nothing; the clip stays as it was.")
                unavailableReason: qsTr("No length change is being processed.")
                visible: !root.live; enabled: root.submitted; onClicked: { root.cancelled = true; root.service.cancelSampleEdit(); root.message = qsTr("Stretch cancelled.") }
            }
            ToolButton {
                objectName: "applyStretch"; text: root.live ? qsTr("Apply pitch / duration") : qsTr("Apply stretch"); iconName: "stretch-horizontal"; showLabel: true
                helpText: root.selectedScope ? qsTr("Scale all selected clips and the spaces between them to the new span. In live mode, add the pitch change to each clip; with live mode off, preserve each clip's audible pitch.")
                    : root.live ? qsTr("Set the clip's pitch, in semitones (twelve to an octave), and its duration as a realtime setting that is computed during playback and export.")
                                    : qsTr("Create a copy at the new length, keeping the same pitch, and use it in place of this clip.")
                helpScope: root.selectedScope ? qsTr("The earliest selected start stays fixed. Any overlap or duration limit refuses the whole edit. One Undo step; original media and group links are retained. Unselected clips, automation and markers keep their positions.")
                    : root.live ? qsTr("Acts on the clip named above. One Undo step; the source audio is never changed and Reset removes the setting. Later clips, automation and markers keep their positions.")
                                     : qsTr("Acts on the clip named above. The clip's start stays fixed and fades scale with it; later clips stay in place. One Undo step; the original audio stays in the project.")
                helpTopic: qsTr("Live mode allows 50–200% length and −12 to +12 semitones. With live mode off, length can be 25–400% and pitch stays the same. Leave room before the next clip. Large changes may sound less natural.")
                unavailableReason: !root.targetValid ? root.targetReason || qsTr("Place the playhead or the selection start inside a mono or stereo clip at least 50 ms long.")
                                   : root.submitted || (root.service && root.service.sampleEditing) ? qsTr("Wait for the current audio edit to finish.")
                                   : root.service && root.service.recording ? qsTr("Stop recording first.")
                                   : root.service && root.service.loading ? qsTr("Wait for the file to finish loading.")
                                   : !root.valid ? root.selectedScope ? qsTr("Enter a span within the available range, and a pitch change from −24 to +24 semitones. Each resulting clip pitch must stay within −12 to +12.")
                                       : qsTr("Enter a duration within the available range, and a pitch from −12 to +12 semitones.")
                                   : qsTr("Change the duration or the pitch first.")
                enabled: root.valid && !root.submitted && root.service && !root.service.recording && !root.service.loading && !root.service.sampleEditing
                    && (root.duration !== root.editTarget.length || (root.live && root.pitch !== (root.selectedScope ? 0 : root.targetClip.semitones)))
                onClicked: {
                    if (root.selectedScope) {
                        root.cancelled = false; root.submittedSelection = true; root.pendingLiveRefresh = root.live
                        root.submitted = !root.live
                        const accepted = root.service.stretchSelectedClips(root.duration, root.live, root.live ? root.pitch : 0, root.selectedTarget.stamp)
                        if (!accepted) { root.submitted = false; root.pendingLiveRefresh = false; root.message = root.service.lastError; return }
                        root.message = root.service.lastError || qsTr("Edit accepted. Undo restores the clips' previous timing and pitch.")
                        return
                    }
                    if (root.live) {
                        root.service.setFocusedClipElastic(root.duration, root.pitch, root.targetClip.stamp, root.targetClip.track, root.targetClip.clip)
                        root.message = root.service.lastError || qsTr("Pitch and duration updated. Undo restores the previous settings.")
                        if (!root.service.lastError) root.reload(false, false)
                        return
                    }
                    root.cancelled = false; root.submittedSelection = false
                    root.service.stretchFocusedClip(root.duration, root.targetClip.stamp, root.targetClip.track, root.targetClip.clip)
                    root.submitted = root.service.sampleEditing
                    root.message = root.service.lastError
                }
            }
        }
    }
}
