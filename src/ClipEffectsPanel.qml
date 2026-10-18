pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// Effects that belong to one clip. They are processed for the clip's whole
// region before its gain, fades and envelopes, and are shared by the clip's
// split pieces. The sound is prepared in the background; until it is ready the
// clip is silent rather than played without its effects.
C.Popup {
    id: root
    objectName: "clipEffectsPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int requestedTrack: -1
    property int requestedClip: -1
    property int pinnedTrack: -1
    property int pinnedClip: -1
    property var target: ({})
    property int expandedSlot: -1
    property string message: ""
    readonly property var effects: target.effects || []
    readonly property bool stale: !!target.valid && service && target.stamp !== service.automationStamp
    readonly property bool editable: !!target.valid && !stale && service && !service.recording
        && !service.loading && !service.sampleEditing
    function openForClip(track, clip) { requestedTrack = track; requestedClip = clip; open() }
    // A clip effect's controls open in the effect panel, over this list.
    function openPanel(slot) {
        if (shell && target.valid) shell.openClipEffectPanel(target.track, target.clip, slot)
    }
    function load(track, clip) {
        if (!service) return
        target = service.clipEffectsTarget(track, clip)
        if (target.valid) { pinnedTrack = target.track; pinnedClip = target.clip }
        if (expandedSlot >= effects.length) expandedSlot = -1
    }
    function reload(currentClip) {
        message = ""
        if (currentClip) load(-1, -1); else load(pinnedTrack, pinnedClip)
    }
    function run(accepted, done) {
        if (!accepted) { message = service.lastError; return false }
        message = done
        load(pinnedTrack, pinnedClip)
        return true
    }
    function statusText() {
        switch (target.status) {
        case "ready": return qsTr("Ready — playback and exports include these effects.")
        case "rendering": return qsTr("Preparing the effected sound… The clip is silent until it is ready.")
        case "bypassed": return qsTr("All effects are bypassed. The clip plays its own audio; the settings are kept.")
        case "failed": return qsTr("These effects could not be prepared: %1").arg(target.problem || "")
        default: return qsTr("No clip effects. Add one to process this clip before its fades and envelopes.")
        }
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(860, parent ? parent.width - 32 : 860)
    height: Math.min(680, parent ? parent.height - 40 : 680)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: {
        load(requestedTrack, requestedClip)
        if (!target.valid) { pinnedTrack = requestedTrack; pinnedClip = requestedClip }
        requestedTrack = -1; requestedClip = -1; expandedSlot = -1; message = ""
    }
    // Handing off to the effect browser leaves keyboard focus with the browser.
    property bool handingOff: false
    onClosed: { if (shell && !handingOff) shell.restoreFocus(); handingOff = false }
    Connections {
        target: root.service
        // Rendering finishes in the background; keep the status line current.
        function onClipEffectsChanged() { if (root.opened) root.load(root.pinnedTrack, root.pinnedClip) }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        RowLayout {
            Text { text: qsTr("Clip effects"); Layout.fillWidth: true; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            ToolButton {
                iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close the clip effects. Accepted edits are kept and can be undone.")
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "clipEffectsTargetName"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: Theme.textPrimary
            text: root.target.valid ? qsTr("%1 · %2 · starts at %3").arg(Theme.shortName(root.target.trackName, 36)).arg(Theme.shortName(root.target.name, 36)).arg(root.shell.formatFrames(root.target.start))
                : root.target.reason || qsTr("Place the playhead or selection start inside a clip, then choose Use current clip.")
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "clipEffectsStatus"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            visible: !!root.target.valid
            color: root.target.status === "failed" ? Theme.warning : Theme.textSecondary
            text: root.statusText() + (root.target.views > 1
                ? " " + qsTr("Shared by %1 pieces of this clip; edits change all of them.").arg(root.target.views) : "")
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 160
            color: Theme.controlBg; border.color: Theme.border; radius: Theme.rControl; clip: true
            ListView {
                id: list; objectName: "clipEffectsList"
                anchors.fill: parent; anchors.margins: Theme.s2; spacing: Theme.s1
                model: root.effects
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar {}
                EmptyState {
                    anchors.centerIn: parent; visible: list.count === 0 && !!root.target.valid
                    title: qsTr("No clip effects")
                    description: qsTr("Effects added here process only this clip and its split pieces.")
                }
                delegate: ColumnLayout {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool expanded: root.expandedSlot === index
                    width: ListView.view.width - Theme.s2
                    spacing: Theme.s1
                    RowLayout {
                        Layout.fillWidth: true
                        ToolButton {
                            objectName: "clipEffectOpen"
                            iconName: "sliders-horizontal"; text: qsTr("Open Panel")
                            helpText: qsTr("Open this effect's controls in the middle of the window.")
                            helpScope: qsTr("Each change there is one Undo step; the clip's sound is prepared again.")
                            enabled: row.modelData.available !== false
                            onClicked: root.openPanel(row.index)
                        }
                        Text {
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                            objectName: "clipEffectName"
                            Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText
                            text: (row.index + 1) + ". " + row.modelData.name
                            color: row.modelData.bypass ? Theme.textMuted : Theme.textPrimary
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.openPanel(row.index)
                                ControlHelp {
                                    title: row.modelData.name
                                    purpose: qsTr("Click to open this effect's controls in the middle of the window.")
                                }
                            }
                        }
                        Text {
                            visible: !row.modelData.available; color: Theme.warning; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                            text: qsTr("Not installed")
                        }
                        Toggle {
                            objectName: "clipEffectEnabled"; label: qsTr("On"); checked: !row.modelData.bypass; enabled: root.editable
                            helpText: qsTr("Turn this effect off without removing it. Its settings are kept.")
                            helpScope: qsTr("One Undo step. With every effect off the clip plays its own audio.")
                            onToggled: root.run(root.service.setClipEffectBypass(root.target.track, root.target.clip, row.index, !checked, root.target.stamp),
                                                checked ? qsTr("Effect turned on.") : qsTr("Effect turned off."))
                        }
                        ToolButton {
                            iconName: "arrow-up"; text: qsTr("Move up"); enabled: root.editable && row.index > 0
                            helpText: qsTr("Process this effect earlier. One Undo step.")
                            onClicked: root.run(root.service.moveClipEffect(root.target.track, root.target.clip, row.index, row.index - 1, root.target.stamp), qsTr("Effect moved."))
                        }
                        ToolButton {
                            iconName: "arrow-down"; text: qsTr("Move down"); enabled: root.editable && row.index < root.effects.length - 1
                            helpText: qsTr("Process this effect later. One Undo step.")
                            onClicked: root.run(root.service.moveClipEffect(root.target.track, root.target.clip, row.index, row.index + 1, root.target.stamp), qsTr("Effect moved."))
                        }
                        ToolButton {
                            iconName: "trash-2"; text: qsTr("Remove"); enabled: root.editable
                            helpText: qsTr("Remove this effect from the clip. One Undo step.")
                            onClicked: root.run(root.service.removeClipEffect(root.target.track, root.target.clip, row.index, root.target.stamp), qsTr("Effect removed."))
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Theme.hairline; color: Theme.border }
                }
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "clipEffectsMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            visible: text !== ""; color: root.stale ? Theme.warning : Theme.textPrimary
            text: root.stale ? (root.message ? root.message + " " : "")
                + qsTr("The project or clip changed. Reload this clip or use the current clip before editing.") : root.message
        }
        RowLayout {
            ToolButton {
                objectName: "clipEffectsAdd"; iconName: "plus"; text: qsTr("Add effect"); showLabel: true; enabled: root.editable
                helpText: qsTr("Choose an installed effect for this clip. It is added after the existing ones.")
                unavailableReason: qsTr("Choose an existing clip first.")
                onClicked: {
                    const track = root.target.track, clip = root.target.clip, stamp = root.target.stamp
                    root.handingOff = true; root.close()
                    root.shell.openClipEffectBrowser(track, clip, stamp)
                }
            }
            ToolButton {
                objectName: "clipEffectsBake"; text: qsTr("Bake into audio"); showLabel: true
                enabled: root.editable && root.target.status === "ready"
                helpText: qsTr("Replace this piece's audio with the effected sound and remove its effects. Other pieces keep theirs.")
                helpScope: qsTr("One Undo step. Sample edits and repairs need baked audio.")
                unavailableReason: qsTr("Wait until the effects are ready.")
                onClicked: root.run(root.service.bakeClipEffects(root.target.track, root.target.clip, root.target.stamp), qsTr("Effects baked into this clip's audio."))
            }
            ToolButton {
                objectName: "clipEffectsClear"; text: qsTr("Remove all"); showLabel: true
                enabled: root.editable && root.effects.length > 0
                helpText: qsTr("Remove every effect from this clip and its pieces. One Undo step.")
                onClicked: root.run(root.service.clearClipEffects(root.target.track, root.target.clip, root.target.stamp), qsTr("Clip effects removed."))
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "clipEffectsReload"; text: qsTr("Reload"); showLabel: true; enabled: root.pinnedTrack >= 0
                helpText: qsTr("Read this clip's effects again.")
                onClicked: root.reload(false)
            }
            ToolButton {
                objectName: "clipEffectsUseCurrent"; text: qsTr("Use current clip"); showLabel: true
                helpText: qsTr("Switch to the clip under the playhead or selection start.")
                onClicked: root.reload(true)
            }
            ToolButton {
                iconName: "undo-2"; text: qsTr("Undo"); enabled: root.service && root.service.canUndo
                helpText: qsTr("Undo the last project edit, then reload this clip.")
                onClicked: { root.service.undo(); root.reload(false) }
            }
        }
    }
}
