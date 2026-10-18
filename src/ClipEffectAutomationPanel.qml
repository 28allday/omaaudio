pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/AutomationCurve.js" as Curve
import "ui/NumericDraft.js" as Numeric

// One clip effect control drawn across its clip. The curve belongs to the
// clip's effect region, so split pieces continue the same line and moving the
// clip moves the curve with it.
C.Popup {
    id: root
    objectName: "clipEffectAutomationPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int track: -1
    property int clipIndex: -1
    property int slot: -1
    property var port: ({})
    property var target: ({})
    property real selectedFrame: -1
    property string message: ""
    readonly property var effect: (target.effects || [])[slot] || ({})
    readonly property var control: (effect.controls || []).find(c => c.index === port.index) || ({points: [], min: 0, max: 1, value: 0})
    readonly property var points: control.points || []
    readonly property var visiblePoints: points.filter(p => p.frame >= 0 && p.frame <= (target.length || 0))
    readonly property var selectedPoint: visiblePoints.find(p => p.frame === selectedFrame) || null
    readonly property bool stale: !!target.valid && service && target.stamp !== service.automationStamp
    readonly property bool editable: !!target.valid && !stale && service && !service.recording && !service.loading
    function openFor(t, c, s, p) { track = t; clipIndex = c; slot = s; port = p; open() }
    function load() { if (service) target = service.clipEffectsTarget(track, clipIndex); plot.requestPaint() }
    function edit(operation, frame, to, value) {
        if (!editable) return false
        const ok = service.editClipEffectAutomation(track, clipIndex, slot, port.index, operation, frame, to, value, target.stamp)
        message = ok ? qsTr("Automation updated. Preparing the new sound…") : service.lastError
        if (ok) { load(); selectedFrame = operation === "point" ? to : -1 }
        return ok
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(820, parent ? parent.width - 32 : 820)
    height: Math.min(520, parent ? parent.height - 40 : 520)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: { selectedFrame = -1; message = ""; load() }
    onClosed: if (shell) shell.openClipEffects(track, clipIndex)
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        RowLayout {
            Text {
                Layout.fillWidth: true; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; elide: Text.ElideRight
                text: qsTr("%1 · %2").arg(root.effect.name || "").arg(root.port.name || "")
            }
            ToolButton { iconName: "x"; text: qsTr("Close"); onClicked: root.close(); helpText: qsTr("Return to the clip's effects.") }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Double-click to add a point, drag to move it, right-click to remove it. Positions are measured from the clip's start.")
        }
        RowLayout {
            Toggle {
                objectName: "clipEffectAutomationEnabled"; label: qsTr("Follow curve"); checked: !!root.control.automationEnabled
                enabled: root.editable && root.points.length > 0
                helpText: qsTr("Play the curve, or keep its points and use the fixed control value.")
                onToggled: root.edit("enabled", -1, 0, checked ? 1 : 0)
            }
            Item { Layout.fillWidth: true }
            Text { color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; text: qsTr("%1 points").arg(root.visiblePoints.length) }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 140
            color: Theme.controlBg; border.color: Theme.border; radius: Theme.rControl; clip: true
            Canvas {
                id: plot; objectName: "clipEffectAutomationPlot"; anchors.fill: parent; anchors.margins: 14
                readonly property real low: root.control.min
                readonly property real high: root.control.max
                readonly property var graph: [root.points, low, high, root.target.length]
                onGraphChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                function px(frame) { return frame / Math.max(1, root.target.length || 1) * width }
                function py(value) { return height - (value - low) / Math.max(1e-20, high - low) * height }
                function frameAt(x) { return Math.round(Math.max(0, Math.min(width, x)) / Math.max(1, width) * (root.target.length || 0)) }
                function valueAt(y) { return low + (1 - Math.max(0, Math.min(height, y)) / Math.max(1, height)) * (high - low) }
                function hit(x, y) {
                    for (let i = 0; i < root.points.length; ++i) {
                        const p = root.points[i]
                        if (Math.abs(px(p.frame) - x) < 10 && Math.abs(py(p.value) - y) < 10) return i
                    }
                    return -1
                }
                onPaint: {
                    const ctx = getContext("2d"); ctx.reset()
                    ctx.strokeStyle = Theme.border; ctx.lineWidth = 1
                    for (let i = 0; i <= 4; ++i) { ctx.beginPath(); ctx.moveTo(0, height * i / 4); ctx.lineTo(width, height * i / 4); ctx.stroke() }
                    const data = Curve.moved(root.points, area.dragIndex, area.dragFrame, area.dragValue)
                    ctx.strokeStyle = root.control.automationEnabled ? Theme.accent : Theme.textMuted; ctx.lineWidth = 2; ctx.beginPath()
                    Curve.trace(ctx, data, 0, root.target.length || 0, false, root.control.value, f => plot.px(f), v => plot.py(v))
                    ctx.stroke()
                    for (const p of data) {
                        ctx.beginPath(); ctx.arc(px(p.frame), py(p.value), p.frame === root.selectedFrame ? 5 : 3, 0, 2 * Math.PI)
                        ctx.fillStyle = p.frame === root.selectedFrame ? Theme.textPrimary : Theme.accent; ctx.fill()
                    }
                }
                MouseArea {
                    id: area; anchors.fill: parent; enabled: root.editable; acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property int dragIndex: -1
                    property real originalFrame: -1
                    property real dragFrame: 0
                    property real dragValue: 0
                    ControlHelp {
                        title: qsTr("Effect control curve")
                        purpose: qsTr("Double-click to add a point. Drag a point to move it. Right-click removes a point.")
                        scope: qsTr("Positions belong to the clip, so split pieces continue one curve. Each edit is one Undo step.")
                        suppressed: area.pressed
                    }
                    onPressed: event => {
                        const index = plot.hit(event.x, event.y)
                        if (event.button === Qt.RightButton) { if (index >= 0) root.edit("delete", root.points[index].frame, 0, 0); return }
                        if (index < 0) { root.selectedFrame = -1; return }
                        const p = root.points[index]
                        root.selectedFrame = p.frame; dragIndex = index; originalFrame = p.frame; dragFrame = p.frame; dragValue = p.value
                    }
                    onPositionChanged: event => { if (dragIndex >= 0) { dragFrame = plot.frameAt(event.x); dragValue = plot.valueAt(event.y); plot.requestPaint() } }
                    onReleased: {
                        if (dragIndex < 0) return
                        dragIndex = -1
                        root.edit("point", originalFrame, dragFrame, dragValue)
                    }
                    onDoubleClicked: event => { if (plot.hit(event.x, event.y) < 0) root.edit("point", -1, plot.frameAt(event.x), plot.valueAt(event.y)) }
                }
            }
        }
        RowLayout {
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("From clip start"); color: Theme.textSecondary }
            TimeField {
                id: frameInput; objectName: "clipEffectAutomationFrame"; Layout.preferredWidth: 145; service: root.service; duration: true
                maximum: root.target.length || 0; enabled: root.editable
                helpText: qsTr("A position relative to this clip's beginning.")
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Value"); color: Theme.textSecondary }
            TextEntry {
                id: valueInput; objectName: "clipEffectAutomationValue"; Layout.preferredWidth: 100; enabled: root.editable; selectByMouse: true
                validator: DoubleValidator { bottom: root.control.min; top: root.control.max; decimals: 6; notation: DoubleValidator.StandardNotation; locale: "C" }
                helpTitle: qsTr("Point value"); helpText: qsTr("The control's value at this point, within its range.")
            }
            ToolButton {
                text: root.selectedPoint ? qsTr("Update point") : qsTr("Add point"); showLabel: true
                enabled: root.editable && frameInput.valid && valueInput.acceptableInput
                helpText: qsTr("Apply the exact position and value shown, as one Undo step.")
                onClicked: root.edit("point", root.selectedPoint ? root.selectedFrame : -1, frameInput.value, Numeric.parse(valueInput.text, "C"))
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Clear curve"); showLabel: true; enabled: root.editable && root.points.length > 0
                helpText: qsTr("Remove every point from this control. One Undo step.")
                onClicked: root.edit("clear", -1, 0, 0)
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; visible: text !== ""
            color: root.stale ? Theme.warning : Theme.textPrimary
            text: root.stale ? qsTr("The project or clip changed. Close and reopen this curve.") : root.message
        }
    }
}
