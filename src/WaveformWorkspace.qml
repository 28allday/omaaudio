// Delegates below reach the workspace's own ids.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The Waveform workspace: the focused take, one lane per channel, precise
// editing. Files | tabs + toolbar + ruler + markers + channel lanes + overview
// | Track Effects. The same engine, the same selection and the same actions as
// the Multitrack page -- this is the same session seen one take at a time,
// with the amplitude scale and the overview the single-file job wants.
Item {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var axis: shell ? shell.timeAxis : null
    readonly property var keymap: shell ? shell.keymap : null

    // The focused track is the take on show.
    readonly property var track: (service && service.tracksRevision >= 0 && service.focusedTrack >= 0)
                                 ? service.trackById(service.focusedTrack) : null
    readonly property int trackId: track ? track.id : -1
    readonly property var levels: track ? track.levels : []
    // Lanes follow the take's own channels, not the peak texture's (which is
    // always the output's two): a mono take is one lane.
    readonly property int channels: {
        if (!track || !levels || levels.length === 0)
            return 0
        const own = track.channels !== undefined ? track.channels : 0
        if (own > 0)
            return own
        const c = levels[0].channels
        return c !== undefined && c > 0 ? c : 1
    }
    readonly property color tint: (track && track.color !== "") ? track.color
                                : Theme.roleColor(track ? track.role : "dialogue")

    function channelLabel(c) {
        if (root.channels === 1) return qsTr("M")
        if (root.channels === 2) return c === 0 ? qsTr("L") : qsTr("R")
        return String(c + 1)
    }

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

        // ── editor ──────────────────────────────────────────────────────
        Item {
            id: editor
            C.SplitView.fillWidth: true
            C.SplitView.minimumWidth: 480

            // Document tabs: every take in the session; the checked one is focused.
            DocumentTabs {
                id: docTabs
                objectName: "docTabs"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                shell: root.shell
                tabObjectName: "docTab"
                tabScope: qsTr("Edits on the Take page act on this track. Its audio is unchanged until you perform an edit.")
                statusVisible: root.track !== null
                statusText: root.track ? Theme.roleLabel(root.track.role) + "  ·  "
                            + (root.channels === 1 ? qsTr("mono") : root.channels === 2 ? qsTr("stereo") : qsTr("%1 channels").arg(root.channels))
                          : ""
            }

            WaveformToolbar {
                id: toolbar
                anchors.top: docTabs.bottom
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
                originX: Theme.wAmpRuler
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
                originX: Theme.wAmpRuler
                onSeekRequested: function (frame) { if (root.service) root.service.seekFrames(frame) }
            }

            // One lane per channel, sharing the height.
            Rectangle {
                id: lanes
                objectName: "channelLanes"
                anchors.top: markerLane.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: overview.top
                color: Theme.windowBg

                // This page owns the shared axis while it is up: the lanes' waveform
                // column is the window's, minus the amplitude ruler.
                function syncAxis() {
                    if (root.axis)
                        root.axis.viewWidth = Math.max(0, lanes.width - Theme.wAmpRuler)
                }
                onWidthChanged: syncAxis()
                Component.onCompleted: syncAxis()

                Column {
                    anchors.fill: parent
                    spacing: 0
                    Repeater {
                        model: root.channels
                        delegate: Item {
                            id: chLane
                            objectName: "channelLane"
                            required property int index
                            width: lanes.width
                            height: lanes.height / Math.max(1, root.channels)

                            AmplitudeRuler {
                                id: amp
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: Theme.hairline
                                label: root.channelLabel(chLane.index)
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: if (root.service) root.service.selectionChannel = root.service.selectionChannel === chLane.index ? -1 : chLane.index
                                    ControlHelp {
                                        title: qsTr("Channel %1").arg(root.channelLabel(chLane.index))
                                        purpose: qsTr("Click to limit edits to this channel only; click again for both.")
                                        scope: qsTr("The selection channel is shown in the status bar.")
                                    }
                                }
                                tint: root.tint
                            }

                            Rectangle {
                                anchors.left: amp.right
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: Theme.hairline
                                color: Theme.laneFocusedBg
                                clip: true

                                Waveform {
                                    objectName: "channelWaveform"
                                    anchors.fill: parent
                                    service: root.service
                                    axis: root.axis
                                    shaderUrl: root.shell ? root.shell.shaderUrl : ""
                                    trackId: root.trackId
                                    channel: chLane.index
                                    levels: root.levels
                                    trackFrames: root.track ? root.track.frames : 0
                                    focused: true
                                    silenced: root.track ? root.track.silenced === true : false
                                    waveColor: root.tint
                                    selectionColor: Theme.accent
                                    wheelResolver: function (modifiers) {
                                        return root.keymap ? root.keymap.actionForWheel(modifiers) : ""
                                    }
                                    onSeekRequested: function (frame) { if (root.service) root.service.seekFrames(frame) }
                                    onSelectionDragged: function (startFrame, endFrame, finished) {
                                        if (!root.service)
                                            return
                                        if (startFrame === endFrame)
                                            root.service.clearSelection()
                                        else
                                            root.service.select(startFrame, endFrame, true, root.trackId)
                                    }
                                    onWheelAction: function (name, deltaY, x) {
                                        if (!root.axis)
                                            return
                                        root.axis.applyWheel(name, deltaY, x)
                                    }
                                }
                                // The pauses Tidy pauses found; the tags on the top channel only.
                                PauseShading {
                                    anchors.fill: parent
                                    service: root.service
                                    axis: root.axis
                                    trackId: root.trackId
                                    action: root.shell ? root.shell.tidyPausesAction : null
                                    showTags: chLane.index === 0
                                    tagY: Theme.s1
                                }
                            }

                            // The seam.
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width; height: Theme.hairline
                                color: Theme.border
                            }

                            // Right-click: the same lane menu as Multitrack.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onPressed: if (root.shell) root.shell.laneContextMenu.popup()
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "wavePlayhead"
                    width: Theme.playheadWidth
                    color: Theme.playhead
                    y: 0; height: lanes.height
                    x: Theme.wAmpRuler + (root.service && root.axis ? root.axis.frameToX(root.service.playhead) : 0)
                    visible: root.track !== null && x >= Theme.wAmpRuler - 1 && x <= lanes.width + 1
                }

                EmptyState {
                    anchors.fill: parent
                    visible: root.track === null
                    iconName: "audio-waveform"
                    title: !root.service ? qsTr("Starting engine…")
                         : root.service.loading ? qsTr("Reading file…")
                         : root.service.trackCount === 0 ? qsTr("No take open") : qsTr("No active track")
                    description: root.service && !root.service.loading && root.service.trackCount === 0
                                 ? qsTr("Open an audio file to edit it here, one take at a time.") : ""
                    actionText: root.service && !root.service.loading && root.service.trackCount === 0 ? qsTr("Open Audio…") : ""
                    onActionTriggered: if (root.shell) root.shell.runAction("open_file")
                }
            }

            OverviewStrip {
                id: overview
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                service: root.service
                mainAxis: root.axis
                trackId: root.trackId
                levels: root.levels
                trackFrames: root.track ? root.track.frames : 0
                shaderUrl: root.shell ? root.shell.shaderUrl : ""
                tint: root.tint
                originX: Theme.wAmpRuler
                visible: root.track !== null
            }
        }

        EffectsDock {
            id: effectsDock
            objectName: "effectsDock"
            shell: root.shell
            visible: root.shell ? root.shell.effectsDockVisible : true
            C.SplitView.preferredWidth: root.shell ? root.shell.rightDockWidth : Theme.wRightDock
            C.SplitView.minimumWidth: 320
            C.SplitView.maximumWidth: 520
            onWidthChanged: if (body.resizing && root.shell) root.shell.rightDockWidth = width
        }
    }
}
