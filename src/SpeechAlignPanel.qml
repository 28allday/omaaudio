pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// Re-time a second performance of the same words to follow a reference clip. The
// clip under the playhead on each chosen track is used; the aligned copy goes on
// a new track and the originals stay as they are.
C.Popup {
    id: root
    objectName: "speechAlignPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    // Track data only while shown: a closed panel would otherwise re-evaluate its
    // per-track controls on every edit, which adds up in large sessions.
    property bool showing: false
    readonly property var tracks: showing && service ? service.trackSummaries : []
    property int referenceTrack: -1
    property int alignTrack: -1
    property real tightness: 70
    readonly property var target: service && referenceTrack >= 0 && alignTrack >= 0
        ? (service.automationStamp, service.playhead, service.speechAlignmentTarget(referenceTrack, alignTrack)) : ({})
    readonly property bool running: !!service && service.aligningSpeech
    readonly property bool ready: !running && referenceTrack >= 0 && alignTrack >= 0 && referenceTrack !== alignTrack
        && !!target.reference && target.reference.valid && !!target.align && target.align.valid

    function clipText(entry) {
        if (!entry || !entry.valid) return qsTr("No clip under the playhead")
        return qsTr("%1 · %2 long").arg(Theme.shortName(entry.name, 48)).arg(shell ? shell.formatFrames(entry.length) : entry.length)
    }
    function align() {
        if (ready) service.alignSpeech(referenceTrack, alignTrack, tightness / 100, service.automationStamp)
    }

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 32 : 560)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: {
        showing = true
        // The focused track is usually the new take; the reference is the track above it.
        const ids = tracks.map(t => t.id)
        const focused = service ? ids.indexOf(service.focusedTrack) : -1
        alignTrack = focused >= 0 ? ids[focused] : (ids.length > 1 ? ids[1] : -1)
        referenceTrack = ids.length > 1 ? ids[focused > 0 ? focused - 1 : (alignTrack === ids[0] ? 1 : 0)] : -1
    }
    onClosed: { showing = false; if (shell) shell.restoreFocus() }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Match speech timing"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Close"); iconName: "x"; onClicked: root.close()
                helpText: qsTr("Close this panel. A running alignment carries on and adds its track when it finishes.")
            }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Re-time a second performance of the same words, such as a re-recorded line, so it follows the reference. Its voice and pitch are kept. The result goes on a new track; the originals are unchanged.")
        }
        ComboField {
            objectName: "speechAlignReference"; Layout.fillWidth: true; caption: qsTr("Reference")
            model: root.tracks.map(t => t.name); currentIndex: root.tracks.findIndex(t => t.id === root.referenceTrack)
            onActivated: index => root.referenceTrack = root.tracks[index].id
            enabled: !root.running
            helpText: qsTr("The track whose timing is kept, such as the original production sound.")
            unavailableReason: qsTr("Wait for the alignment to finish, or cancel it.")
        }
        Text { text: root.clipText(root.target.reference); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        ComboField {
            objectName: "speechAlignTake"; Layout.fillWidth: true; caption: qsTr("Track to align")
            model: root.tracks.map(t => t.name); currentIndex: root.tracks.findIndex(t => t.id === root.alignTrack)
            onActivated: index => root.alignTrack = root.tracks[index].id
            enabled: !root.running
            helpText: qsTr("The track with the performance to re-time.")
            unavailableReason: qsTr("Wait for the alignment to finish, or cancel it.")
        }
        Text { text: root.clipText(root.target.align); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        SliderField {
            objectName: "speechAlignTightness"; Layout.fillWidth: true
            label: qsTr("Tightness"); from: 0; to: 100; stepSize: 5; decimals: 0; suffix: "%"; value: root.tightness
            enabled: !root.running
            helpText: qsTr("Higher follows the reference closely moment to moment; lower keeps more of the performance's own rhythm over short spans.")
            onEdited: function(v) { root.tightness = v }
        }
        Text {
            objectName: "speechAlignMessage"; visible: text !== ""
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.service ? root.service.speechAlignMessage : ""
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "speechAlignCancel"; text: qsTr("Cancel"); showLabel: true; visible: root.running
                onClicked: root.service.cancelSpeechAlignment()
                helpText: qsTr("Stop aligning. Nothing is changed.")
            }
            ToolButton {
                objectName: "speechAlignApply"; text: qsTr("Match timing"); showLabel: true
                enabled: root.ready; onClicked: root.align()
                helpText: qsTr("Align the take to the reference and place it on a new track. One Undo step removes it.")
                unavailableReason: qsTr("Choose two different tracks, each with a clip under the playhead.")
            }
        }
    }
}
