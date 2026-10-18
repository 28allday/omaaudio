import QtQuick
import "ui"

// A one-line horizontal slider for a track's gain and pan.
//
// Deliberately tiny: a lane header has room for two of these and a name, and a
// mixer strip that pushed the waveform down would be the wrong trade in an editor.
//
// The value is always a fraction in 0..1. What that means -- decibels, pan
// position -- is the caller's business, which is what lets the same control do both.
Item {
    id: root

    property string label: ""
    property real value: 0.5
    // A centred slider fills from the middle outward, so "no pan" reads as neutral
    // rather than as half of something.
    property bool centred: false

    signal moved(real fraction)
    signal reset()
    signal touched(bool pressed)

    property string helpTitle: label
    property string helpText: ""
    property string helpScope: ""
    ControlHelp {
        title: root.helpTitle
        purpose: root.helpText
        scope: root.helpScope
        suppressed: area.pressed
    }

    implicitHeight: 14
    height: implicitHeight

    Text {
        id: caption
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 26
        text: root.label
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
    }

    Rectangle {
        id: groove
        anchors.left: caption.right
        anchors.leftMargin: Theme.s1
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 3
        radius: 1
        color: Theme.controlBg

        readonly property real clamped: Math.max(0, Math.min(1, root.value))

        Rectangle {
            radius: 1
            height: parent.height
            color: Theme.accent
            x: root.centred ? Math.min(parent.width / 2, parent.clamped * parent.width)
                            : 0
            width: root.centred
                   ? Math.abs(parent.clamped - 0.5) * parent.width
                   : parent.clamped * parent.width
        }

        Rectangle {
            id: knob
            width: 8
            height: 8
            radius: 4
            color: area.pressed ? Theme.accentPressed : Theme.textPrimary
            anchors.verticalCenter: parent.verticalCenter
            x: Math.max(0, Math.min(parent.width - width,
                                    parent.clamped * parent.width - width / 2))
            // Hovered or held: a ring of the accent says it can be pulled.
            Rectangle {
                visible: area.pressed || area.containsMouse
                anchors.centerIn: parent
                width: 16; height: 16; radius: 8; z: -1
                color: Theme.withAlpha(Theme.accent, 0.28)
            }
        }

        MouseArea {
            id: area
            objectName: "sliderArea"
            anchors.fill: parent
            hoverEnabled: true
            // Taller than the groove: a 3px hit target is not a control, it is a dare.
            anchors.topMargin: -6
            anchors.bottomMargin: -6
            cursorShape: Qt.PointingHandCursor
            preventStealing: true
            property real grabOffset: 0

            function emitAt(x) {
                if (groove.width <= 0)
                    return
                root.moved(Math.max(0, Math.min(1, x / groove.width)))
            }

            onPressed: function (event) {
                const onKnob = event.x >= knob.x && event.x <= knob.x + knob.width
                grabOffset = onKnob ? event.x - groove.clamped * groove.width : 0
                root.touched(true)
                if (!onKnob) emitAt(event.x)
            }
            onReleased: root.touched(false)
            onCanceled: root.touched(false)
            onPositionChanged: function (event) { if (pressed) emitAt(event.x - grabOffset) }
            // Double-click to return to unity or centre, which is the one value anyone
            // ever wants to get back to exactly.
            onDoubleClicked: root.reset()
        }
    }
}
