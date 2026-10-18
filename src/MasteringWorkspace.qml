// Delegates below reach the workspace's own ids.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The Mastering workspace: Source / Queue | toolbar + ruler + markers + the
// mastered mix, one lane per channel, with its loudness history under it and
// the delivery along the bottom | the master bus chain, the bus, and the R128
// meter. The mix is what the transport plays: the tracks through the bus trim
// and the master inserts, measured before the monitor level.
Item {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var axis: shell ? shell.timeAxis : null
    readonly property var keymap: shell ? shell.keymap : null

    readonly property bool loaded: service !== null && service.trackCount > 0
    readonly property var levels: service ? service.masterLevels : []
    readonly property int channels: 2
    readonly property color tint: Theme.roleColor("master")

    function channelLabel(c) { return c === 0 ? qsTr("L") : qsTr("R") }

    // Analyse on arrival, and again a moment after the mix changes while the
    // page is up -- debounced, because an analysis is a full render.
    function analyseIfStale() {
        if (root.service && root.loaded && root.service.analysisStale && !root.service.analysing
            && !root.service.rendering && !root.service.loading)
            root.service.analyseMaster()
    }
    Component.onCompleted: analyseIfStale()
    Timer {
        id: reanalyse
        interval: 1500
        repeat: false
        onTriggered: root.analyseIfStale()
    }
    Connections {
        target: root.service
        function onAnalysisChanged() {
            if (root.service && root.service.analysisStale && !root.service.analysing)
                reanalyse.restart()
        }
        function onTakeChanged() { reanalyse.restart() }
    }

    C.SplitView {
        id: body
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: SplitterHandle {
            pressed: C.SplitHandle.pressed
            hovered: C.SplitHandle.hovered
        }

        MasteringSourceDock {
            id: sourceDock
            shell: root.shell
            visible: root.shell ? root.shell.leftDockVisible : true
            C.SplitView.preferredWidth: root.shell ? root.shell.leftDockWidth : Theme.wLeftDock
            C.SplitView.minimumWidth: 230
            C.SplitView.maximumWidth: 420
            onWidthChanged: if (body.resizing && root.shell) root.shell.leftDockWidth = width
        }

        // ── the mix ─────────────────────────────────────────────────────
        Item {
            id: editor
            C.SplitView.fillWidth: true
            C.SplitView.minimumWidth: 480

            MasteringToolbar {
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
                objectName: "masterLanes"
                anchors.top: markerLane.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: history.top
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
                        model: root.channels
                        delegate: Item {
                            id: chLane
                            objectName: "masterLane"
                            required property int index
                            width: lanes.width
                            height: lanes.height / root.channels

                            AmplitudeRuler {
                                id: amp
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: Theme.hairline
                                label: root.channelLabel(chLane.index)
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
                                    objectName: "masterWaveform"
                                    anchors.fill: parent
                                    service: root.service
                                    axis: root.axis
                                    shaderUrl: root.shell ? root.shell.shaderUrl : ""
                                    trackId: -1
                                    channel: chLane.index
                                    levels: root.levels
                                    trackFrames: root.service ? root.service.masterFrames : 0
                                    focused: true
                                    waveColor: root.tint
                                    selectionColor: Theme.accent
                                    // A stale analysis still draws, dimmed: it is the
                                    // last known shape, not the current one.
                                    opacity: root.service && root.service.analysisStale ? 0.45 : 1
                                    enabled: false
                                }

                                // The mix cannot be selected or edited here; a click
                                // seeks, the wheel zooms and scrolls like any lane.
                                MouseArea {
                                    objectName: "masterMouse"
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton
                                    cursorShape: Qt.PointingHandCursor
                                    ControlHelp {
                                        title: qsTr("Mix")
                                        purpose: qsTr("The whole mix as it will be exported. Click to place the playhead; the wheel scrolls and, with Ctrl, zooms.")
                                        scope: qsTr("The mix cannot be selected or edited here; edit the tracks on the other pages.")
                                    }
                                    onClicked: function (e) {
                                        if (root.service && root.axis)
                                            root.service.seekFrames(Math.round(root.axis.xToFrame(e.x)))
                                    }
                                }
                                WheelHandler {
                                    target: null
                                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                    onWheel: function (event) {
                                        if (!root.axis || !root.keymap)
                                            return
                                        const name = root.keymap.actionForWheel(event.modifiers)
                                        if (name === "zoom")
                                            root.axis.zoomAt(event.x, event.angleDelta.y > 0 ? 1 / 1.15 : 1.15)
                                        else if (name === "scroll_horizontal")
                                            root.axis.scrollByPixels(event.angleDelta.y > 0 ? -80 : 80)
                                        event.accepted = true
                                    }
                                }
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width; height: Theme.hairline
                                color: Theme.border
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "masterPlayhead"
                    width: Theme.playheadWidth
                    color: Theme.playhead
                    y: 0; height: lanes.height
                    x: Theme.wAmpRuler + (root.service && root.axis ? root.axis.frameToX(root.service.playhead) : 0)
                    visible: root.loaded && x >= Theme.wAmpRuler - 1 && x <= lanes.width + 1
                }

                // Over the lanes while there is no shape to draw yet.
                Text {
                    anchors.centerIn: parent
                    visible: root.loaded && (!root.service.analysed || root.levels.length === 0)
                    text: root.service && root.service.analysing ? qsTr("Measuring the mix…")
                        : qsTr("Analyse the mix to see it here")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                }

                EmptyState {
                    anchors.fill: parent
                    visible: !root.loaded
                    iconName: "gauge"
                    title: !root.service ? qsTr("Starting engine…")
                         : root.service.loading ? qsTr("Reading file…")
                         : qsTr("No project to master")
                    description: root.service && !root.service.loading
                                 ? qsTr("Open an audio file or a project. The whole mix, through the master bus, is what is mastered and delivered here.") : ""
                    actionText: root.service && !root.service.loading ? qsTr("Open Audio…") : ""
                    onActionTriggered: if (root.shell) root.shell.runAction("open_file")
                }
            }

            LoudnessHistory {
                id: history
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: delivery.top
                shell: root.shell
                originX: Theme.wAmpRuler
            }

            DeliveryBar {
                id: delivery
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                shell: root.shell
            }
        }

        EffectsDock {
            id: masterRack
            objectName: "masterRack"
            shell: root.shell
            target: root.service ? root.service.masterTarget : -2
            visible: root.shell ? root.shell.effectsDockVisible : true
            C.SplitView.preferredWidth: root.shell ? root.shell.rightDockWidth : Theme.wRightDock
            C.SplitView.minimumWidth: 320
            C.SplitView.maximumWidth: 520
            onWidthChanged: if (body.resizing && root.shell) root.shell.rightDockWidth = width
        }
    }
}
