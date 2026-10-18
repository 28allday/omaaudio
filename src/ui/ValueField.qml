import QtQuick
import QtQuick.Controls as C

// Numeric field with horizontal drag-scrubbing — the Inspector's Position,
// Scale and Rotation boxes. Dragging is how these are actually used in an
// edit; typing is the precise fallback, so both must work and neither may
// fight the other: a press that never travels becomes a text edit, a press
// that travels becomes a scrub.
Rectangle {
    id: root

    property real value: 0
    property real from: -999999
    property real to: 999999
    // Value change per pixel dragged. Fine drag (Shift) divides this by ten.
    property real step: 1.0
    property int decimals: 1
    property string suffix: ""
    property string label: ""

    property string helpTitle: label
    property string helpText: ""
    property string helpScope: ""
    property string helpTopic: ""
    property string unavailableReason: ""
    property string shortcut: ""
    readonly property alias controlHelp: help
    property bool helpEnabled: true
    readonly property bool interacting: scrub.pressed

    property bool editable: true

    signal edited(real value)
    signal editingStarted()
    signal editingFinished(real value)

    implicitWidth: 72
    implicitHeight: Theme.hControl
    color: !input.activeFocus && (scrub.containsMouse || scrub.scrubbing) ? Theme.hovered(Theme.controlBg) : Theme.controlBg
    radius: Theme.rControl
    border.width: Theme.hairline
    border.color: input.activeFocus ? Theme.accent
                : scrub.scrubbing ? Theme.borderStrong
                : scrub.containsMouse ? Theme.borderStrong : Theme.border
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.SpinBox
    Accessible.name: label
    Accessible.description: [display(), help.description].join(". ")

    function display() {
        return value.toFixed(decimals) + (suffix ? " " + suffix : "")
    }

    function commit(v) {
        if (!Number.isFinite(v)) return
        const clamped = Math.max(from, Math.min(to, v))
        // The owner supplies value. Assigning it here breaks that binding and
        // leaves the number stale after a preset, reset or Undo.
        if (clamped !== value) {
            if (!scrub.scrubbing) editingStarted()
            edited(clamped)
            if (!scrub.scrubbing) editingFinished(clamped)
        }
    }

    C.TextField {
        id: input
        objectName: "valueInput"
        anchors.fill: parent
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        readOnly: !root.editable
        Accessible.name: root.helpTitle
        Accessible.description: help.description
        onTextEdited: Help.dismiss()

        text: root.display()
        color: Theme.textPrimary
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsControl
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        padding: 0
        background: null
        validator: DoubleValidator { bottom: root.from; top: root.to; locale: "C" }
        property bool discardDraft: false
        // Shared buttons call this on press, before their action reads a value.
        function commitNumericDraft() { focus = false }
        function parsedValue() { return Number.fromLocaleString(Qt.locale("C"), text) }
        function stepValue(direction) {
            const base = acceptableInput ? parsedValue() : root.value
            root.commit(base + direction * root.step)
            text = root.value.toFixed(root.decimals)
            selectAll()
        }

        onActiveFocusChanged: {
            // Entering the field swaps the formatted display for a raw,
            // fully-selected number so the whole value is replaceable.
            if (activeFocus) {
                text = root.value.toFixed(root.decimals)
                selectAll()
            } else {
                if (!discardDraft && acceptableInput) root.commit(parsedValue())
                discardDraft = false
                text = Qt.binding(() => root.display())
            }
        }
        onAccepted: focus = false
        Keys.onEscapePressed: { discardDraft = true; focus = false }
        Keys.onUpPressed: stepValue(1)
        Keys.onDownPressed: stepValue(-1)
    }

    // Sits above the field and swallows the press only while the field is not
    // being typed into; once it has focus, this steps aside so selection,
    // caret placement and drag-select all behave normally.
    MouseArea {
        id: scrub
        objectName: "valueScrub"
        preventStealing: true
        anchors.fill: parent
        enabled: root.editable && !input.activeFocus
        hoverEnabled: true
        cursorShape: scrubbing ? Qt.SizeHorCursor
                   : containsMouse ? Qt.SizeHorCursor : Qt.ArrowCursor

        property bool scrubbing: false
        property real startX: 0
        property real lastX: 0
        property real dragValue: 0

        onPressed: function (mouse) {
            startX = mouse.x
            lastX = mouse.x
            dragValue = root.value
            scrubbing = false
        }

        onPositionChanged: function (mouse) {
            if (!pressed) return
            const dx = mouse.x - startX
            if (!scrubbing) {
                if (Math.abs(dx) < Theme.dragThreshold) return
                scrubbing = true
                root.editingStarted()
            }
            const fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
            // Apply the speed to the next movement, so pressing/releasing
            // Shift midway does not recalculate and jump over the whole drag.
            dragValue = Math.max(root.from, Math.min(root.to,
                dragValue + (mouse.x - lastX) * root.step * (fine ? 0.1 : 1.0)))
            lastX = mouse.x
            root.commit(dragValue)
        }

        onReleased: {
            // A press that never travelled is a request to type, not to scrub.
            if (!scrubbing)
                input.forceActiveFocus()
            else
                root.editingFinished(root.value)
            scrubbing = false
        }
        onCanceled: {
            if (scrubbing) root.editingFinished(root.value)
            scrubbing = false
        }
    }
    ControlHelp {
        id: help
        title: root.helpEnabled ? root.helpTitle : ""
        purpose: root.helpText
        scope: root.helpScope
        topic: root.helpTopic
        shortcut: root.shortcut
        unavailableReason: root.unavailableReason
        suppressed: !root.helpEnabled || scrub.pressed
    }
}
