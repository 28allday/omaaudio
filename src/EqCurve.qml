pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The frequency curve of an equaliser or filter (Backend::pluginResponse): what
// the effect does to each frequency at its current settings, measured, drawn over
// a log frequency grid. Each band the effect's controls describe is a numbered
// handle: drag it sideways for its frequency and up or down for its gain; the
// wheel, or a drag with Shift held, changes its width (Q). A drag is one edit,
// like a slider's. A band that is switched off shows as a hollow handle.
Item {
    id: root
    objectName: "eqCurve"
    implicitHeight: 190

    property var service: null
    property int selected: -1
    property int target: -1
    // The slot's port descriptors (Backend.pluginPorts); the curve reads its own.
    property var ports: []

    property var response: ({})
    readonly property var frequencies: response.frequencies || []
    readonly property var levels: response.db || []
    readonly property var bands: response.bands || []
    readonly property bool bypassed: response.bypassed === true

    // The band under the pointer or being dragged, for the readout.
    property int activeBand: -1
    property real hoverX: -1

    // Asked again when the settings or the measurement change, at most about
    // thirty times a second.
    function refresh() { if (!throttle.running) throttle.start() }
    Timer {
        id: throttle
        interval: 33
        onTriggered: if (root.service && root.selected >= 0) root.response = root.service.pluginResponse(root.selected, root.target)
    }
    Connections {
        target: root.service
        function onPluginResponseChanged() { root.refresh() }
        function onPluginValuesChanged() { root.refresh() }
        function onPluginPortsChanged() { root.refresh() }
    }
    onSelectedChanged: refresh()
    onTargetChanged: refresh()
    Component.onCompleted: refresh()

    // ── scales ──────────────────────────────────────────────────────────────
    readonly property real low: Math.log(20)
    readonly property real high: Math.log(20000)
    readonly property real leftPad: 34
    readonly property real bottomPad: 16
    readonly property real plotWidth: Math.max(1, width - leftPad - 6)
    readonly property real plotHeight: Math.max(1, height - bottomPad - 6)
    // ±12, ±24 or ±48 dB: the smallest that holds the bands' boosts and cuts and
    // any boost in the curve. A low cut's slope or a notch runs off the bottom,
    // as it should: the scale is for what can be set.
    readonly property real range: {
        let most = 0
        for (const b of bands) if (b.gainPort >= 0) most = Math.max(most, Math.abs(gainDb(b)))
        for (const d of levels) most = Math.max(most, d)
        return most <= 11 ? 12 : most <= 23 ? 24 : 48
    }
    function xOf(f) { return leftPad + (Math.log(Math.max(1, f)) - low) / (high - low) * plotWidth }
    function frequencyAt(x) { return Math.exp(low + (x - leftPad) / plotWidth * (high - low)) }
    function yOf(d) { return 6 + (range - Math.max(-range, Math.min(range, d))) / (2 * range) * plotHeight }
    // Unclamped, for the curve: drawn clipped to the plot.
    function yRaw(d) { return 6 + (range - Math.max(-4 * range, Math.min(4 * range, d))) / (2 * range) * plotHeight }
    function dbAt(y) { return range - (y - 6) / plotHeight * 2 * range }
    function gainDb(b) { return b.gainLinear ? 20 * Math.log(Math.max(1e-6, b.gain)) / Math.LN10 : b.gain }
    // The measured gain at a frequency, between the points.
    function curveAt(f) {
        const fs = frequencies, ds = levels
        if (!fs.length) return 0
        for (let i = 1; i < fs.length; ++i)
            if (fs[i] >= f) {
                const t = (Math.log(f) - Math.log(fs[i - 1])) / (Math.log(fs[i]) - Math.log(fs[i - 1]))
                return ds[i - 1] + t * (ds[i] - ds[i - 1])
            }
        return ds[ds.length - 1]
    }
    function hz(f) {
        return f >= 1000 ? qsTr("%1 kHz").arg((f / 1000).toFixed(f >= 10000 ? 1 : 2))
                         : qsTr("%1 Hz").arg(f >= 100 ? Math.round(f) : f.toFixed(1))
    }
    function db(d) { return qsTr("%1 dB").arg((d > 0.05 ? "+" : "") + d.toFixed(1)).replace("-", "−") }
    readonly property string readout: {
        if (activeBand >= 0 && activeBand < bands.length) {
            const b = bands[activeBand]
            const parts = [hz(b.freq), b.gainPort >= 0 ? db(gainDb(b)) : db(curveAt(b.freq))]
            if (b.qPort >= 0) parts.push(qsTr("Q %1").arg(b.q.toFixed(2)))
            if (b.enablePort >= 0 && !b.enabled) parts.push(qsTr("off"))
            return parts.join(" · ")
        }
        if (hoverX >= leftPad && levels.length) { const f = frequencyAt(hoverX); return hz(f) + " · " + db(curveAt(f)) }
        return bypassed ? qsTr("Bypassed: the curve is what it will do when it is on") : ""
    }

    // ── drawing ─────────────────────────────────────────────────────────────
    Rectangle { anchors.fill: parent; color: Theme.laneBg; radius: Theme.rControl; border.color: Theme.border }
    Canvas {
        id: plot
        objectName: "eqCurvePlot"
        anchors.fill: parent
        onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
        Connections {
            target: root
            function onResponseChanged() { plot.requestPaint() }
            function onRangeChanged() { plot.requestPaint() }
        }
        onPaint: {
            const g = getContext("2d"); g.reset()
            g.font = "9px sans-serif"; g.lineWidth = 1
            // Grid: frequencies, and dB in sixths of the range.
            // A label that would touch the one before it is left out (narrow docks).
            let labelEnd = -Infinity
            for (const f of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) {
                const x = Math.round(root.xOf(f)) + 0.5
                g.strokeStyle = Theme.withAlpha(Theme.border, f === 100 || f === 1000 || f === 10000 ? 1 : 0.55)
                g.beginPath(); g.moveTo(x, 6); g.lineTo(x, 6 + root.plotHeight); g.stroke()
                g.fillStyle = Theme.textMuted
                const label = f >= 1000 ? (f / 1000) + "k" : String(f)
                const w = g.measureText(label).width
                const at = Math.min(x + 2, root.width - w - 2)
                if (at < labelEnd + 4) continue
                g.fillText(label, at, height - 4)
                labelEnd = at + w
            }
            for (let i = -3; i <= 3; ++i) {
                const d = i * root.range / 3, y = Math.round(root.yOf(d)) + 0.5
                g.strokeStyle = i === 0 ? Theme.borderStrong : Theme.withAlpha(Theme.border, 0.55)
                g.beginPath(); g.moveTo(root.leftPad, y); g.lineTo(root.leftPad + root.plotWidth, y); g.stroke()
                g.fillStyle = Theme.textMuted
                g.fillText((d > 0 ? "+" : "") + Math.round(d), 4, y + 3)
            }
            const fs = root.frequencies, ds = root.levels
            if (!fs.length) return
            const colour = root.bypassed ? Theme.textMuted : Theme.accent
            g.save()
            g.beginPath(); g.rect(root.leftPad, 6, root.plotWidth, root.plotHeight); g.clip()
            // A soft fill between the curve and 0 dB, then the curve.
            g.beginPath(); g.moveTo(root.xOf(fs[0]), root.yOf(0))
            for (let i = 0; i < fs.length; ++i) g.lineTo(root.xOf(fs[i]), root.yRaw(ds[i]))
            g.lineTo(root.xOf(fs[fs.length - 1]), root.yOf(0)); g.closePath()
            g.fillStyle = Theme.withAlpha(colour, 0.14); g.fill()
            g.beginPath()
            for (let i = 0; i < fs.length; ++i) {
                const x = root.xOf(fs[i]), y = root.yRaw(ds[i])
                i ? g.lineTo(x, y) : g.moveTo(x, y)
            }
            g.strokeStyle = colour; g.lineWidth = 2
            if (root.bypassed) g.setLineDash([4, 3])
            g.stroke()
            g.restore()
        }
    }

    // Where the pointer is, for the readout.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onPositionChanged: function (e) { root.hoverX = e.x }
        onExited: root.hoverX = -1
    }

    Text {
        objectName: "eqCurveReadout"
        anchors.right: parent.right; anchors.top: parent.top
        anchors.rightMargin: Theme.s2; anchors.topMargin: Theme.s1
        text: root.readout
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        font.features: { "tnum": 1 }
        color: root.activeBand >= 0 ? Theme.textPrimary : Theme.textSecondary
    }
    Text {
        visible: root.response.valid !== true
        anchors.centerIn: parent
        text: root.response.pending === true ? qsTr("Measuring…") : (root.response.reason || "")
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }

    // ── the band handles ────────────────────────────────────────────────────
    Repeater {
        // By count, not by the list: a new measurement must not rebuild the
        // handle under a drag.
        model: root.bands.length
        delegate: Rectangle {
            id: handle
            objectName: "eqHandle"
            readonly property var modelData: root.bands[index] || ({ label: "", freq: 20, freqPort: -1, gainPort: -1, qPort: -1, enablePort: -1 })
            required property int index
            readonly property bool on: modelData.enablePort < 0 || modelData.enabled
            readonly property real centreY: root.yOf(modelData.gainPort >= 0 ? root.gainDb(modelData) : root.curveAt(modelData.freq))
            x: root.xOf(modelData.freq) - width / 2
            y: Math.max(0, Math.min(root.height - height, centreY - height / 2))
            width: 18; height: 18; radius: 9
            color: !on ? Theme.laneBg : drag.pressed ? Theme.pressedOn(Theme.accent)
                 : drag.containsMouse ? Theme.hovered(Theme.accent) : Theme.accent
            // Hovered or held: the same accent ring as a slider's handle.
            Rectangle {
                visible: drag.pressed || drag.containsMouse
                anchors.centerIn: parent
                width: 28; height: 28; radius: 14; z: -1
                color: Theme.withAlpha(Theme.accent, 0.28)
            }
            border.width: on ? 0 : 1.5
            border.color: Theme.textMuted
            Text {
                anchors.centerIn: parent
                text: handle.modelData.label
                font.family: Theme.fontFamily; font.pixelSize: handle.modelData.label.length > 1 ? 8 : 10
                font.weight: Theme.wHeading
                color: handle.on ? Theme.accentText : Theme.textMuted
            }

            MouseArea {
                id: drag
                objectName: "eqHandleMouse"
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                preventStealing: true
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property real startY: 0
                property real startQ: 0
                readonly property var band: handle.modelData

                function ports() {
                    const out = [band.freqPort]
                    if (band.gainPort >= 0) out.push(band.gainPort)
                    if (band.qPort >= 0) out.push(band.qPort)
                    return out
                }
                function gesture(open) {
                    if (!root.service) return
                    for (const p of ports()) root.service.automationGesture(root.target, "fx:" + root.selected + ":" + p, open)
                }
                function set(port, value, low, high) {
                    if (root.service && port >= 0) root.service.setPluginControl(root.selected, port, Math.max(low, Math.min(high, value)), root.target)
                }
                function setQ(q) { set(band.qPort, q, band.qMin, band.qMax) }

                ControlHelp {
                    title: qsTr("Band %1 · %2").arg(drag.band.label).arg(root.hz(drag.band.freq))
                    purpose: drag.band.gainPort >= 0
                             ? qsTr("Drag sideways for its frequency, up or down for its gain.")
                             : qsTr("Drag sideways for its frequency.")
                    scope: drag.band.qPort >= 0 ? qsTr("Wheel, or drag with Shift held, for its width (Q).") : ""
                }

                onEntered: root.activeBand = handle.index
                onExited: if (!pressed) root.activeBand = -1
                onPressed: function (e) {
                    root.activeBand = handle.index
                    startY = e.y; startQ = band.q
                    gesture(true)
                }
                onPositionChanged: function (e) {
                    if (!pressed) return
                    const p = mapToItem(root, e.x, e.y)
                    if (e.modifiers & Qt.ShiftModifier) {
                        if (band.qPort >= 0) setQ(startQ * Math.pow(2, (startY - e.y) / 60))
                    } else {
                        set(band.freqPort, root.frequencyAt(p.x), band.freqMin, band.freqMax)
                        if (band.gainPort >= 0) {
                            const d = root.dbAt(p.y)
                            set(band.gainPort, band.gainLinear ? Math.pow(10, d / 20) : d, band.gainMin, band.gainMax)
                        }
                    }
                    // Drawn from the settings as they go, before the drag is committed.
                    root.response = root.service.pluginResponse(root.selected, root.target)
                    root.refresh()
                }
                onReleased: {
                    gesture(false)
                    if (root.service) root.service.flushPluginPorts()
                    if (!containsMouse) root.activeBand = -1
                }
                onCanceled: {
                    gesture(false)
                    if (root.service) root.service.flushPluginPorts()
                    root.activeBand = -1
                }
                onWheel: function (w) {
                    if (band.qPort < 0 || !root.service) return
                    gesture(true)
                    setQ(band.q * Math.pow(2, w.angleDelta.y / 120 * 0.25))
                    gesture(false)
                    root.service.flushPluginPorts()
                    root.refresh()
                }
            }
        }
    }
}
