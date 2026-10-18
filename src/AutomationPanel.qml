pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/AutomationCurve.js" as Curve
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "automationPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property string errorText: ""
    property int target: -2
    property int requestedTrack: -1
    property string requestedCurve: "volume"
    function openForTrack(track, key) { requestedTrack = track; requestedCurve = key || "volume"; open() }
    property string curveKey: "volume"
    property var targets: []
    property var curve: ({points: [], minimum: 0, maximum: 4, base: 1, factor: 100, available: true})
    property real selectedFrame: -1
    property real viewStart: 0
    property real viewEnd: 1
    readonly property bool editable: curve.editable !== false
    readonly property var points: curve.points || []
    readonly property var selectedPoint: points.find(p => p.frame === selectedFrame) || null
    readonly property real factor: curve.factor || 1
    // Track lists only while shown (see GeneratedDuckingPanel).
    property bool showing: false
    readonly property var trackNames: showing && service ? [qsTr("Master bus")].concat(service.trackSummaries.map(t => t.name)) : [qsTr("Master bus")]
    readonly property var trackIds: showing && service ? [-2].concat(service.trackIds) : [-2]
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(900, parent ? parent.width - 32 : 900)
    padding: Theme.s4
    // The scrolling column reaches into this margin (ScrollPane).
    leftPadding: Theme.s4 - Theme.s2; rightPadding: Theme.s4 - Theme.s3
    height: Math.min(automationContent.implicitHeight + padding * 2, parent ? parent.height - 40 : 900)
    modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: showing = true
    onOpened: {
        target = requestedTrack >= 0 ? requestedTrack : service && service.automationMode !== "read" ? service.automationTrack : service && service.focusedTrack >= 0 ? service.focusedTrack : -2
        errorText = ""
        if (scroll.contentItem) scroll.contentItem.contentY = 0
        curveKey = requestedTrack >= 0 ? requestedCurve : service && service.automationMode !== "read" ? service.automationKey : "volume"; selectedFrame = -1
        requestedTrack = -1
        viewStart = 0; viewEnd = Math.max(1, service ? service.takeFrames : 1)
        refresh()
    }
    onClosed: { showing = false; mouse.cancelDrag(); if (service) service.touchAutomation(false); if (shell) shell.restoreFocus() }
    function refresh() {
        if (!service) return
        targets = service.automationTargets(target)
        const found = targets.find(c => c.key === curveKey)
        curve = found || targets[0] || {points: [], minimum: 0, maximum: 4, base: 1, factor: 100, available: false}
        if (curve.key) curveKey = curve.key
        const point = points.find(p => p.frame === selectedFrame)
        frameInput.show(point ? point.frame : Math.round(service.playhead))
        // To the precision a person reads a level or a setting at.
        valueInput.text = String(Number(((point ? point.value : curve.base) * factor).toFixed(2)))
        if (!point) selectedFrame = -1
        plot.requestPaint()
    }
    function edit(operation, frame, to, value, stamp) {
        if (!editable) return false
        const ok = service.editAutomation(curveKey, operation, frame, to, value, stamp || "", target)
        errorText = ok ? "" : service.lastError
        if (ok) { selectedFrame = operation === "point" ? to : operation === "interpolation" ? frame : -1; refresh() }
        return ok
    }
    Connections {
        target: root.service
        function onTimelineChanged() { if (root.visible) root.refresh() }
        function onTracksChanged() { if (root.visible) root.refresh() }
        function onAutomationLiveChanged() { if (root.visible && root.service.automationWriting) root.refresh() }
        function onAutomationStampChanged() {
            if (mouse.dragIndex >= 0 && mouse.stamp !== root.service.automationStamp) { mouse.dragIndex = -1; plot.requestPaint() }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ScrollPane {
        id: scroll; objectName: "automationScroll"; clip: true; contentWidth: availableWidth
        ColumnLayout {
        id: automationContent; x: scroll.reachLeft; width: scroll.columnWidth
        spacing: Theme.s3
        RowLayout {
            Layout.fillWidth: true
            Text { text: qsTr("Automation"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                objectName: "automationGenerateDucking"; text: qsTr("Generate ducking…"); showLabel: true
                enabled: root.target >= 0 && root.service !== null
                helpText: qsTr("Analyse other tracks and preview editable volume points that lower this track while they are audible.")
                helpScope: qsTr("Opens a separate preview. Only Apply points changes automation; the realtime Ducking effect remains available in Quick Fixes.")
                unavailableReason: qsTr("Choose a track as the ducking target first.")
                onClicked: { const track = root.target; root.close(); root.shell.openGeneratedDucking(track) }
            }
            ToolButton { iconName: "x"; text: qsTr("Close automation"); helpText: qsTr("Close the editor. Curves already written are kept."); onClicked: root.close() }
        }
        RowLayout {
            ComboField {
                objectName: "automationTrack"; caption: qsTr("Track")
                helpText: qsTr("Choose the track, or the master bus, whose automation to edit.")
                helpScope: qsTr("Switching also returns the mode to Read.")
                Layout.preferredWidth: 230; model: root.trackNames; currentIndex: root.trackIds.indexOf(root.target)
                onActivated: index => { root.service.configureAutomation(root.trackIds[index], root.curveKey, "read"); root.target = root.trackIds[index]; root.selectedFrame = -1; root.refresh() }
            }
            ComboField {
                objectName: "automationParameter"; caption: qsTr("Parameter"); Layout.fillWidth: true
                helpText: qsTr("Choose which control's curve to edit: volume, pan or a control of one of the track's effects.")
                helpScope: qsTr("Volume multiplies the fader; pan overrides the pan control. Switching also returns the mode to Read.")
                model: root.targets.map(c => c.label); currentIndex: root.targets.findIndex(c => c.key === root.curveKey)
                onActivated: index => { root.service.configureAutomation(root.target, root.targets[index].key, "read"); root.curveKey = root.targets[index].key; root.selectedFrame = -1; root.refresh() }
            }
        }
        RowLayout {
            ComboField {
                objectName: "automationWriteMode"; caption: qsTr("Mode"); Layout.preferredWidth: 160
                helpText: qsTr("Read plays the curve. Write records the Live control from the moment playback starts. Touch records while you hold a control and returns to the curve over the Touch return time. Latch keeps recording after you let go, until Stop.")
                helpScope: root.curve.instance !== undefined
                    ? qsTr("The Live control writes only this channel, as one Undo step per pass. The shared rack control still affects both channels.")
                    : qsTr("Recording replaces the curve over the range played, as one Undo step per pass. The matching mixer and rack controls write too.")
                model: [qsTr("Read"), qsTr("Write"), qsTr("Touch"), qsTr("Latch")]
                currentIndex: root.service ? ["read", "write", "touch", "latch"].indexOf(root.service.automationMode) : 0
                onActivated: index => {
                    root.service.configureAutomation(root.target, root.curveKey, ["read", "write", "touch", "latch"][index])
                    root.errorText = root.service.lastError
                }
            }
            SliderField {
                objectName: "automationLiveControl"; Layout.fillWidth: true
                label: qsTr("Live"); labelWidth: 32
                helpTitle: qsTr("Live value")
                helpText: qsTr("Move this while playing in Write, Touch or Latch mode to record the parameter's value.")
                helpScope: qsTr("The range follows the parameter, in the units shown under the plot.")
                unavailableReason: !root.curve.available ? qsTr("This parameter is unavailable; its effect is missing or disabled.")
                                     : root.curve.recordable === false ? qsTr("Choose Left or Right to record an independent curve.")
                                                         : qsTr("Choose Write, Touch or Latch mode first.")
                from: root.curve.minimum * root.factor; to: root.curve.maximum * root.factor
                value: root.service ? root.service.automationLiveValue * root.factor : root.curve.base * root.factor
                decimals: 3; enabled: root.curve.available && root.curve.recordable !== false && root.service && root.service.automationMode !== "read"
                onEditingStarted: root.service.touchAutomation(true)
                onEdited: value => root.service.writeAutomationValue(value / root.factor)
                onEditingFinished: root.service.touchAutomation(false)
            }
            ToolButton {
                objectName: "automationFinishWrite"; text: qsTr("Finish"); showLabel: true
                helpText: qsTr("Return to Read mode. A pass being recorded is kept.")
                unavailableReason: qsTr("The mode is already Read.")
                enabled: root.service && root.service.automationMode !== "read"
                onClicked: { root.service.configureAutomation(root.target, root.curveKey, "read"); root.refresh() }
            }
            ToolButton {
                objectName: "automationCancelWrite"; text: qsTr("Discard pass"); showLabel: true
                helpText: qsTr("Throw away the pass being recorded and return to Read mode.")
                helpScope: qsTr("The curve goes back to what it was before this pass.")
                unavailableReason: qsTr("No pass is being recorded.")
                enabled: root.service && root.service.automationWriting
                onClicked: { root.service.finishAutomationRecording(true); root.service.configureAutomation(root.target, root.curveKey, "read"); root.refresh() }
            }
        }
        SliderField {
            objectName: "automationTouchReturn"; Layout.fillWidth: true
            label: qsTr("Touch return"); labelWidth: 110
            from: 0; to: 5; stepSize: 0.01; decimals: 2; suffix: qsTr(" s")
            value: root.service ? root.service.automationReturnSeconds : 1
            enabled: root.shell !== null
            helpText: qsTr("How long Touch takes to return to the existing curve after you release a control. Zero returns immediately; the default is 1 second.")
            helpScope: qsTr("Saved for all projects, like the same setting in Preferences. A return already in progress keeps its timing.")
            onEdited: value => { if (root.shell) root.shell.automationReturnSeconds = value }
            onEditingFinished: { if (root.shell) root.shell.syncPreferences() }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "automationWriteStatus"; Layout.fillWidth: true; wrapMode: Text.WordWrap
            color: root.service && root.service.automationWriting ? Theme.danger : Theme.textSecondary
            text: root.service ? root.service.automationLiveStatus : ""
        }
        Text {
            objectName: "automationTiming"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            visible: text !== ""
            text: !root.curve || !root.curve.key ? ""
                : root.curve.timing === "sample" ? (root.curve.key === "volume" || root.curve.key === "pan"
                      ? qsTr("Volume and pan follow the curve smoothly, one audio sample at a time.")
                      : qsTr("Automation points have exact timing. Changes between points use very small steps; busy passages can use larger steps."))
                : root.curve.timing === "block" ? qsTr("Automation points have exact timing. Changes between points use steps of 64 audio samples.")
                : root.curve.available === false ? qsTr("The plugin is not installed; the curve is kept and not played.") : ""
            ControlHelp {
                title: qsTr("Playback timing")
                purpose: qsTr("The curve stores changes over time. Volume and pan update every audio sample. CLAP effects support exact points and small steps; many simultaneous changes can make steps larger. LV2 effects use 64-sample steps between exact points. Live movements are recorded at the timing of each audio processing block.")
                scope: qsTr("Describes how this curve is played. Nothing here changes the curve.")
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "automationInstanceNotice"; Layout.fillWidth: true; wrapMode: Text.WordWrap
            color: Theme.textSecondary; textFormat: Text.PlainText
            visible: root.curve.independent === true || root.curve.instance !== undefined
            text: root.curve.independent === true
                ? qsTr("Left and right have separate curves. Choose a channel to edit its points.")
                : root.curve.inherited === true
                    ? qsTr("This channel follows the shared curve. Editing makes it independent.")
                    : qsTr("Edits affect only this channel. Use shared curve removes its override.")
        }
        RowLayout {
            ToolButton {
                objectName: "automationEnabled"; text: qsTr("Enabled"); iconName: "power"; showLabel: true
                helpTitle: qsTr("Curve enabled")
                helpText: qsTr("Switch this curve on or off. A disabled curve is kept but not played.")
                helpScope: qsTr("Acts on this parameter of this track. One Undo step.")
                checkable: true; checked: !!root.curve.enabled; enabled: root.editable
                unavailableReason: qsTr("Choose Left or Right to edit an independent curve.")
                onClicked: root.edit("enabled", -1, 0, root.curve.enabled ? 0 : 1)
            }
            ComboField {
                objectName: "automationInterpolation"; caption: qsTr("Default")
                helpTitle: qsTr("Curve shape")
                helpText: qsTr("Linear moves at a steady rate between points. Hold keeps each value until the next point. Individual segments can override this default.")
                helpScope: qsTr("Changes segments that use Curve default. One Undo step.")
                model: [qsTr("Linear"), qsTr("Hold")]; currentIndex: root.curve.hold ? 1 : 0
                enabled: root.editable; unavailableReason: qsTr("Choose Left or Right to edit an independent curve.")
                onActivated: index => root.edit("hold", -1, 0, index)
            }
            ToolButton {
                objectName: "automationInherit"; text: qsTr("Use shared curve"); showLabel: true
                visible: root.curve.instance !== undefined; enabled: root.curve.inherited === false
                helpText: qsTr("Remove this channel's override and follow the shared curve again.")
                helpScope: qsTr("The other channel is unchanged. One Undo step.")
                unavailableReason: qsTr("This channel already follows the shared curve.")
                onClicked: root.edit("inherit", -1, 0, 0)
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Selected time"); showLabel: true; enabled: root.service && !root.service.selectionEmpty
                helpText: qsTr("Show only the selected time range in the plot.")
                helpScope: qsTr("Changes the view, not the curve.")
                unavailableReason: qsTr("Select a time range first.")
                onClicked: { root.viewStart = root.service.selectionStart; root.viewEnd = Math.max(root.viewStart + 1, root.service.selectionEnd); plot.requestPaint() }
            }
            ToolButton {
                text: qsTr("Whole project"); showLabel: true
                helpText: qsTr("Show the whole project in the plot.")
                helpScope: qsTr("Changes the view, not the curve.")
                onClicked: { root.viewStart = 0; root.viewEnd = Math.max(1, root.service.takeFrames); plot.requestPaint() }
            }
        }
        RowLayout {
            ComboField {
                objectName: "automationPointInterpolation"; caption: qsTr("After selected point")
                Layout.preferredWidth: 310
                model: [qsTr("Curve default"), qsTr("Linear"), qsTr("Hold"), qsTr("Smooth")]
                enabled: root.editable && root.selectedPoint !== null
                currentIndex: root.selectedPoint ? (root.selectedPoint.interpolation || 0) : 0
                onActivated: index => root.edit("interpolation", root.selectedFrame, 0, index)
                helpTitle: qsTr("Selected segment shape")
                helpText: qsTr("Choose how the value reaches the next point. Linear moves at a steady rate. Smooth eases out of this point and into the next. Hold keeps the value until the next point. Curve default follows the setting above.")
                helpScope: qsTr("Changes the segment after the selected point. Partial transitions keep their shape when points move. To start a full Smooth transition, choose another shape and then Smooth. One Undo step.")
                unavailableReason: qsTr("Select an automation point in the plot first.")
            }
            Item { Layout.fillWidth: true }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 230
            color: Theme.controlBg; border.color: Theme.border; radius: Theme.rControl
            Canvas {
                id: plot; objectName: "automationPlot"; anchors.fill: parent; anchors.margins: 16; focus: true
                readonly property var graph: [root.points, root.curve.lanes, root.curve.hold, root.curve.minimum, root.curve.maximum, root.viewStart, root.viewEnd]
                onGraphChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                function sample(frame, data) { return Curve.sample(data, frame, root.curve.hold, root.curve.base) }
                function px(frame) { return (frame - root.viewStart) / (root.viewEnd - root.viewStart) * width }
                function py(value) { return height - (value - root.curve.minimum) / Math.max(1e-20, root.curve.maximum - root.curve.minimum) * height }
                function frameAt(x) { return Math.round(root.viewStart + Math.max(0, Math.min(width, x)) / width * (root.viewEnd - root.viewStart)) }
                function valueAt(y) { return root.curve.minimum + (1 - Math.max(0, Math.min(height, y)) / height) * (root.curve.maximum - root.curve.minimum) }
                function hit(x, y) {
                    for (let i = 0; i < root.points.length; ++i)
                        if (root.points[i].frame >= root.viewStart && root.points[i].frame <= root.viewEnd && Math.abs(px(root.points[i].frame) - x) < 10 && Math.abs(py(root.points[i].value) - y) < 10) return i
                    return -1
                }
                onPaint: {
                    const ctx = getContext("2d"); ctx.reset()
                    ctx.strokeStyle = Theme.border; ctx.lineWidth = 1
                    for (let i = 0; i <= 4; ++i) { ctx.beginPath(); ctx.moveTo(0, height * i / 4); ctx.lineTo(width, height * i / 4); ctx.stroke() }
                    const lanes = root.curve.lanes || [root.curve]
                    for (let channel = 0; channel < lanes.length; ++channel) {
                        const lane = lanes[channel]
                        const data = Curve.moved(lane.points, root.editable ? mouse.dragIndex : -1, mouse.dragFrame, mouse.dragValue)
                        const ink = channel === 0 ? Theme.accent : Theme.success
                        ctx.strokeStyle = lane.enabled ? ink : Theme.textMuted; ctx.lineWidth = 2
                        ctx.beginPath()
                        Curve.trace(ctx, data, root.viewStart, root.viewEnd, lane.hold, lane.base, frame => plot.px(frame), value => plot.py(value))
                        ctx.stroke()
                        for (const p of data) if (p.frame >= root.viewStart && p.frame <= root.viewEnd) {
                            ctx.beginPath(); ctx.arc(px(p.frame), py(p.value), root.editable && p.frame === root.selectedFrame ? 6 : 4, 0, 2 * Math.PI)
                            ctx.fillStyle = root.editable && p.frame === root.selectedFrame ? Theme.textPrimary : ink; ctx.fill()
                        }
                    }
                }
                Keys.onEscapePressed: event => {
                    if (mouse.dragIndex >= 0) mouse.cancelDrag()
                    else event.accepted = false
                }
                Keys.onDeletePressed: root.edit("delete", root.selectedFrame, 0, 0)
                MouseArea {
                    id: mouse; objectName: "automationPlotMouse"; anchors.fill: parent; enabled: root.editable; acceptedButtons: Qt.LeftButton | Qt.RightButton
                    ControlHelp {
                        title: qsTr("Automation curve")
                        purpose: qsTr("Double-click to add a point. Drag a point to move it. Right-click a point to remove it. Click a point to select it, then Delete removes it.")
                        scope: qsTr("Every change is one Undo step. The fields below set a selected point exactly.")
                        suppressed: mouse.pressed
                    }
                    property int dragIndex: -1
                    property real originalFrame: -1
                    property real dragFrame: 0
                    property real dragValue: 0
                    property string stamp: ""
                    onEnabledChanged: if (!enabled) cancelDrag()
                    function cancelDrag() { dragIndex = -1; plot.requestPaint() }
                    onPressed: event => {
                        plot.forceActiveFocus()
                        const index = plot.hit(event.x, event.y)
                        if (event.button === Qt.RightButton) {
                            if (index >= 0) root.edit("delete", root.points[index].frame, 0, 0)
                            return
                        }
                        if (index < 0) { root.selectedFrame = -1; root.refresh(); return }
                        root.selectedFrame = root.points[index].frame; root.refresh()
                        if (!root.curve.available) return
                        stamp = root.service.automationStamp; dragIndex = index
                        originalFrame = root.points[index].frame; dragFrame = originalFrame; dragValue = root.points[index].value
                        plot.requestPaint()
                    }
                    onPositionChanged: event => {
                        if (dragIndex < 0) return
                        dragFrame = plot.frameAt(event.x); dragValue = plot.valueAt(event.y); plot.requestPaint()
                    }
                    onReleased: {
                        if (dragIndex < 0) return
                        dragIndex = -1; root.edit("point", originalFrame, dragFrame, dragValue, stamp); plot.requestPaint()
                    }
                    onCanceled: cancelDrag()
                    onDoubleClicked: event => {
                        if (event.button === Qt.LeftButton && plot.hit(event.x, event.y) < 0)
                            root.edit("point", -1, plot.frameAt(event.x), plot.valueAt(event.y))
                    }
                }
            }
        }
        RowLayout {
            visible: root.curve.independent === true
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Left"); color: Theme.accent }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Right"); color: Theme.success }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.shell ? root.shell.formatFrames(root.viewStart) : ""; color: Theme.textMuted }
            Item { Layout.fillWidth: true }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Range: %1 to %2").arg(root.curve.minimum * root.factor).arg(root.curve.maximum * root.factor); color: Theme.textMuted }
            Item { Layout.fillWidth: true }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.shell ? root.shell.formatFrames(root.viewEnd) : ""; color: Theme.textMuted }
        }
        RowLayout {
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("At"); color: Theme.textSecondary }
            TimeField {
                id: frameInput; objectName: "automationFrame"; Layout.preferredWidth: 145; selectByMouse: true
                service: root.service
                helpTitle: qsTr("Point position")
                helpText: qsTr("Where on the timeline the point sits. Add point or Update point uses this position.")
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Value"); color: Theme.textSecondary }
            TextEntry {
                id: valueInput; objectName: "automationValue"; Layout.preferredWidth: 120; selectByMouse: true
                validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                helpTitle: qsTr("Point value")
                helpText: qsTr("The parameter's value at that point, in the units of the Range line under the plot.")
                helpScope: qsTr("Values outside the range are refused.")
            }
            ToolButton {
                objectName: "automationApply"; text: root.selectedFrame >= 0 ? qsTr("Update point") : qsTr("Add point"); showLabel: true
                helpText: root.selectedFrame >= 0 ? qsTr("Move the selected point to the position and value typed here.")
                                                  : qsTr("Add a point at the position and value typed here.")
                helpScope: qsTr("One Undo step. Double-clicking the plot adds a point too.")
                unavailableReason: !root.curve.available ? qsTr("This parameter is unavailable; its effect is missing or disabled.")
                                     : !root.editable ? qsTr("Choose Left or Right to edit an independent curve.")
                                                         : qsTr("Enter a position and a value first.")
                enabled: root.editable && !!root.curve.available && frameInput.valid && valueInput.acceptableInput
                onClicked: root.edit("point", root.selectedFrame, Math.round(frameInput.value), Numeric.parse(valueInput.text) / root.factor)
            }
            ToolButton { objectName: "automationDelete"; iconName: "trash-2"; text: qsTr("Delete point")
                helpText: qsTr("Remove the selected point from the curve.")
                helpScope: qsTr("One Undo step. Right-clicking a point in the plot removes it too.")
                unavailableReason: !root.editable ? qsTr("Choose Left or Right to edit an independent curve.")
                                                  : qsTr("Click a point in the plot to select it first.")
                enabled: root.editable && root.selectedFrame >= 0; onClicked: root.edit("delete", root.selectedFrame, 0, 0) }
            Item { Layout.fillWidth: true }
            ToolButton { objectName: "automationClear"; text: qsTr("Clear curve"); showLabel: true
                helpText: qsTr("Remove every point from this curve.")
                helpScope: root.curve.instance !== undefined
                    ? qsTr("One Undo step. This channel then uses its base value; the other channel is unchanged.")
                    : qsTr("One Undo step. The parameter then follows its ordinary control.")
                unavailableReason: !root.editable ? qsTr("Choose Left or Right to edit an independent curve.")
                                                  : qsTr("This curve has no points.")
                enabled: root.editable && root.points.length > 0; onClicked: root.edit("clear", -1, 0, 0) }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; visible: root.errorText !== ""; text: root.errorText
            textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.textPrimary
        }
        RowLayout {
            ToolButton { objectName: "automationPlay"; iconName: root.service && root.service.playing ? "pause" : "play"; text: qsTr("Play / pause")
                helpText: qsTr("Start or pause playback without leaving the editor. In Write, Touch or Latch mode, playing records the Live control.")
                onClicked: root.shell.runAction("play_stop") }
            ToolButton { objectName: "automationStop"; iconName: "square"; text: qsTr("Stop")
                helpText: qsTr("Stop playback. A Latch or Write pass ends here.")
                onClicked: root.shell.runAction("stop") }
            ToolButton { objectName: "automationUndo"; iconName: "undo-2"; text: qsTr("Undo")
                helpText: qsTr("Reverse the last edit, in this editor or anywhere else in the project.")
                unavailableReason: qsTr("Nothing to undo.")
                enabled: root.service && root.service.canUndo; onClicked: root.shell.runAction("undo") }
            ToolButton { objectName: "automationRedo"; iconName: "redo-2"; text: qsTr("Redo")
                helpText: qsTr("Redo the edit you just undid.")
                unavailableReason: qsTr("Nothing to redo.")
                enabled: root.service && root.service.canRedo; onClicked: root.shell.runAction("redo") }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: !root.editable ? qsTr("Choose Left or Right to edit an independent curve.") : root.curve.available
                    ? qsTr("Double-click to add · Drag to move · Right-click to remove · Undo restores each edit. The volume curve adjusts the track level; the pan curve replaces the pan setting. See the timing note above for effects.")
                    : qsTr("This effect parameter is unavailable. Its saved curve is retained; you can disable, delete or clear it.")
            }
        }
    }
    }
}
