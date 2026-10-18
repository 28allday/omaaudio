import QtQuick

// A position or a length on the timeline, written and typed in the display the
// person chose for the whole application (View › Time Display): clock time,
// samples, decimal seconds, CD frames, musical time or timecode. The owner gives it samples with show() and reads samples back
// from `value`; what the text looks like is never the owner's concern.
//
// Typing accepts the display's own form and the general ones (1.5, 0:01.5,
// 25ms, 1200smp). Up and Down move by one step of the display (a millisecond,
// a sample, a musical tick, a CD or picture frame; Shift is ten). Dragging sideways scrubs the same
// way (Shift slows dragging to a tenth); a press that does not travel becomes
// a text edit. Escape discards the current draft or drag.
TextEntry {
    id: root

    property var service: null
    property bool duration: false
    function formatted(samples) { return root.duration ? root.service.formatDuration(samples) : root.service.formatTime(samples) }
    function parsed(text) { return root.duration ? root.service.parseDuration(text) : root.service.parseTime(text) }
    property real minimum: 0
    property real maximum: 9007199254740991
    // What the text means now, in samples; -1 while it is not a time in range.
    readonly property real value: {
        if (!root.service) return -1
        void root.service.timeDisplay; void root.service.sampleRate
        void root.text; void root.written; void root.minimum; void root.maximum
        return root.meaning()
    }
    function meaning() {
        if (!root.service) return -1
        // Text that is still what writeValue() wrote stands for the exact sample count
        // it was given, not for the display's rounding of it.
        const samples = root.text === root.formatted(root.written) ? root.written : root.parsed(root.text)
        return samples >= root.minimum && samples <= root.maximum ? samples : -1
    }
    readonly property bool valid: root.value >= 0
    // The last value shown or accepted, so a change of display can rewrite it.
    property real shown: 0
    // The formatted draft may differ from the last accepted value. Changing
    // displays must neither accept it nor round away its exact samples.
    property real written: 0
    // A change made here and finished: Return, leaving the field, a key step, a drag.
    signal edited(real samples)

    function show(samples) {
        root.shown = Math.max(0, Math.round(Number(samples) || 0))
        root.writeValue(root.shown)
    }
    function writeValue(samples) {
        root.written = samples
        root.text = root.service ? root.formatted(samples) : String(samples)
        // Two exact counts can have identical rounded text, with no textChanged.
        root.meant = root.meaning()
    }
    function accept() {
        if (!root.enabled || root.readOnly || !root.valid) return false
        const changed = root.value !== root.shown
        root.show(root.value)
        if (changed) root.edited(root.shown)
        return true
    }
    function nudge(steps) {
        if (!root.enabled || root.readOnly || !root.service) return
        const base = root.valid ? root.value : root.shown
        const next = Math.max(root.minimum, Math.min(root.maximum, root.service.advanceTime(base, steps)))
        if (next === base) return
        root.show(next)
        root.edited(next)
    }

    helpScope: root.service ? (root.duration ? root.service.durationDisplayHint : root.service.timeDisplayHint) : ""
    font.family: Theme.monoFamily
    // Shared buttons call this on press, before their action reads the value.
    function commitNumericDraft() { root.accept() }
    onEditingFinished: root.accept()
    Keys.onUpPressed: event => root.nudge(event.modifiers & Qt.ShiftModifier ? 10 : 1)
    Keys.onDownPressed: event => root.nudge(event.modifiers & Qt.ShiftModifier ? -10 : -1)
    Keys.onEscapePressed: event => {
        const acceptedText = root.service ? root.formatted(root.shown) : String(root.shown)
        if (root.text !== acceptedText || root.value !== root.shown) {
            root.show(root.shown)
            root.focus = false
        } else {
            // With no draft to discard, Escape belongs to the surrounding panel.
            event.accepted = false
        }
    }
    // What the text meant in the display it was typed in. When the display
    // changes, the same text would be read by the new one ("4799" samples
    // becoming 4799 seconds), so the meaning is taken when the text changes.
    property real meant: -1
    onTextChanged: root.meant = root.meaning()
    Connections {
        target: root.service
        function onTimeDisplayChanged() {
            if (scrub.pressed) scrub.cancel()
            if (root.meant >= 0) root.writeValue(root.meant)
        }
    }

    // Over the field until it is being typed into; then it steps aside so the
    // caret, selection and drag-select behave as in any text field.
    MouseArea {
        id: scrub
        objectName: "timeScrub"
        anchors.fill: parent
        enabled: root.enabled && !root.readOnly && !root.activeFocus
        visible: enabled
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        property real pressX: 0
        property real lastX: 0
        property real base: 0
        property real stepBase: 0
        property real dragSteps: 0
        property bool scrubbing: false
        property bool cancelled: false
        function cancel() {
            if (scrubbing) root.show(base)
            scrubbing = false
            cancelled = true
        }
        Keys.onEscapePressed: event => {
            if (scrub.pressed) scrub.cancel()
            else event.accepted = false
        }
        onPressed: event => {
            pressX = event.x; lastX = event.x
            base = root.valid ? root.value : root.shown
            stepBase = base
            dragSteps = 0; scrubbing = false; cancelled = false
            scrub.forceActiveFocus(Qt.MouseFocusReason)
        }
        onPositionChanged: event => {
            if (!pressed || cancelled || !root.service) return
            const travel = event.x - pressX
            if (!scrubbing && Math.abs(travel) < 4) return
            scrubbing = true
            const fine = (event.modifiers & Qt.ShiftModifier) ? 0.1 : 1
            // Shift changes the speed of the next movement, not all travel
            // since the press. Keep sub-step movement for precision dragging.
            dragSteps += (event.x - lastX) * fine
            lastX = event.x
            const wanted = root.service.advanceTime(stepBase, Math.round(dragSteps))
            const next = Math.max(root.minimum, Math.min(root.maximum, wanted))
            // A CD frame or musical tick need not span an integer number of
            // samples. Clamp its actual position, then restart movement from
            // the bound so reversing never has to undo overshoot. Keep `base`
            // untouched: Escape still restores the original value.
            if ((wanted >= root.maximum && dragSteps > 0)
                    || (wanted <= root.minimum && dragSteps < 0)) {
                stepBase = next
                dragSteps = 0
            }
            if (next !== root.shown) root.show(next)
        }
        onReleased: {
            if (cancelled) return
            if (scrubbing) { scrubbing = false; if (root.shown !== base) root.edited(root.shown); return }
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.selectAll()
        }
        onCanceled: scrub.cancel()
    }
}
