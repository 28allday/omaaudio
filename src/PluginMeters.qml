pragma ComponentBehavior: Bound
import QtQuick
import "ui"
import "PluginScale.js" as Scale

// An effect's output controls (Backend.pluginOutputs) as meters: what the
// running effect reports, such as its input and output level or how much it is
// turning the sound down. Sorted by PluginScale.meterModel: a level reads in dB
// (−60 to 0), a left/right pair is one meter with two bars, a clip light rides
// at the end of its meter, gain reduction fills amber, and an on/off light is a
// light. Read twenty times a second while the project plays; each bar holds its
// highest point for a moment. Stopped, every meter rests.
Column {
    id: root
    objectName: "pluginMeters"

    property var service: null
    property int selected: -1
    property int target: -1
    property var outputs: []
    // The effect is switched off (its power, or Before): it is not run, so
    // nothing new reaches its meters.
    property bool bypassed: false
    // The dock's narrow form: name and reading over the bar.
    property bool compact: false
    property int labelWidth: 110

    readonly property var model: Scale.meterModel(outputs)
    // Why the meters rest, said plainly rather than left at silence: a bypassed
    // effect and Hear Without Effects stop the effect running; stopped, nothing plays.
    readonly property string restReason: !service ? ""
        : service.hearWithoutEffects === true ? qsTr("Effects are off (Alt+B), so this effect is not running.")
        : bypassed ? qsTr("Bypassed: the effect is not running, so its meters rest.")
        : service.playing !== true ? qsTr("Play to see the meters.") : ""
    readonly property bool live: visible && service !== null && service.playing === true && outputs.length > 0 && restReason === ""
    spacing: root.compact ? Theme.s2 : Theme.s1
    visible: outputs.length > 0

    // Readings by output index, updated together so every bar moves at once.
    property var values: ({})
    property var holds: ({})
    property var heldAt: ({})

    function read() {
        if (!service || selected < 0) return
        const now = Date.now()
        const next = ({}), hold = Object.assign({}, holds), at = Object.assign({}, heldAt)
        for (const item of model) {
            for (const o of item.ports.concat(item.clips)) {
                const v = service.pluginOutputValue(selected, o.index, target)
                next[o.index] = v
                const f = Scale.meterFraction(item, o, v)
                if (hold[o.index] === undefined || f >= Scale.meterFraction(item, o, hold[o.index]) || now - (at[o.index] || 0) > 1200) {
                    hold[o.index] = v; at[o.index] = now
                }
            }
        }
        values = next; holds = hold; heldAt = at
    }
    function rest() { values = ({}); holds = ({}); heldAt = ({}) }
    Timer {
        interval: 50; repeat: true; running: root.live
        onTriggered: root.read()
    }
    onLiveChanged: live ? read() : rest()
    onOutputsChanged: { rest(); if (live) read() }

    Text {
        objectName: "pluginMetersNote"
        visible: root.restReason !== ""
        width: root.width
        wrapMode: Text.WordWrap; textFormat: Text.PlainText
        text: root.restReason
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
    Repeater {
        model: root.model
        delegate: Item {
            id: meter
            objectName: "pluginMeter"
            required property var modelData
            readonly property var item: modelData
            readonly property bool bars: item.kind !== "led" && item.kind !== "value"
            readonly property bool hasLed: item.clips.length > 0 || item.kind === "led"
            width: root.width
            height: root.compact ? (bars ? 28 : 18) : 22

            function reading(port) {
                const v = root.values[port.index]
                return v !== undefined ? v : Scale.meterRest(meter.item, port)
            }
            function held(port) {
                const v = root.holds[port.index]
                return v !== undefined ? v : meter.reading(port)
            }
            // The loudest channel is what the number says.
            readonly property var loudest: {
                void root.values
                let best = item.ports[0], bestF = -1
                for (const p of item.ports) {
                    const f = Scale.meterFraction(item, p, reading(p))
                    if (f > bestF) { bestF = f; best = p }
                }
                return best
            }
            readonly property bool lit: {
                void root.holds
                const lights = item.kind === "led" ? item.ports : item.clips
                for (const p of lights) if (held(p) >= 0.5) return true
                return false
            }

            Text {
                id: name
                objectName: "pluginMeterName"
                x: 0
                width: root.compact ? parent.width - readout.width - Theme.s2 : root.labelWidth
                y: root.compact ? 0 : (parent.height - height) / 2
                horizontalAlignment: root.compact ? Text.AlignLeft : Text.AlignRight
                elide: Text.ElideRight; textFormat: Text.PlainText
                text: meter.item.kind === "led" && meter.item.clip ? qsTr("%1 clip").arg(meter.item.name) : meter.item.name
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
            }

            // The bars: one for a mono reading, two thin ones for a pair.
            Item {
                id: track
                objectName: "pluginMeterBar"
                visible: meter.bars
                x: root.compact ? 0 : root.labelWidth + Theme.s3
                y: root.compact ? 18 : (parent.height - height) / 2
                width: (root.compact ? parent.width : parent.width - x - readout.width - Theme.s3)
                       - (meter.hasLed ? led.width + Theme.s2 : 0)
                height: meter.item.ports.length > 1 ? 9 : 8
                Rectangle {
                    anchors.fill: parent
                    radius: 2
                    color: Theme.meterBg
                    border.width: Theme.hairline; border.color: Theme.border
                }
                Repeater {
                    model: meter.item.ports
                    Item {
                        id: lane
                        required property var modelData
                        required property int index
                        readonly property int count: meter.item.ports.length
                        x: 1; width: track.width - 2
                        height: count > 1 ? 3 : track.height - 2
                        y: count > 1 ? 1 + index * 4 : 1
                        readonly property real f: { void root.values; return Scale.meterFraction(meter.item, modelData, meter.reading(modelData)) }
                        readonly property real h: { void root.holds; return Scale.meterFraction(meter.item, modelData, meter.held(modelData)) }
                        Rectangle {
                            objectName: "pluginMeterFill"
                            width: lane.width * lane.f; height: parent.height; radius: 1
                            color: meter.item.kind === "reduction" ? Theme.warning
                                 : lane.f > 59 / 60 ? Theme.meterHigh : lane.f > 0.9 ? Theme.meterMid : Theme.meterLow
                        }
                        Rectangle {
                            visible: lane.h > 0.01
                            x: Math.max(0, lane.width * lane.h - 1); y: -1
                            width: 2; height: parent.height + 2
                            color: Theme.meterHold
                        }
                    }
                }
            }

            // A clip light at the end of its meter, or an on/off light on its own.
            Rectangle {
                id: led
                objectName: "pluginMeterLed"
                visible: meter.hasLed
                width: 8; height: 8; radius: 4
                x: meter.bars ? track.x + track.width + Theme.s2
                   : root.compact ? parent.width - width : root.labelWidth + Theme.s3
                y: meter.bars ? track.y + (track.height - height) / 2 : (parent.height - height) / 2
                readonly property color on: meter.item.kind === "led" && !meter.item.clip ? Theme.accent : Theme.meterHigh
                color: meter.lit ? on : Theme.meterBg
                border.width: Theme.hairline; border.color: meter.lit ? on : Theme.borderStrong
            }

            Text {
                id: readout
                objectName: "pluginMeterValue"
                visible: meter.item.kind !== "led"
                anchors.right: parent.right
                y: root.compact ? 0 : (parent.height - height) / 2
                width: 72
                horizontalAlignment: Text.AlignRight
                textFormat: Text.PlainText
                text: { void root.values; return Scale.meterText(meter.item, meter.loudest, meter.reading(meter.loudest)) }
                font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; font.features: { "tnum": 1 }
                color: Theme.textSecondary
            }
        }
    }
}
