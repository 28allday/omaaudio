// Delegates below reach the lane's own `root` id.
pragma ComponentBehavior: Bound

import QtQuick
import "ui"
import "ui/FadeCurve.js" as FadeCurve

// One track: its header strip, and its waveform on the shared time axis.
//
// The header follows the concept's anatomy -- a role colour bar, the name,
// mute / solo, then gain and pan, then a small meter -- and folds down through
// the four heights the brief names: minimized keeps the name and M/S on one
// line, compact drops the sliders, normal is the full header, expanded adds
// the role. The waveform carries the clip outlines and names over it.
Item {
    id: root
    objectName: "trackLane"

    property var service: null
    property var axis: null
    property var track: null            // one entry from service.tracks
    property string shaderUrl: ""
    property var wheelResolver: null
    property real headerWidth: Theme.wTrackHeader
    // What the Tidy pauses page will do, for the pause shading ({then, changed, lengths}).
    property var pauseAction: null

    readonly property int trackId: track ? track.id : -1
    readonly property bool focused:
        service !== null && track !== null && service.focusedTrack === track.id
    readonly property bool selected: service !== null && service.selectedTracks.indexOf(trackId) >= 0
    readonly property string role: track ? track.role : "dialogue"
    readonly property string heightName: track ? track.height : "normal"
    readonly property bool minimized: heightName === "minimized"
    readonly property bool compact: heightName === "compact"
    readonly property bool expanded: heightName === "expanded"
    // The explicit colour when there is one, else the role's.
    readonly property color roleColor: (track && track.color !== "") ? track.color
                                                                      : Theme.roleColor(role)
    // The meter reads by id off the list the status tick publishes.
    readonly property var meter: (service && service.meterRevision >= 0 && trackId >= 0)
                                 ? service.trackMeter(trackId) : null

    property alias waveArea: waveFrame

    signal action(string name)
    signal seekRequested(real frame)   // a frame count: past 2^31 on a long take, so not an int
    signal contextMenuRequested()
    // The rename field is done with the keyboard. A hidden TextInput KEEPS active
    // focus (Qt does not clear it on hide), so the shell must be told to take the
    // keys back, or Ctrl+A after a rename does nothing. Found by the shell suite.
    signal keysReleased()
    // A wheel turn no binding claims (plain Scroll, by default) belongs to the lane
    // list, not the time axis. The waveform's WheelHandler has already taken the
    // event, so it is handed up rather than left to fall through to the Flickable.
    signal laneWheel(real deltaY)

    implicitHeight: Theme.laneHeight(heightName)

    // Keep the full fade geometry for inherited partial ramps, but allocate a
    // raster only for its visible intersection. Zooming into a long fade must
    // not allocate a texture as wide as the off-screen clip.
    component FadeShade: Item {
        id: shade
        property real fadeLength: 0
        property real offset: 0
        property real span: 0
        property int shape: 0
        property real bend: 0
        property bool reverse: false
        property real timelineX: 0
        property real viewportWidth: 0
        readonly property real startAlpha: 0.75 * (1 - amplitudeAt(0))
        readonly property real endAlpha: 0.75 * (1 - amplitudeAt(span))
        readonly property real middleAlpha: 0.75 * (1 - amplitudeAt(span / 2))
        function amplitudeAt(frame) {
            return fadeLength > 0 ? FadeCurve.value((offset + (reverse ? span - frame : frame)) / fadeLength, shape, bend) : 1
        }
        Canvas {
            id: fadePaint; objectName: "clipFadePaint"
            x: Math.max(0, -shade.timelineX)
            width: Math.max(0, Math.min(shade.width, shade.viewportWidth - shade.timelineX) - x)
            height: shade.height
            visible: width > 0 && height > 0
            readonly property color shadeColour: Theme.windowBg
            readonly property color curveColour: Theme.withAlpha(Theme.textSecondary, 0.55)
            readonly property var geometry: [shade.fadeLength, shade.offset, shade.span, shade.shape, shade.bend,
                shade.reverse, shade.width, x, width, height]
            onGeometryChanged: requestPaint()
            onShadeColourChanged: requestPaint()
            onCurveColourChanged: requestPaint()
            onVisibleChanged: if (visible) requestPaint()
            onPaint: {
                const ctx = getContext("2d"); ctx.reset()
                if (width <= 0 || height <= 0 || shade.width <= 0) return
                const steps = Math.max(2, Math.min(512, Math.ceil(width / 2)))
                const gradient = ctx.createLinearGradient(0, 0, width, 0)
                for (let i = 0; i <= steps; ++i) {
                    const t = i / steps
                    const level = shade.amplitudeAt((x + width * t) / shade.width * shade.span)
                    gradient.addColorStop(t, Qt.rgba(shadeColour.r, shadeColour.g, shadeColour.b, 0.75 * (1 - level)))
                }
                ctx.fillStyle = gradient; ctx.fillRect(0, 0, width, height)
                ctx.beginPath(); ctx.strokeStyle = curveColour; ctx.lineWidth = 1
                const top = Math.min(Theme.hClipLabel + 2, height / 3), bottom = Math.max(top, height - 3)
                for (let i = 0; i <= steps; ++i) {
                    const px = width * i / steps
                    const level = shade.amplitudeAt((x + px) / shade.width * shade.span)
                    const py = top + (1 - level) * (bottom - top)
                    if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                }
                ctx.stroke()
            }
        }
    }

    // Right button only, so every left-button interaction underneath -- seeking,
    // selection drags, the header controls -- is untouched. The window owns the
    // menu; the lane only says where the click landed.
    MouseArea {
        anchors.fill: parent
        z: 10
        acceptedButtons: Qt.RightButton
        onPressed: root.contextMenuRequested()
    }

    // The seam under every lane.
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
        z: 5
    }

    // --- header -------------------------------------------------------------------

    Rectangle {
        id: header
        objectName: "laneHeader"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.hairline
        width: root.headerWidth
        color: root.selected ? Theme.panelRaised : Theme.panelBg

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Theme.hairline
            color: Theme.border
        }

        // The role colour bar; brighter when focused.
        Rectangle {
            id: roleBar
            objectName: "laneRoleBar"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: root.focused ? 4 : 3
            color: root.roleColor
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onClicked: (mouse) => { if (root.service && root.track) root.service.selectTrack(root.track.id,
                (mouse.modifiers & Qt.ControlModifier) !== 0, (mouse.modifiers & Qt.ShiftModifier) !== 0) }
            z: -1
            ControlHelp {
                title: qsTr("Track header")
                purpose: qsTr("Click to select this track. Ctrl-click toggles tracks; Shift-click selects a range. Delete removes the selected tracks when no time range is selected. Right-click opens the track menu.")
                scope: qsTr("Cut, Copy and Delete use the time range on every selected track. The rack, inspector and other edits use the focused track.")
            }
        }

        // Minimized: one line, name and M/S side by side.
        Item {
            visible: root.minimized
            anchors.fill: parent
            anchors.leftMargin: Theme.s2 + roleBar.width
            anchors.rightMargin: Theme.s2
            Text {
                anchors.left: parent.left
                anchors.right: miniButtons.left
                anchors.rightMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                text: root.track ? root.track.name + (root.track.automationCount > 0 ? " · A" : "") : ""
                textFormat: Text.PlainText
                color: root.focused ? Theme.textPrimary : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                font.weight: Theme.wHeading
                elide: Text.ElideRight
            }
            Row {
                id: miniButtons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s1
                StateButton {
                    objectName: "laneMiniMute"
                    label: "M"; height: 16; width: 20
                    tip: qsTr("Mute"); helpText: qsTr("Silence this track in playback and export without removing anything.")
                    active: root.track ? root.track.muted === true : false
                    activeColor: Theme.warning
                    onClicked: if (root.service && root.track) root.service.toggleMute(root.track.id)
                }
                StateButton {
                    objectName: "laneMiniSolo"
                    label: "S"; height: 16; width: 20
                    tip: qsTr("Solo"); helpText: qsTr("Hear only the soloed tracks. Solo several to compare them.")
                    active: root.track ? root.track.soloed === true : false
                    onClicked: if (root.service && root.track) root.service.toggleSolo(root.track.id)
                }
            }
        }

        Column {
            visible: !root.minimized
            anchors.fill: parent
            anchors.leftMargin: Theme.s2 + roleBar.width
            anchors.rightMargin: Theme.s2
            anchors.topMargin: Theme.s1
            anchors.bottomMargin: Theme.s1
            spacing: Theme.s1

            // Name, editable in place. Double-click rather than a dialog: renaming a
            // track is a small enough act that it should not open a window.
            Item {
                width: parent.width
                height: Theme.hRow

                Text {
                    id: nameLabel
                    objectName: "laneName"
                    anchors.fill: parent
                    visible: !nameField.visible
                    text: root.track ? root.track.name + (root.track.automationCount > 0 ? " · A" : "") : ""
                    textFormat: Text.PlainText
                    color: root.focused ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    font.weight: Theme.wHeading
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter

                    MouseArea {
                        anchors.fill: parent
                        ControlHelp {
                            title: qsTr("Track name")
                            purpose: qsTr("Click to select a track. Ctrl-click toggles tracks; Shift-click selects a range. Double-click to rename.")
                            scope: qsTr("The name is saved with the project. An A after it means the track has automation.")
                        }
                        onDoubleClicked: {
                            nameField.text = nameLabel.text
                            nameField.visible = true
                            nameField.forceActiveFocus()
                            nameField.selectAll()
                        }
                        onClicked: (mouse) => { if (root.service && root.track) root.service.selectTrack(root.track.id,
                            (mouse.modifiers & Qt.ControlModifier) !== 0, (mouse.modifiers & Qt.ShiftModifier) !== 0) }
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    visible: nameField.visible
                    color: Theme.controlBg
                    radius: Theme.rControl
                    border.width: Theme.hairline
                    border.color: Theme.accent
                }

                TextInput {
                    id: nameField
                    objectName: "laneNameField"
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s1
                    anchors.rightMargin: Theme.s1
                    visible: false
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    verticalAlignment: Text.AlignVCenter
                    selectByMouse: true
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentText
                    // The window's key handler must not see these keystrokes, or typing
                    // a track name would run half the keymap.
                    Keys.onEscapePressed: {
                        nameField.visible = false
                        root.keysReleased()
                    }
                    onAccepted: {
                        if (root.service && root.track)
                            root.service.renameTrack(root.track.id, nameField.text)
                        nameField.visible = false
                        root.keysReleased()
                    }
                    onActiveFocusChanged: if (!activeFocus) nameField.visible = false
                }
            }

            // The role, spelled out, when there is room for it.
            Text {
                visible: root.expanded
                objectName: "laneRole"
                text: Theme.roleLabel(root.role)
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: root.roleColor
            }

            // Mute, solo, and the level readout.
            Row {
                spacing: Theme.s1

                StateButton {
                    objectName: "laneMute"
                    label: "M"; tip: qsTr("Mute")
                    helpText: qsTr("Silence this track in playback and export without removing anything.")
                    active: root.track ? root.track.muted === true : false
                    activeColor: Theme.warning
                    activeText: Theme.accentText
                    onClicked: if (root.service && root.track) root.service.toggleMute(root.track.id)
                }
                StateButton {
                    objectName: "laneSolo"
                    label: "S"; tip: qsTr("Solo")
                    helpText: qsTr("Hear only the soloed tracks. Solo several to compare them.")
                    active: root.track ? root.track.soloed === true : false
                    activeColor: Theme.accent
                    onClicked: if (root.service && root.track) root.service.toggleSolo(root.track.id)
                }
                StateButton {
                    objectName: "laneRecordArm"
                    label: "R"; tip: qsTr("Arm this track for recording")
                    helpText: qsTr("Recording goes onto this track when it starts. One track is armed at a time.")
                    activeColor: Theme.record
                    active: root.service !== null && root.track !== null && root.service.recordArmedTrack === root.track.id
                    enabled: root.service !== null && !root.service.recording
                    onClicked: if (root.track) root.service.setRecordArmedTrack(active ? -1 : root.track.id)
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: Theme.s1
                    text: root.track ? root.track.gainDb.toFixed(1) + " dB" : ""
                    color: Theme.textMuted
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel
                    font.features: { "tnum": 1 }
                }
            }

            // Gain, then pan. Dragging is horizontal on both, which is the same
            // gesture twice rather than a fader and a knob behaving differently.
            Slider {
                objectName: "laneGain"
                visible: !root.compact
                width: parent.width
                label: qsTr("Vol")
                helpTitle: qsTr("Track level")
                helpText: qsTr("Drag to set this track's level, from −60 to +12 dB. Double-click returns it to 0 dB.")
                helpScope: qsTr("Applies in playback and exports; the source audio is unchanged. The mixer strip shows the same level with a readout.")
                // -60..+12 dB, the useful range for levelling a take rather than the
                // full range the engine will accept.
                value: root.track ? (root.track.gainDb + 60) / 72 : 60 / 72
                centred: false
                onMoved: function (fraction) {
                    if (root.service && root.track)
                        root.service.setTrackGainDb(root.track.id, fraction * 72 - 60)
                }
                onTouched: pressed => { if (root.service && root.track) root.service.automationGesture(root.track.id, "volume", pressed) }
                onReset: {
                    if (root.service && root.track)
                        root.service.setTrackGainDb(root.track.id, 0)
                }
            }

            Slider {
                objectName: "lanePan"
                visible: !root.compact
                width: parent.width
                label: qsTr("Pan")
                helpTitle: qsTr("Track pan")
                helpText: qsTr("Drag to place this track between left and right. Double-click returns it to the centre.")
                helpScope: qsTr("Applies in playback and exports. Pan automation, when enabled, takes over.")
                value: root.track ? (root.track.pan + 1) / 2 : 0.5
                centred: true
                onMoved: function (fraction) {
                    if (root.service && root.track)
                        root.service.setTrackPan(root.track.id, fraction * 2 - 1)
                }
                onTouched: pressed => { if (root.service && root.track) root.service.automationGesture(root.track.id, "pan", pressed) }
                onReset: {
                    if (root.service && root.track)
                        root.service.setTrackPan(root.track.id, 0)
                }
            }

            // A small stereo meter: the track's own level, post-fader.
            Item {
                id: laneMeter
                objectName: "laneMeter"
                visible: !root.compact
                width: parent.width
                height: 7
                Column {
                    anchors.fill: parent
                    spacing: 1
                    Repeater {
                        model: 2
                        // Sized off the outer id, not `parent`: a Repeater delegate
                        // is briefly parentless on rebuild (trap 38).
                        Rectangle {
                            objectName: "laneMeterBar"
                            required property int index
                            readonly property real peak: !root.meter ? 0 : (index === 0 ? root.meter.left : root.meter.right)
                            readonly property real db: peak > 0 ? 20 * Math.log(peak) / Math.LN10 : -200
                            readonly property real fraction: Math.max(0, Math.min(1, (db + 60) / 60))
                            width: laneMeter.width
                            height: 3
                            color: Theme.meterBg
                            Rectangle {
                                width: Math.round(parent.width * parent.fraction)
                                height: parent.height
                                color: parent.fraction > 0.95 ? Theme.meterHigh
                                     : parent.fraction > 0.8 ? Theme.meterMid : Theme.meterLow
                            }
                        }
                    }
                }
            }
        }
    }

    // --- waveform -----------------------------------------------------------------

    Rectangle {
        id: waveFrame
        anchors.left: parent.left
        anchors.leftMargin: root.headerWidth
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.hairline
        color: root.selected ? Theme.laneFocusedBg : Theme.laneBg
        clip: true

        Waveform {
            id: waveform
            objectName: "waveform"
            anchors.fill: parent
            service: root.service
            axis: root.axis
            shaderUrl: root.shaderUrl
            trackId: root.trackId
            levels: root.track ? root.track.levels : []
            trackFrames: root.track ? root.track.frames : 0
            focused: root.focused
            clipSelectionEnabled: true
            silenced: root.track ? root.track.silenced === true : false
            wheelResolver: root.wheelResolver
            waveColor: root.roleColor
            selectionColor: Theme.accent

            onSeekRequested: function (frame) { root.seekRequested(frame) }
            onClipPickRequested: (frame, toggle) => {
                if (root.service) root.service.selectClipAt(root.trackId, frame, toggle)
                root.keysReleased()
            }

            onFocusRequested: {
                if (root.service && root.track)
                    root.service.focusSelectedTrack(root.track.id)
            }

            onSelectionDragged: function (startFrame, endFrame, finished) {
                if (!root.service)
                    return
                if (startFrame === endFrame)
                    root.service.clearSelection()
                else
                    root.service.select(startFrame, endFrame, true, root.trackId)
            }

            onWheelAction: function (name, deltaY, x) {
                if (name === "") {
                    root.laneWheel(deltaY)
                    return
                }
                if (!root.axis)
                    return
                root.axis.applyWheel(name, deltaY, x)
            }
        }

        Repeater {
            model: {
                if (!root.service || !root.service.clipMoving) return []
                const preview = root.service.clipMovePreview
                if (preview.clips !== undefined) return preview.clips.filter(clip => clip.track === root.trackId)
                return preview.track === root.trackId ? [preview] : []
            }
            delegate: Rectangle {
                required property var modelData
                objectName: "clipMoveGhost"
                readonly property var preview: root.service ? root.service.clipMovePreview : ({})
                x: root.axis ? root.axis.frameToX(modelData.start) : 0
                width: root.axis ? Math.max(2, modelData.length / root.axis.framesPerPixel) : 0
                height: waveFrame.height
                z: 5
                color: Theme.withAlpha(preview.valid ? Theme.accent : Theme.danger, 0.2)
                border.width: 2
                border.color: preview.valid ? Theme.accent : Theme.danger
                Text {
                    anchors.top: parent.top; anchors.left: parent.left
                    anchors.margins: Theme.s1
                    anchors.topMargin: root.compact ? Theme.s1 : Theme.hClipLabel + Theme.s1
                    width: Math.max(0, parent.width - Theme.s1 * 2)
                    elide: Text.ElideRight
                    text: parent.preview.slip ? qsTr("Source offset: %1%2").arg(root.service ? (root.service.timeDisplay, root.service.musicalTime, root.service.formatDuration(Math.round(parent.preview.sourceOffset))) : "").arg(parent.preview.limited ? qsTr(" — source limit") : "")
                        : parent.preview.valid ? qsTr("Move here") : qsTr("Cannot move here")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                }
            }
        }

        // The clips, outlined and named over the waveform. Only the ones in view
        // are instantiated: a heavily edited take has thousands and a Repeater
        // over all of them would be the cost with none of the benefit.
        readonly property var visibleClips: {
            if (!root.track || !root.track.clips || !root.axis || root.minimized)
                return []
            const from = root.axis.viewStart
            const to = root.axis.viewEnd
            var out = []
            const clips = root.track.clips
            for (var i = 0; i < clips.length && out.length < 400; ++i) {
                const c = clips[i]
                if (c.end > from && c.start < to)
                    out.push(c)
            }
            return out
        }

        Repeater {
            model: waveFrame.visibleClips

            delegate: Item {
                id: clipBox
                objectName: "clipBox"
                required property var modelData
                // An edge being trimmed, in frames (-1: none): the clip is drawn
                // to it while it is dragged, and set on release.
                property real trimStart: -1
                property real trimEnd: -1
                readonly property bool trimming: trimStart >= 0
                readonly property real x0: root.axis ? root.axis.frameToX(trimming ? trimStart : modelData.start) : 0
                readonly property real x1: root.axis ? root.axis.frameToX(trimming ? trimEnd : modelData.end) : 0
                x: x0
                y: 0
                width: Math.max(1, x1 - x0)
                height: waveFrame.height
                readonly property bool wide: width >= Theme.wClipLabelMin
                readonly property bool selected: {
                    if (!root.service) return false
                    for (const clip of root.service.selectedClips)
                        if (clip.track === root.trackId && clip.clip === modelData.index) return true
                    return false
                }

                Repeater {
                    model: {
                        const fade = clipBox.modelData.crossfade
                        if (!root.focused || !fade || !fade.valid) return []
                        let edges = []
                        if (clipBox.modelData.start === fade.start) edges.push(0)
                        if (clipBox.modelData.end === fade.end) edges.push(1)
                        return edges
                    }
                    delegate: Rectangle {
                        id: crossfadeHandle
                        required property int modelData
                        readonly property bool endHandle: modelData === 1
                        objectName: endHandle ? "laneCrossfadeEnd" : "laneCrossfadeStart"
                        property var targetClip: ({})
                        property real previewFrame: 0
                        property bool dragging: false
                        x: dragging && root.axis ? root.axis.frameToX(previewFrame) - clipBox.x - width / 2
                            : endHandle ? clipBox.width - width : 0
                        y: Math.max(Theme.hClipLabel, clipBox.height - 22)
                        width: 12; height: 18; z: 8
                        color: crossfadeMouse.containsMouse || dragging ? Theme.accent : Theme.panelRaised
                        border.color: Theme.accent; radius: 2
                        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; anchors.centerIn: parent; text: crossfadeHandle.endHandle ? "›" : "‹"; color: Theme.textPrimary }
                        MouseArea {
                            id: crossfadeMouse; anchors.fill: parent; hoverEnabled: true
                            enabled: root && root.service && !root.service.sampleEditing
                            ControlHelp {
                                title: qsTr("Crossfade edge")
                                purpose: qsTr("Drag to move this edge of the crossfade, making the overlap longer or shorter. Alt+F sets exact times.")
                                scope: qsTr("One Undo step on release. The surrounding clips must still match the original pair.")
                                suppressed: crossfadeMouse.pressed
                            }
                            cursorShape: Qt.SizeHorCursor
                            onPressed: {
                                crossfadeHandle.targetClip = root.service.crossfadeTarget(root.trackId, clipBox.modelData.index)
                                if (!crossfadeHandle.targetClip.valid) return
                                crossfadeHandle.previewFrame = crossfadeHandle.endHandle ? crossfadeHandle.targetClip.end : crossfadeHandle.targetClip.start
                                crossfadeHandle.dragging = true; forceActiveFocus()
                            }
                            onPositionChanged: event => {
                                if (!crossfadeHandle.dragging || !root.axis) return
                                const frame = Math.round(root.axis.xToFrame(mapToItem(waveFrame, event.x, event.y).x))
                                const t = crossfadeHandle.targetClip
                                crossfadeHandle.previewFrame = crossfadeHandle.endHandle ? Math.max(t.join + 1, Math.min(t.maximum, frame)) : Math.max(t.minimum, Math.min(t.join - 1, frame))
                            }
                            onReleased: {
                                if (!crossfadeHandle.dragging) return
                                const t = crossfadeHandle.targetClip, frame = crossfadeHandle.previewFrame
                                crossfadeHandle.dragging = false
                                root.service.adjustCrossfade(crossfadeHandle.endHandle ? t.start : frame,
                                    crossfadeHandle.endHandle ? frame : t.end, t.equalPower, t.stamp, t.track, t.clip)
                                root.keysReleased()
                            }
                            onCanceled: { crossfadeHandle.dragging = false; root.keysReleased() }
                            Keys.onEscapePressed: { crossfadeHandle.dragging = false; root.keysReleased() }
                        }
                    }
                }

                readonly property color clipColour: clipBox.modelData.groupColour || root.roleColor
                // A fade being dragged, in frames (-1: none), drawn before it is set.
                property real dragFadeIn: -1
                property real dragFadeOut: -1
                // The pointer over this clip, as the waveform underneath sees it: a
                // hover handler here would take the waveform's hover (and its help).
                readonly property bool hovered: waveform.pointerInside
                    && waveform.pointerX >= clipBox.x && waveform.pointerX < clipBox.x + clipBox.width

                // The outline.
                Rectangle {
                    anchors.fill: parent; z: 1
                    color: clipBox.selected ? Theme.withAlpha(Theme.accent, 0.08) : "transparent"
                    border.width: clipBox.selected ? 2 : Theme.hairline
                    border.color: clipBox.selected ? Theme.accent : Theme.withAlpha(clipBox.clipColour, root.focused ? 0.55 : 0.35)
                }

                // Shade only the part of each original curve this clip contains.
                FadeShade {
                    id: fadeInShade; objectName: "clipFadeIn"
                    visible: fadeLength > 0 && span > 0 && root.axis !== null
                    fadeLength: clipBox.dragFadeIn >= 0 ? clipBox.dragFadeIn : clipBox.modelData.fadeIn || 0
                    offset: clipBox.dragFadeIn >= 0 ? 0 : clipBox.modelData.fadeInOffset || 0
                    span: Math.max(0, Math.min(clipBox.modelData.length, fadeLength - offset))
                    shape: clipBox.modelData.fadeInShape || 0; bend: clipBox.modelData.fadeInCurve || 0
                    x: 0
                    width: root.axis ? Math.min(clipBox.width, span / Math.max(0.0001, root.axis.framesPerPixel)) : 0
                    height: parent.height
                    timelineX: clipBox.x + x; viewportWidth: waveFrame.width
                }
                FadeShade {
                    id: fadeOutShade; objectName: "clipFadeOut"
                    visible: fadeLength > 0 && span > 0 && root.axis !== null
                    fadeLength: clipBox.dragFadeOut >= 0 ? clipBox.dragFadeOut : clipBox.modelData.fadeOut || 0
                    offset: clipBox.dragFadeOut >= 0 ? 0 : clipBox.modelData.fadeOutOffset || 0
                    span: Math.max(0, Math.min(clipBox.modelData.length, fadeLength - offset))
                    shape: clipBox.modelData.fadeOutShape || 0; bend: clipBox.modelData.fadeOutCurve || 0
                    reverse: true
                    width: root.axis ? Math.min(clipBox.width, span / Math.max(0.0001, root.axis.framesPerPixel)) : 0
                    x: clipBox.width - width
                    height: parent.height
                    timelineX: clipBox.x + x; viewportWidth: waveFrame.width
                }

                // Fade handles, at the top corners below the name: drag one in to
                // fade the clip in or out, back to the edge to remove the fade.
                // Shown on hover, on a selected clip, and wherever a fade is set.
                Repeater {
                    model: root && root.service && root.axis && clipBox.width >= 24 ? [0, 1] : []
                    delegate: Rectangle {
                        id: fadeHandle
                        required property int modelData
                        readonly property bool outHandle: modelData === 1
                        objectName: outHandle ? "clipFadeOutHandle" : "clipFadeInHandle"
                        // The fade this clip shows now: the visible part of a split fade.
                        readonly property real shown: outHandle
                            ? Math.max(0, (clipBox.modelData.fadeOut || 0) - (clipBox.modelData.fadeOutOffset || 0))
                            : Math.max(0, (clipBox.modelData.fadeIn || 0) - (clipBox.modelData.fadeInOffset || 0))
                        readonly property real frames: outHandle ? (clipBox.dragFadeOut >= 0 ? clipBox.dragFadeOut : shown)
                                                                 : (clipBox.dragFadeIn >= 0 ? clipBox.dragFadeIn : shown)
                        readonly property real px: root && root.axis ? frames / Math.max(0.0001, root.axis.framesPerPixel) : 0
                        width: 10; height: 10; radius: 2; z: 9
                        y: root && root.compact ? 1 : Theme.hClipLabel
                        x: outHandle ? Math.max(0, clipBox.width - px - width) : Math.min(clipBox.width - width, px)
                        visible: clipBox.hovered || fadeMouse.containsMouse || fadeMouse.pressed || clipBox.selected || frames > 0
                        color: fadeMouse.containsMouse || fadeMouse.pressed ? Theme.accent : Theme.panelRaised
                        border.width: Theme.hairline
                        border.color: Theme.accent
                        MouseArea {
                            id: fadeMouse
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            preventStealing: true
                            cursorShape: Qt.SizeHorCursor
                            enabled: root !== null && root.service !== null && !root.service.sampleEditing
                            ControlHelp {
                                title: fadeHandle.outHandle ? qsTr("Fade out") : qsTr("Fade in")
                                purpose: fadeHandle.outHandle ? qsTr("Drag left to fade the end of this clip out; drag back to the edge to remove the fade.")
                                                              : qsTr("Drag right to fade the start of this clip in; drag back to the edge to remove the fade.")
                                scope: qsTr("This clip only. One Undo step on release; the fade's shape is kept. Clip › Properties sets exact times.")
                                suppressed: fadeMouse.pressed
                            }
                            // Where in the handle it was grabbed, so it does not jump.
                            property real grab: 0
                            function along(x) {
                                const inClip = mapToItem(clipBox, x, 0).x
                                return fadeHandle.outHandle ? clipBox.width - inClip : inClip
                            }
                            function framesAt(x) {
                                const f = Math.round((along(x) - grab) * root.axis.framesPerPixel)
                                const other = fadeHandle.outHandle ? (clipBox.modelData.fadeIn || 0) - (clipBox.modelData.fadeInOffset || 0)
                                                                   : (clipBox.modelData.fadeOut || 0) - (clipBox.modelData.fadeOutOffset || 0)
                                return Math.max(0, Math.min(clipBox.modelData.length - Math.max(0, other), f))
                            }
                            onPressed: event => {
                                grab = along(event.x) - fadeHandle.px
                                if (fadeHandle.outHandle) clipBox.dragFadeOut = fadeHandle.shown
                                else clipBox.dragFadeIn = fadeHandle.shown
                            }
                            onPositionChanged: event => {
                                if (!pressed) return
                                if (fadeHandle.outHandle) clipBox.dragFadeOut = framesAt(event.x)
                                else clipBox.dragFadeIn = framesAt(event.x)
                            }
                            onReleased: {
                                const m = clipBox.modelData
                                const fadeIn = clipBox.dragFadeIn >= 0 ? clipBox.dragFadeIn : Math.max(0, (m.fadeIn || 0) - (m.fadeInOffset || 0))
                                const fadeOut = clipBox.dragFadeOut >= 0 ? clipBox.dragFadeOut : Math.max(0, (m.fadeOut || 0) - (m.fadeOutOffset || 0))
                                const changed = Math.round(fadeIn) !== Math.round(Math.max(0, (m.fadeIn || 0) - (m.fadeInOffset || 0)))
                                             || Math.round(fadeOut) !== Math.round(Math.max(0, (m.fadeOut || 0) - (m.fadeOutOffset || 0)))
                                clipBox.dragFadeIn = -1; clipBox.dragFadeOut = -1
                                // A refusal says why in the status line.
                                if (changed && root.service)
                                    root.service.setClipFades(root.trackId, m.index, Math.round(fadeIn), Math.round(fadeOut))
                                root.keysReleased()
                            }
                            onCanceled: { clipBox.dragFadeIn = -1; clipBox.dragFadeOut = -1; root.keysReleased() }
                        }
                    }
                }

                // Trim: the lower half of a clip's left and right edges, drag
                // in to hide audio or out to show more of the take. The audio keeps
                // its place in time; the file is never changed.
                property var trimLimits: ({})
                // A count, not a list: a new list each time the width changes would
                // rebuild the edges under a drag.
                Repeater {
                    model: root && root.service && root.axis && !root.compact && clipBox.width >= 16
                           && clipBox.height / 2 - 4 >= 10 && clipBox.height - Theme.hClipLabel - 14 >= 10 ? 2 : 0
                    delegate: Item {
                        id: trimEdge
                        required property int index
                        readonly property bool rightEdge: index === 1
                        objectName: rightEdge ? "clipTrimEnd" : "clipTrimStart"
                        width: 6; z: 7
                        x: rightEdge ? clipBox.width - width : 0
                        // The lower half of the edge: above it, a drag from the edge
                        // (the lane's border included) still selects.
                        y: Math.max(Theme.hClipLabel + 14, Math.round(clipBox.height / 2) + 4)
                        height: clipBox.height - y
                        // The edge lights while it is pointed at or dragged.
                        Rectangle {
                            visible: trimMouse.containsMouse || trimMouse.pressed
                            x: trimEdge.rightEdge ? parent.width - width : 0
                            width: 2; height: parent.height
                            color: Theme.accent
                        }
                        MouseArea {
                            id: trimMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            preventStealing: true
                            cursorShape: Qt.SizeHorCursor
                            enabled: root !== null && root.service !== null && !root.service.sampleEditing
                            ControlHelp {
                                title: trimEdge.rightEdge ? qsTr("Clip end") : qsTr("Clip start")
                                purpose: qsTr("Drag to trim this clip: in to hide audio, out to show more of the recording. The audio stays where it is in time.")
                                scope: qsTr("This clip only, never the file. It stops at the recording's ends, at prepared clip effects and at the next clip. Snap applies. One Undo step on release.")
                                suppressed: trimMouse.pressed
                            }
                            // How far from the edge it was grabbed, in frames, so it does not jump.
                            property real grab: 0
                            function frameAt(x) {
                                return root.axis.xToFrame(mapToItem(waveFrame, x, 0).x)
                            }
                            onPressed: mouse => {
                                const limits = root.service.clipTrimLimits(root.trackId, clipBox.modelData.index)
                                // A crossfade has its own edges: the press selects as usual.
                                if (!limits.valid) { mouse.accepted = false; return }
                                if (root.service.focusedTrack !== root.trackId) root.service.focusTrack(root.trackId)
                                clipBox.trimLimits = limits
                                grab = frameAt(mouse.x) - (trimEdge.rightEdge ? clipBox.modelData.end : clipBox.modelData.start)
                                clipBox.trimEnd = clipBox.modelData.end
                                clipBox.trimStart = clipBox.modelData.start
                            }
                            onPositionChanged: mouse => {
                                if (!pressed || !clipBox.trimming) return
                                const l = clipBox.trimLimits
                                let f = Math.round(frameAt(mouse.x) - grab)
                                if (root.service.snapping) f = root.service.snapFrame(f)
                                if (trimEdge.rightEdge)
                                    clipBox.trimEnd = Math.max(clipBox.trimStart + l.minimumLength, Math.min(l.latestEnd, f))
                                else
                                    clipBox.trimStart = Math.max(l.earliestStart, Math.min(clipBox.trimEnd - l.minimumLength, f))                            }
                            onReleased: {
                                if (!clipBox.trimming) return
                                const start = Math.round(clipBox.trimStart), end = Math.round(clipBox.trimEnd)
                                const m = clipBox.modelData
                                clipBox.trimStart = -1; clipBox.trimEnd = -1
                                // A refusal says why in the status line.
                                if (start !== m.start || end !== m.end)
                                    root.service.trimClip(root.trackId, m.index, start, end)
                                root.keysReleased()
                            }
                            onCanceled: { clipBox.trimStart = -1; clipBox.trimEnd = -1; root.keysReleased() }
                        }
                        // The clip's new length while it is dragged.
                        Text {
                            objectName: "clipTrimReadout"
                            visible: trimMouse.pressed && clipBox.trimming
                            x: trimEdge.rightEdge ? -implicitWidth - Theme.s1 : parent.width + Theme.s1
                            y: Theme.s1
                            text: qsTr("%1 s").arg(((clipBox.trimEnd - clipBox.trimStart) / Math.max(1, root && root.service ? root.service.sampleRate : 48000)).toFixed(3))
                            font.family: Theme.monoFamily; font.pixelSize: Theme.fsRuler; font.features: { "tnum": 1 }
                            color: Theme.textPrimary
                            Rectangle { z: -1; anchors.fill: parent; anchors.margins: -2; radius: 2; color: Theme.withAlpha(Theme.panelBg, 0.85) }
                        }
                    }
                }

                // Clip gain: a line across the clip at its level, from +12 dB at the
                // top to −40 dB near the bottom, with a handle in the middle to drag.
                property real dragGainDb: NaN
                readonly property real gainDb: isNaN(dragGainDb) ? (modelData.gainDb || 0) : dragGainDb
                readonly property real gainTop: Theme.hClipLabel + 6
                readonly property real gainBottom: height - 6
                function gainY(db) {
                    const d = Math.max(-40, Math.min(12, db))
                    return gainTop + (12 - d) / 52 * Math.max(1, gainBottom - gainTop)
                }
                function gainAt(y) {
                    const d = 12 - (y - gainTop) / Math.max(1, gainBottom - gainTop) * 52
                    const rounded = Math.round(Math.max(-40, Math.min(12, d)) * 10) / 10
                    return Math.abs(rounded) < 0.5 ? 0 : rounded   // 0 dB holds the line
                }
                readonly property bool gainShown: root !== null && root.service !== null && !root.compact
                    && clipBox.width >= 60 && clipBox.height >= 56
                Rectangle {
                    objectName: "clipGainLine"
                    visible: clipBox.gainShown && (clipBox.hovered || gainMouse.containsMouse || gainMouse.pressed || clipBox.selected)
                    x: 0; width: clipBox.width; height: 1; z: 6
                    y: Math.round(clipBox.gainY(clipBox.gainDb))
                    color: Theme.withAlpha(Theme.accent, gainMouse.pressed || gainMouse.containsMouse ? 0.9 : 0.5)
                }
                Rectangle {
                    id: gainHandle
                    objectName: "clipGainHandle"
                    visible: clipBox.gainShown && (clipBox.hovered || gainMouse.containsMouse || gainMouse.pressed || clipBox.selected)
                    width: 22; height: 8; radius: 3; z: 9
                    x: Math.round((clipBox.width - width) / 2)
                    y: Math.round(clipBox.gainY(clipBox.gainDb) - height / 2)
                    color: gainMouse.containsMouse || gainMouse.pressed ? Theme.accent : Theme.panelRaised
                    border.width: Theme.hairline; border.color: Theme.accent
                    MouseArea {
                        id: gainMouse
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: Qt.SizeVerCursor
                        enabled: root !== null && root.service !== null && !root.service.sampleEditing
                        ControlHelp {
                            title: qsTr("Clip gain")
                            purpose: qsTr("Drag up or down to make this clip louder or quieter; hold Shift for fine steps. Double-click to return to 0 dB.")
                            scope: qsTr("This clip only, from −40 to +12 dB. One Undo step on release. Clip › Properties takes an exact value.")
                            suppressed: gainMouse.pressed
                        }
                        property real grab: 0
                        property real pressY: 0
                        property real pressDb: 0
                        function along(y) { return mapToItem(clipBox, 0, y).y }
                        onPressed: mouse => {
                            pressY = along(mouse.y)
                            pressDb = clipBox.modelData.gainDb || 0
                            grab = clipBox.gainY(pressDb) - pressY
                            clipBox.dragGainDb = pressDb
                        }
                        onPositionChanged: mouse => {
                            if (!pressed || isNaN(clipBox.dragGainDb)) return
                            const y = along(mouse.y)
                            if (mouse.modifiers & Qt.ShiftModifier)
                                clipBox.dragGainDb = Math.round(Math.max(-40, Math.min(12, pressDb + (pressY - y) * 0.05)) * 10) / 10
                            else
                                clipBox.dragGainDb = clipBox.gainAt(y + grab)
                        }
                        onReleased: {
                            const db = clipBox.dragGainDb
                            clipBox.dragGainDb = NaN
                            if (!isNaN(db) && Math.abs(db - (clipBox.modelData.gainDb || 0)) >= 0.05 && root.service)
                                root.service.setClipGainDb(root.trackId, clipBox.modelData.index, db)
                            root.keysReleased()
                        }
                        onDoubleClicked: {
                            clipBox.dragGainDb = NaN
                            if (Math.abs(clipBox.modelData.gainDb || 0) >= 0.05 && root.service)
                                root.service.setClipGainDb(root.trackId, clipBox.modelData.index, 0)
                        }
                        onCanceled: { clipBox.dragGainDb = NaN; root.keysReleased() }
                    }
                    // The level while it is dragged.
                    Text {
                        objectName: "clipGainReadout"
                        visible: gainMouse.pressed && !isNaN(clipBox.dragGainDb)
                        anchors.left: parent.right; anchors.leftMargin: Theme.s1
                        anchors.verticalCenter: parent.verticalCenter
                        text: (clipBox.dragGainDb > 0 ? "+" : "") + clipBox.dragGainDb.toFixed(1) + " dB"
                        font.family: Theme.monoFamily; font.pixelSize: Theme.fsRuler; font.features: { "tnum": 1 }
                        color: Theme.textPrimary
                        Rectangle { z: -1; anchors.fill: parent; anchors.margins: -2; radius: 2; color: Theme.withAlpha(Theme.panelBg, 0.85) }
                    }
                }

                // The name, in a pill that stays inside the clip.
                Rectangle {
                    visible: clipBox.wide && !root.compact
                    x: 2; y: 2
                    width: Math.min(clipBox.width - 4, clipLabel.implicitWidth + Theme.s1 * 2)
                    height: Theme.hClipLabel - 2
                    MouseArea {
                        objectName: "clipSelectMouse"
                        anchors.fill: parent
                        ControlHelp {
                            title: qsTr("Select clip")
                            purpose: qsTr("Click to select this whole clip and its linked group. Ctrl-click toggles the group across tracks. Use Move to drag the selected clips together.")
                            scope: qsTr("Cut, Copy and Delete use the selected clips. Properties and Pitch and duration offer selected-clip or reference-clip scope. Clip envelopes edit the reference clip only. Time-range processing keeps its own scope. Turn off Clip › Link Group Selection to choose individual members.")
                        }
                        onClicked: mouse => {
                            if (root.service) root.service.selectClipAt(root.trackId, clipBox.modelData.start,
                                (mouse.modifiers & Qt.ControlModifier) !== 0)
                            root.keysReleased()
                        }
                    }
                    radius: Theme.rControl
                    color: Theme.withAlpha(clipBox.clipColour, root.focused ? 0.35 : 0.22)
                    clip: true
                    Text {
                        id: clipLabel
                        objectName: "clipName"
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s1
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - Theme.s1 * 2
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: clipBox.modelData.name + (clipBox.modelData.groupName ? " · " + clipBox.modelData.groupName : "") + (clipBox.modelData.elastic
                            ? " · " + (clipBox.modelData.semitones > 0 ? "+" : "") + clipBox.modelData.semitones
                              + " st · " + Math.round(clipBox.modelData.durationRatio * 100) + "%" : "")
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsRuler + 1
                        font.weight: Theme.wHeading
                        color: Theme.textPrimary
                    }
                }

                // A small stored-envelope cue leaves the clip's move/trim surface intact.
                Text {
                    objectName: "clipEnvelopeBadge"
                    readonly property bool hasVolume: (clipBox.modelData.volumeEnvelopePoints || 0) > 0
                    readonly property bool hasPan: (clipBox.modelData.panEnvelopePoints || 0) > 0
                    visible: clipBox.wide && !root.compact && (hasVolume || hasPan)
                    anchors.right: parent.right; anchors.rightMargin: Theme.s1
                    anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.s1
                    text: (hasVolume ? (clipBox.modelData.volumeEnvelopeEnabled ? qsTr("Vol") : qsTr("Vol off")) : "")
                        + (hasVolume && hasPan ? " · " : "")
                        + (hasPan ? (clipBox.modelData.panEnvelopeEnabled ? qsTr("Pan") : qsTr("Pan off")) : "")
                    color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler
                    width: Math.min(implicitWidth, Math.max(0, clipBox.width - Theme.s1 * 2)); elide: Text.ElideRight
                }

                // Clip gain, only when it is not unity.
                Text {
                    objectName: "clipGain"
                    visible: clipBox.wide && !root.compact && Math.abs(clipBox.modelData.gainDb) >= 0.05
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.s1
                    y: 3
                    text: (clipBox.modelData.gainDb > 0 ? "+" : "") + clipBox.modelData.gainDb.toFixed(1) + " dB"
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsRuler
                    font.features: { "tnum": 1 }
                    color: Theme.textSecondary
                }
            }
        }

        // The pauses Tidy pauses found, over the clips: a click keeps one.
        PauseShading {
            anchors.fill: parent
            z: 6
            service: root.service
            axis: root.axis
            trackId: root.trackId
            action: root.pauseAction
            tagY: root.compact ? Theme.s1 : Theme.hClipLabel + Theme.s1
        }

        Text {
            anchors.centerIn: parent
            visible: root.track !== null && root.track.clipCount === 0
            text: qsTr("empty track")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
        }
    }
}
