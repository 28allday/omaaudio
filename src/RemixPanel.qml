pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// Re-arrange a music clip to a new duration by skipping or repeating passages
// that match, keeping its opening and ending. Replaces the clip; one Undo step.
C.Popup {
    id: root
    objectName: "remixPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var target: service ? (service.automationStamp, service.playhead, service.focusedTrack, service.remixTarget()) : ({})
    readonly property bool running: !!service && service.remixing
    readonly property bool ready: !running && !!target.valid && duration.valid && duration.value >= 5 * (service ? service.sampleRate : 48000)

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 32 : 520)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: if (target.valid) duration.show(target.length)
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Fit music to length"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Close"); iconName: "x"; onClicked: root.close()
                helpText: qsTr("Close this panel. Fitting carries on and replaces the clip when it finishes.")
            }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Make a piece of music shorter or longer by skipping or repeating passages where it matches itself, joined with short crossfades. The opening and ending are kept. Works best on music with repeating sections.")
        }
        Text {
            objectName: "remixClip"
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.target.valid
                ? qsTr("%1 · now %2 long").arg(Theme.shortName(root.target.name, 48)).arg(root.shell ? root.shell.formatDuration(root.target.length) : root.target.length)
                : qsTr("Put the playhead over the music clip on the active track.")
        }
        RowLayout {
            Text { text: qsTr("New duration"); color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; Layout.preferredWidth: 120 }
            TimeField {
                id: duration; objectName: "remixDuration"; duration: true; Layout.fillWidth: true; selectByMouse: true
                service: root.service; minimum: 1
                enabled: !root.running && !!root.target.valid
                helpTitle: qsTr("New duration")
                helpText: qsTr("How long the music should become, at least 5 seconds and at most four times its length. The result lands within about 2% of it.")
                helpScope: qsTr("Later clips on the track must leave room for a longer result.")
            }
        }
        Text {
            objectName: "remixMessage"; visible: text !== ""
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.service ? root.service.remixMessage : ""
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "remixCancel"; text: qsTr("Cancel"); showLabel: true; visible: root.running
                onClicked: root.service.cancelRemix()
                helpText: qsTr("Stop fitting. Nothing is changed.")
            }
            ToolButton {
                objectName: "remixApply"; text: qsTr("Fit"); showLabel: true
                enabled: root.ready
                onClicked: root.service.remixClip(duration.value / root.service.sampleRate, root.service.automationStamp)
                helpText: qsTr("Re-arrange the clip to the new duration. One Undo step restores the original.")
                unavailableReason: qsTr("Put the playhead over a music clip and type a duration of at least 5 seconds.")
            }
        }
    }
}
