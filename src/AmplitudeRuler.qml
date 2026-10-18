import QtQuick
import "ui"

// The column beside a channel lane on the Waveform page: the channel's name at
// the top and a dB scale down the side, mirrored about the centre line the
// way the waveform is. Ticks sit exactly where the shader draws those levels
// (amplitude a lands at 0.5 ∓ 0.46·a of the height, see wave.frag).
Rectangle {
    id: root
    objectName: "amplitudeRuler"

    property string label: ""
    property color tint: Theme.audioPrimary

    implicitWidth: Theme.wAmpRuler
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
        text: root.label
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        font.weight: Theme.wHeading
        color: root.tint
    }

    // Levels, top half labelled, bottom half ticked only: the numbers are the
    // same and the eye reads them once.
    Repeater {
        model: [
            { db: "0",   a: 1.0 },
            { db: "-6",  a: 0.5 },
            { db: "-12", a: 0.25 },
            { db: "-18", a: 0.125 },
        ]
        Item {
            required property var modelData
            readonly property real upper: root.height * (0.5 - modelData.a * 0.46)
            readonly property real lower: root.height * (0.5 + modelData.a * 0.46)
            width: root.width
            Rectangle { x: root.width - 5; y: parent.upper; width: 4; height: 1; color: Theme.rulerTick }
            Rectangle { x: root.width - 5; y: parent.lower; width: 4; height: 1; color: Theme.rulerTick }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 7
                y: parent.upper - height / 2
                visible: root.height > 60 || modelData.a >= 0.5
                text: modelData.db
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsRuler
                font.features: { "tnum": 1 }
                color: Theme.rulerText
            }
        }
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 7
        y: root.height / 2 - height / 2
        text: "-∞"
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsRuler
        color: Theme.textMuted
    }
}
