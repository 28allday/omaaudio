pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The global transport strip: one transport model drives every workspace
// (brief section 5). Transport buttons at the left, the big tabular timecode,
// the selection's start / end / length, and the master L/R meter at the right.
//
// Every button routes through a named action rather than calling the service
// directly, so the keymap and the mouse reach the same code path.
Rectangle {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool loaded: service !== null && service.loaded

    implicitHeight: Theme.hTransport
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    // In the display chosen under View › Time Display, like every other time shown.
    function formatTime(seconds, duration = false) {
        if (!service) return ""
        void service.timeDisplay; void service.musicalTime
        const rate = service.sampleRate > 0 ? service.sampleRate : 48000
        const samples = Math.round((isFinite(seconds) && seconds > 0 ? seconds : 0) * rate)
        return duration ? service.formatDuration(samples) : service.formatTime(samples)
    }

    function frames(f) {
        return (service && service.sampleRate > 0) ? f / service.sampleRate : 0
    }

    readonly property real lengthSeconds: service ? frames(service.takeFrames) : 0
    readonly property real positionSeconds: service ? frames(service.recording ? service.recordingStart + service.recordingFrames : service.playhead) : 0

    // ── transport ───────────────────────────────────────────────────────
    Row {
        id: buttons
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1

        component TransportButton: IconButton {
            property string actionName: ""
            objectName: "transport_" + actionName
            implicitWidth: Theme.szTransportHit
            implicitHeight: Theme.szTransportHit
            flat: false
            shortcut: root.shell ? root.shell.bindingFor(actionName) : ""
            enabled: root.loaded
            unavailableReason: qsTr("Open or import audio first.")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction(actionName)
        }

        TransportButton {
            iconName: "skip-back"; text: qsTr("Go to start"); actionName: "go_start"
            helpText: qsTr("Move the playhead to the start of the project.")
            helpScope: qsTr("Changes the playhead only.")
        }
        TransportButton {
            iconName: "rewind"; text: qsTr("Back one second"); actionName: "nudge_back"
            helpText: qsTr("Move the playhead one second earlier.")
            helpScope: qsTr("Changes the playhead only.")
        }
        TransportButton {
            iconName: root.service !== null && root.service.playing ? "pause" : "play"
            text: root.service !== null && root.service.playing ? qsTr("Pause") : qsTr("Play")
            helpText: qsTr("Start playback from the playhead, or pause it where it is.")
            helpScope: qsTr("Every workspace shares the same transport, so playback carries on when you switch pages.")
            actionName: "play_stop"
            iconColor: root.service !== null && root.service.playing ? Theme.accent : Theme.textPrimary
        }
        TransportButton {
            iconName: "square"; text: qsTr("Stop"); actionName: "stop"
            helpText: qsTr("Stop playback, or stop the recording in progress and keep the take.")
            enabled: root.loaded || (root.service !== null && root.service.recording)
            unavailableReason: qsTr("Open or import audio, or start a recording, first.")
        }
        TransportButton {
            iconName: "circle"; text: root.service && root.service.recording ? qsTr("Stop recording") : qsTr("Record…"); actionName: "record"
            helpText: qsTr("Open the recording panel to choose an input and record a take, or stop the recording in progress.")
            helpScope: qsTr("A recording lands on the chosen track as a new clip. Its file is kept for recovery until the project is saved.")
            enabled: root.service !== null
            unavailableReason: qsTr("The audio engine is still starting.")
            // Lit while recording, but not a toggle: clicking opens the panel, and
            // leaving it without recording must not leave the button lit.
            checkable: false; checked: root.service !== null && root.service.recording
            iconColor: Theme.record
        }
        TransportButton {
            iconName: "fast-forward"; text: qsTr("Forward one second"); actionName: "nudge_forward"
            helpText: qsTr("Move the playhead one second later.")
            helpScope: qsTr("Changes the playhead only.")
        }
        TransportButton {
            iconName: "skip-forward"; text: qsTr("Go to end"); actionName: "go_end"
            helpText: qsTr("Move the playhead to the end of the project.")
            helpScope: qsTr("Changes the playhead only.")
        }
        Item { width: Theme.s1; height: 1 }
        TransportButton {
            iconName: "repeat"; text: qsTr("Loop"); actionName: "toggle_loop"
            helpText: qsTr("Repeat the selected time range during playback.")
            helpScope: qsTr("Playback only. Turn it off to play on past the selection.")
            checkable: true; checked: root.service !== null && root.service.loopEnabled
            enabled: root.loaded && !root.service.recording && (checked || !root.service.selectionEmpty)
            unavailableReason: !root.loaded ? qsTr("Open or import audio first.")
                             : root.service.recording ? qsTr("Looping is unavailable while recording.")
                             : qsTr("Select a time range first.")
        }
    }

    // ── timecode ────────────────────────────────────────────────────────
    Rectangle {
        id: clock
        anchors.left: buttons.right
        anchors.leftMargin: Theme.s4
        anchors.verticalCenter: parent.verticalCenter
        // As wide as the longest thing it will show in this display (the project's
        // end, digits levelled to zeros), so a sample count or a rolling timecode
        // does not make the box breathe as the playhead moves.
        width: Math.max(clockText.implicitWidth, clockWidest.advanceWidth) + Theme.s3 * 2
        TextMetrics {
            id: clockWidest
            font: clockText.font
            text: root.formatTime(Math.max(root.positionSeconds, root.lengthSeconds)).replace(/[0-9]/g, "0")
        }
        height: Theme.hTransport - Theme.s3 * 2
        color: Theme.meterBg
        radius: Theme.rControl
        border.width: Theme.hairline
        border.color: Theme.border
        readonly property alias controlHelp: clockHelp
        ControlHelp {
            id: clockHelp
            title: qsTr("Playhead position")
            purpose: qsTr("Where the playhead is, in the display chosen under View › Time Display. While recording it shows the point being written. A tag shows the speed while J or L plays faster or backwards.")
            scope: qsTr("Read-only. Move the playhead with the transport buttons or by clicking the time ruler.")
        }

        Text {
            id: clockText
            objectName: "clockText"
            anchors.centerIn: parent
            text: root.formatTime(root.positionSeconds)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsTimecode
            font.features: { "tnum": 1 }
            color: root.service && root.service.recording ? Theme.record : root.loaded ? Theme.accent : Theme.textMuted
        }

        // J/K/L: how fast, and which way, while it is not normal speed.
        Rectangle {
            objectName: "shuttleSpeed"
            readonly property int speed: root.service ? root.service.playSpeed : 1
            visible: speed !== 1
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: -Theme.s1
            anchors.topMargin: -Theme.s1
            width: speedText.implicitWidth + Theme.s2
            height: speedText.implicitHeight
            radius: Theme.rControl
            color: Theme.accent
            Text {
                id: speedText
                objectName: "shuttleSpeedText"
                anchors.centerIn: parent
                text: parent.speed < 0 ? qsTr("◀ %1×").arg(-parent.speed) : qsTr("%1×").arg(parent.speed)
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                font.features: { "tnum": 1 }
                color: Theme.panelBg
            }
        }
    }

    // ── selection ───────────────────────────────────────────────────────
    Row {
        id: fields
        anchors.left: clock.right
        anchors.leftMargin: Theme.s4
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s4

        component Readout: Column {
            id: timeField
            property string label: ""
            property string help: ""
            property real seconds: 0
            property bool duration: false
            readonly property string display: root.formatTime(seconds, duration)
            readonly property alias controlHelp: timeHelp
            spacing: 1
            // A positioner must not position the adapter: zero size, hover on the column.
            ControlHelp {
                id: timeHelp
                anchors.fill: undefined
                width: 0; height: 0
                title: timeField.label
                purpose: timeField.help
                scope: qsTr("Read-only, in the display chosen under View › Time Display.")
            }
            HoverHandler { onHoveredChanged: hovered ? Help.request(timeHelp) : Help.leave(timeHelp) }
            Text {
                text: parent.label
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
            Text {
                text: root.formatTime(parent.seconds, parent.duration)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsTimecodeSmall
                font.features: { "tnum": 1 }
                color: Theme.textSecondary
            }
        }

        Readout {
            label: qsTr("Start")
            help: qsTr("Where the selected time range begins. Zero when nothing is selected.")
            seconds: root.service && !root.service.selectionEmpty ? root.frames(root.service.selectionStart) : 0
        }
        Readout {
            label: qsTr("End")
            help: qsTr("Where the selected time range ends. Zero when nothing is selected.")
            seconds: root.service && !root.service.selectionEmpty ? root.frames(root.service.selectionEnd) : 0
        }
        Readout {
            objectName: "fieldLength"; duration: true
            label: qsTr("Length")
            help: qsTr("How long the selected time range is. Zero when nothing is selected.")
            seconds: root.service && !root.service.selectionEmpty ? root.frames(root.service.selectionLength) : 0
        }
        Readout {
            objectName: "fieldDuration"; duration: true
            label: qsTr("Duration")
            help: qsTr("How long the whole project is, to the end of its last clip.")
            seconds: root.service ? root.service.durationSeconds : 0
        }
    }

    // ── master meter ────────────────────────────────────────────────────
    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Meter { objectName: "meterL"; channel: "L"; peak: root.service ? root.service.peakLeft : 0 }
            Meter { channel: "R"; peak: root.service ? root.service.peakRight : 0 }

            // The scale, once, under the pair, each label at its dB position.
            Item {
                width: 128 + Theme.s2 + 10
                height: Theme.fsRuler + 2
                Repeater {
                    model: [-48, -36, -24, -12, 0]
                    Text {
                        required property int modelData
                        readonly property real at: (modelData + 60) / 60
                        x: Theme.s2 + 10 + Math.round(128 * at) - width
                        text: String(modelData)
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsRuler
                        font.features: { "tnum": 1 }
                        color: Theme.textMuted
                    }
                }
            }
        }

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: "volume-2"
            color: Theme.textMuted
        }
    }

    // One horizontal meter bar. Linear peak in, a −60..0 dBFS scale out; green
    // to −12, amber to −3, red above. The held peak is a thin line that decays
    // on its own clock, and a clip sticks until the meter is clicked.
    component Meter: Item {
        id: meter
        property string channel: ""
        property real peak: 0
        readonly property real db: peak > 0 ? 20 * Math.log(peak) / Math.LN10 : -200
        readonly property real fraction: Math.max(0, Math.min(1, (db + 60) / 60))
        property real held: 0
        property bool clipped: false

        onPeakChanged: {
            if (peak >= 1.0) clipped = true
            if (fraction > held) held = fraction
        }
        Timer {
            interval: 40; repeat: true
            running: meter.held > 0 && (!root.service || root.service.playing)
            onTriggered: meter.held = Math.max(meter.fraction, meter.held - 0.012)
        }

        width: 128 + Theme.s2 + 10
        height: Theme.wMeter + 2
        readonly property alias controlHelp: meterHelp
        ControlHelp {
            id: meterHelp
            title: qsTr("Master level %1").arg(meter.channel)
            purpose: qsTr("Peak level of the master output in dBFS (decibels below full scale, where 0 is the loudest a file can hold). Green to −12, amber to −3, red above.")
            scope: qsTr("The thin line is the held peak, which decays on its own. A red border means the output clipped at 0 dBFS and stays until you click the meter.")
        }

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: meter.channel
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsRuler
            color: Theme.textMuted
        }

        Rectangle {
            id: bar
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 128
            height: Theme.wMeter
            color: Theme.meterBg
            border.width: Theme.hairline
            border.color: meter.clipped ? Theme.meterHigh : Theme.border

            Rectangle {
                x: 1; y: 1
                height: parent.height - 2
                width: Math.round((parent.width - 2) * meter.fraction)
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Theme.meterLow }
                    GradientStop { position: 0.80; color: Theme.meterLow }
                    GradientStop { position: 0.81; color: Theme.meterMid }
                    GradientStop { position: 0.95; color: Theme.meterMid }
                    GradientStop { position: 0.96; color: Theme.meterHigh }
                    GradientStop { position: 1.0; color: Theme.meterHigh }
                }
            }
            Rectangle {
                visible: meter.held > 0.01
                x: 1 + Math.round((parent.width - 2) * meter.held) - 1
                y: 1; width: 1; height: parent.height - 2
                color: Theme.meterHold
            }
            MouseArea {
                anchors.fill: parent
                onClicked: { meter.clipped = false; meter.held = 0 }
                ControlHelp {
                    title: qsTr("Master peak meter")
                    purpose: qsTr("The level of what you hear, left and right. A red frame means it clipped; click to clear the held peak and the clip light.")
                }
            }
        }
    }
}
