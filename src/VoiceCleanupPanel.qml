pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

C.Popup {
    id: root
    objectName: "voiceCleanupPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var target: ({})
    property real amount: 75
    property string message: ""
    property bool submitted: false
    property bool cancelled: false
    property int revision: 0
    readonly property bool stale: {
        return revision >= 0 && service !== null && target.stamp !== undefined
            && target.stamp !== service.voiceCleanupTarget().stamp
    }
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(540, parent ? parent.width - 32 : 540)
    padding: Theme.s4
    modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    function reload() {
        target = service ? service.voiceCleanupTarget() : {}
        message = ""; cancelled = false
    }
    onAboutToShow: { submitted = service && service.sampleEditing && service.sampleEditLabel === "voice cleanup"; reload() }
    onClosed: if (shell) shell.restoreFocus()
    Connections {
        target: root.service
        function onTimelineChanged() { ++root.revision }
        function onTracksChanged() { ++root.revision }
        function onSelectionChanged() { ++root.revision }
        function onSampleEditChanged() {
            ++root.revision
            if (root.submitted && !root.service.sampleEditing) {
                root.submitted = false
                root.message = root.service.lastError || (root.cancelled ? qsTr("Cleanup cancelled.")
                    : qsTr("Cleanup finished. Undo restores the previous audio."))
            }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Voice Cleanup"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                iconName: "x"; text: qsTr("Close")
                helpText: qsTr("Close this panel.")
                helpScope: qsTr("Cleanup that is already running carries on; its result still lands with one Undo step.")
                onClicked: root.close()
            }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            text: qsTr("Reduces noise in the selected speech using local RNNoise processing. Quiet speech and music can also change. Apply once, listen, and compare with Undo.")
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "voiceCleanupSelection"; Layout.fillWidth: true
            wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: Theme.textPrimary
            text: root.target.valid ? qsTr("%1 · %2 seconds · %3").arg(Theme.shortName(root.target.trackName, 48))
                .arg(Number(root.target.seconds).toFixed(2)).arg(root.target.channels === 1 ? qsTr("Mono") : qsTr("Stereo"))
                : root.target.reason || qsTr("Select speech to clean.")
        }
        SliderField {
            objectName: "voiceCleanupAmount"; Layout.fillWidth: true
            label: qsTr("Cleanup amount"); labelWidth: 124; from: 0; to: 100; decimals: 0; stepSize: 5; suffix: " %"
            helpText: qsTr("How much of the cleaned audio to use, from 0 to 100%. 100% is the fully processed speech; lower values blend the original back in, keeping more of its character along with some noise.")
            helpScope: qsTr("Applies to the next Apply cleanup. 0% does nothing, so Apply stays unavailable.")
            unavailableReason: qsTr("Wait for the current cleanup to finish.")
            value: root.amount; enabled: !root.submitted
            onEdited: function(v) { root.amount = v }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: qsTr("Amount blends the cleaned audio with the original. Processing keeps the timing and leaves audio outside the selection unchanged.")
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "voiceCleanupStale"; Layout.fillWidth: true; wrapMode: Text.WordWrap
            visible: root.stale && !root.submitted; color: Theme.warning
            text: qsTr("The selection or track changed. Reload before applying cleanup.")
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "voiceCleanupMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap
            textFormat: Text.PlainText; color: Theme.textPrimary
            visible: text !== ""; text: root.submitted ? qsTr("Cleaning speech…") : root.message
        }
        RowLayout {
            ToolButton {
                objectName: "voiceCleanupReload"; text: qsTr("Reload"); showLabel: true; enabled: !root.submitted
                helpText: qsTr("Read the current selection and track again, after they changed while this panel was open.")
                helpScope: qsTr("No audio changes.")
                unavailableReason: qsTr("Wait for the current cleanup to finish.")
                onClicked: root.reload()
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "voiceCleanupCancel"; text: qsTr("Cancel processing"); showLabel: true; visible: root.submitted
                helpText: qsTr("Stop the cleanup that is running.")
                helpScope: qsTr("A cancelled cleanup writes nothing; the audio stays as it was.")
                onClicked: { root.cancelled = true; root.service.cancelSampleEdit() }
            }
            ToolButton {
                objectName: "voiceCleanupApply"; text: qsTr("Apply cleanup"); showLabel: true
                helpText: qsTr("Run RNNoise over the selected speech in the background and blend the result in at the Cleanup amount.")
                helpScope: qsTr("Acts on the selection of the track shown above. The result is laid over the selection as a new source; the original audio stays in the project and Undo restores it. Audio outside the selection is untouched.")
                helpTopic: qsTr("RNNoise is a local speech noise suppressor; it needs no network. It is trained on speech, so quiet speech, breaths and any music in the selection can also change. Apply once, listen, and compare with Undo at a few amounts.")
                unavailableReason: root.target.valid !== true ? (root.target.reason || qsTr("Select speech to clean."))
                                   : root.stale ? qsTr("The selection or track changed. Press Reload first.")
                                   : root.submitted ? qsTr("Wait for the current cleanup to finish.")
                                   : root.amount <= 0 ? qsTr("Set a cleanup amount above 0%.")
                                   : root.service && root.service.recording ? qsTr("Finish recording first.")
                                   : root.service && root.service.loading ? qsTr("Wait for the file to finish opening.")
                                   : root.service && root.service.sampleEditing ? qsTr("Finish the current audio edit first.") : ""
                enabled: root.target.valid === true && !root.stale && !root.submitted && root.amount > 0
                    && root.service && !root.service.sampleEditing && !root.service.recording && !root.service.loading
                onClicked: {
                    root.submitted = true; root.cancelled = false; root.message = ""
                    if (!root.service.applyVoiceCleanup(root.amount, root.target.stamp)) {
                        root.submitted = false; root.message = root.service.lastError
                    }
                }
            }
        }
    }
}
