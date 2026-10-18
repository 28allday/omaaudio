pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The pauses Tidy pauses found (Backend::pauses), shaded over one lane's
// waveform: amber where the chosen action will change the audio, teal and
// dashed where a pause is kept as it is, faint where it is already short
// enough to be left. A click on a shaded pause keeps it, or lets it change
// again. Nothing is drawn, and nothing takes a click, while there are no
// pauses on this track.
Item {
    id: root
    objectName: "pauseShading"

    property var service: null
    property var axis: null
    property int trackId: -1
    // What the Tidy pauses page will do: {then, changed: [pause indices], lengths:
    // [each one's seconds after]} from Backend::pausePlan; decides amber or faint.
    property var action: null
    // The length tags, on one channel only when a take shows several.
    property bool showTags: true
    property real tagY: Theme.hClipLabel + Theme.s1

    readonly property var pauses: service ? service.pauses : ({})
    readonly property bool active: pauses.valid === true && (pauses.tracks || []).indexOf(trackId) >= 0
    readonly property bool stale: pauses.stale === true
    readonly property real rate: service && service.sampleRate > 0 ? service.sampleRate : 48000
    visible: active

    // Would the action change this pause, and to how long? The backend's plan says.
    readonly property var planned: {
        const out = ({})
        const changed = action && action.changed ? action.changed : []
        const lengths = action && action.lengths ? action.lengths : []
        for (let i = 0; i < changed.length; ++i) out[changed[i]] = lengths[i]
        return out
    }
    function changes(p) { return !p.kept && planned[p.index] !== undefined }
    function newLength(p) { return planned[p.index] !== undefined ? planned[p.index] : p.seconds }
    function tag(s) { return qsTr("%1 s").arg(Number(s).toFixed(1)) }

    // Only the pauses in view are made, like the clips. A pause right after
    // another (a breath between them) has no tag of its own, so the tags never
    // sit on top of each other.
    readonly property var shown: {
        if (!active || !axis) return []
        const from = axis.viewStart, to = axis.viewEnd
        const fpp = Math.max(0.0001, axis.framesPerPixel)
        const out = []
        let lastTag = -Infinity
        for (const p of pauses.pauses || []) {
            if (!(p.end > from && p.start < to) || out.length >= 600) continue
            const centre = (p.start + p.end) / 2 / fpp
            const tagged = centre - lastTag > 44
            if (tagged) lastTag = centre
            out.push(Object.assign({ tagged: tagged }, p))
        }
        return out
    }

    Repeater {
        model: root.shown
        delegate: Item {
            id: pause
            objectName: "pauseShade"
            required property var modelData
            readonly property bool kept: modelData.kept === true
            readonly property bool changing: root.changes(modelData)
            readonly property real x0: root.axis ? root.axis.frameToX(modelData.start) : 0
            readonly property real x1: root.axis ? root.axis.frameToX(modelData.end) : 0
            x: x0
            width: Math.max(2, x1 - x0)
            height: root.height
            opacity: root.stale ? 0.45 : 1

            Rectangle {
                anchors.fill: parent
                color: pause.kept ? Theme.maskKeepFill
                     : pause.changing ? (mouse.containsMouse ? Theme.withAlpha(Theme.maskOutline, 0.4) : Theme.maskFill)
                     : Theme.withAlpha(Theme.textMuted, mouse.containsMouse ? 0.22 : 0.12)
                border.width: mouse.containsMouse && !pause.kept ? 1 : 0
                border.color: pause.changing ? Theme.maskOutline : Theme.textMuted
            }
            // Its edges: solid amber, or teal dashes when kept.
            Repeater {
                model: pause.kept ? 0 : 2
                Rectangle {
                    required property int index
                    x: index === 0 ? 0 : pause.width - 1
                    width: 1; height: pause.height
                    color: pause.changing ? Theme.maskOutline : Theme.textMuted
                    opacity: 0.8
                }
            }
            Repeater {
                model: pause.kept ? 2 : 0
                Column {
                    required property int index
                    x: index === 0 ? 0 : pause.width - 1
                    spacing: 3
                    Repeater {
                        model: Math.floor(pause.height / 7)
                        Rectangle { width: 1; height: 4; color: Theme.maskKeepOutline }
                    }
                }
            }
            Rectangle {
                objectName: "pauseTag"
                visible: root.showTags && pause.modelData.tagged && root.height >= Theme.hLaneCompact
                // Centred on the pause, but never past the lane's left edge.
                x: Math.max(-pause.x, (pause.width - width) / 2)
                y: root.tagY
                height: 15
                width: tagText.implicitWidth + 8
                radius: Theme.rControl
                color: pause.kept ? Theme.maskKeepOutline : pause.changing ? Theme.maskOutline : Theme.controlBg
                Text {
                    id: tagText
                    anchors.centerIn: parent
                    text: (pause.kept ? "✓ " : "") + root.tag(pause.modelData.seconds)
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler + 1; font.weight: Theme.wHeading
                    font.features: { "tnum": 1 }
                    color: pause.kept || pause.changing ? Theme.windowBg : Theme.textSecondary
                }
            }
            MouseArea {
                id: mouse
                objectName: "pauseShadeMouse"
                anchors.fill: parent
                enabled: !root.stale
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.service) root.service.setPauseKept(pause.modelData.index, !pause.kept)
                ControlHelp {
                    title: pause.kept ? qsTr("Pause · %1 · kept").arg(root.tag(pause.modelData.seconds))
                         : pause.changing ? qsTr("Pause · %1 → %2").arg(root.tag(pause.modelData.seconds)).arg(root.tag(root.newLength(pause.modelData)))
                         : qsTr("Pause · %1").arg(root.tag(pause.modelData.seconds))
                    purpose: pause.kept ? qsTr("Kept as it is. Click to let it change with the others.")
                           : pause.changing ? qsTr("Click to keep it as it is.")
                           : qsTr("Already short enough; it is left as it is.")
                    scope: qsTr("Found by Tidy pauses in the Sound tab. Amber will change; teal dashed is kept.")
                }
            }
        }
    }
}
