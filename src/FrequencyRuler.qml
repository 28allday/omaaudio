import QtQuick
import "ui"

// The column beside a spectral lane: the channel's name and a frequency scale
// down the side, on the same mapping the spectrogram draws with (linear or
// logarithmic; see Spectrogram.hzToY).
Rectangle {
    id: root
    objectName: "frequencyRuler"

    property string label: ""
    property color tint: Theme.audioPrimary
    property string frequencyScale: "log"
    property real nyquist: 24000
    property real minHz: 20

    implicitWidth: Theme.wAmpRuler
    color: Theme.panelBg

    function hzToY(hz) {
        var up
        if (root.frequencyScale === "log") {
            const lo = Math.max(1, root.minHz)
            up = Math.log(Math.max(lo, hz) / lo) / Math.log(root.nyquist / lo)
        } else {
            up = hz / root.nyquist
        }
        return (1 - Math.max(0, Math.min(1, up))) * root.height
    }

    readonly property var ticks: {
        var out = []
        if (root.frequencyScale === "log") {
            const candidates = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
            for (var i = 0; i < candidates.length; ++i) {
                if (candidates[i] >= root.minHz && candidates[i] <= root.nyquist)
                    out.push(candidates[i])
            }
        } else {
            const step = root.nyquist > 30000 ? 8000 : 4000
            for (var f = 0; f <= root.nyquist; f += step)
                out.push(f)
        }
        return out
    }

    readonly property var labelTicks: {
        const out = []
        let lastY = -1000
        // Highest first. Reserve the channel caption and one text-height gap.
        for (let i = root.ticks.length - 1; i >= 0; --i) {
            const y = Math.max(Theme.fsLabel + 5, Math.min(root.height - Theme.fsRuler, root.hzToY(root.ticks[i]) - Theme.fsRuler / 2))
            if (y - lastY >= Theme.fsRuler + 4) { out.push(root.ticks[i]); lastY = y }
        }
        return out
    }

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
        text: root.label
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        font.weight: Theme.wHeading
        color: root.tint
    }

    Repeater {
        model: root.ticks
        Item {
            required property var modelData
            required property int index
            readonly property real yAt: root.hzToY(modelData)
            width: root.width
            Rectangle { x: root.width - 5; y: parent.yAt; width: 4; height: 1; color: Theme.rulerTick }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 7
                y: Math.max(Theme.fsLabel + 5, Math.min(root.height - height, parent.yAt - height / 2))
                // Keep labels clear of the caption and one another on short lanes.
                visible: root.labelTicks.indexOf(parent.modelData) >= 0
                text: Theme.formatHz(parent.modelData)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsRuler
                font.features: { "tnum": 1 }
                color: Theme.rulerText
            }
        }
    }
}
