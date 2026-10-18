pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "crossfadePanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var targetClip: ({})
    property bool submitted: false
    property string message: ""
    readonly property real from: startInput.value
    readonly property real to: endInput.value
    readonly property bool valid: !!targetClip.valid && startInput.valid && endInput.valid
        && from >= targetClip.minimum && from < targetClip.join && to > targetClip.join && to <= targetClip.maximum
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(570, parent ? parent.width - 32 : 570)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    function refresh() {
        targetClip = service ? service.crossfadeTarget() : {}
        startInput.show(targetClip.start || 0)
        endInput.show(targetClip.end || 0)
        shape.currentIndex = targetClip.equalPower ? 1 : 0
    }
    onOpened: { refresh(); message = ""; submitted = false }
    onClosed: if (shell) shell.restoreFocus()
    Connections {
        target: root.service
        function onSampleEditChanged() {
            if (root.submitted && !root.service.sampleEditing) {
                root.submitted = false
                root.message = root.service.lastError || qsTr("Crossfade updated.")
                root.refresh()
            }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Adjust crossfade"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                iconName: "x"; text: qsTr("Close")
                helpText: qsTr("Close this panel.")
                helpScope: qsTr("A crossfade that is already updating carries on; its result still lands with one Undo step.")
                onClicked: root.close()
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: root.targetClip.valid
                ? qsTr("The crossfade can run anywhere from %1 to %2. The clips join at %3. Drag the handles or type the times.").arg(root.shell.formatFrames(root.targetClip.minimum)).arg(root.shell.formatFrames(root.targetClip.maximum)).arg(root.shell.formatFrames(root.targetClip.join))
                : qsTr("Place the playhead inside an editable crossfade. Its surrounding clips must still match the original pair.")
        }
        RowLayout {
            enabled: !root.submitted
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Start"); color: Theme.textSecondary }
            TimeField {
                id: startInput; objectName: "crossfadeStart"; Layout.fillWidth: true; selectByMouse: true; service: root.service

                helpTitle: qsTr("Crossfade start"); helpText: qsTr("Where on the timeline the crossfade begins. The first clip is still on its own before this. Within the range the message above gives; applied with Apply crossfade.")
            }
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("End"); color: Theme.textSecondary }
            TimeField {
                id: endInput; objectName: "crossfadeEnd"; Layout.fillWidth: true; selectByMouse: true; service: root.service

                helpTitle: qsTr("Crossfade end"); helpText: qsTr("Where on the timeline the crossfade ends. Only the second clip plays after this. Within the range the message above gives; applied with Apply crossfade.")
            }
        }
        ComboField {
            id: shape; objectName: "crossfadeShape"; caption: qsTr("Shape"); Layout.fillWidth: true
            helpTitle: qsTr("Crossfade shape")
            helpText: qsTr("How the two clips blend across the join. Linear fades one out and the other in by equal steps; Equal power keeps the loudness steady through the middle, which suits sounds that do not share phase, such as two different takes.")
            helpScope: qsTr("Applies to the next Apply crossfade. The plot above shows the two curves.")
            unavailableReason: qsTr("Wait for the current crossfade update to finish.")
            model: [qsTr("Linear"), qsTr("Equal power")]; enabled: !root.submitted
        }
        Rectangle {
            id: graph; Layout.fillWidth: true; Layout.preferredHeight: 130
            color: Theme.controlBg; border.color: Theme.border; radius: Theme.rControl
            function px(frame) { return (frame - (root.targetClip.minimum || 0)) / Math.max(1, (root.targetClip.maximum || 1) - (root.targetClip.minimum || 0)) * width }
            Canvas {
                id: plot; anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d"); ctx.reset()
                    for (let line = 0; line < 2; ++line) {
                        ctx.beginPath(); ctx.strokeStyle = line ? Theme.audioPrimary : Theme.accent; ctx.lineWidth = 2
                        for (let x = 0; x <= width; ++x) {
                            const t = Math.max(0, Math.min(1, (x - graph.px(root.from)) / Math.max(1, graph.px(root.to) - graph.px(root.from))))
                            const v = shape.currentIndex ? (line ? Math.sin(t * Math.PI / 2) : Math.cos(t * Math.PI / 2)) : (line ? t : 1 - t)
                            const y = 12 + (1 - v) * (height - 24)
                            if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                        }
                        ctx.stroke()
                    }
                }
                Connections {
                    target: root
                    function onFromChanged() { plot.requestPaint() }
                    function onToChanged() { plot.requestPaint() }
                    function onTargetClipChanged() { plot.requestPaint() }
                }
                Connections { target: shape; function onCurrentIndexChanged() { plot.requestPaint() } }
            }
            Repeater {
                model: root.targetClip.valid ? 2 : 0
                delegate: Rectangle {
                    id: handle; required property int index
                    objectName: index ? "crossfadeEndHandle" : "crossfadeStartHandle"
                    x: Math.max(0, Math.min(graph.width - width, graph.px(index ? root.to : root.from) - width / 2))
                    width: 12; height: parent.height; color: Theme.withAlpha(Theme.accent, 0.25); border.color: Theme.accent
                    MouseArea {
                        anchors.fill: parent; enabled: !root.submitted; cursorShape: Qt.SizeHorCursor
                        onPositionChanged: event => {
                            if (!pressed) return
                            const p = mapToItem(graph, event.x, event.y)
                            let frame = Math.round(root.targetClip.minimum + p.x / graph.width * (root.targetClip.maximum - root.targetClip.minimum))
                            if (handle.index) endInput.show(Math.max(root.targetClip.join + 1, Math.min(root.targetClip.maximum, frame)))
                            else startInput.show(Math.max(root.targetClip.minimum, Math.min(root.targetClip.join - 1, frame)))
                        }
                    }
                }
            }
        }
        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; text: qsTr("Changes rebuild from the original takes. Other clips stay in place. Apply creates one Undo step.") }
        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; objectName: "crossfadeMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: Theme.textPrimary; text: root.submitted ? qsTr("Updating crossfade…") : root.message; visible: text !== "" }
        RowLayout {
            Item { Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Cancel processing"); showLabel: true; enabled: root.submitted
                helpText: qsTr("Stop the crossfade update that is running.")
                helpScope: qsTr("A cancelled update writes nothing; the crossfade stays as it was.")
                unavailableReason: qsTr("No crossfade update is running.")
                onClicked: root.service.cancelSampleEdit()
            }
            ToolButton {
                objectName: "applyCrossfade"; text: qsTr("Apply crossfade"); showLabel: true; iconName: "blend"
                helpText: qsTr("Rebuild the crossfade between the two original clips over the new start and end, with the chosen shape.")
                helpScope: qsTr("Acts on the crossfade at the playhead. It is reexported from the original takes, so adjusting again loses nothing; other clips stay in place. One Undo step.")
                unavailableReason: !root.targetClip.valid ? qsTr("Place the playhead inside an editable crossfade whose surrounding clips still match the original pair.")
                                   : root.submitted ? qsTr("Wait for the current crossfade update to finish.")
                                   : qsTr("Keep the start before the join and the end after it, both within the range shown above.")
                enabled: root.valid && !root.submitted
                onClicked: {
                    root.service.adjustCrossfade(root.from, root.to, shape.currentIndex === 1, root.targetClip.stamp, root.targetClip.track, root.targetClip.clip)
                    root.submitted = root.service.sampleEditing; root.message = root.service.lastError
                }
            }
        }
    }
}
