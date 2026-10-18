pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// An effect picked from the Effects menu on the Waveform page with a selection:
// heard on the selection, adjusted here, then applied to the selected audio as
// one Undo step. The effect sits first in the track's chain while this is open
// (Backend.beginEffectPreview); Apply and Cancel both take it away again and
// leave no trace of the preview in the history.
//
// The selection starts playing through the effect, over and over, as soon as the
// panel opens (when there is an audio output to hear it on), so every change is
// heard as it is made. Before/After switches the effect off and on without
// stopping. Dynamics effects show their controls as knobs; the rest as rows.
//
// Modal, because the preview is a temporary state of the project: nothing else
// should be edited while it is in place. The effect's own window still works.
C.Popup {
    id: root
    objectName: "effectApplyPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var preview: service ? service.effectPreview : ({})
    readonly property bool active: preview.active === true
    readonly property int slot: active ? preview.slot : -1
    readonly property int track: active ? preview.track : -1
    // Set once Apply is accepted: the panel then waits for the edit to land.
    property bool submitted: false
    // Playback this panel started, so closing stops only what it began.
    property bool previewing: false
    property bool loopBefore: false
    // Start playing once the effect is in place, the first time only.
    property bool autoStartPending: false

    readonly property var trackEntry: service && root.track >= 0 && service.tracksRevision >= 0
                                      ? service.trackById(root.track) : null
    readonly property var insert: root.trackEntry && root.trackEntry.inserts && root.slot >= 0
                                  && root.slot < root.trackEntry.inserts.length ? root.trackEntry.inserts[root.slot] : null
    readonly property bool bypassed: root.insert !== null && root.insert.bypass === true
    readonly property bool portsOwn: root.service !== null && root.slot >= 0
        && root.service.pluginPortsSlot === root.slot && root.service.pluginPortsTarget === root.track
    readonly property var ports: root.portsOwn ? (root.service.pluginPorts || []) : []
    readonly property var outputs: root.portsOwn ? (root.service.pluginOutputs || []) : []
    readonly property string kind: root.portsOwn ? root.service.pluginPortsKind : "other"
    readonly property bool knobs: root.kind === "dynamics" && root.ports.length > 0 && root.ports.length <= 16
    readonly property real rate: service && service.sampleRate > 0 ? service.sampleRate : 48000
    readonly property real length: Math.max(1, (root.preview.end || 0) - (root.preview.start || 0))
    // Where playback is within the selection, 0..1, or -1 outside it.
    readonly property real playFraction: {
        if (!service || !active) return -1
        const at = service.playhead - root.preview.start
        return at >= 0 && at <= root.length ? at / root.length : -1
    }

    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(740, parent ? parent.width - 32 : 740)
    // As tall as the effect's controls need, up to most of the window; a long
    // list scrolls inside.
    height: Math.min(parent ? parent.height - 64 : 720, implicitHeight)
    padding: Theme.s4
    modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape

    onOpened: {
        root.submitted = false
        root.previewing = false
        root.loopBefore = root.service ? root.service.loopEnabled : false
        root.autoStartPending = true
        if (root.service && root.slot >= 0) root.service.requestPluginPorts(root.slot, root.track)
        root.maybeAutoStart()
        body.forceActiveFocus()
    }
    // Escape, the close button and Cancel all land here.
    onClosed: {
        root.autoStartPending = false
        root.stopPreview()
        if (root.service && root.active && !root.preview.applying && !root.submitted)
            root.service.cancelEffectPreview()
        if (root.shell) root.shell.restoreFocus()
    }
    // The slot can only be read once the effect is in the chain.
    onSlotChanged: {
        if (root.opened && root.service && root.slot >= 0) root.service.requestPluginPorts(root.slot, root.track)
        root.maybeAutoStart()
    }

    function maybeAutoStart() {
        if (!root.autoStartPending || !root.opened || !root.service || !root.active || root.slot < 0) return
        root.autoStartPending = false
        // Nothing to hear without an audio output: leave the transport alone.
        if (!root.service.audioOutputRunning()) return
        root.startPreview()
    }
    function startPreview() {
        if (!root.service || !root.active || root.submitted) return
        if (!root.previewing) root.loopBefore = root.service.loopEnabled
        if (loopButton.checked && !root.service.loopEnabled) root.service.setLoopEnabled(true)
        if (root.playFraction < 0 || root.playFraction >= 1) root.service.seekFrames(root.preview.start)
        root.service.play()
        root.previewing = true
    }
    function togglePreview() {
        if (!root.service || !root.active) return
        if (root.service.playing) { root.service.stop(); return }
        root.startPreview()
    }
    function stopPreview() {
        if (!root.previewing || !root.service) return
        root.previewing = false
        if (root.service.playing) root.service.stop()
        if (root.service.loopEnabled !== root.loopBefore) root.service.setLoopEnabled(root.loopBefore)
    }
    // What the backend said about the edit, such as that it changed nothing.
    property string outcomeNotice: ""
    function apply() {
        if (!root.service || !root.active || root.submitted) return
        root.stopPreview()
        root.outcomeNotice = ""
        root.submitted = true
        if (!root.service.applyEffectPreview()) {
            root.submitted = false
            if (root.shell) root.shell.say(root.service.lastError)
        }
    }
    function resetAll() {
        if (root.knobs) knobPanel.resetAll()
        else controls.resetAll()
    }
    function clock(frames) {
        const s = Math.max(0, frames) / root.rate
        const m = Math.floor(s / 60)
        const rest = s - m * 60
        return m + ":" + (rest < 10 ? "0" : "") + rest.toFixed(2)
    }

    // Applying is done when the preview has gone and the edit it started has
    // finished (or was refused); the outcome is said in the status line.
    Connections {
        target: root.service
        function onEffectPreviewChanged() { root.finishIfDone(); root.maybeAutoStart() }
        function onSampleEditChanged() { root.finishIfDone() }
        function onNotice(text) { if (root.submitted) root.outcomeNotice = text }
    }
    function finishIfDone() {
        if (!root.submitted || root.active || root.service.sampleEditing) return
        root.submitted = false
        root.close()
        if (root.shell)
            root.shell.say(root.service.lastError !== "" ? root.service.lastError
                           : root.outcomeNotice !== "" ? root.outcomeNotice
                           : qsTr("Effect applied to the selection. Undo restores the previous audio."))
        root.outcomeNotice = ""
    }

    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        id: body
        spacing: Theme.s3
        focus: true
        // Space plays or pauses the selection through the effect.
        Keys.onSpacePressed: (event) => { root.togglePreview(); event.accepted = true }
        Keys.onReturnPressed: (event) => { root.apply(); event.accepted = true }
        Keys.onEnterPressed: (event) => { root.apply(); event.accepted = true }

        RowLayout {
            Text {
                objectName: "effectApplyTitle"
                Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText
                text: qsTr("Apply %1").arg(Theme.shortName(root.preview.name, 48) || qsTr("effect"))
                color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading
            }
            ToolButton {
                iconName: "x"; text: qsTr("Cancel")
                helpText: qsTr("Close without applying. The effect is taken off and the audio stays as it was.")
                onClicked: root.close()
            }
        }
        Text {
            objectName: "effectApplySelection"
            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            text: root.trackEntry && root.service && root.service.sampleRate > 0
                  ? qsTr("%1 · %2 seconds selected").arg(Theme.shortName(root.trackEntry.name, 48))
                        .arg(Number((root.preview.end - root.preview.start) / root.service.sampleRate).toFixed(2))
                  : ""
        }

        // ── the preview: heard as you change it ───────────────────────────
        Rectangle {
            objectName: "effectApplyPreview"
            Layout.fillWidth: true
            Layout.preferredHeight: previewColumn.implicitHeight + Theme.s3 * 2
            color: Theme.panelRaised; radius: Theme.rMenu
            border.width: Theme.hairline; border.color: Theme.border
            ColumnLayout {
                id: previewColumn
                anchors.fill: parent; anchors.margins: Theme.s3
                spacing: Theme.s2
                RowLayout {
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "effectApplyPlay"
                        iconName: root.service && root.service.playing ? "pause" : "play"
                        text: root.service && root.service.playing ? qsTr("Pause") : qsTr("Play selection")
                        showLabel: true
                        helpText: qsTr("Play the selection through the effect, so you can adjust it while you listen. It starts by itself when this opens.")
                        helpScope: qsTr("Space does the same while this panel is open.")
                        unavailableReason: qsTr("Wait for the effect to be applied.")
                        enabled: root.active && !root.submitted
                        onClicked: root.togglePreview()
                    }
                    ToolButton {
                        id: loopButton
                        objectName: "effectApplyLoop"
                        iconName: "repeat"; text: qsTr("Loop"); showLabel: true
                        checkable: true; checked: true
                        helpText: qsTr("Play the selection over and over. Turn it off to hear it once and carry on.")
                        enabled: root.active && !root.submitted
                        onToggled: if (root.service && root.previewing) root.service.setLoopEnabled(checked)
                    }
                    SegmentedControl {
                        objectName: "effectApplyBypass"
                        labels: [qsTr("Before"), qsTr("After")]
                        currentIndex: root.bypassed ? 0 : 1
                        helpTitle: qsTr("Before and after")
                        helpText: qsTr("Before is the selection without the effect, After is with it. Switches straight away, without stopping.")
                        helpScope: qsTr("Apply still applies the effect, whichever you are hearing.")
                        enabled: root.active && !root.submitted
                        onActivated: function (index) {
                            if (root.service && root.slot >= 0) root.service.setPluginBypass(root.slot, index === 0, root.track)
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Row {
                        spacing: Theme.s1
                        visible: root.service !== null && root.service.playing && root.previewing
                        Rectangle { width: 6; height: 6; radius: 3; color: Theme.success; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            objectName: "effectApplyLive"
                            text: root.bypassed ? qsTr("Before · the effect is off") : qsTr("Live · changes are heard as you move them")
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                        }
                    }
                }
                RowLayout {
                    spacing: Theme.s3
                    // The selection, with where playback is.
                    Rectangle {
                        id: wave
                        objectName: "effectApplyWave"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 52
                        color: Theme.windowBg; radius: Theme.rControl
                        border.width: Theme.hairline; border.color: Theme.border
                        readonly property var peaks: root.opened && root.service && root.active && width > 4
                            ? root.service.selectionPeaks(root.track, root.preview.start, root.preview.end, Math.max(16, Math.floor(width / 3))) : []
                        Canvas {
                            id: peaksCanvas
                            anchors.fill: parent; anchors.margins: 3
                            readonly property var peaks: wave.peaks
                            readonly property color colour: root.bypassed ? Theme.textMuted : Theme.audioPrimary
                            onPeaksChanged: requestPaint()
                            onColourChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                const n = peaks.length
                                if (!n) return
                                const mid = height / 2, w = width / n
                                ctx.fillStyle = colour
                                for (let i = 0; i < n; ++i) {
                                    const h = Math.max(1, peaks[i] * (height - 2))
                                    ctx.fillRect(i * w, mid - h / 2, Math.max(1, w - 1), h)
                                }
                            }
                        }
                        Rectangle {
                            objectName: "effectApplyPlayhead"
                            visible: root.playFraction >= 0
                            x: 3 + (wave.width - 6) * Math.max(0, root.playFraction)
                            y: 2; width: 2; height: wave.height - 4
                            color: Theme.playhead
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: function (e) {
                                if (!root.service || !root.active) return
                                root.service.seekFrames(Math.round(root.preview.start + root.length * Math.max(0, Math.min(1, (e.x - 3) / (wave.width - 6)))))
                            }
                            ControlHelp {
                                title: qsTr("The selection")
                                purpose: qsTr("The selected audio, before the effect, and where playback is in it. Click to play from there.")
                            }
                        }
                    }
                    Column {
                        Layout.preferredWidth: 96
                        Text {
                            objectName: "effectApplyTime"
                            anchors.right: parent.right
                            text: root.clock(root.playFraction >= 0 ? root.service.playhead - root.preview.start : 0)
                            font.family: Theme.monoFamily; font.pixelSize: Theme.fsTimecodeSmall; font.features: { "tnum": 1 }
                            color: Theme.textPrimary
                        }
                        Text {
                            anchors.right: parent.right
                            text: qsTr("of %1").arg(root.clock(root.length))
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.features: { "tnum": 1 }
                            color: Theme.textMuted
                        }
                    }
                }
            }
        }

        // ── the effect's window, its defaults and presets ─────────────────
        RowLayout {
            spacing: Theme.s2
            ToolButton {
                objectName: "effectApplyWindow"
                iconName: "maximize-2"; text: qsTr("Effect window"); showLabel: true
                readonly property bool available: root.active && root.slot >= 0 && !root.bypassed
                    && root.service.pluginHasEditor(root.slot, root.track)
                helpText: qsTr("Open the effect's own window, as its maker designed it. What you set there is applied too.")
                unavailableReason: root.bypassed ? qsTr("Switch to After to open the effect's window.")
                                   : qsTr("This effect has no window of its own; use its controls below.")
                enabled: available && !root.submitted
                onClicked: root.service.openPluginEditor(root.slot, root.track)
            }
            ToolButton {
                objectName: "effectApplyReset"
                iconName: "rotate-ccw"; text: qsTr("Reset all"); showLabel: true
                helpText: qsTr("Put every control back to the effect's default.")
                enabled: root.active && !root.submitted && root.ports.length > 0
                onClicked: root.resetAll()
            }
            Item { Layout.fillWidth: true }
            Text {
                text: qsTr("Preset")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }
            PresetChoice {
                id: presetChoice
                objectName: "effectApplyPreset"
                Layout.preferredWidth: 200
                context: root.opened && root.service && root.active && root.slot >= 0 && root.portsOwn
                    ? root.service.pluginPresetContext(root.track, root.slot) : ({})
                enabled: root.active && !root.submitted && context.available === true
                onDefaultChosen: root.resetAll()
                onPresetChosen: function (id) {
                    if (!root.service.applyPluginPreset(root.track, root.slot, context.stamp, id) && root.shell)
                        root.shell.say(root.service.lastError)
                }
            }
            ToolButton {
                objectName: "effectApplySavePreset"
                text: qsTr("Save…"); showLabel: true
                helpText: qsTr("Save these settings as a preset for this effect, to use again.")
                enabled: root.active && !root.submitted && presetChoice.context.available === true
                onClicked: if (root.shell) root.shell.openPluginPresets(root.track, root.slot)
            }
        }

        Text {
            visible: root.active && root.service !== null && !root.service.pluginPortsComplete
            color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Reading controls…")
        }
        PluginControlList {
            id: controls
            objectName: "effectApplyControls"
            visible: !root.knobs
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: visible ? Theme.hControl * 3 : 0
            Layout.preferredHeight: visible ? Math.min(contentHeight, 440) : 0
            rowName: "effectApplyPort"
            enabled: !root.submitted
            service: root.service
            selected: root.slot
            target: root.track
            ports: root.knobs ? [] : root.ports
            outputs: []  // the meters have their own box below
            bypassed: root.bypassed
            kind: root.kind
            uri: root.portsOwn ? root.service.pluginPortsUri : ""
        }
        PluginKnobs {
            id: knobPanel
            objectName: "effectApplyKnobs"
            visible: root.knobs
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: visible ? 160 : 0
            Layout.preferredHeight: visible ? Math.min(contentHeight, 460) : 0
            enabled: !root.submitted
            service: root.service
            selected: root.slot
            target: root.track
            ports: root.knobs ? root.ports : []
            outputs: []
            bypassed: root.bypassed
        }
        // The meters stay in view under the controls, however long they are.
        PluginMeterBox {
            objectName: "effectApplyMeterBox"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            service: root.service
            selected: root.slot
            target: root.track
            outputs: root.outputs
            bypassed: root.bypassed
            labelWidth: root.knobs ? 110 : controls.labelWidth
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "effectApplyStatus"
            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            visible: root.submitted; color: Theme.textPrimary
            text: qsTr("Applying %1…").arg(Theme.shortName(root.preview.name, 48) || qsTr("the effect"))
        }
        RowLayout {
            Text {
                Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText
                visible: !root.submitted
                text: qsTr("Written into the selected audio as one Undo step.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }
            Item { Layout.fillWidth: true; visible: root.submitted }
            ToolButton {
                objectName: "effectApplyStop"; text: qsTr("Cancel processing"); showLabel: true
                visible: root.submitted && root.service !== null && root.service.sampleEditing
                helpText: qsTr("Stop applying the effect. Nothing is written; the audio stays as it was.")
                onClicked: root.service.cancelSampleEdit()
            }
            ToolButton {
                objectName: "effectApplyCancel"; text: qsTr("Cancel"); showLabel: true
                visible: !root.submitted
                helpText: qsTr("Close without applying. The effect is taken off and the audio stays as it was.")
                onClicked: root.close()
            }
            ToolButton {
                objectName: "effectApplyApply"; text: qsTr("Apply"); showLabel: true; prominent: true
                visible: !root.submitted
                helpText: qsTr("Write the effect into the selected audio, with the settings it has now.")
                helpScope: qsTr("Runs in the background. Audio outside the selection is untouched; the effect's tail is not added past the selection's end. One Undo step.")
                unavailableReason: root.service && root.service.sampleEditing ? qsTr("Finish the current audio edit first.")
                                   : qsTr("The effect is still being put on the track.")
                enabled: root.active && root.slot >= 0 && root.service !== null && !root.service.sampleEditing
                onClicked: root.apply()
            }
        }
    }
}
