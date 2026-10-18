// Delegates below reach the workspace's own ids.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The Spectral workspace: the focused take as a time-frequency display, one
// lane per channel, on the same session, axis, selection and takes as the
// Waveform page -- plus a frequency band beside the time selection, and the
// repairs that act on both. Files | tabs + toolbar + ruler + markers + lanes +
// overview | Repair panel.
Item {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var axis: shell ? shell.timeAxis : null
    readonly property var keymap: shell ? shell.keymap : null

    readonly property var track: (service && service.tracksRevision >= 0 && service.focusedTrack >= 0)
                                 ? service.trackById(service.focusedTrack) : null
    readonly property int trackId: track ? track.id : -1
    readonly property var levels: track ? track.levels : []
    readonly property var spectral: service ? service.spectral : null
    // The cache is for this take when it says so; while it is for another, or
    // not built, the lanes wait.
    readonly property bool spectralReady: spectral !== null && spectral.ready === true
                                          && spectral.track === root.trackId
                                          && !!spectral.midSide === (root.service.spectralView === "mid-side" && root.track.channels === 2)
    readonly property int channels: {
        if (!track)
            return 0
        if (root.spectralReady)
            return spectral.channels
        const own = track.channels !== undefined ? track.channels : 0
        return own > 0 ? own : 1
    }
    readonly property bool overlaid: channels === 2 && service && service.spectralView === "overlay"
    readonly property int laneCount: overlaid ? 1 : channels
    readonly property color tint: (track && track.color !== "") ? track.color
                                : Theme.roleColor(track ? track.role : "dialogue")

    function channelLabel(c) {
        if (root.overlaid) return qsTr("L + R")
        if (root.channels === 2 && root.service && root.service.spectralView === "mid-side")
            return c === 0 ? qsTr("Mid") : qsTr("Side")
        if (root.channels === 1) return qsTr("M")
        if (root.channels === 2) return c === 0 ? qsTr("L") : qsTr("R")
        return String(c + 1)
    }

    // The engine analyses only while somebody is looking.
    Component.onCompleted: if (root.service) root.service.spectralWanted = true
    Component.onDestruction: if (root.service) root.service.spectralWanted = false

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

        Item {
            id: editor
            C.SplitView.fillWidth: true
            C.SplitView.minimumWidth: 480

            DocumentTabs {
                id: docTabs
                objectName: "spectralDocTabs"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                shell: root.shell
                tabObjectName: "spectralDocTab"
                tabScope: qsTr("Spectral selections and repairs act on this track. Switching tracks does not change audio.")
                statusVisible: root.track !== null
                statusText: root.spectralReady
                            ? qsTr("%1-point %2 · %3").arg(root.spectral.fftSize).arg(root.service.spectralWindow)
                                  .arg(root.channels === 1 ? qsTr("mono") : root.channels === 2 ? qsTr("stereo") : qsTr("%1 channels").arg(root.channels))
                            : root.service && root.service.spectralBuilding
                              ? qsTr("Building %1 %").arg(Math.round(root.service.spectralProgress * 100))
                              : ""
            }

            SpectralToolbar {
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

            Rectangle {
                id: lanes
                objectName: "spectralLanes"
                anchors.top: markerLane.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: overview.top
                color: Theme.windowBg

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
                        model: root.track ? root.laneCount : 0
                        delegate: Item {
                            id: chLane
                            objectName: "spectralLane"
                            required property int index
                            width: lanes.width
                            height: lanes.height / Math.max(1, root.laneCount)

                            FrequencyRuler {
                                id: freq
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: Theme.hairline
                                label: root.channelLabel(chLane.index)
                                tint: root.tint
                                frequencyScale: root.shell ? root.shell.spectralScale : "log"
                                nyquist: root.service ? root.service.sampleRate / 2 : 24000
                            }

                            Rectangle {
                                anchors.left: freq.right
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: Theme.hairline
                                color: Theme.spectralBg
                                clip: true

                                Spectrogram {
                                    objectName: "spectrogram"
                                    anchors.fill: parent
                                    service: root.service
                                    axis: root.axis
                                    shaderUrl: root.shell ? root.shell.spectroShaderUrl : ""
                                    spectral: root.spectralReady ? root.spectral : null
                                    channel: chLane.index
                                    overlayChannel: root.overlaid ? 1 : -1
                                    trackId: root.trackId
                                    trackFrames: root.track ? root.track.frames : 0
                                    frequencyScale: root.shell ? root.shell.spectralScale : "log"
                                    rangeDb: root.shell ? root.shell.spectralRange : 60
                                    gainDb: root.shell ? root.shell.spectralGain : 0
                                    stops: Theme.spectralPalette(root.shell ? root.shell.spectralPalette : "navy")
                                    tool: root.shell ? root.shell.spectralTool : "rect"
                                    snapping: root.shell ? root.shell.snapping : true
                                    brushRadius: root.shell ? root.shell.spectralBrushSize : 24
                                    maskSubtract: root.shell ? root.shell.spectralMaskSubtract : false
                                    maskIntersect: root.shell ? root.shell.spectralMaskIntersect : false
                                    harmonicPartials: root.shell ? root.shell.spectralHarmonicPartials : 16
                                    harmonicFundamental: root.shell ? root.shell.spectralHarmonicFundamental : true
                                    onHarmonicsPicked: function (fromSeconds, toSeconds, hz, subtract, intersect, seedSeconds) {
                                        if (root.shell)
                                            root.shell.addHarmonics(fromSeconds, toSeconds, hz, subtract, intersect, seedSeconds)
                                    }
                                    wheelResolver: function (modifiers) {
                                        return root.keymap ? root.keymap.actionForWheel(modifiers) : ""
                                    }
                                    onSeekRequested: function (frame) { if (root.service) root.service.seekFrames(frame) }
                                    onWheelAction: function (name, deltaY, x) {
                                        if (!root.axis)
                                            return
                                        root.axis.applyWheel(name, deltaY, x)
                                    }
                                }
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width; height: Theme.hairline
                                color: Theme.border
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onPressed: if (root.shell) root.shell.laneContextMenu.popup()
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "spectralPlayhead"
                    width: Theme.playheadWidth
                    color: Theme.playhead
                    y: 0; height: lanes.height
                    x: Theme.wAmpRuler + (root.service && root.axis ? root.axis.frameToX(root.service.playhead) : 0)
                    visible: root.track !== null && x >= Theme.wAmpRuler - 1 && x <= lanes.width + 1
                }

                // The cache is being built (or is for another take): say so over
                // the lanes rather than leaving them black.
                Rectangle {
                    objectName: "spectralBuildingNote"
                    visible: root.track !== null && !root.spectralReady
                    anchors.centerIn: parent
                    width: buildingRow.implicitWidth + Theme.s4 * 2
                    height: buildingRow.implicitHeight + Theme.s3 * 2
                    color: Theme.panelRaised
                    border.width: Theme.hairline
                    border.color: Theme.border
                    radius: Theme.rMenu
                    Row {
                        id: buildingRow
                        anchors.centerIn: parent
                        spacing: Theme.s2
                        Icon { name: "loader"; size: Theme.szIcon; color: Theme.warning; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.service && root.service.spectralBuilding
                                  ? qsTr("Building the spectral display… %1 %").arg(Math.round(root.service.spectralProgress * 100))
                                  : qsTr("Spectral display not built yet")
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                            color: Theme.textPrimary
                        }
                    }
                }

                EmptyState {
                    anchors.fill: parent
                    visible: root.track === null
                    iconName: "waves"
                    title: !root.service ? qsTr("Starting engine…")
                         : root.service.loading ? qsTr("Reading file…")
                         : root.service.trackCount === 0 ? qsTr("No take open") : qsTr("No active track")
                    description: root.service && !root.service.loading && root.service.trackCount === 0
                                 ? qsTr("Open an audio file to see its spectrum here, one take at a time.") : ""
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

        SpectralDock {
            id: dock
            shell: root.shell
            visible: root.shell ? root.shell.effectsDockVisible : true
            C.SplitView.preferredWidth: root.shell ? root.shell.rightDockWidth : Theme.wRightDock
            C.SplitView.minimumWidth: 320
            C.SplitView.maximumWidth: 520
            onWidthChanged: if (body.resizing && root.shell) root.shell.rightDockWidth = width
        }
    }
}
