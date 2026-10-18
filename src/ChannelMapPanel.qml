pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "channelMapPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int target: -1
    property int inputs: 1
    property int outputs: 2
    property var matrix: []
    property string stamp: ""
    property string message: ""
    property bool submitted: false
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(780, parent ? parent.width - 32 : 780)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    function reset(kind, count) {
        outputs = count
        const rows = []
        for (let o = 0; o < outputs; ++o) {
            const cells = []
            for (let i = 0; i < inputs; ++i)
                cells.push(kind === "mono" ? 1 / inputs : kind === "swap" ? (i === 1 - o ? 1 : 0)
                    : kind === "stereo" && inputs === 1 ? 1 : i === o ? 1 : 0)
            rows.push(cells)
        }
        matrix = rows
    }
    function reload() {
        target = service ? service.focusedTrack : -1
        const take = service ? service.trackById(target) : null
        inputs = Math.max(1, take && take.channels ? take.channels : 1)
        stamp = service ? service.automationStamp : ""
        reset("identity", inputs)
    }
    function apply() {
        const rows = []
        for (let o = 0; o < outputs; ++o) {
            const cells = []
            for (let i = 0; i < inputs; ++i) {
                const field = outputRows.itemAt(o).cells.itemAt(i)
                if (!field.acceptableInput || field.text.trim() === "") { message = qsTr("Enter a gain from −4 to +4 in every cell."); return }
                cells.push(Numeric.parse(field.text, "C"))
            }
            rows.push(cells)
        }
        submitted = true; message = ""
        service.mapTrackChannels(rows, stamp)
        if (!service.sampleEditing) { submitted = false; message = service.lastError }
    }
    onOpened: { reload(); message = ""; submitted = false }
    onClosed: if (shell) shell.restoreFocus()
    Connections {
        target: root.service
        function onSampleEditChanged() {
            if (root.submitted && !root.service.sampleEditing) {
                root.submitted = false
                root.message = root.service.lastError || qsTr("Channel mapping finished.")
                root.reload()
            }
        }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Map track channels"); Layout.fillWidth: true; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            ToolButton {
                iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close this panel. A conversion in progress carries on.")
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: qsTr("Each output is the sum of its row. 1 keeps an input, 0 omits it, and −1 inverts it. Clip timing, gain and fades remain in place. Missing channels in mixed-layout sources are silent.")
        }
        RowLayout {
            ComboField {
                objectName: "channelMapOutputs"; caption: qsTr("Outputs"); Layout.preferredWidth: 132
                helpTitle: qsTr("Output channels")
                helpText: qsTr("How many channels the track has after mapping.")
                helpScope: qsTr("Range: 1 to 8. Changing it resets the matrix to identity.")
                model: [1, 2, 3, 4, 5, 6, 7, 8]; currentIndex: root.outputs - 1
                onActivated: function(i) { root.reset("identity", i + 1) }
            }
            ToolButton {
                objectName: "channelMapIdentity"; text: qsTr("Identity"); showLabel: true; onClicked: root.reset("identity", root.inputs)
                helpText: qsTr("Fill the matrix so each output takes its matching input unchanged.")
                helpScope: qsTr("Edits the matrix only. Apply mapping converts the track.")
            }
            ToolButton {
                objectName: "channelMapSwap"; text: qsTr("Swap L / R"); showLabel: true; enabled: root.inputs === 2; onClicked: root.reset("swap", 2)
                helpText: qsTr("Fill the matrix so left and right exchange places.")
                helpScope: qsTr("Edits the matrix only. Apply mapping converts the track.")
                unavailableReason: qsTr("Swapping needs a stereo track.")
            }
            ToolButton {
                objectName: "channelMapMono"; text: qsTr("Average to mono"); showLabel: true; onClicked: root.reset("mono", 1)
                helpText: qsTr("Fill the matrix so every input is summed into one channel at equal gain.")
                helpScope: qsTr("Edits the matrix only. Apply mapping converts the track.")
            }
            ToolButton {
                objectName: "channelMapStereo"; text: qsTr("Stereo"); showLabel: true; onClicked: root.reset("stereo", 2)
                helpText: qsTr("Fill the matrix for two outputs: a mono input goes to both sides; otherwise each output takes its matching input.")
                helpScope: qsTr("Edits the matrix only. Apply mapping converts the track.")
            }
        }
        Row {
            spacing: Theme.s2
            Item { width: 56; height: 20 }
            Repeater {
                model: root.inputs
                Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; required property int index; width: 64; text: qsTr("In %1").arg(index + 1); horizontalAlignment: Text.AlignHCenter; color: Theme.textSecondary }
            }
        }
        Repeater {
            id: outputRows
            model: root.outputs
            Row {
                id: outputRow
                required property int index
                property alias cells: inputCells
                spacing: Theme.s2
                Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; width: 56; height: Theme.hControl; verticalAlignment: Text.AlignVCenter; text: qsTr("Out %1").arg(outputRow.index + 1); color: Theme.textPrimary }
                Repeater {
                    id: inputCells
                    model: root.inputs
                    TextEntry {
                        required property int index
                        objectName: "channelMapCell" + outputRow.index + "_" + index
                        width: 64; height: Theme.hControl
                        text: root.matrix[outputRow.index] && root.matrix[outputRow.index][index] !== undefined ? String(Number(root.matrix[outputRow.index][index].toFixed(6))) : "0"
                        color: Theme.textPrimary; horizontalAlignment: Text.AlignHCenter; selectByMouse: true
                        validator: DoubleValidator { bottom: -4; top: 4; decimals: 6; notation: DoubleValidator.StandardNotation; locale: "C" }
                        helpTitle: qsTr("Input %1 into output %2").arg(index + 1).arg(outputRow.index + 1)
                        helpText: qsTr("How much of this input channel goes to this output channel: 1 is all of it, 0 none, 0.5 half, a negative number inverts it.")
                        helpScope: qsTr("Between −4 and 4. Applied when you press Apply, as one Undo step.")
                        background: Rectangle { color: Theme.controlBg; border.color: Theme.border; radius: Theme.rControl }
                    }
                }
            }
        }
        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; objectName: "channelMapMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: root.message; color: Theme.textPrimary; visible: text !== "" }
        RowLayout {
            ToolButton {
                objectName: "channelMapReload"; text: qsTr("Reload"); showLabel: true; enabled: !root.service.sampleEditing; onClicked: root.reload()
                helpText: qsTr("Reload the active track and set each input channel to its matching output.")
                unavailableReason: qsTr("Wait for the current conversion to finish.")
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Cancel processing"); showLabel: true; visible: root.service && root.service.sampleEditing; onClicked: root.service.cancelSampleEdit()
                helpText: qsTr("Stop the conversion. The track keeps its previous channels.")
            }
            ToolButton {
                objectName: "channelMapApply"; text: qsTr("Apply mapping"); iconName: "check"; showLabel: true; enabled: root.target >= 0 && root.service && !root.service.sampleEditing; onClicked: root.apply()
                helpText: qsTr("Apply the channel mix shown in the grid to the active track.")
                helpScope: qsTr("Runs in the background. Clip timing, gain and fades stay in place; Undo restores the previous audio.")
                unavailableReason: root.target < 0 ? qsTr("Click a track to make it active first.")
                                                   : qsTr("Wait for the current conversion to finish.")
            }
        }
    }
}
