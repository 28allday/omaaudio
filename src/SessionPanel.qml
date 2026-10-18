pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "sessionPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property string stamp: ""
    property string message: ""
    property bool submitted: false
    property bool cancelled: false
    property string musicStamp: ""
    property string musicMessage: ""
    function reloadMusic() {
        musicStamp = service ? service.automationStamp : ""
        const music = service ? service.musicalTime : {enabled: false, tempo: 120, beats: 4, unit: 4}
        musicEnabled.checked = music.enabled
        tempo.text = String(music.tempo)
        beatsPerBar.text = String(music.beats)
        beatUnit.currentIndex = [1, 2, 4, 8, 16, 32].indexOf(music.unit)
    }
    function reload() {
        stamp = service ? service.automationStamp : ""
        rate.text = String(service ? service.sampleRate : 48000)
        bits.currentIndex = service && service.defaultExportBits === 16 ? 0 : service && service.defaultExportBits === 32 ? 2 : 1
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(540, parent ? parent.width - 32 : 540)
    padding: Theme.s4; modal: true; focus: true
    // The scrolling column reaches into this margin (ScrollPane).
    leftPadding: Theme.s4 - Theme.s2; rightPadding: Theme.s4 - Theme.s3
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onOpened: { reload(); reloadMusic(); message = ""; musicMessage = ""; submitted = false; cancelled = false }
    onClosed: if (shell) shell.restoreFocus()
    Connections {
        target: root.service
        function onSampleEditChanged() {
            if (root.submitted && !root.service.sampleEditing) {
                root.submitted = false
                root.message = root.service.lastError || (root.cancelled ? qsTr("Conversion cancelled. The project is unchanged.") : root.stamp === root.service.automationStamp ? qsTr("Settings already match.") : qsTr("Sample settings applied. Undo restores the previous settings and audio."))
                root.reload()
            }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    height: Math.min(implicitHeight, parent ? parent.height - 32 : implicitHeight)
    contentItem: ScrollPane {
        id: sessionScroll
        clip: true
        contentWidth: availableWidth
        implicitHeight: contents.implicitHeight
        ColumnLayout {
            id: contents
            x: sessionScroll.reachLeft; width: sessionScroll.columnWidth
            spacing: Theme.s3
            RowLayout {
                Text { text: qsTr("Project properties"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
                ToolButton {
                    iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                    helpText: qsTr("Close this panel. A conversion in progress carries on.")
                }
            }
            Text {
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; color: Theme.textPrimary
                text: root.service ? (root.service.projectName || root.service.takeName || qsTr("Untitled project")) + " · " + root.service.trackCount + (root.service.trackCount === 1 ? qsTr(" track · ") : qsTr(" tracks · ")) + root.service.durationSeconds.toFixed(3) + qsTr(" seconds") : ""
            }
            Text { text: qsTr("Musical timing"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            Toggle {
                id: musicEnabled; objectName: "sessionMusicEnabled"; Layout.fillWidth: true
                label: qsTr("Enable project tempo and meter")
                helpText: qsTr("Use the tempo and time signature below for Bars and Beats and the musical snap grid.")
                helpScope: qsTr("Applies with Apply musical timing as one Undo step. Audio and clip positions stay unchanged.")
            }
            RowLayout {
                Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Tempo (BPM)"); color: Theme.textSecondary }
                TextEntry {
                    id: tempo; objectName: "sessionTempo"; Layout.fillWidth: true; selectByMouse: true
                    validator: DoubleValidator { bottom: 1; top: 480; decimals: 3; notation: DoubleValidator.StandardNotation; locale: "C" }
                    helpTitle: qsTr("Project tempo")
                    helpText: qsTr("Quarter notes per minute, from 1 to 480, with up to three decimal places.")
                    helpScope: qsTr("Used for musical display and snapping. Does not stretch audio or change the recording count-in.")
                }
                Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Meter"); color: Theme.textSecondary }
                TextEntry {
                    id: beatsPerBar; objectName: "sessionBeats"; Layout.preferredWidth: 48; selectByMouse: true
                    validator: IntValidator { bottom: 1; top: 32; locale: "C" }
                    helpTitle: qsTr("Beats per bar"); helpText: qsTr("The time signature's numerator: 1–32 beats in each bar.")
                }
                Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: "/"; color: Theme.textSecondary }
                ComboField {
                    id: beatUnit; objectName: "sessionBeatUnit"; Layout.preferredWidth: 76
                    model: ["1", "2", "4", "8", "16", "32"]
                    helpTitle: qsTr("Beat note"); helpText: qsTr("The time signature's denominator: 4 is a quarter note, 8 an eighth note.")
                }
            }
            Text {
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
                text: qsTr("Bars and Beats starts positions at 1:1:000 and durations at 0:0:000. A quarter note has 960 ticks. Choose the display under View › Time Display and an optional grid under Edit › Snap Grid. The magnet button switches snapping on or off.")
            }
            Text {
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                objectName: "sessionMusicMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap
                textFormat: Text.PlainText; text: root.musicMessage; color: Theme.textPrimary; visible: text !== ""
            }
            RowLayout {
                ToolButton {
                    text: qsTr("Reload timing"); showLabel: true; onClicked: { root.reloadMusic(); root.musicMessage = "" }
                    helpText: qsTr("Read the current musical timing again, discarding the edits in these fields.")
                }
                Item { Layout.fillWidth: true }
                ToolButton {
                    objectName: "sessionMusicApply"; text: qsTr("Apply musical timing"); showLabel: true
                    helpText: qsTr("Save this tempo and time signature in the project.")
                    helpScope: qsTr("One Undo step. Updates musical display and snap spacing without changing existing audio, clip positions or recording count-in.")
                    enabled: root.service && !root.service.recording && !root.service.loading && !root.service.saving && !root.service.sampleEditing && tempo.acceptableInput && beatsPerBar.acceptableInput
                    unavailableReason: qsTr("Finish loading, saving, recording or converting audio, and enter a valid tempo and meter.")
                    onClicked: {
                        if (root.service.setMusicalTime(musicEnabled.checked, Numeric.parse(tempo.text, "C"), Numeric.parse(beatsPerBar.text, "C"), [1, 2, 4, 8, 16, 32][beatUnit.currentIndex], root.musicStamp)) {
                            root.reloadMusic()
                            root.stamp = root.service.automationStamp
                            root.musicMessage = qsTr("Musical timing applied. Audio and clip positions are unchanged.")
                        } else root.musicMessage = root.service.lastError
                    }
                }
            }
            Text { text: qsTr("Sample settings"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            Text {
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
                text: qsTr("Changing the project rate resamples its audio and preserves timing in seconds. Playback stops while the new rate is applied. Original audio is retained for undo.")
            }
            RowLayout {
                Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Project rate (Hz)"); color: Theme.textSecondary; Layout.preferredWidth: 140 }
                TextEntry {
                    id: rate; objectName: "sessionSampleRate"; Layout.fillWidth: true
                    color: Theme.textPrimary; selectByMouse: true
                    helpTitle: qsTr("Project sample rate")
                    helpText: qsTr("How many samples a second the project uses, in hertz: 48000 is the common choice for video, 44100 for music. Every take is converted to it.")
                    helpScope: qsTr("Between 8000 and 192000. Changing it converts the whole project as one Undo step.")
                    validator: IntValidator { bottom: 8000; top: 192000; locale: "C" }
                }
            }
            ComboField {
                id: bits; objectName: "sessionSampleType"; Layout.fillWidth: true
                caption: qsTr("Default delivery type"); model: [qsTr("16-bit PCM"), qsTr("24-bit PCM"), qsTr("32-bit float")]
                helpText: qsTr("The bit depth exports start from: 16-bit or 24-bit integer, or 32-bit float.")
                helpScope: qsTr("Editing stays 32-bit float whatever you choose; this only sets the export default.")
            }
            Text {
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
                text: qsTr("This chooses the default data type for exported files. The project keeps its full editing precision.")
            }
            Text {
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                objectName: "sessionSampleMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap
                textFormat: Text.PlainText; text: root.service && root.service.sampleEditing ? qsTr("Converting project audio…") : root.message
                color: Theme.textPrimary; visible: text !== ""
            }
            RowLayout {
                ToolButton {
                    text: qsTr("Reload values"); showLabel: true; enabled: root.service && !root.service.sampleEditing; onClicked: root.reload()
                    helpText: qsTr("Read the project's current rate and delivery type again, discarding edits here.")
                    unavailableReason: qsTr("Wait for the current conversion to finish.")
                }
                Item { Layout.fillWidth: true }
                ToolButton {
                    text: qsTr("Cancel processing"); showLabel: true; visible: root.service && root.service.sampleEditing; onClicked: { root.cancelled = true; root.service.cancelSampleEdit() }
                    helpText: qsTr("Stop the rate conversion. The project keeps its previous rate and audio.")
                }
                ToolButton {
                    objectName: "sessionSampleApply"; text: qsTr("Apply"); showLabel: true
                    helpText: qsTr("Change the project sample rate and the default delivery type.")
                    helpScope: qsTr("Resamples every track in the background and stops playback. Undo restores the previous rate and audio.")
                    unavailableReason: !root.service ? qsTr("The audio engine is not available.")
                        : root.service.recording ? qsTr("Stop recording first.")
                        : root.service.sampleEditing ? qsTr("Wait for the current conversion to finish.")
                        : qsTr("Enter a project rate from 8,000 to 192,000 Hz.")
                    enabled: root.service && !root.service.recording && !root.service.sampleEditing && rate.acceptableInput
                    onClicked: {
                        root.submitted = true; root.cancelled = false; root.message = ""
                        root.service.changeProjectSamples(Numeric.parse(rate.text, "C"), [16, 24, 32][bits.currentIndex], root.stamp)
                        if (!root.service.sampleEditing) { root.submitted = false; root.message = root.service.lastError || qsTr("Settings already match.") }
                    }
                }
            }
        }
    }
}
