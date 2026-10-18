pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "audioEditPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property string operation: "silence"
    property string message: ""
    property real incomingDb: 0
    property real destinationDb: 0
    property bool submitted: false
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(550, parent ? parent.width - 32 : 550)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onOpened: {
        atInput.show(service ? service.playhead : 0)
        durationInput.show(service ? service.sampleRate : 48000)     // one second
        message = ""; submitted = false
    }
    onClosed: if (shell) shell.restoreFocus()
    Connections {
        target: root.service
        function onSampleEditChanged() {
            if (root.submitted && !root.service.sampleEditing) {
                root.submitted = false
                root.message = root.service.lastError || qsTr("Edit finished. Undo restores the previous audio.")
            }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: root.operation === "silence" ? qsTr("Insert silence") : qsTr("Paste and Mix"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                iconName: "x"; text: qsTr("Close")
                helpText: qsTr("Close this panel.")
                helpScope: qsTr("An edit that is already running carries on; its result still lands with one Undo step.")
                onClicked: root.close()
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: root.operation === "silence" ? qsTr("Opens a gap at the position below and moves later clips along. The duration is retained even at the end of a take. Automation and markers keep their timeline positions.")
                : qsTr("Adds the copied audio to the destination. The destination's channel selection is respected. Mono copies feed each selected channel; a multichannel copy is averaged for a mono destination.")
        }
        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; visible: root.operation === "mix"; text: root.service ? root.service.clipboardSummary : ""; textFormat: Text.PlainText; color: Theme.textPrimary }
        RowLayout {
            Text { text: qsTr("At"); color: Theme.textSecondary; Layout.preferredWidth: 136; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
            TimeField {
                id: atInput; objectName: "audioEditAt"; Layout.fillWidth: true; selectByMouse: true; color: Theme.textPrimary
                service: root.service
                helpTitle: qsTr("Position")
                helpText: qsTr("Where on the timeline the edit happens. It starts at the playhead.")
            }
        }
        RowLayout {
            visible: root.operation === "silence"
            Text { text: qsTr("Duration"); color: Theme.textSecondary; Layout.preferredWidth: 136; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
            TimeField {
                duration: true
                id: durationInput; objectName: "audioEditDuration"; Layout.fillWidth: true; selectByMouse: true; color: Theme.textPrimary
                service: root.service; minimum: 1
                helpTitle: qsTr("Duration")
                helpText: qsTr("How much silence to insert.")
            }
        }
        ComboField {
            id: scope; objectName: "audioEditScope"; visible: root.operation === "silence"; Layout.fillWidth: true
            caption: qsTr("Tracks")
            helpTitle: qsTr("Tracks to open")
            helpText: qsTr("Insert the silence on the active track only, or on every track at once so the whole project stays in step.")
            helpScope: qsTr("Later clips on the chosen tracks move later by the duration. Automation and markers keep their timeline positions.")
            model: [qsTr("Active track"), qsTr("All tracks")]
        }
        SliderField {
            objectName: "mixPasteIncoming"; visible: root.operation === "mix"; Layout.fillWidth: true
            label: qsTr("Clipboard"); from: -90; to: 24; decimals: 1; stepSize: .5; suffix: " dB"; value: root.incomingDb
            helpTitle: qsTr("Clipboard level")
            helpText: qsTr("How loud the copied audio is mixed in, in decibels (dB), from −90 to +24. 0 keeps its level; negative values make it quieter.")
            helpScope: qsTr("Applies to the next Paste and Mix only; the clipboard itself is unchanged.")
            onEdited: function(v) { root.incomingDb = v }
        }
        SliderField {
            objectName: "mixPasteDestination"; visible: root.operation === "mix"; Layout.fillWidth: true
            label: qsTr("Destination"); from: -90; to: 24; decimals: 1; stepSize: .5; suffix: " dB"; value: root.destinationDb
            helpTitle: qsTr("Destination level")
            helpText: qsTr("How loud the audio already at the paste point is kept under the mix, in decibels (dB), from −90 to +24. 0 keeps its level; −90 all but replaces it.")
            helpScope: qsTr("Applies within the pasted range only. The original audio stays in the project and Undo restores it.")
            onEdited: function(v) { root.destinationDb = v }
        }
        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; objectName: "audioEditMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: root.message; color: Theme.textPrimary; visible: text !== "" }
        RowLayout {
            Item { Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Cancel processing"); showLabel: true; visible: root.service && root.service.sampleEditing
                helpText: qsTr("Stop the audio edit that is running.")
                helpScope: qsTr("A cancelled edit writes nothing; the audio stays as it was.")
                onClicked: root.service.cancelSampleEdit()
            }
            ToolButton {
                objectName: "audioEditApply"; text: root.operation === "silence" ? qsTr("Insert silence") : qsTr("Paste and Mix"); showLabel: true
                helpText: root.operation === "silence" ? qsTr("Open a gap of the given duration at the position shown, moving later clips along to make room.")
                                                       : qsTr("Add the copied audio on top of what is already at the position shown, at the two levels set above.")
                helpScope: root.operation === "silence" ? qsTr("Acts on the active track, or every track, from the At position. One Undo step; the source audio never changes.")
                                                        : qsTr("Acts on the active track from the At position, respecting its channel selection. Mono copies feed each selected channel; a multichannel copy is averaged for a mono destination. One Undo step; the original audio stays in the project.")
                unavailableReason: root.service && root.service.sampleEditing ? qsTr("Wait for the current audio edit to finish.")
                                   : !atInput.valid ? qsTr("Enter a position on the timeline, such as 0:01.5.")
                                   : root.operation === "silence" && !durationInput.valid ? qsTr("Enter how long the silence should be, such as 0.5 or 250ms.") : ""
                enabled: root.service && !root.service.sampleEditing && atInput.valid && (root.operation === "mix" || durationInput.valid)
                onClicked: {
                    root.submitted = true; root.message = ""
                    if (root.operation === "silence") {
                        root.service.insertSilence(durationInput.value, scope.currentIndex === 1, atInput.value)
                    }
                    else root.service.mixPaste(atInput.value, root.incomingDb, root.destinationDb)
                    if (!root.service.sampleEditing) { root.submitted = false; root.message = root.service.lastError }
                }
            }
        }
    }
}
