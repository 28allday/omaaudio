import QtQuick
import QtQuick.Layouts
import "ui"

// Bottom strip: sample rate and channels, engine state, the latest message,
// what an edit would land on, what undo would reverse, the zoom. Nothing here
// guesses -- a fact the engine cannot give (buffer size, CPU, autosave) is
// shown as absent rather than invented.
Rectangle {
    id: bar
    objectName: "statusBar"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var focusedInfo: (service && service.tracksRevision >= 0 && service.focusedTrack >= 0)
                                      ? service.trackById(service.focusedTrack) : null
    property var axis: null
    // Audio load, polled four times a second; warns once (at most every 30 s) when it
    // stays high or PipeWire reports a dropout.
    property var load: ({})
    property int seenDropouts: 0
    property bool newDropouts: false
    property int highPolls: 0
    property double lastWarning: 0
    Timer {
        interval: 250; repeat: true; running: !!bar.service
        onTriggered: {
            const now = bar.service.audioLoad()
            bar.newDropouts = (now.dropouts || 0) > bar.seenDropouts
            bar.highPolls = now.active && now.percent > 85 ? bar.highPolls + 1 : 0
            const stamp = Date.now()
            if ((bar.newDropouts || bar.highPolls >= 8) && stamp - bar.lastWarning > 30000 && bar.shell) {
                bar.lastWarning = stamp
                bar.shell.say(bar.newDropouts
                    ? qsTr("Playback dropped out. Click Audio load for what is using the time, or choose a larger buffer in Audio Setup.")
                    : qsTr("Playback is using most of each audio block. Click Audio load to see why; a larger buffer or fewer heavy effects helps."))
            }
            bar.seenDropouts = now.dropouts || 0
            bar.load = now
        }
    }
    property string workspace: ""

    height: Theme.hStatusBar
    color: Theme.panelBg

    Rectangle {
        anchors.top: parent.top
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        spacing: Theme.s3

        Text {
            objectName: "statusAutomation"
            visible: bar.service && bar.service.automationMode !== "read"
            ControlHelp {
                title: qsTr("Automation mode")
                purpose: qsTr("Shown while automation is set to Write, Touch or Latch, when moving a control records its changes over time.")
                scope: qsTr("Red means a pass is being written now. Read mode, the default, plays curves back and shows nothing here.")
            }
            text: bar.service ? qsTr("Auto: %1").arg(bar.service.automationMode) : ""
            color: bar.service && bar.service.automationWriting ? Theme.danger : Theme.textSecondary
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        }

        // Engine health, first: a dead engine is the one fact everything else
        // depends on.
        Row {
            spacing: Theme.s2
            // A positioner must not position the adapter: zero size, hover on the row.
            ControlHelp {
                id: engineHelp
                anchors.fill: undefined
                width: 0; height: 0
                title: qsTr("Engine state")
                purpose: qsTr("Shows what OmaAudio is doing, such as opening a file, saving, exporting or repairing audio. A percentage appears when progress can be measured.")
                scope: qsTr("Green is ready, amber is a background job or a lost connection, red means the engine could not be found or a recording is running. Editing waits for a running job.")
            }
            HoverHandler { onHoveredChanged: hovered ? Help.request(engineHelp) : Help.leave(engineHelp) }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 8; height: 8; radius: 4
                color: !bar.service ? Theme.textMuted
                     : bar.service.recording ? Theme.record
                     : bar.service.engineMissing ? Theme.danger
                     : !bar.service.connected ? Theme.warning
                     : bar.service.loading || bar.service.saving || bar.service.rendering || bar.service.analysing
                       || bar.service.storageBusy || bar.service.noisePrintBuilding || bar.service.sampleEditing || bar.service.spectralBuilding || bar.service.repairing || bar.service.detecting ? Theme.warning
                     : Theme.success
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: !bar.service ? qsTr("Starting")
                    : bar.service.recording ? bar.service.recordingStatus
                    : bar.service.unsupportedPlaybackLatency ? qsTr("Effect delay exceeds playback limits")
                    : bar.service.elasticFailed ? qsTr("Pitch/stretch preparation failed")
                    : bar.service.elasticPreparing ? qsTr("Preparing pitch/stretch")
                    : bar.service.primingPlayback ? qsTr("Priming effects")
                    : bar.service.engineMissing ? qsTr("Engine not found")
                    : !bar.service.connected ? qsTr("Engine offline")
                    : bar.service.loading ? qsTr("Reading file")
                    : bar.service.saveQueued ? qsTr("Save queued")
                    : bar.service.saving ? qsTr("Saving")
                    : bar.service.storageBusy ? qsTr("Managing source storage")
                    : bar.service.noisePrintBuilding ? qsTr("Measuring noise sample")
                    : bar.service.sampleEditing ? (bar.service.sampleEditQueued ? qsTr("Queued: %1").arg(bar.service.sampleEditLabel) : bar.service.sampleEditLabel)
                    : bar.service.rendering ? qsTr("Exporting %1%").arg(Math.round(bar.service.renderProgress * 100))
                    : bar.service.analysing ? qsTr("Analysing the mix")
                    : bar.service.repairing ? qsTr("Repairing the selection")
                    : bar.service.detecting ? qsTr("Looking for clicks")
                    : bar.service.spectralBuilding ? qsTr("Building spectral display %1%").arg(Math.round(bar.service.spectralProgress * 100))
                    : qsTr("Ready")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
            }
        }

        Rectangle { Layout.preferredWidth: Theme.hairline; Layout.preferredHeight: 14; color: Theme.border }

        Text {
            objectName: "statusFacts"
            visible: bar.service !== null && bar.service.loaded
            ControlHelp {
                title: qsTr("Project format")
                purpose: qsTr("The project's sample rate (samples per second) and the active track's channel count.")
                scope: qsTr("Says so when the take was converted from another rate on import; the original file is unchanged.")
                topic: qsTr("\"device N kHz\" appears when the audio system's clock runs at a different rate from the project; PipeWire converts the output, so the project itself is unchanged. Change the project rate in Project Properties, or the system rate in the desktop's audio settings, to make them match.")
            }
            text: {
                if (!bar.service || !bar.service.loaded)
                    return ""
                var channels = bar.focusedInfo && bar.focusedInfo.channels ? bar.focusedInfo.channels : bar.service.takeChannels
                var bits = [(bar.service.sampleRate / 1000) + " kHz",
                            channels === 1 ? qsTr("mono") : channels === 2 ? qsTr("stereo") : qsTr("%1 channels").arg(channels)]
                // Say so when the take was resampled on import. Quietly changing
                // someone's sample rate without telling them is the kind of thing
                // an editor should not do.
                if (bar.service.resampled)
                    bits.push(qsTr("resampled from %1").arg(bar.service.originalSampleRate))
                // The PipeWire graph runs at its own clock; when that differs from the
                // project, PipeWire converts on the way out, and it is worth knowing.
                if (bar.service.graphSampleRate > 0 && bar.service.graphSampleRate !== bar.service.sampleRate)
                    bits.push(qsTr("device %1 kHz").arg(bar.service.graphSampleRate / 1000))
                return bits.join("  ·  ")
            }
            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: Theme.textSecondary
        }

        Text {
            objectName: "playbackLatencyStatus"
            visible: bar.service && bar.service.playbackLatencyFrames > 0
            ControlHelp {
                title: qsTr("Effect delay")
                purpose: qsTr("The extra delay the effects in use add to playback, in milliseconds.")
                scope: qsTr("Tracks are aligned to compensate for it, so it does not shift audio in the mix or in exports.")
            }
            text: bar.service ? qsTr("FX delay: %1 ms").arg((1000 * bar.service.playbackLatencyFrames / bar.service.sampleRate).toFixed(1)) : ""
            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        }
        ToolButton {
            objectName: "streamCacheStatus"
            visible: bar.service && (bar.service.streamCacheMisses > 0 || bar.service.streamReadErrors > 0)
            helpTitle: qsTr("Disk streaming")
            helpText: qsTr("Audio streamed from disk was not ready in time this many times, and this many reads failed.")
            helpScope: qsTr("Occasional misses can cause a dropout during playback; read errors mean a source file could not be read. Opens the Storage panel.")
            text: bar.service ? qsTr("Disk misses: %1 · errors: %2").arg(bar.service.streamCacheMisses).arg(bar.service.streamReadErrors) : ""
            showLabel: true
            onClicked: bar.shell.runAction("show_storage")
        }
        ToolButton {
            objectName: "fileBatchStatus"
            readonly property int pausedFiles: bar.service ? bar.service.fileBatchJobs.filter(j => j.state === "paused").length : 0
            readonly property bool queueUnavailable: !!bar.service && bar.service.fileBatchStatus.available === false
            visible: bar.service && (bar.service.fileBatchJobs.length > 0 || queueUnavailable)
            text: queueUnavailable ? qsTr("Batch queue unavailable")
                : bar.service && bar.service.fileBatchBusy ? qsTr("Processing files…")
                : pausedFiles > 0 ? qsTr("Batch paused · %1").arg(pausedFiles) : qsTr("Batch results")
            showLabel: true
            helpText: qsTr("Open the file batch queue to review saved jobs, resume paused files, see results or cancel unfinished files.")
            onClicked: bar.shell.runAction("show_file_batch")
        }
        ToolButton {
            objectName: "stopRecordingStatus"
            visible: bar.service !== null && bar.service.recording
            helpText: qsTr("Stop the recording in progress and keep the take.")
            text: qsTr("Stop recording"); iconName: "square"; showLabel: true
            onClicked: bar.shell.runAction("stop")
        }

        ToolButton {
            objectName: "cancelLoad"
            visible: bar.service !== null && bar.service.loading
            text: qsTr("Cancel"); iconName: "x"; showLabel: true
            helpText: qsTr("Stop reading or decoding the file. Nothing is added to the project.")
            helpScope: qsTr("A file already in the project is unaffected.")
            onClicked: bar.service.cancelLoad()
        }

        ToolButton {
            objectName: "cancelSampleEdit"
            visible: bar.service !== null && bar.service.sampleEditing
            helpTitle: qsTr("Cancel audio edit")
            helpText: qsTr("Stop the audio edit that is running and leave the project as it was before it started.")
            text: qsTr("Cancel"); iconName: "x"; showLabel: true
            shortcut: bar.shell ? bar.shell.bindingFor("cancel_sample_edit") : ""
            onClicked: bar.shell.runAction("cancel_sample_edit")
        }

        // Where the latest render landed: one click opens its folder.
        ToolButton {
            objectName: "statusShowInFolder"
            visible: bar.shell !== null && bar.shell.lastRenderPath !== "" && (!bar.service || bar.service.lastError === "")
            text: qsTr("Show in folder"); iconName: "folder-open"; showLabel: true
            helpText: qsTr("Open the folder that holds the file the last export wrote.")
            helpScope: qsTr("Uses the desktop's file manager. The file itself is not opened.")
            onClicked: bar.shell.showInFolder(bar.shell.lastRenderPath)
        }

        // The message. An error in red; then what the application last reported
        // (a render result, a queued job); otherwise what an edit would act on.
        Text {
            objectName: "statusMessage"
            ControlHelp {
                title: qsTr("Message")
                purpose: qsTr("Shows the latest error or completed action. When there is no message, it names the active track, unsaved work, the next Undo action and the selection length.")
                scope: qsTr("An error stays until the next action that succeeds. The selection length is written in the display chosen under View › Time Display.")
            }
            // The engine's refusals are terse fragments; shown as a sentence.
            function sentence(t) {
                if (t === "") return ""
                const first = t.charAt(0).toUpperCase() + t.slice(1)
                return /[.!?…)]$/.test(first) ? first : first + "."
            }
            Layout.fillWidth: true
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: bar.service && bar.service.lastError !== "" ? Theme.danger
                 : bar.shell && bar.shell.statusMessage !== "" ? Theme.textSecondary : Theme.textMuted
            text: {
                if (!bar.service)
                    return ""
                if (bar.service.lastError !== "")
                    return sentence(bar.service.lastError)
                if (bar.shell && bar.shell.statusMessage !== "")
                    return bar.shell.statusMessage
                if (bar.service.trackCount === 0)
                    return qsTr("Open an audio file or record a take to begin")
                var bits = []
                if (bar.service.selectedClips.length > 0)
                    bits.push(qsTr("%1 clips selected").arg(bar.service.selectedClips.length))
                else if (bar.service.selectedTracks.length > 1)
                    bits.push(qsTr("%1 tracks selected").arg(bar.service.selectedTracks.length))
                if (bar.service.dirty && !bar.service.saving)
                    bits.push(qsTr("unsaved"))
                if (bar.focusedInfo && bar.focusedInfo.name)
                    bits.push(qsTr("editing: %1").arg(Theme.shortName(bar.focusedInfo.name, 40)))
                if (bar.service.canUndo)
                    bits.push(qsTr("undo: %1").arg(bar.service.undoLabel))
                if (!bar.service.selectionEmpty)
                    bits.push(qsTr("%1 selected").arg(!bar.shell ? bar.service.selectionLength
                        : bar.service.timeDisplay === "samples" ? bar.shell.formatLength(bar.service.selectionLength)   // a bare count needs its unit
                        : bar.shell.formatDuration(bar.service.selectionLength)))
                if (bar.service.anySolo)
                    bits.push(qsTr("SOLO"))
                return bits.join("  ·  ")
            }
        }

        Text {
            visible: bar.axis !== null && bar.service !== null && bar.service.loaded
            ControlHelp {
                title: qsTr("Zoom")
                purpose: qsTr("How much audio each screen pixel covers at the current zoom. Smaller is more detail.")
                scope: qsTr("Read-only; use the zoom buttons or the wheel to change it.")
            }
            text: bar.axis && bar.service ? (bar.service.timeDisplay, bar.service.sampleRate, bar.service.formatPerPixel(bar.axis.framesPerPixel)) : ""
            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: Theme.textMuted
        }

        // Audio load: share of each audio block playback needs; amber above 70%,
        // red above 90% or after a dropout. Opens the breakdown.
        RowLayout {
            spacing: 4
            Rectangle {
                width: 8; height: 8; radius: 4
                color: !bar.load.active ? Theme.textMuted
                    : (bar.load.percent || 0) > 90 || bar.newDropouts ? Theme.danger
                    : (bar.load.percent || 0) > 70 ? Theme.warning : Theme.success
            }
            ToolButton {
                objectName: "audioLoadStatus"
                text: bar.load.active ? qsTr("Audio load %1%").arg(Math.round(bar.load.percent || 0)) : qsTr("Audio load")
                showLabel: true
                onClicked: bar.shell.runAction("show_audio_load")
                helpText: qsTr("How much of each audio block playback needs: effects, mixing and routing together. Above 100% you hear a dropout.")
                helpScope: qsTr("Opens the breakdown: recent peak, dropouts, the effects using the most time, and Check this computer.")
            }
        }
        Rectangle { Layout.preferredWidth: Theme.hairline; Layout.preferredHeight: 14; color: Theme.border }

        // The two edit modes, so a ripple that is on is never a surprise.
        Text {
            objectName: "statusSnap"
            ControlHelp {
                title: qsTr("Snapping")
                purpose: bar.service ? bar.service.snapHint : ""
                scope: qsTr("Switch it with the magnet button. Choose a musical grid under Edit › Snap Grid.")
            }
            text: bar.service && bar.service.musicalSnapActive ? qsTr("Snap · %1").arg(bar.service.snapGridLabel(bar.service.snapGrid)) : qsTr("Snap")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: bar.shell && bar.shell.snapping ? Theme.accent : Theme.textMuted
            opacity: bar.shell && bar.shell.snapping ? 1.0 : 0.6
        }
        // When the recovery layer was last written, so "unsaved" is never a worry
        // without a number beside it.
        Text {
            objectName: "statusAutosave"
            visible: bar.service && (bar.service.autosaving || bar.service.lastAutosave !== "")
            ControlHelp {
                title: qsTr("Autosave")
                purpose: qsTr("When a recovery copy of your unsaved changes was last written. It is offered at the next launch if this run ends without saving.")
                scope: qsTr("A recovery copy is not a save: use Save to write the project itself.")
            }
            text: bar.service && bar.service.autosaving ? qsTr("Autosaving…")
                : qsTr("Autosaved %1").arg(bar.service ? bar.service.lastAutosave : "")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: Theme.textMuted
        }
        Text {
            objectName: "statusBefore"
            visible: bar.service && bar.service.previewBefore
            ControlHelp {
                title: qsTr("Before")
                purpose: qsTr("Playback is the mix before the master bus effects, for comparison.")
                scope: qsTr("Playback only; exports always include the bus. Switch it off with the headphones button on the Mastering toolbar.")
            }
            text: qsTr("Before")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.warning
        }
        // Without effects: lit, and a click puts them back.
        Text {
            objectName: "statusWithoutEffects"
            visible: bar.service !== null && bar.service.hearWithoutEffects
            ControlHelp {
                title: qsTr("Without effects")
                purpose: qsTr("Playback is the project with every effect off (tracks, clips and master), for comparing. Click to hear the effects again.")
                scope: qsTr("Listening only: the project and exports keep their effects. Alt+B switches it.")
            }
            text: qsTr("Without effects")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.warning
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.service.hearWithoutEffects = false
            }
        }
        Text {
            objectName: "statusRipple"
            ControlHelp {
                title: qsTr("Ripple editing")
                purpose: qsTr("Lit when delete and cut close the gap and paste pushes later audio along.")
                scope: qsTr("Later audio on the edited track changes position. Switch it with the toolbar button.")
            }
            text: qsTr("Ripple")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            font.weight: bar.shell && bar.shell.ripple ? Theme.wHeading : Theme.wNormal
            color: bar.shell && bar.shell.ripple ? Theme.warning : Theme.textMuted
            opacity: bar.shell && bar.shell.ripple ? 1.0 : 0.6
        }

        Rectangle { Layout.preferredWidth: Theme.hairline; Layout.preferredHeight: 14; color: Theme.border }

        Text {
            objectName: "statusKeymap"
            ControlHelp {
                title: qsTr("Keymap")
                purpose: qsTr("Whether the built-in shortcuts are in use, or your own keymap file overrides some of them.")
                scope: qsTr("Every menu shows its current binding.")
            }
            text: qsTr("Keymap: %1").arg(bar.shell && bar.shell.keymap && bar.shell.keymap.userOverrides
                                         ? qsTr("User") : qsTr("Default"))
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Text {
            ControlHelp {
                title: qsTr("Workspace")
                purpose: qsTr("The page in view. Take edits one recording, Arrange lays out clips, Spectral repairs by frequency and Mastering measures and exports the mix.")
                scope: qsTr("Switch with the tabs in the title bar. All four share one project and one transport.")
            }
            text: qsTr("Workspace: %1").arg(Pages.label(bar.workspace))
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }
}
