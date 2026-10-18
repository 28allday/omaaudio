// Delegates below reach the workspace's own ids.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The Multitrack workspace: Files dock | toolbar + ruler + markers + lanes |
// Mixer | Track Effects. Every boundary is a SplitView the shell persists.
Item {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    // The page can be unloaded with a clip drag in hand (a workspace shortcut
    // pressed mid-drag). Nothing delivers a cancel then, so it is given here.
    Component.onDestruction: if (shell && shell.service && shell.service.clipMoving) shell.service.cancelClipMove()
    readonly property var axis: shell ? shell.timeAxis : null
    readonly property var keymap: shell ? shell.keymap : null

    // Exposed so the shell can find the lane column for the playhead and tests.
    readonly property alias lanes: lanes
    function showEffect(slot) { effectsDock.currentTab = 0; effectsDock.select(slot) }

    C.SplitView {
        id: body
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: SplitterHandle {
            pressed: C.SplitHandle.pressed
            hovered: C.SplitHandle.hovered
        }

        FileDock {
            id: fileDock
            shell: root.shell
            visible: root.shell ? root.shell.leftDockVisible : true
            C.SplitView.preferredWidth: root.shell ? root.shell.leftDockWidth : Theme.wLeftDock
            C.SplitView.minimumWidth: 230
            C.SplitView.maximumWidth: 420
            onWidthChanged: if (body.resizing && root.shell) root.shell.leftDockWidth = width
        }

        // ── timeline ────────────────────────────────────────────────────
        Item {
            id: timeline
            C.SplitView.fillWidth: true
            C.SplitView.minimumWidth: 480

            TimelineToolbar {
                id: toolbar
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                shell: root.shell
            }

            TimeRuler {
                id: ruler
                anchors.top: toolbar.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                axis: root.axis
                service: root.service
                originX: Theme.wTrackHeader
                onSeekRequested: function (frame) { if (root.service) root.service.seekFrames(frame) }
            }

            MarkerLane {
                id: markerLane
                anchors.top: ruler.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                axis: root.axis
                service: root.service
                shell: root.shell
                originX: Theme.wTrackHeader
                onSeekRequested: function (frame) { if (root.service) root.service.seekFrames(frame) }
            }

            Rectangle {
                id: lanes
                objectName: "lanes"
                anchors.top: markerLane.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: Theme.windowBg

                readonly property real headerWidth: Theme.wTrackHeader

                // Every lane draws on the same axis, so the axis is told the width
                // of one lane's waveform column -- not the window's.
                function syncAxis() {
                    if (root.axis)
                        root.axis.viewWidth = Math.max(0, laneScroll.width - headerWidth)
                }
                onWidthChanged: syncAxis()
                Component.onCompleted: syncAxis()

                // "normal" lanes share whatever height the fixed ones leave, and
                // never drop below their token; past that the column scrolls.
                readonly property real normalHeight: {
                    if (!root.service)
                        return Theme.hLaneNormal
                    var fixed = 0
                    var normals = 0
                    const tracks = root.service.trackSummaries
                    for (var i = 0; i < tracks.length; ++i) {
                        if (tracks[i].height === "normal")
                            ++normals
                        else
                            fixed += Theme.laneHeight(tracks[i].height)
                    }
                    if (normals === 0)
                        return Theme.hLaneNormal
                    return Math.max(Theme.hLaneNormal, (laneScroll.height - fixed) / normals)
                }

                Flickable {
                    id: laneScroll
                    anchors.fill: parent
                    contentHeight: laneColumn.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    // Vertical only: horizontal movement is the time axis's business,
                    // and two competing scroll models in one view is a mess.
                    flickableDirection: Flickable.VerticalFlick
                    C.ScrollBar.vertical: ScrollBar {}

                    // The focused lane is kept in view when focus moves to another
                    // track (a new recording, Add Track, the keyboard), scrolling no
                    // further than needed; a person's own scrolling is left alone.
                    property int revealed: -1
                    // Set when focus moves; cleared once the lane is in view. A new
                    // lane is laid out after the change that made it, so the lanes
                    // growing tries again.
                    property bool revealPending: false
                    function revealFocused() {
                        const id = root.service ? root.service.focusedTrack : -1
                        if (id !== revealed) { revealed = id; revealPending = id >= 0 }
                        if (!revealPending) return
                        // A lane only just made has no place yet: lay the column out first.
                        laneColumn.forceLayout()
                        for (let i = 0; i < laneColumn.children.length; ++i) {
                            const lane = laneColumn.children[i]
                            if (lane.trackId !== id) continue
                            const end = Math.max(0, contentHeight - height)
                            if (lane.y < contentY || lane.height > height) contentY = Math.min(end, lane.y)
                            else if (lane.y + lane.height > contentY + height) contentY = Math.min(end, lane.y + lane.height - height)
                            revealPending = lane.y < contentY - 0.5
                                || (lane.height <= height && lane.y + lane.height > contentY + height + 0.5)
                            return
                        }
                    }
                    Connections {
                        target: root.service
                        function onTracksChanged() { Qt.callLater(laneScroll.revealFocused) }
                    }
                    onContentHeightChanged: if (revealPending) Qt.callLater(revealFocused)
                    onHeightChanged: if (revealPending) Qt.callLater(revealFocused)

                    // Same feel as a Shift+wheel turn on the time axis: 80 px a notch.
                    function scrollByWheel(deltaY) {
                        if (!Number.isFinite(deltaY) || deltaY === 0)
                            return
                        const end = Math.max(0, contentHeight - height)
                        contentY = Math.max(0, Math.min(end, contentY - deltaY * 2 / 3))
                    }

                    // Headers and the seams between lanes: without this the Flickable's
                    // own animated wheel scroll would take them, so the list would move
                    // one way over a waveform and another over a header.
                    WheelHandler {
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onWheel: function (event) {
                            const name = root.keymap ? root.keymap.actionForWheel(event.modifiers) : ""
                            const delta = root.axis ? root.axis.wheelDelta(event) : 0
                            if (name === "")
                                laneScroll.scrollByWheel(delta)
                            else if (root.axis)
                                root.axis.applyWheel(name, delta, event.x - lanes.headerWidth)
                            event.accepted = true
                        }
                    }

                    Column {
                        id: laneColumn
                        width: laneScroll.width
                        spacing: 0

                        Repeater {
                            // Ids, not track objects -- see Backend::trackIds for why.
                            model: root.service ? root.service.trackIdModel : null

                            delegate: TrackLane {
                                id: laneDelegate
                                required property var modelData
                                // Re-read only when this track's data changed: a binding on
                                // the whole track list would rebuild every lane (and its
                                // clips) on any edit in a large session.
                                property int revision: 0
                                Connections {
                                    target: root.service
                                    function onTrackDataChanged(ids) { if (ids.indexOf(laneDelegate.modelData) >= 0) ++laneDelegate.revision }
                                }
                                track: {
                                    laneDelegate.revision
                                    const candidate = root.service ? root.service.trackById(modelData) : null
                                    return candidate && candidate.id !== undefined ? candidate : null
                                }
                                width: laneColumn.width
                                height: track && track.height !== "normal"
                                        ? Theme.laneHeight(track.height)
                                        : lanes.normalHeight
                                service: root.service
                                axis: root.axis
                                headerWidth: lanes.headerWidth
                                pauseAction: root.shell ? root.shell.tidyPausesAction : null
                                shaderUrl: root.shell ? root.shell.shaderUrl : ""
                                wheelResolver: function (modifiers) {
                                    return root.keymap ? root.keymap.actionForWheel(modifiers) : ""
                                }
                                onSeekRequested: function (frame) {
                                    if (root.service)
                                        root.service.seekFrames(frame)
                                }
                                onLaneWheel: function (deltaY) { laneScroll.scrollByWheel(deltaY) }
                                onKeysReleased: if (root.shell) root.shell.restoreFocus()
                                onContextMenuRequested: {
                                    // Keep an existing group when opening its menu.
                                    if (root.service && track)
                                        root.service.focusSelectedTrack(track.id)
                                    if (root.shell)
                                        root.shell.laneContextMenu.popup()
                                }
                            }
                        }
                    }
                }

                MouseArea {
                    id: clipDrag
                    objectName: "clipMoveMouse"
                    anchors.fill: parent
                    z: 4
                    ControlHelp {
                        title: root.shell && root.shell.timelineSlipTool ? qsTr("Slip tool") : qsTr("Move tool")
                        purpose: root.shell && root.shell.timelineSlipTool
                                 ? qsTr("Drag inside a clip to change which part of its source plays, without moving the clip.")
                                 : qsTr("Click to select a clip; Ctrl-click toggles clips. Drag a selected clip to move the group together. Red previews cannot be applied.")
                        scope: qsTr("One Undo step on release. Choose the Select tool to get selection back.")
                        suppressed: clipDrag.pressed
                    }
                    enabled: root.shell !== null && (root.shell.timelineMoveTool || root.shell.timelineSlipTool)
                    // An inactive input overlay must also leave the hover path;
                    // disabled HoverHandlers still observe events in Qt Quick.
                    visible: enabled
                    acceptedButtons: Qt.LeftButton
                    preventStealing: true
                    cursorShape: root.service && root.service.clipMoving
                        ? (root.service.clipMovePreview.valid ? Qt.ClosedHandCursor : Qt.ForbiddenCursor)
                        : Qt.OpenHandCursor
                    property real grabOffset: 0
                    property real grabFrame: 0
                    property real pressX: 0
                    property real pressY: 0
                    property bool moved: false
                    property bool selectionOnly: false
                    property int grabbedTrack: -1

                    function laneAt(x, y) {
                        var point = clipDrag.mapToItem(laneColumn, x, y)
                        var child = laneColumn.childAt(point.x, point.y)
                        return child && child.trackId !== undefined ? child : null
                    }
                    function pointerFrame(x) {
                        return Math.round(root.axis.viewStart + (x - lanes.headerWidth) * root.axis.framesPerPixel)
                    }
                    function updateMove(x, y) {
                        if (!root.service || !root.service.clipMoving)
                            return
                        if (!moved) {
                            if (Math.abs(x - pressX) < Theme.dragThreshold
                                && Math.abs(y - pressY) < Theme.dragThreshold)
                                return
                            moved = true
                        }
                        if (root.shell.timelineSlipTool) {
                            root.service.previewClipSlip(pointerFrame(x) - grabFrame)
                            return
                        }
                        var lane = laneAt(x, y)
                        root.service.previewClipMove(lane ? lane.trackId : -1, pointerFrame(x) - grabOffset, true)
                    }
                    onPressed: function(mouse) {
                        pressX = mouse.x; pressY = mouse.y; moved = false; selectionOnly = false
                        var lane = laneAt(mouse.x, mouse.y)
                        if (!root.service || !root.axis || !lane || mouse.x < lanes.headerWidth) { mouse.accepted = false; return }
                        grabbedTrack = lane.trackId; grabFrame = pointerFrame(mouse.x)
                        if (!root.shell.timelineSlipTool && (mouse.modifiers & Qt.ControlModifier) !== 0) {
                            selectionOnly = true
                            root.service.selectClipAt(lane.trackId, grabFrame, true)
                            return
                        }
                        if (!(root.shell.timelineSlipTool ? root.service.beginClipSlip(lane.trackId, pointerFrame(mouse.x))
                                : root.service.beginClipMove(lane.trackId, pointerFrame(mouse.x)))) {
                            mouse.accepted = false
                            return
                        }
                        grabFrame = pointerFrame(mouse.x)
                        grabOffset = grabFrame - root.service.clipMovePreview.start
                    }
                    onPositionChanged: function(mouse) { if (pressed) updateMove(mouse.x, mouse.y) }
                    onReleased: function(mouse) {
                        updateMove(mouse.x, mouse.y)
                        if (root.service && !selectionOnly && root.service.clipMoving) {
                            if (moved) root.service.finishClipMove()
                            else {
                                root.service.cancelClipMove()
                                if (!root.shell.timelineSlipTool) root.service.selectClipAt(grabbedTrack, grabFrame, false)
                            }
                        }
                        if (root.shell) root.shell.restoreFocus()
                    }
                    onCanceled: if (root.service) root.service.cancelClipMove()
                    onWheel: function(wheel) { wheel.accepted = false }
                    Timer {
                        interval: 40; repeat: true
                        running: clipDrag.pressed && clipDrag.moved && root.service !== null && root.service.clipMoving
                        onTriggered: {
                            if (clipDrag.mouseY < 16)
                                laneScroll.contentY = Math.max(0, laneScroll.contentY - 12)
                            else if (clipDrag.mouseY > clipDrag.height - 16)
                                laneScroll.contentY = Math.max(0, Math.min(laneScroll.contentHeight - laneScroll.height, laneScroll.contentY + 12))
                            if (clipDrag.mouseX < lanes.headerWidth + 16)
                                root.axis.scrollByPixels(-12)
                            else if (clipDrag.mouseX > clipDrag.width - 16)
                                root.axis.scrollByPixels(12)
                            clipDrag.updateMove(clipDrag.mouseX, clipDrag.mouseY)
                        }
                    }
                }

                // The playhead is drawn once, over every lane: it is one position
                // in one timeline. Red, per the brief -- the one place red is allowed
                // besides the record state.
                Rectangle {
                    id: playhead
                    objectName: "playhead"
                    width: Theme.playheadWidth
                    color: Theme.playhead
                    y: 0
                    height: Math.min(laneScroll.height, laneColumn.height)
                    visible: root.service !== null && root.service.trackCount > 0
                             && x >= lanes.headerWidth - 1 && x <= lanes.width + 1
                    x: lanes.headerWidth + (root.service && root.axis ? root.axis.frameToX(root.service.playhead) : 0)
                }

                // Ready and empty: the welcome card, until it is dismissed.
                GettingStartedCard {
                    anchors.fill: parent
                    shell: root.shell
                    visible: root.service !== null && root.service.trackCount === 0
                             && root.service.connected && !root.service.loading
                             && root.shell && !root.shell.gettingStartedDismissed
                }
                EmptyState {
                    anchors.fill: parent
                    visible: (root.service === null || root.service.trackCount === 0)
                             && !(root.service && root.service.connected && !root.service.loading
                                  && root.shell && !root.shell.gettingStartedDismissed)
                    iconName: "audio-lines"
                    title: {
                        if (!root.service) return qsTr("Starting engine…")
                        if (root.service.engineMissing) return qsTr("Engine not found")
                        if (root.service.loading) return qsTr("Reading file…")
                        if (!root.service.connected) return qsTr("Connecting to engine…")
                        return qsTr("No project")
                    }
                    description: root.service && root.service.connected && !root.service.loading
                                 ? qsTr("Open an audio file to start a project, or import one as a track.") : ""
                    actionText: root.service && root.service.connected && !root.service.loading ? qsTr("Open Audio…") : ""
                    onActionTriggered: if (root.shell) root.shell.runAction("open_file")
                }
            }
        }

        MixerDock {
            id: mixerDock
            shell: root.shell
            visible: root.shell ? root.shell.mixerVisible : true
            C.SplitView.preferredWidth: root.shell ? root.shell.mixerWidth : Theme.wMixerDock
            C.SplitView.minimumWidth: 180
            C.SplitView.maximumWidth: 720
            onWidthChanged: if (body.resizing && root.shell) root.shell.mixerWidth = width
        }

        EffectsDock {
            id: effectsDock
            objectName: "effectsDock"
            shell: root.shell
            // The master bus when the mixer's master strip asked for it.
            target: root.shell && root.shell.effectsDockShowsMaster && root.service ? root.service.masterTarget : -1
            visible: root.shell ? root.shell.effectsDockVisible : true
            C.SplitView.preferredWidth: root.shell ? root.shell.rightDockWidth : Theme.wRightDock
            C.SplitView.minimumWidth: 320
            C.SplitView.maximumWidth: 520
            onWidthChanged: if (body.resizing && root.shell) root.shell.rightDockWidth = width
        }
    }
}
