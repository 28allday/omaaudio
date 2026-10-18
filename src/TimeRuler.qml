pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The time ruler over the lanes: ticks and labels on the shared axis, and a
// click-or-drag scrubs the playhead. The tick interval is picked so labels
// never crowd: the smallest step from a musical list that leaves at least
// 80 px between labels at the current zoom.
Rectangle {
    id: root
    objectName: "timeRuler"

    property var axis: null
    property var service: null
    // Pixel offset of the axis origin inside this item (the track header column).
    property real originX: 0

    signal seekRequested(real frame)   // a frame count: past 2^31 on a long take, so not an int

    implicitHeight: Theme.hRuler
    color: Theme.rulerBg
    clip: true

    readonly property real sampleRate: service && service.sampleRate > 0 ? service.sampleRate : 48000
    readonly property real fpp: axis ? Math.max(0.0001, axis.framesPerPixel) : 1
    readonly property real viewStart: axis ? axis.viewStart : 0
    readonly property bool cdFrames: service && service.timeDisplay === "cdframes"

    readonly property bool musical: service && service.timeDisplay === "barsbeats" && service.musicalTime.enabled
    readonly property real musicalStep: {
        if (!root.musical) return 1
        const music = root.service.musicalTime
        const beat = music.ticksPerBeat
        const bar = beat * music.beats
        const needed = root.fpp * 80 * music.tempo * 16 / root.sampleRate
        // Exact divisors of a beat, then whole beats, then whole bars.
        for (const division of [beat, 16, 8, 4, 2, 1]) {
            const ticks = beat / division
            if (Number.isInteger(ticks) && ticks >= needed) return ticks
        }
        if (bar >= needed) return bar
        let bars = 2
        while (bars * bar < needed && bars < 1073741824) bars *= 2
        return bars * bar
    }

    // Seconds per label, from the zoom.
    readonly property real stepSeconds: {
        const steps = root.cdFrames
            ? [1/75, 2/75, 5/75, 10/75, 15/75, 25/75, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600]
            : [0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600]
        const secondsPerPixel = fpp / sampleRate
        for (var i = 0; i < steps.length; ++i) {
            if (steps[i] / secondsPerPixel >= 80)
                return steps[i]
        }
        return steps[steps.length - 1]
    }

    readonly property int minorDivisions: {
        if (root.musical) {
            const music = root.service.musicalTime
            const bar = music.beats * music.ticksPerBeat
            // One bar divides into its beats, including odd meters. At wider
            // zooms, divide into whole bars; close zooms use tick subdivisions.
            if (root.musicalStep >= bar)
                return root.musicalStep === bar ? music.beats : Math.min(8, root.musicalStep / bar)
            return root.musicalStep % 4 === 0 ? 4 : root.musicalStep % 2 === 0 ? 2 : 1
        }
        return !root.cdFrames ? 5 : Math.round(root.stepSeconds * 75) % 5 === 0 ? 5
            : Math.round(root.stepSeconds * 75) === 2 ? 2 : 1
    }

    function cdIndex(sample) {
        return Math.floor(sample / root.sampleRate) * 75
            + Math.floor((sample % root.sampleRate) * 75 / root.sampleRate)
    }

    function cdSample(frame) {
        // Split seconds from frames before scaling. Multiplying an approximate
        // 1/75 second can move a long ruler's tick one sample past its boundary.
        return Math.floor(frame / 75) * root.sampleRate
            + Math.ceil((frame % 75) * root.sampleRate / 75)
    }

    function label(seconds) {
        // Samples or timecode when that is the chosen display; the ruler's own
        // compact clock otherwise.
        if (root.service && root.service.timeDisplay !== "time")
            return root.service.formatTime(Math.round(seconds * root.sampleRate))
        const total = Math.floor(seconds)
        const h = Math.floor(total / 3600)
        const m = Math.floor((total % 3600) / 60)
        const s = total % 60
        const frac = seconds - total
        var out = (h > 0 ? h + ":" : "") + (m < 10 && h > 0 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
        if (root.stepSeconds < 1)
            out += "." + Math.round(frac * 100).toString().padStart(2, "0")
        return out
    }

    // The tick model is rebuilt when the view moves; it is a handful of numbers.
    readonly property var ticks: {
        if (!axis || root.width <= 0)
            return []
        const viewWidth = root.width - root.originX
        if (root.musical) {
            // Depend on timing as well as zoom, even when the chosen step stays.
            void root.service.musicalTime; void root.sampleRate
            const firstTick = Math.floor(root.service.musicalTickAt(Math.floor(root.viewStart)) / root.musicalStep) * root.musicalStep
            const lastFrame = root.viewStart + root.fpp * viewWidth
            const ticks = []
            for (let tick = Math.max(0, firstTick); ticks.length < 400; tick += root.musicalStep) {
                const sample = root.service.sampleAtMusicalTick(tick)
                if (sample < 0 || sample > lastFrame) break
                ticks.push(sample)
            }
            return ticks
        }
        const cdStep = Math.round(root.stepSeconds * 75)
        const first = root.cdFrames ? Math.floor(root.cdIndex(Math.floor(root.viewStart)) / cdStep)
            : Math.floor(root.viewStart / root.sampleRate / root.stepSeconds)
        const last = root.viewStart + root.fpp * viewWidth
        var out = []
        for (var index = first; out.length < 400; ++index) {
            const frame = root.cdFrames ? root.cdSample(index * cdStep) : index * root.stepSeconds * root.sampleRate
            if (frame > last) break
            out.push(frame)
        }
        return out
    }

    // The chosen editing grid is visible in every time display. At wide zooms
    // show a bounded subset of exact grid points instead of a solid line.
    readonly property var snapTicks: {
        if (!axis || !service || !service.musicalSnapActive || width <= originX) return []
        void service.musicalTime; void root.sampleRate
        const grid = service.snapGridTicks
        let step = grid
        const spacing = root.sampleRate * grid / (service.musicalTime.tempo * 16)
        const pixels = Math.max(6, (width - originX) / 380)
        while (spacing * (step / grid) / root.fpp < pixels && step < 1125899906842624) step *= 2
        const first = Math.max(0, Math.floor(service.musicalTickAt(Math.max(0, Math.floor(viewStart))) / step) * step)
        const last = viewStart + root.fpp * (width - originX)
        const result = []
        for (let tick = first; result.length < 400; tick += step) {
            const sample = service.sampleAtMusicalTick(tick)
            if (sample < 0 || sample > last) break
            result.push(sample)
        }
        return result
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    Item {
        x: root.originX
        width: root.width - root.originX
        height: root.height
        clip: true

        Repeater {
            model: root.ticks
            delegate: Item {
                id: tick
                required property real modelData
                x: root.axis ? root.axis.frameToX(modelData) : 0
                height: root.height
                Rectangle {
                    y: tick.height - height
                    width: Theme.hairline; height: 7
                    color: Theme.rulerTick
                }
                // Four minor ticks between labels. Positioned by id rather than
                // through `parent`: a Repeater's delegates lose their parent while
                // the outer delegate is being torn down, and an anchor on null
                // logs a TypeError per tick per rebuild.
                Repeater {
                    model: root.minorDivisions - 1
                    Rectangle {
                        required property int index
                        x: {
                            // Start from the rational CD boundary, not its rounded
                            // sample, so minor ticks do not inherit a sample error.
                            const at = root.musical
                                ? root.service.sampleAtMusicalTick(root.service.musicalTickAt(tick.modelData) + (index + 1) * root.musicalStep / root.minorDivisions)
                                : root.cdFrames
                                ? root.cdSample(root.cdIndex(tick.modelData) + (index + 1) * Math.round(root.stepSeconds * 75) / root.minorDivisions)
                                : tick.modelData + (index + 1) * root.stepSeconds * root.sampleRate / root.minorDivisions
                            return (at - tick.modelData) / root.fpp
                        }
                        y: tick.height - height
                        width: Theme.hairline; height: 3
                        color: Theme.border
                    }
                }
                Text {
                    x: Theme.s1
                    y: 2
                    text: { void root.service?.musicalTime; return root.label(tick.modelData / root.sampleRate) }
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsRuler
                    font.features: { "tnum": 1 }
                    color: Theme.rulerText
                }
            }
        }

        Repeater {
            model: root.snapTicks
            Rectangle {
                required property real modelData
                x: root.axis ? root.axis.frameToX(modelData) : 0
                y: root.height - height
                width: Theme.hairline; height: 4
                color: Theme.accent
            }
        }

        MouseArea {
            id: scrub
            objectName: "rulerScrub"
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            ControlHelp {
                title: qsTr("Time ruler")
                purpose: qsTr("Click or drag along the ruler to move the playhead.")
                suppressed: scrub.pressed
            }
            function seekAt(x) {
                if (root.axis)
                    root.seekRequested(Math.max(0, Math.round(root.axis.xToFrame(x))))
            }
            onPressed: function (e) { seekAt(e.x) }
            onPositionChanged: function (e) { if (pressed) seekAt(e.x) }
        }
    }
}
