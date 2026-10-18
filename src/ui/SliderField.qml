import QtQuick
import QtQuick.Controls as C

// Label + track + numeric readout, as used for Opacity and the grading
// controls. The readout is a real ValueField, so a value can always be typed
// exactly rather than hunted for with the handle.
Item {
    id: root

    property string label: ""

    property string helpTitle: label
    property string helpText: ""
    property string helpScope: ""
    property string helpTopic: ""
    property string unavailableReason: ""
    property string shortcut: ""
    readonly property alias controlHelp: help

    property real value: 0
    property real from: 0
    property real to: 100
    property real stepSize: 0
    property int decimals: 1
    property string suffix: ""
    property int labelWidth: 84
    // The readout's width. 64 fits "100.0 %"; a signed dB value wants more
    // (trap 59: a ValueField clips from the left, taking the sign first).
    property int readoutWidth: 64
    // Where the fill starts. Bipolar controls (a −1..1 colour trim) fill out
    // from the centre instead of from the left.
    property real origin: from

    signal edited(real value)
    signal editingStarted()
    // Emitted once at the end of a drag, for callers that must not commit an
    // undo step per pixel.
    signal editingFinished(real value)

    implicitHeight: Theme.hControl
    implicitWidth: labelWidth + 160 + Theme.s2 * 2
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Text {
        id: caption
        text: root.label
        visible: root.label !== ""
        width: root.labelWidth
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
        elide: Text.ElideRight
    }

    C.Slider {
        id: slider
        objectName: "fieldSlider"
        anchors.left: root.label !== "" ? caption.right : parent.left
        anchors.leftMargin: root.label !== "" ? Theme.s2 : 0
        anchors.right: readout.left
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height

        from: root.from
        to: root.to
        value: root.value
        stepSize: root.stepSize

        onMoved: {
            // Keyboard changes have no pointer press/release to delimit them.
            if (!pressed) root.editingStarted()
            root.edited(value)
            if (!pressed) root.editingFinished(value)
        }
        onPressedChanged: { if (pressed) root.editingStarted(); else root.editingFinished(value) }

        Accessible.role: Accessible.Slider
        Accessible.name: root.helpTitle
        Accessible.description: help.description

        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: slider.availableWidth
            height: 3
            radius: 1
            color: Theme.controlBg

            // Fill runs from `origin` to the handle, so a bipolar control
            // reads its direction from the centre outwards.
            Rectangle {
                readonly property real originPos:
                    (root.origin - slider.from) / (slider.to - slider.from)
                x: Math.min(originPos, slider.visualPosition) * parent.width
                width: Math.abs(slider.visualPosition - originPos) * parent.width
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        }

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition
               * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: 10
            height: 10
            radius: 5
            color: slider.pressed ? Theme.accentPressed : Theme.textPrimary
            border.width: slider.visualFocus ? Theme.focusRing : 0
            border.color: Theme.accent
            // Hovered or held: a ring of the accent says it can be pulled.
            Rectangle {
                visible: slider.pressed || slider.hovered
                anchors.centerIn: parent
                width: 18; height: 18; radius: 9; z: -1
                color: Theme.withAlpha(Theme.accent, 0.28)
            }

            Behavior on color {
                ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
            }
        }
    }

    ValueField {
        id: readout
        objectName: "sliderReadout"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: root.readoutWidth
        label: root.label
        helpEnabled: false
        helpTitle: root.helpTitle
        helpText: root.helpText
        helpScope: root.helpScope
        helpTopic: root.helpTopic
        unavailableReason: root.unavailableReason
        shortcut: root.shortcut
        value: root.value
        from: root.from
        to: root.to
        decimals: root.decimals
        suffix: root.suffix
        step: root.stepSize > 0 ? root.stepSize : (root.to - root.from) / 200
        onEditingStarted: root.editingStarted()
        onEdited: v => root.edited(v)
        onEditingFinished: v => root.editingFinished(v)
    }
    ControlHelp {
        id: help
        title: root.helpTitle
        purpose: root.helpText
        scope: root.helpScope
        topic: root.helpTopic
        shortcut: root.shortcut
        unavailableReason: root.unavailableReason
        suppressed: slider.pressed || readout.interacting
    }
}
