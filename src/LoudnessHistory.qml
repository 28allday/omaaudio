pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The programme's loudness over time, under the mix on the shared axis: the
// short-term reading as a line, the momentary one faintly behind it, the
// delivery target as a band. Drawn on a Canvas from the analysed history --
// thousands of points for an hour, which is a path, not a Repeater.
Rectangle {
    id: root
    objectName: "loudnessHistory"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var axis: shell ? shell.timeAxis : null
    property real originX: Theme.wAmpRuler

    readonly property var shortTerm: service ? service.shortTermHistory : []
    readonly property var momentary: service ? service.momentaryHistory : []
    readonly property int hop: service ? service.historyHopFrames : 4800
    readonly property bool stale: service ? service.analysisStale : true
    readonly property bool analysed: service ? service.analysed : false
    readonly property real target: shell ? shell.deliveryTargetLufs : -16
    readonly property bool loudnessMode: shell ? shell.deliveryNormalise === "loudness" : true

    implicitHeight: Theme.hLoudnessHistory
    color: Theme.panelBg

    Rectangle {
        anchors.top: parent.top
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    // The scale column, like the amplitude ruler beside a lane.
    Rectangle {
        id: scale
        x: 0; y: Theme.hairline
        width: root.originX
        height: parent.height - Theme.hairline
        color: Theme.panelBg
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Theme.hairline
            color: Theme.border
        }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s1
            anchors.top: parent.top
            anchors.topMargin: 2
            text: qsTr("LUFS")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.loudnessShortTerm
        }
        Repeater {
            model: [-12, -24, -36]
            Item {
                required property int modelData
                readonly property real at: scale.height * (1 - Theme.lufsFraction(modelData))
                width: scale.width
                Rectangle { x: scale.width - 5; y: parent.at; width: 4; height: 1; color: Theme.rulerTick }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    y: parent.at - height / 2
                    text: String(parent.modelData)
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsRuler
                    font.features: { "tnum": 1 }
                    color: Theme.rulerText
                }
            }
        }
    }

    Item {
        id: field
        x: root.originX
        y: Theme.hairline
        width: root.width - root.originX
        height: root.height - Theme.hairline
        clip: true

        Canvas {
            id: canvas
            objectName: "loudnessCanvas"
            anchors.fill: parent
            opacity: root.stale && root.analysed ? 0.45 : 1

            readonly property real viewStart: root.axis ? root.axis.viewStart : 0
            readonly property real fpp: root.axis ? root.axis.framesPerPixel : 1
            onViewStartChanged: requestPaint()
            onFppChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            Connections {
                target: root
                function onShortTermChanged() { canvas.requestPaint() }
                function onTargetChanged() { canvas.requestPaint() }
                function onLoudnessModeChanged() { canvas.requestPaint() }
            }

            function yOf(lufs) {
                return height * (1 - Theme.lufsFraction(lufs))
            }

            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                if (!root.axis)
                    return

                // The target band: +-1 LU around the delivery target.
                if (root.loudnessMode) {
                    const top = yOf(root.target + 1)
                    const bottom = yOf(root.target - 1)
                    ctx.fillStyle = Theme.loudnessBand
                    ctx.fillRect(0, top, width, Math.max(1, bottom - top))
                    ctx.strokeStyle = Theme.loudnessTarget
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    ctx.moveTo(0, Math.round(yOf(root.target)) + 0.5)
                    ctx.lineTo(width, Math.round(yOf(root.target)) + 0.5)
                    ctx.stroke()
                }

                const st = root.shortTerm
                const mo = root.momentary
                if (!st || st.length === 0 || root.hop <= 0)
                    return

                // Only what is in view: point i ends at frame (i + 1) * hop.
                const first = Math.max(0, Math.floor(root.axis.viewStart / root.hop) - 2)
                const last = Math.min(st.length - 1, Math.ceil(root.axis.viewEnd / root.hop) + 1)
                if (last < first)
                    return

                function trace(series, colour, widthPx) {
                    ctx.strokeStyle = colour
                    ctx.lineWidth = widthPx
                    ctx.beginPath()
                    var pen = false
                    for (var i = first; i <= last; ++i) {
                        const v = series[i]
                        if (v === undefined || v <= -100) {
                            pen = false
                            continue
                        }
                        const x = root.axis.frameToX((i + 1) * root.hop)
                        const y = yOf(v)
                        if (!pen) {
                            ctx.moveTo(x, y)
                            pen = true
                        } else {
                            ctx.lineTo(x, y)
                        }
                    }
                    ctx.stroke()
                }
                if (mo && mo.length === st.length)
                    trace(mo, Theme.loudnessMomentary, 1)
                trace(st, Theme.loudnessShortTerm, 1.5)
            }
        }

        // The playhead, in step with the lanes above.
        Rectangle {
            width: Theme.playheadWidth
            color: Theme.playhead
            y: 0; height: parent.height
            x: root.service && root.axis ? root.axis.frameToX(root.service.playhead) : 0
            visible: root.service !== null && root.service.trackCount > 0 && x >= -1 && x <= parent.width + 1
        }

        Text {
            objectName: "loudnessHistoryNote"
            anchors.centerIn: parent
            visible: !root.analysed || (root.stale && !root.service.analysing)
            text: !root.service || root.service.trackCount === 0 ? qsTr("Open a project to see its loudness over time")
                : root.service.analysing ? qsTr("Analysing…")
                : !root.analysed ? qsTr("Analyse the mix to see its loudness over time")
                : qsTr("Mix changed — analyse again")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }
}
