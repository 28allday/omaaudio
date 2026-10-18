pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// Tones, sweeps and noise onto the focused track: replacing the selection on that
// track, or from the playhead for a typed duration. One Undo step.
C.Popup {
    id: root
    objectName: "generatePanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int kind: 0           // 0 tone, 1 noise
    property string message: ""
    readonly property bool intoSelection: !!service && !service.selectionEmpty
    readonly property bool valid: level.acceptableInput && (intoSelection || seconds.acceptableInput)
        && (kind === 1 || (frequency.acceptableInput && (!sweep.checked || endFrequency.acceptableInput)))

    function generate() {
        if (!service || !valid) return
        const spec = kind === 0
            ? {kind: "tone", shape: ["sine", "square", "triangle", "sawtooth"][shape.currentIndex],
               frequency: Number(frequency.text), endFrequency: sweep.checked ? Number(endFrequency.text) : 0}
            : {kind: "noise", colour: ["white", "pink", "brown"][colour.currentIndex], seed: Math.floor(Math.random() * 1e9) + 1}
        spec.levelDb = Number(level.text)
        if (!intoSelection) spec.seconds = Number(seconds.text)
        if (service.generateAudio(spec)) close()
        else message = service.lastError
    }

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 32 : 520)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: message = ""
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    component Labelled: RowLayout {
        property string label: ""
        property string unit: ""
        default property alias content: holder.data
        Text { text: parent.label; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; Layout.preferredWidth: 120 }
        Item { id: holder; Layout.fillWidth: true; implicitHeight: children.length ? children[0].implicitHeight : 0 }
        Text { text: parent.unit; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; Layout.preferredWidth: 40 }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Generate"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            SegmentedControl {
                objectName: "generateKind"; labels: [qsTr("Tone"), qsTr("Noise")]; names: labels
                currentIndex: root.kind; onActivated: index => root.kind = index
                helpTitle: qsTr("Generate"); helpText: qsTr("Choose a tone or sweep, or noise.")
            }
            ToolButton { text: qsTr("Close"); iconName: "x"; onClicked: root.close(); helpText: qsTr("Close without generating.") }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.intoSelection ? qsTr("Replaces the selected time on the focused track.")
                                     : qsTr("Placed at the playhead on the focused track, replacing what is there for its length.")
        }
        Labelled {
            visible: root.kind === 0; label: qsTr("Shape")
            ComboField {
                id: shape; objectName: "generateShape"; width: parent.width
                model: [qsTr("Sine"), qsTr("Square"), qsTr("Triangle"), qsTr("Sawtooth")]
                helpText: qsTr("Square, triangle and sawtooth are band-limited, so they do not alias.")
            }
        }
        Labelled {
            visible: root.kind === 0; label: sweep.checked ? qsTr("Start frequency") : qsTr("Frequency"); unit: "Hz"
            TextEntry {
                id: frequency; objectName: "generateFrequency"; width: parent.width; text: "1000"; selectByMouse: true
                validator: DoubleValidator { bottom: 1; top: 96000; decimals: 3; notation: DoubleValidator.StandardNotation; locale: "C" }
                helpTitle: qsTr("Frequency"); helpText: qsTr("The tone's pitch in hertz, below half the project sample rate.")
            }
        }
        Toggle {
            id: sweep; objectName: "generateSweep"; visible: root.kind === 0; label: qsTr("Sweep")
            helpText: qsTr("Glide from the start frequency to the end frequency, evenly by octave.")
        }
        Labelled {
            visible: root.kind === 0 && sweep.checked; label: qsTr("End frequency"); unit: "Hz"
            TextEntry {
                id: endFrequency; objectName: "generateEndFrequency"; width: parent.width; text: "20000"; selectByMouse: true
                validator: DoubleValidator { bottom: 1; top: 96000; decimals: 3; notation: DoubleValidator.StandardNotation; locale: "C" }
                helpTitle: qsTr("End frequency"); helpText: qsTr("Where the sweep finishes, below half the project sample rate.")
            }
        }
        Labelled {
            visible: root.kind === 1; label: qsTr("Colour")
            ComboField {
                id: colour; objectName: "generateColour"; width: parent.width
                model: [qsTr("White"), qsTr("Pink"), qsTr("Brown")]
                helpText: qsTr("White is equal energy per hertz; pink is equal per octave; brown falls further towards the top.")
            }
        }
        Labelled {
            label: root.kind === 0 ? qsTr("Peak level") : qsTr("RMS level"); unit: "dBFS"
            TextEntry {
                id: level; objectName: "generateLevel"; width: parent.width; text: "-18"; selectByMouse: true
                validator: DoubleValidator { bottom: -120; top: 0; decimals: 2; notation: DoubleValidator.StandardNotation; locale: "C" }
                helpTitle: qsTr("Level"); helpText: qsTr("How loud the generated audio is. Tones are set by their peak, noise by its average (RMS).")
                helpScope: qsTr("Range: −120 to 0 dBFS.")
            }
        }
        Labelled {
            visible: !root.intoSelection; label: qsTr("Duration"); unit: qsTr("s")
            TextEntry {
                id: seconds; objectName: "generateSeconds"; width: parent.width; text: "5"; selectByMouse: true
                validator: DoubleValidator { bottom: 0.001; top: 21600; decimals: 3; notation: DoubleValidator.StandardNotation; locale: "C" }
                helpTitle: qsTr("Duration"); helpText: qsTr("How many seconds to generate from the playhead.")
            }
        }
        Text {
            objectName: "generateMessage"; visible: root.message !== ""
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.warning; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.message
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "generateApply"; text: qsTr("Generate"); showLabel: true
                enabled: root.valid; onClicked: root.generate()
                helpText: qsTr("Create the audio as a new clip. One Undo step removes it and restores what it replaced.")
                unavailableReason: qsTr("Check the values: frequencies from 1 Hz, levels from −120 to 0 dBFS.")
            }
        }
    }
}
