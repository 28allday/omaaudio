pragma ComponentBehavior: Bound
import QtQuick
import "ui"
import "PluginScale.js" as Scale

// One effect control as a knob: the value over the name, a box you can type
// into under the dial. Drag up or down to turn it (Shift for fine steps), roll
// the wheel, double-click it (or click the dot) for the effect's default. Used
// for dynamics effects in the Apply panel, where a row of knobs reads like the
// controls on the unit itself.
Item {
    id: root
    objectName: "pluginKnob"

    property var port: ({})
    property real value: 0
    // The plugin's own text for the value, when it formats one.
    property string nativeText: ""
    readonly property bool changed: Scale.changed(port, shown)
    readonly property real shown: dial.pressed ? dial.dragValue : value
    readonly property real fraction: Scale.fraction(port, shown)
    readonly property var reading: Scale.valueText(port, shown)
    readonly property string valueText: nativeText !== "" ? nativeText : reading.text + (reading.unit ? " " + reading.unit : "")

    // A drag in progress (live), a change to keep (commit), a gesture's ends.
    signal moved(real value)
    signal committed()
    signal gesture(bool begin)

    implicitWidth: 92
    implicitHeight: 112

    Canvas {
        id: face
        objectName: "knobFace"
        width: 52; height: 52
        anchors.horizontalCenter: parent.horizontalCenter
        y: 2
        readonly property real at: Math.max(0, Math.min(1, root.fraction))
        readonly property real zero: Scale.origin(root.port)
        readonly property color track: Theme.controlBg
        readonly property color cap: dial.pressed ? Theme.pressedOn(Theme.panelRaised)
                                   : dial.containsMouse ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
        readonly property color fill: dial.containsMouse || dial.pressed ? Theme.hovered(Theme.accent) : Theme.accent
        readonly property color line: Theme.textPrimary
        readonly property color edge: Theme.borderStrong
        onAtChanged: requestPaint()
        onFillChanged: requestPaint()
        onCapChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = width / 2, r = c - 4
            const start = Math.PI * 0.75, sweep = Math.PI * 1.5
            ctx.lineCap = "round"
            ctx.lineWidth = 4
            ctx.strokeStyle = track
            ctx.beginPath(); ctx.arc(c, c, r, start, start + sweep); ctx.stroke()
            const a = start + sweep * Math.min(zero, at), b = start + sweep * Math.max(zero, at)
            if (b - a > 0.001) {
                ctx.strokeStyle = fill
                ctx.beginPath(); ctx.arc(c, c, r, a, b); ctx.stroke()
            }
            ctx.fillStyle = cap
            ctx.strokeStyle = edge
            ctx.lineWidth = 1
            ctx.beginPath(); ctx.arc(c, c, r - 7, 0, Math.PI * 2); ctx.fill(); ctx.stroke()
            const angle = start + sweep * at
            ctx.strokeStyle = line
            ctx.lineWidth = 2
            ctx.beginPath()
            ctx.moveTo(c + Math.cos(angle) * (r - 16), c + Math.sin(angle) * (r - 16))
            ctx.lineTo(c + Math.cos(angle) * (r - 8), c + Math.sin(angle) * (r - 8))
            ctx.stroke()
        }

        MouseArea {
            id: dial
            objectName: "knobDial"
            anchors.fill: parent
            cursorShape: Qt.SizeVerCursor
            hoverEnabled: true
            preventStealing: true
            property real dragValue: root.value
            property real startFraction: 0
            property real startY: 0
            onPressed: function (e) {
                root.gesture(true)
                dragValue = root.value
                startFraction = root.fraction
                startY = e.y
            }
            onPositionChanged: function (e) {
                if (!pressed) return
                const per = (e.modifiers & Qt.ShiftModifier) ? 800 : 180
                let v = Scale.valueAt(root.port, startFraction + (startY - e.y) / per)
                if (root.port.integer) v = Math.round(v)
                dragValue = v
                root.moved(v)
            }
            onReleased: { root.gesture(false); root.committed() }
            onCanceled: { root.gesture(false); root.committed() }
            onDoubleClicked: { root.moved(root.port.defaultValue); root.committed() }
            onWheel: function (w) {
                const dir = w.angleDelta.y > 0 ? 1 : -1
                const step = (w.modifiers & Qt.ShiftModifier) ? 0.002 : 0.02
                let v = root.port.integer ? Math.round(root.value) + dir : Scale.valueAt(root.port, root.fraction + dir * step)
                v = Math.max(root.port.min, Math.min(root.port.max, v))
                root.gesture(true); root.moved(v); root.gesture(false); root.committed()
            }
            ControlHelp {
                title: root.port.name || ""
                purpose: qsTr("Drag up or down to turn it; hold Shift for fine steps. Double-click for the effect's default.")
                scope: qsTr("Control changes are not Undo steps; a parameter preset can save and restore them.")
                suppressed: dial.pressed
            }
        }
    }

    // The value: click it to type.
    Rectangle {
        id: box
        objectName: "knobValueBox"
        width: Math.min(root.width, 84); height: 22
        anchors.horizontalCenter: parent.horizontalCenter
        y: face.y + face.height + 4
        radius: Theme.rControl
        color: editor.visible ? Theme.windowBg : "transparent"
        border.width: editor.visible ? 1 : 0; border.color: Theme.accent
        Text {
            objectName: "knobValue"
            visible: !editor.visible
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width)
            elide: Text.ElideRight; textFormat: Text.PlainText
            text: root.valueText
            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel + 1; font.features: { "tnum": 1 }
            color: Theme.accent
        }
        TextInput {
            id: editor
            objectName: "knobValueEdit"
            visible: false
            anchors.fill: parent; anchors.margins: 3
            horizontalAlignment: TextInput.AlignHCenter; verticalAlignment: TextInput.AlignVCenter
            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel + 1
            color: Theme.textPrimary; selectionColor: Theme.selectionFill
            selectByMouse: true; clip: true
            function begin() { text = Scale.editText(root.port, root.value); visible = true; forceActiveFocus(); selectAll() }
            function finish(keep) {
                if (!visible) return
                visible = false
                if (!keep) return
                const v = Scale.parse(root.port, text)
                if (isNaN(v)) return
                root.gesture(true); root.moved(v); root.gesture(false); root.committed()
            }
            Keys.onReturnPressed: (event) => { finish(true); event.accepted = true }
            Keys.onEnterPressed: (event) => { finish(true); event.accepted = true }
            Keys.onEscapePressed: (event) => { finish(false); event.accepted = true }
            onActiveFocusChanged: if (!activeFocus) finish(true)
        }
        MouseArea {
            objectName: "knobValueClick"
            anchors.fill: parent; visible: !editor.visible
            cursorShape: Qt.IBeamCursor
            onClicked: editor.begin()
        }
    }
    Rectangle {
        objectName: "knobDefaultDot"
        visible: root.changed
        width: 6; height: 6; radius: 3; color: Theme.accent
        x: face.x + face.width + 2; y: face.y + 4
        MouseArea {
            anchors.fill: parent; anchors.margins: -5
            cursorShape: Qt.PointingHandCursor
            onClicked: { root.gesture(true); root.moved(root.port.defaultValue); root.gesture(false); root.committed() }
        }
    }
    Text {
        objectName: "knobName"
        anchors.horizontalCenter: parent.horizontalCenter
        y: box.y + box.height + 2
        width: root.width
        horizontalAlignment: Text.AlignHCenter
        // Two lines before it shortens: "Max Gain Reduction" fits under a knob.
        wrapMode: Text.WordWrap; maximumLineCount: 2
        elide: Text.ElideRight; textFormat: Text.PlainText
        text: root.port.name || ""
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        color: root.changed ? Theme.textPrimary : Theme.textSecondary
    }
}
