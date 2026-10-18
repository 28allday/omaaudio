pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/AutomationCurve.js" as Curve
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "clipEnvelopePanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int requestedTrack: -1
    property int requestedClip: -1
    property int pinnedTrack: -1
    property int pinnedClip: -1
    property var targetClip: ({})
    property string curveKey: "volume"
    property real selectedFrame: -1
    property string message: ""
    property bool awaitingEdit: false
    property var pendingChange: ({})
    readonly property var curves: targetClip.curves || []
    readonly property var curve: curves.find(c => c.key === curveKey) || {points: [], minimum: 0, maximum: 4, base: 1, factor: 100, available: false}
    readonly property var points: curve.points || []
    readonly property var visiblePoints: points.filter(p => p.frame >= 0 && p.frame <= (targetClip.length || 0))
    readonly property var selectedPoint: visiblePoints.find(p => p.frame === selectedFrame) || null
    readonly property real factor: curve.factor || 1
    readonly property bool stale: !!targetClip.valid && service && targetClip.stamp !== service.automationStamp
    readonly property bool editable: !!targetClip.valid && curve.available !== false && !stale && !awaitingEdit
        && service && !service.recording && !service.loading && !service.sampleEditing
    function openForClip(track, clip) { requestedTrack = track; requestedClip = clip; open() }
    function showDraft() {
        const point = selectedPoint
        frameInput.show(point ? point.frame : Math.max(0, Math.min(targetClip.length || 0, service ? service.playhead - (targetClip.start || 0) : 0)))
        valueInput.text = String(Number(((point ? point.value : curve.base) * factor).toFixed(6)))
        if (!point) selectedFrame = -1
        plot.requestPaint()
    }
    function reload(currentClip) {
        if (!service) return
        targetClip = currentClip ? service.clipEnvelopeTarget() : service.clipEnvelopeTarget(pinnedTrack, pinnedClip)
        if (targetClip.valid) { pinnedTrack = targetClip.track; pinnedClip = targetClip.clip }
        if (!curves.some(c => c.key === curveKey)) curveKey = "volume"
        selectedFrame = -1; awaitingEdit = false; pendingChange = ({}); message = ""; showDraft()
    }
    function edit(operation, frame, to, value, stamp) {
        if (!editable) return false
        // A control can activate its current choice. Do not wait for a commit
        // when the requested state already exists and no Undo step is needed.
        const point = points.find(p => p.frame === frame)
        if ((operation === "point" && curve.enabled && point && frame === to && point.value === value)
            || (operation === "hold" && !!curve.hold === (value !== 0))
            || (operation === "enabled" && !!curve.enabled === (value !== 0))
            || (operation === "interpolation" && point && (point.interpolation || 0) === value)
            || (operation === "clear" && points.length === 0 && !curve.enabled && !curve.hold)) return true
        pendingChange = {track: targetClip.track, clip: targetClip.clip, stamp: stamp || targetClip.stamp, key: curveKey, operation: operation, frame: frame, to: to, value: value}
        awaitingEdit = true
        const accepted = service.editClipEnvelope(targetClip.track, targetClip.clip, curveKey, operation, frame, to, value, stamp || targetClip.stamp)
        if (!accepted) { awaitingEdit = false; pendingChange = ({}); message = service.lastError; return false }
        if (awaitingEdit) message = qsTr("Clip envelope edit accepted. Finishing the current audio edit…")
        return true
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(860, parent ? parent.width - 32 : 860)
    height: Math.min(660, parent ? parent.height - 40 : 660)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: {
        if (awaitingEdit) { requestedTrack = -1; requestedClip = -1; return }
        targetClip = service ? service.clipEnvelopeTarget(requestedTrack, requestedClip) : ({})
        pinnedTrack = targetClip.valid ? targetClip.track : requestedTrack
        pinnedClip = targetClip.valid ? targetClip.clip : requestedClip
        requestedTrack = -1; requestedClip = -1; curveKey = "volume"; selectedFrame = -1
        awaitingEdit = false; pendingChange = ({}); message = ""; showDraft()
    }
    onClosed: { mouse.cancelDrag(); if (shell) shell.restoreFocus() }
    Connections {
        target: root.service
        function onClipEnvelopeEditFinished(track, clip, stamp, success, message) {
            if (!root.awaitingEdit || track !== root.pendingChange.track || clip !== root.pendingChange.clip || stamp !== root.pendingChange.stamp) return
            const change = root.pendingChange
            root.awaitingEdit = false; root.pendingChange = ({})
            root.message = message || (success ? qsTr("Clip envelope updated. Undo restores the previous curve.") : qsTr("The clip envelope edit could not be applied."))
            if (success) {
                root.targetClip = root.service.clipEnvelopeTarget(track, clip)
                root.selectedFrame = change.operation === "point" ? change.to : change.operation === "interpolation" ? change.frame : -1
                root.showDraft()
            }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        RowLayout {
            Text { text: qsTr("Clip envelopes"); Layout.fillWidth: true; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            ToolButton {
                iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close the clip envelope editor. Accepted edits are kept and can be undone.")
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "clipEnvelopeTargetName"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: Theme.textPrimary
            text: root.targetClip.valid ? qsTr("%1 · %2 · starts at %3").arg(Theme.shortName(root.targetClip.trackName, 36)).arg(Theme.shortName(root.targetClip.name, 36)).arg(root.shell.formatFrames(root.targetClip.start))
                : root.targetClip.reason || qsTr("Place the playhead or selection start inside a clip, then choose Use current clip.")
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("These points belong to this clip, including when it is grouped. Positions are measured from its start. Other clips retain their own envelopes.")
        }
        RowLayout {
            ComboField {
                objectName: "clipEnvelopeCurve"; caption: qsTr("Envelope"); Layout.fillWidth: true
                model: root.curves.map(c => c.label); currentIndex: root.curves.findIndex(c => c.key === root.curveKey)
                enabled: !!root.targetClip.valid && !root.awaitingEdit
                helpText: qsTr("Volume multiplies clip gain and fades. Pan balances the stereo channels before track effects, with both channels unchanged at the centre.")
                helpScope: qsTr("Pan is available for clips with at least two source channels. Track automation remains separate.")
                onActivated: index => {
                    if (index >= 0 && index < root.curves.length) { root.curveKey = root.curves[index].key; root.selectedFrame = -1; root.showDraft() }
                }
            }
            ToolButton {
                objectName: "clipEnvelopeEnabled"; text: qsTr("Enabled"); iconName: "power"; showLabel: true
                checkable: true; checked: !!root.curve.enabled; enabled: root.editable
                helpText: qsTr("Play this envelope, or disable it while retaining all its points.")
                helpScope: qsTr("One Undo step. A disabled volume curve uses unity; a disabled pan curve leaves the channels balanced.")
                unavailableReason: root.stale ? qsTr("Reload after the clip changed.") : qsTr("Choose an available clip envelope and finish any pending edit.")
                onClicked: root.edit("enabled", -1, 0, root.curve.enabled ? 0 : 1)
            }
            ComboField {
                objectName: "clipEnvelopeDefault"; caption: qsTr("Default shape"); Layout.preferredWidth: 230
                model: [qsTr("Linear"), qsTr("Hold")]; currentIndex: root.curve.hold ? 1 : 0; enabled: root.editable
                helpText: qsTr("Linear moves steadily between points. Hold keeps a value until the next point. Individual segments can override this default.")
                helpScope: qsTr("One Undo step for this clip envelope.")
                onActivated: index => root.edit("hold", -1, 0, index)
            }
        }
        RowLayout {
            ComboField {
                objectName: "clipEnvelopeSegment"; caption: qsTr("After selected point"); Layout.preferredWidth: 310
                model: [qsTr("Curve default"), qsTr("Linear"), qsTr("Hold"), qsTr("Smooth")]
                currentIndex: root.selectedPoint ? root.selectedPoint.interpolation || 0 : 0
                enabled: root.editable && root.selectedPoint !== null
                helpText: qsTr("Linear moves at a steady rate. Smooth eases out of this point and into the next. Hold keeps the value until the next point. An inherited point outside the visible clip can still define its boundary curve.")
                helpScope: qsTr("Partial transitions keep their shape when points move. To start a full Smooth transition, choose another shape and then Smooth. One Undo step.")
                unavailableReason: qsTr("Select a point inside this clip first.")
                onActivated: index => root.edit("interpolation", root.selectedFrame, 0, index)
            }
            Item { Layout.fillWidth: true }
            Text { text: qsTr("%1 visible · %2 inherited outside").arg(root.visiblePoints.length).arg(root.points.length - root.visiblePoints.length); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "clipEnvelopeUnavailable"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            visible: !!root.targetClip.valid && root.curve.available === false
            text: qsTr("Pan needs at least two source channels. Mono clips keep their existing channel behaviour.")
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 120; Layout.preferredHeight: 220
            color: Theme.controlBg; border.color: Theme.border; radius: Theme.rControl; clip: true
            Canvas {
                id: plot; objectName: "clipEnvelopePlot"; anchors.fill: parent; anchors.margins: 14; focus: true
                readonly property color ink: root.curve.enabled ? Theme.accent : Theme.textMuted
                readonly property color gridInk: Theme.border
                readonly property var graph: [root.points, root.curve.hold, root.curve.minimum, root.curve.maximum, root.curve.base, root.targetClip.length]
                onInkChanged: requestPaint()
                onGridInkChanged: requestPaint()
                onGraphChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                function px(frame) { return frame / Math.max(1, root.targetClip.length || 1) * width }
                function py(value) { return height - (value - root.curve.minimum) / Math.max(1e-20, root.curve.maximum - root.curve.minimum) * height }
                function frameAt(x) { return Math.round(Math.max(0, Math.min(width, x)) / Math.max(1, width) * (root.targetClip.length || 0)) }
                function valueAt(y) { return root.curve.minimum + (1 - Math.max(0, Math.min(height, y)) / Math.max(1, height)) * (root.curve.maximum - root.curve.minimum) }
                function sample(frame, data) { return Curve.sample(data, frame, root.curve.hold, root.curve.base) }
                function hit(x, y) {
                    for (let i = 0; i < root.points.length; ++i) {
                        const p = root.points[i]
                        if (p.frame < 0 || p.frame > root.targetClip.length) continue
                        if (Math.abs(px(p.frame) - x) < 10 && Math.abs(py(p.value) - y) < 10) return i
                    }
                    return -1
                }
                onPaint: {
                    const ctx = getContext("2d"); ctx.reset()
                    ctx.strokeStyle = gridInk; ctx.lineWidth = 1
                    for (let i = 0; i <= 4; ++i) { ctx.beginPath(); ctx.moveTo(0, height * i / 4); ctx.lineTo(width, height * i / 4); ctx.stroke() }
                    const data = Curve.moved(root.points, mouse.dragIndex, mouse.dragFrame, mouse.dragValue)
                    ctx.strokeStyle = ink; ctx.lineWidth = 2; ctx.beginPath()
                    Curve.trace(ctx, data, 0, root.targetClip.length || 0, root.curve.hold, root.curve.base, frame => plot.px(frame), value => plot.py(value))
                    ctx.stroke()
                    for (const p of data) if (p.frame >= 0 && p.frame <= root.targetClip.length) {
                        ctx.beginPath(); ctx.arc(px(p.frame), py(p.value), p.frame === root.selectedFrame ? 5 : 3, 0, 2 * Math.PI)
                        ctx.fillStyle = p.frame === root.selectedFrame ? Theme.textPrimary : ink; ctx.fill()
                    }
                }
                Keys.onDeletePressed: if (root.selectedPoint) root.edit("delete", root.selectedFrame, 0, 0)
                Keys.onEscapePressed: event => {
                    if (mouse.dragIndex >= 0) mouse.cancelDrag()
                    else event.accepted = false
                }
                MouseArea {
                    id: mouse; objectName: "clipEnvelopePlotMouse"; anchors.fill: parent
                    enabled: root.editable; acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property int dragIndex: -1
                    property real originalFrame: -1
                    property real originalValue: 0
                    property real dragFrame: 0
                    property real dragValue: 0
                    property string stamp: ""
                    onEnabledChanged: if (!enabled) cancelDrag()
                    function cancelDrag() { dragIndex = -1; plot.requestPaint() }
                    ControlHelp {
                        title: qsTr("Clip envelope curve")
                        purpose: qsTr("Double-click to add a point. Drag a point to move it. Right-click removes a point; click selects it for the exact fields below.")
                        scope: qsTr("Only points from this clip's start through its end can be edited here. Hidden inherited points still shape the boundary. Each edit is one Undo step.")
                        suppressed: mouse.pressed
                    }
                    onPressed: event => {
                        plot.forceActiveFocus()
                        const index = plot.hit(event.x, event.y)
                        if (event.button === Qt.RightButton) { if (index >= 0) root.edit("delete", root.points[index].frame, 0, 0); return }
                        if (index < 0) { root.selectedFrame = -1; root.showDraft(); return }
                        const point = root.points[index]
                        root.selectedFrame = point.frame; root.showDraft()
                        stamp = root.targetClip.stamp; dragIndex = index; originalFrame = point.frame; originalValue = point.value
                        dragFrame = originalFrame; dragValue = originalValue
                    }
                    onPositionChanged: event => { if (dragIndex >= 0) { dragFrame = plot.frameAt(event.x); dragValue = plot.valueAt(event.y); plot.requestPaint() } }
                    onReleased: {
                        if (dragIndex < 0) return
                        dragIndex = -1
                        if (dragFrame !== originalFrame || dragValue !== originalValue) root.edit("point", originalFrame, dragFrame, dragValue, stamp)
                        plot.requestPaint()
                    }
                    onCanceled: cancelDrag()
                    onDoubleClicked: event => { if (event.button === Qt.LeftButton && plot.hit(event.x, event.y) < 0) root.edit("point", -1, plot.frameAt(event.x), plot.valueAt(event.y)) }
                }
            }
        }
        RowLayout {
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.service ? root.service.formatDuration(0) : "0"; color: Theme.textMuted }
            Item { Layout.fillWidth: true }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.curveKey === "pan" ? qsTr("−100 left · 0 centre · +100 right") : qsTr("Volume 0–400%; 100% keeps the clip's level"); color: Theme.textMuted }
            Item { Layout.fillWidth: true }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.service ? root.service.formatDuration(root.targetClip.length || 0) : ""; color: Theme.textMuted }
        }
        RowLayout {
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("From clip start"); color: Theme.textSecondary }
            TimeField {
                id: frameInput; objectName: "clipEnvelopeFrame"; Layout.preferredWidth: 145; service: root.service; duration: true
                maximum: root.targetClip.length || 0; enabled: root.editable
                helpText: qsTr("A position relative to this clip's beginning. Zero is the clip start; the duration is its end. The source file's offset and timeline start are separate.")
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.curveKey === "pan" ? qsTr("Pan %") : qsTr("Volume %"); color: Theme.textSecondary }
            TextEntry {
                id: valueInput; objectName: "clipEnvelopeValue"; Layout.preferredWidth: 88; enabled: root.editable; selectByMouse: true
                validator: DoubleValidator { bottom: root.curve.minimum * root.factor; top: root.curve.maximum * root.factor; decimals: 6; notation: DoubleValidator.StandardNotation; locale: "C" }
                helpTitle: qsTr("Point value"); helpText: root.curveKey === "pan" ? qsTr("Stereo balance from −100 left to +100 right, with 0 centre and unity level on both channels.") : qsTr("Volume relative to the clip's gain and fades. 100% leaves that level, 50% halves the amplitude, and 0 is silence.")
            }
            ToolButton {
                objectName: "clipEnvelopeApply"; text: root.selectedPoint ? qsTr("Update point") : qsTr("Add point"); showLabel: true
                enabled: root.editable && frameInput.valid && valueInput.acceptableInput
                helpText: qsTr("Apply the exact point position and value shown, as one Undo step.")
                unavailableReason: root.stale ? qsTr("Reload after the clip changed.") : qsTr("Choose an available curve and values within this clip's range.")
                onClicked: root.edit("point", root.selectedPoint ? root.selectedFrame : -1, frameInput.value, Numeric.parse(valueInput.text, "C") / root.factor)
            }
            ToolButton {
                objectName: "clipEnvelopeDelete"; iconName: "trash-2"; text: qsTr("Delete point")
                enabled: root.editable && root.selectedPoint !== null; onClicked: root.edit("delete", root.selectedFrame, 0, 0)
                helpText: qsTr("Remove the selected point. One Undo step."); unavailableReason: qsTr("Select an editable point first.")
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "clipEnvelopeClear"; text: qsTr("Clear curve"); showLabel: true; enabled: root.editable && (root.points.length > 0 || root.curve.enabled || root.curve.hold)
                helpText: qsTr("Remove all points from this envelope, including inherited points outside the clip. One Undo step.")
                unavailableReason: qsTr("Choose an available curve with points first.")
                onClicked: root.edit("clear", -1, 0, 0)
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "clipEnvelopeMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            visible: text !== ""; color: root.stale && !root.awaitingEdit ? Theme.warning : Theme.textPrimary
            text: root.stale && !root.awaitingEdit ? (root.message ? root.message + " " : "")
                + qsTr("The project or clip changed. Reload this track position or use the current clip before editing.") : root.message
        }
        RowLayout {
            ToolButton {
                objectName: "clipEnvelopeReload"; text: qsTr("Reload"); showLabel: true; enabled: !root.awaitingEdit && root.pinnedTrack >= 0
                helpText: qsTr("Read the clip at the named track position again and discard draft values. If clips were split or moved, check the new name and start.")
                onClicked: root.reload(false)
            }
            ToolButton {
                objectName: "clipEnvelopeUseCurrent"; text: qsTr("Use current clip"); showLabel: true; enabled: !root.awaitingEdit
                helpText: qsTr("Switch to the clip under the current playhead or selection start. This explicitly changes which clip is edited.")
                onClicked: root.reload(true)
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "clipEnvelopeUndo"; iconName: "undo-2"; text: qsTr("Undo"); enabled: root.service && root.service.canUndo && !root.awaitingEdit
                helpText: qsTr("Undo the last project edit, then reload this clip position.")
                onClicked: { root.service.undo(); root.reload(false) }
            }
            ToolButton {
                objectName: "clipEnvelopeRedo"; iconName: "redo-2"; text: qsTr("Redo"); enabled: root.service && root.service.canRedo && !root.awaitingEdit
                helpText: qsTr("Redo the last undone project edit, then reload this clip position.")
                onClicked: { root.service.redo(); root.reload(false) }
            }
        }
    }
}
