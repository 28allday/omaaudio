pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "preferencesPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int page: 0
    property string message: ""
    property string nextAction: ""
    readonly property var fftSizes: service ? service.spectralFftSizes : []
    readonly property var windows: service ? service.spectralWindows : []
    readonly property var overlaps: service ? service.spectralOverlaps : []
    readonly property var timeDisplays: service ? service.timeDisplays : []
    readonly property var snapGrids: service ? service.snapGrids : []
    readonly property string validationMessage: {
        if (snapGrid.currentIndex < 0 || snapGrid.currentIndex >= snapGrids.length) return qsTr("Editing & recovery: choose a snap grid.")
        if (!autosave.acceptableInput) return qsTr("Editing & recovery: autosave interval must be a whole number from 0 to 3600 seconds.")
        if (!touchReturn.acceptableInput) return qsTr("Editing & recovery: Touch return must be from 0 to 5 seconds.")
        if (!gain.acceptableInput) return qsTr("Spectral: display gain must be a whole number from −24 to +24 dB.")
        if (!target.acceptableInput) return qsTr("Delivery: loudness target must be from −40 to −5 LUFS.")
        if (!ceiling.acceptableInput) return qsTr("Delivery: true-peak ceiling must be from −12 to 0 dBTP.")
        if (!memoryAuto.checked && !memory.acceptableInput)
            return qsTr("Performance: memory budget must be a whole number from 1024 to %1 MiB.").arg(maximumMemory)
        if (!spectralStore.acceptableInput) return qsTr("Performance: spectral cache limit must be a whole number from 1 to 1024 GiB.")
        if (scale.currentIndex < 0 || scale.currentIndex >= 2) return qsTr("Spectral: choose a frequency scale.")
        if (palette.currentIndex < 0 || palette.currentIndex >= Theme.spectralPalettes.length) return qsTr("Spectral: choose a palette.")
        if (range.currentIndex < 0 || range.currentIndex >= 3) return qsTr("Spectral: choose a dynamic range.")
        if (fft.currentIndex < 0 || fft.currentIndex >= fftSizes.length) return qsTr("Spectral: choose an FFT size.")
        if (window.currentIndex < 0 || window.currentIndex >= windows.length) return qsTr("Spectral: choose an analysis window.")
        if (overlap.currentIndex < 0 || overlap.currentIndex >= overlaps.length) return qsTr("Spectral: choose an overlap.")
        return ""
    }
    readonly property bool valid: validationMessage === ""
    readonly property int maximumMemory: service ? service.maximumMemoryMiB : 1024
    // A stored value as the field can hold it. Other controls legitimately keep
    // more decimals (a dragged -0.7000000000000001); written out in full, the
    // field's own validator refused it and Apply was disabled on every page.
    function fixed(value, decimals) { return String(Number(Number(value).toFixed(decimals))) }
    function gib(mib) { return (mib / 1024).toFixed(mib % 1024 === 0 ? 0 : 2) + " GiB" }

    function reload(defaults) {
        if (!shell) return
        snapping.checked = defaults ? true : shell.snapping
        snapGrid.currentIndex = Math.max(0, snapGrids.indexOf(defaults ? "none" : shell.snapGrid))
        ripple.checked = defaults ? false : shell.ripple
        autosave.text = String(defaults ? 30 : shell.autosaveSeconds)
        touchReturn.text = fixed(defaults ? 1 : shell.automationReturnSeconds, 2)
        timeDisplay.currentIndex = Math.max(0, timeDisplays.indexOf(defaults ? "time" : shell.timeDisplay))
        scale.currentIndex = ["log", "linear"].indexOf(defaults ? "log" : shell.spectralScale)
        palette.currentIndex = Theme.spectralPalettes.indexOf(defaults ? "navy" : shell.spectralPalette)
        range.currentIndex = [60, 90, 120].indexOf(defaults ? 60 : shell.spectralRange)
        gain.text = fixed(defaults ? 0 : shell.spectralGain, 0)
        fft.currentIndex = fftSizes.indexOf(defaults ? 2048 : shell.spectralFftSize)
        window.currentIndex = windows.indexOf(defaults ? "hann" : shell.spectralWindow)
        overlap.currentIndex = overlaps.indexOf(defaults ? 4 : shell.spectralOverlap)
        target.text = fixed(defaults ? -14 : shell.deliveryTargetLufs, 1)
        ceiling.text = fixed(defaults ? -1 : shell.deliveryCeilingDbTp, 1)
        dither.checked = defaults ? true : shell.deliveryDither
        memoryAuto.checked = defaults ? true : shell.memoryMiB === 0
        memory.text = String(defaults || shell.memoryMiB === 0 ? (service ? service.effectiveMemoryMiB : 1024) : shell.memoryMiB)
        spectralStore.text = String(defaults ? 4 : shell.spectralStoreGiB)
        vst3.checked = defaults ? true : shell.vst3Discovery
        message = defaults ? qsTr("Default values loaded. Apply to use them.") : ""
    }
    function apply() {
        if (!valid || !shell) return false
        shell.snapping = snapping.checked
        shell.snapGrid = snapGrids[snapGrid.currentIndex]
        shell.ripple = ripple.checked
        shell.autosaveSeconds = Numeric.parse(autosave.text, "C")
        shell.automationReturnSeconds = Numeric.parse(touchReturn.text, "C")
        if (timeDisplay.currentIndex >= 0) shell.timeDisplay = timeDisplays[timeDisplay.currentIndex]
        shell.spectralScale = ["log", "linear"][scale.currentIndex]
        shell.spectralPalette = Theme.spectralPalettes[palette.currentIndex]
        shell.spectralRange = [60, 90, 120][range.currentIndex]
        shell.spectralGain = Numeric.parse(gain.text, "C")
        shell.setSpectralAnalysis(fftSizes[fft.currentIndex], windows[window.currentIndex], overlaps[overlap.currentIndex])
        if (shell.deliveryTargetLufs !== Numeric.parse(target.text, "C") || shell.deliveryCeilingDbTp !== Numeric.parse(ceiling.text, "C")
            || shell.deliveryDither !== dither.checked) {
            shell.setDelivery("deliveryTargetLufs", Numeric.parse(target.text, "C"))
            shell.setDelivery("deliveryCeilingDbTp", Numeric.parse(ceiling.text, "C"))
            shell.setDelivery("deliveryDither", dither.checked)
        }
        shell.memoryMiB = memoryAuto.checked ? 0 : Numeric.parse(memory.text, "C")
        shell.spectralStoreGiB = Numeric.parse(spectralStore.text, "C")
        shell.vst3Discovery = vst3.checked
        shell.syncPreferences()
        message = qsTr("Preferences applied.")
        return true
    }
    function openRelated(action) { nextAction = action; close() }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(640, parent ? parent.width - 32 : 640)
    height: Math.min(580, parent ? parent.height - 40 : 580)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: { nextAction = ""; page = 0; reload(false); if (service) service.refreshSpectralStore() }
    onClosed: {
        if (!shell) return
        if (nextAction) { const action = nextAction; nextAction = ""; shell.runAction(action) }
        else shell.restoreFocus()
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    component Note: Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
        color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
    }
    component NumberRow: RowLayout {
        id: numberRow
        property string label: ""
        property real minimum: 0
        property real maximum: 100
        property int decimals: 0
        property string help: ""
        property alias text: entry.text
        readonly property bool acceptableInput: entry.acceptableInput
        Layout.fillWidth: true
        Text { text: numberRow.label; Layout.preferredWidth: 195; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
        TextEntry {
            id: entry; objectName: numberRow.objectName + "Input"; Layout.fillWidth: true; selectByMouse: true
            helpTitle: numberRow.label
            helpText: numberRow.help
            helpScope: qsTr("Between %1 and %2. Takes effect when you press Apply.").arg(numberRow.minimum).arg(numberRow.maximum)
            onTextEdited: root.message = ""
            validator: DoubleValidator { bottom: numberRow.minimum; top: numberRow.maximum; decimals: numberRow.decimals; notation: DoubleValidator.StandardNotation; locale: "C" }
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Preferences"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close Preferences without applying draft changes.")
            }
        }
        SegmentedControl {
            objectName: "preferencesPages"; labels: [qsTr("Editing & recovery"), qsTr("Spectral"), qsTr("Delivery"), qsTr("Performance"), qsTr("More")]
            helpTitle: qsTr("Preference pages")
            helpText: qsTr("Which group of settings is shown: editing and recovery, the spectral display, delivery defaults, memory use, or the rest.")
            currentIndex: root.page; onActivated: index => root.page = index
        }
        ScrollPane {
            id: scroll; Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                x: scroll.reachLeft; width: scroll.columnWidth; spacing: Theme.s3
                ColumnLayout {
                    visible: root.page === 0; Layout.fillWidth: true; spacing: Theme.s3
                    Note { text: qsTr("These settings belong to OmaAudio. Changes take effect when you press Apply.") }
                    Toggle {
                        id: snapping; objectName: "preferencesSnapping"; Layout.fillWidth: true; label: qsTr("Snap editing gestures")
                        helpText: qsTr("Selection edges land on clip edges, markers or the playhead when dragged nearby.")
                        helpScope: qsTr("The same setting as Snapping in the toolbar. Takes effect on Apply.")
                    }
                    ComboField {
                        id: snapGrid; objectName: "preferencesSnapGrid"; Layout.fillWidth: true
                        caption: qsTr("Musical snap grid")
                        model: root.snapGrids.map(key => root.service.snapGridLabel(key))
                        helpText: qsTr("Use bars, beats or beat divisions for selection edges and moved clips.")
                        helpScope: qsTr("Requires Snapping and musical timing in Project properties. Nearby clip edges, markers and the playhead take priority. No musical grid keeps the current audio-target and zero-crossing behaviour. Takes effect on Apply.")
                    }
                    Toggle {
                        id: ripple; objectName: "preferencesRipple"; Layout.fillWidth: true; label: qsTr("Close gaps when deleting time on a track")
                        helpText: qsTr("Ripple editing: delete and cut close the gap, and paste pushes later audio along.")
                        helpScope: qsTr("Later audio on the edited track changes position. The same setting as Ripple in the toolbar; takes effect on Apply.")
                    }
                    NumberRow { id: autosave; objectName: "preferencesAutosave"; label: qsTr("Autosave interval (seconds)"); minimum: 0; maximum: 3600 }
                    Note { text: qsTr("Use 0 to turn automatic recovery writes off. Manual autosave remains available.") }
                    NumberRow {
                        id: touchReturn; objectName: "preferencesTouchReturn"; label: qsTr("Touch return (seconds)")
                        minimum: 0; maximum: 5; decimals: 2
                        help: qsTr("After releasing a control in Touch mode, return to the existing curve over this time. Zero returns immediately; the default is 1 second. A return already in progress keeps its timing.")
                    }
                    ComboField {
                        id: timeDisplay; objectName: "preferencesTimeDisplay"; Layout.fillWidth: true; caption: qsTr("Time display")
                        model: root.timeDisplays.map(key => root.service.timeDisplayLabel(key))
                        helpText: qsTr("How every position and length is written and typed: clock time, samples, decimal seconds, CD frames at 75 per second, Bars and Beats using project tempo, or picture timecode.")
                        helpScope: qsTr("The same setting as View › Time Display. Display only; takes effect on Apply.")
                    }
                    Note { text: root.service ? (root.service.ready ? qsTr("PipeWire output ready") : qsTr("Audio output unavailable")) + " · " + root.service.sampleRate + " Hz" : "" }
                }
                ColumnLayout {
                    visible: root.page === 1; Layout.fillWidth: true; spacing: Theme.s3
                    ComboField {
                        id: scale; objectName: "preferencesScale"; Layout.fillWidth: true; caption: qsTr("Frequency scale"); model: [qsTr("Logarithmic"), qsTr("Linear")]
                        helpText: qsTr("How frequency is spread up the spectral display. Logarithmic gives each octave equal space; linear gives each hertz equal space.")
                        helpScope: qsTr("Display only; the audio is unchanged. Takes effect on Apply.")
                    }
                    ComboField {
                        id: palette; objectName: "preferencesPalette"; Layout.fillWidth: true; caption: qsTr("Palette"); model: Theme.spectralPalettes.map(value => Theme.spectralPaletteLabel(value))
                        helpText: qsTr("The colours that show level in the spectral display.")
                        helpScope: qsTr("Display only; the audio is unchanged.")
                    }
                    ComboField {
                        id: range; objectName: "preferencesRange"; Layout.fillWidth: true; caption: qsTr("Dynamic range"); model: ["60 dB", "90 dB", "120 dB"]
                        helpText: qsTr("How many decibels below the loudest level the spectral display shows before fading to the background.")
                        helpScope: qsTr("60 dB shows the strong parts clearly; 120 dB reveals quiet detail. Display only.")
                    }
                    NumberRow { id: gain; objectName: "preferencesSpectralGain"; label: qsTr("Display gain (dB)"); minimum: -24; maximum: 24; decimals: 0 }
                    ComboField {
                        id: fft; objectName: "preferencesFft"; Layout.fillWidth: true; caption: qsTr("FFT size"); model: root.fftSizes
                        helpText: qsTr("Samples in each analysis frame. A larger size separates close frequencies better but blurs fast changes in time.")
                        helpScope: qsTr("Changing it rebuilds the spectral display. The audio is unchanged.")
                        helpTopic: qsTr("The FFT (fast Fourier transform) turns each frame of audio into the levels drawn in the spectrogram. 2048 is a good start for speech; go larger to separate close tones and smaller to follow fast events.")
                    }
                    ComboField {
                        id: window; objectName: "preferencesWindow"; Layout.fillWidth: true; caption: qsTr("Window"); model: root.windows
                        helpText: qsTr("The shape applied to each frame before analysis. Hann suits most material.")
                        helpScope: qsTr("Rebuilds the spectral display. The audio is unchanged.")
                    }
                    ComboField {
                        id: overlap; objectName: "preferencesOverlap"; Layout.fillWidth: true; caption: qsTr("Overlap"); model: root.overlaps
                        helpText: qsTr("How many analysis frames overlap each step. More overlap draws time more smoothly at more processing cost.")
                        helpScope: qsTr("Rebuilds the spectral display. The audio is unchanged.")
                    }
                    Note { text: qsTr("Analysis changes rebuild the spectral display. Display gain and colour do not alter the audio.") }
                }
                ColumnLayout {
                    visible: root.page === 2; Layout.fillWidth: true; spacing: Theme.s3
                    Note { text: qsTr("These are delivery defaults. Changing them marks the delivery preset as Custom; it does not change the current project audio.") }
                    NumberRow { id: target; objectName: "preferencesLoudness"; label: qsTr("Loudness target (LUFS)"); minimum: -40; maximum: -5; decimals: 1 }
                    NumberRow { id: ceiling; objectName: "preferencesCeiling"; label: qsTr("True-peak ceiling (dBTP)"); minimum: -12; maximum: 0; decimals: 1 }
                    Toggle {
                        id: dither; objectName: "preferencesDither"; Layout.fillWidth: true; label: qsTr("Dither 16/24-bit exports")
                        helpText: qsTr("Add very quiet noise when exporting to 16- or 24-bit, which avoids distortion in quiet audio.")
                        helpScope: qsTr("A delivery default. Not used for 32-bit float or compressed exports.")
                    }
                    Note { text: qsTr("Loudness target and ceiling are used when delivery normalization is set to Loudness. Dither is used for integer PCM delivery.") }
                }
                ColumnLayout {
                    visible: root.page === 3; Layout.fillWidth: true; spacing: Theme.s3
                    Note { text: qsTr("How much memory OmaAudio keeps for audio it can read again from disk. A larger budget keeps more of a long session ready, so seeking, looping and busy sessions play without gaps.") }
                    Toggle {
                        id: memoryAuto; objectName: "preferencesMemoryAuto"; Layout.fillWidth: true; label: qsTr("Automatic memory budget")
                        helpText: qsTr("Use a quarter of this computer's memory, up to 8 GiB.")
                        helpScope: qsTr("Turn it off to set your own budget below. Takes effect on Apply.")
                        onCheckedChanged: root.message = ""
                    }
                    NumberRow {
                        id: memory; objectName: "preferencesMemory"; label: qsTr("Memory budget (MiB)"); enabled: !memoryAuto.checked
                        minimum: 1024; maximum: root.maximumMemory; decimals: 0
                        help: qsTr("Your own memory budget, when the automatic one is off. Half of it holds audio ready for playback.")
                    }
                    Note {
                        objectName: "preferencesMemorySummary"
                        text: root.service ? qsTr("This computer has %1. In use now: a budget of %2, of which up to %3 holds audio ready for playback.")
                            .arg(root.gib(root.service.systemMemoryMiB)).arg(root.gib(root.service.effectiveMemoryMiB)).arg(root.gib(root.service.playbackCacheMiB)) : ""
                    }
                    Note { text: qsTr("The budget is a target, not a limit on the whole application: the project's own audio, plug-ins and an export in progress need memory of their own. The playback share fills only as audio is played, and stereo audio uses a quarter of it. A changed budget applies at once; playback may pause for a moment while the new cache fills.") }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                    Note { text: qsTr("Spectral displays that took time to compute are kept on disk, so reopening a project or returning to an earlier edit shows them at once.") }
                    NumberRow {
                        id: spectralStore; objectName: "preferencesSpectralStore"; label: qsTr("Spectral cache limit (GiB)")
                        minimum: 1; maximum: 1024; decimals: 0
                        help: qsTr("The most disk space kept spectral displays may use. The displays used longest ago are removed first.")
                    }
                    Note {
                        objectName: "preferencesSpectralStoreUsage"
                        text: root.service ? [root.service.spectralStoreUsage, root.service.spectralStoreStatus].filter(part => part !== "").join(" · ") : ""
                    }
                    ToolButton {
                        objectName: "preferencesSpectralStoreClear"
                        text: qsTr("Clear spectral cache"); showLabel: true
                        enabled: root.service && !root.service.spectralStoreBusy
                        onClicked: root.service.clearSpectralStore()
                        helpText: qsTr("Delete every kept spectral display now.")
                        helpScope: qsTr("Only the cache is removed: projects, audio and edits are untouched, and displays are computed again when needed. Does not wait for Apply.")
                        unavailableReason: qsTr("The cache is being checked. Try again in a moment.")
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                    Note { text: qsTr("How many tracks this computer can play smoothly depends on its speed, the buffer size and the effects in use. OmaAudio plays up to %1 tracks.").arg(root.service ? root.service.maxTracks : 256) }
                    ComputerCheck { objectName: "preferencesComputerCheck"; service: root.service; Layout.fillWidth: true }
                }
                ColumnLayout {
                    visible: root.page === 4; Layout.fillWidth: true; spacing: Theme.s3
                    Note { text: qsTr("Related controls open their own panels. Apply any draft changes here before opening another panel.") }
                    ToolButton {
                        objectName: "preferencesAudioSetup"
                        text: qsTr("Audio Setup…"); showLabel: true; onClicked: root.openRelated("show_audio_setup")
                        helpText: qsTr("See the audio system: outputs, inputs, sound cards, rates and JACK clients, with what to check when there is no sound.")
                        helpScope: qsTr("Read-only apart from routing playback to a JACK client on request. Closes Preferences first.")
                    }
                    ToolButton {
                        text: qsTr("Recording inputs and timing…"); showLabel: true; onClicked: root.openRelated("show_recording")
                        helpText: qsTr("Open the Recording panel: inputs, channels, punch, count-in and alignment.")
                        helpScope: qsTr("Closes Preferences first, so apply any draft changes before opening it.")
                    }
                    ToolButton {
                        text: qsTr("Source storage and cleanup…"); showLabel: true; onClicked: root.openRelated("show_storage")
                        helpText: qsTr("Open Storage and Cleanup: what audio the project holds and what can be released.")
                        helpScope: qsTr("Closes Preferences first, so apply any draft changes before opening it.")
                    }
                    Toggle {
                        id: vst3; objectName: "preferencesVst3"; Layout.fillWidth: true; label: qsTr("Find VST3 effects")
                        enabled: !!root.service && root.service.vst3DiscoveryAvailable
                        helpText: qsTr("Include installed VST3 effects in Add Effect, beside LV2 and CLAP effects. Each one is checked in a separate helper first, so a faulty effect cannot stop OmaAudio.")
                        helpScope: qsTr("Takes effect the next time the effect list is read. Effects already in a project keep working.")
                        unavailableReason: qsTr("The VST3 scanner is not installed with this copy of OmaAudio.")
                    }
                    ToolButton {
                        text: qsTr("Plug-in Manager…"); showLabel: true; onClicked: root.openRelated("show_plugin_manager")
                        helpText: qsTr("Open the Plug-in Manager to enable, disable and star installed effects.")
                        helpScope: qsTr("Closes Preferences first, so apply any draft changes before opening it.")
                    }
                    Note { text: qsTr("Where OmaAudio keeps things. Each line follows the matching XDG folder; back up the first two, the rest can be recreated.") }
                    component LocationField: C.TextField {
                        property string role: ""
                        property string help: ""
                        readonly property alias controlHelp: locationHelp
                        Layout.fillWidth: true; readOnly: true; selectByMouse: true; color: Theme.textPrimary
                        // The theme's type and height, as every other field (TextEntry).
                        implicitHeight: Theme.hControl; verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                        padding: 0; leftPadding: Theme.s2; rightPadding: Theme.s2
                        selectionColor: Theme.accent; selectedTextColor: Theme.accentText
                        background: Rectangle { color: Theme.controlBg; radius: Theme.rControl; border.width: Theme.hairline; border.color: Theme.border }
                        ControlHelp { id: locationHelp; title: parent.role; purpose: parent.help
                                      scope: qsTr("Read-only here. Select the text to copy the path.") }
                    }
                    Note { text: qsTr("Settings (window layout, delivery, spectral defaults):") }
                    LocationField {
                        objectName: "locationSettings"; role: qsTr("Settings file")
                        text: root.service ? root.service.settingsFilePath() : ""
                        help: qsTr("Every preference and layout choice, written when it changes. Delete it to return to the defaults.")
                    }
                    Note { text: qsTr("Keyboard shortcuts are read from:") }
                    LocationField {
                        objectName: "locationKeymap"; role: qsTr("Keymap file")
                        text: root.service ? root.service.keymapFilePath() : ""
                        help: qsTr("Your own key bindings, in TOML, overriding the defaults compiled in. Changes are picked up while OmaAudio runs.")
                    }
                    Note { text: qsTr("Recovery copies and recorded takes (kept until you save or discard them):") }
                    LocationField {
                        objectName: "locationState"; role: qsTr("State folder")
                        text: root.service ? root.service.stateDirectoryPath() : ""
                        help: qsTr("Autosaved recovery projects and recoverable recordings. Worth keeping while a project is unsaved; safe to clear afterwards.")
                    }
                    Note { text: qsTr("Caches (disposable; rebuilt when missing):") }
                    LocationField {
                        objectName: "locationCache"; role: qsTr("Cache folder")
                        text: root.service ? root.service.cacheDirectoryPath() : ""
                        help: qsTr("Disk-backed audio scratch and other data OmaAudio can rebuild. Deleting it loses nothing.")
                    }
                    Note {
                        objectName: "storageMigrationNote"
                        text: root.service ? root.service.storageMigrationMessage() : ""
                        visible: text !== ""
                    }
                }
            }
        }
        Note { objectName: "preferencesMessage"; text: root.valid ? root.message : root.validationMessage; visible: text !== "" }
        RowLayout {
            ToolButton {
                objectName: "preferencesDefaults"; text: qsTr("Load defaults"); showLabel: true; onClicked: root.reload(true)
                helpText: qsTr("Fill every field with the standard values.")
                helpScope: qsTr("Nothing changes until you press Apply.")
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "preferencesCancel"; text: qsTr("Close"); showLabel: true; onClicked: root.close()
                helpText: qsTr("Close Preferences without applying draft changes.")
            }
            ToolButton {
                objectName: "preferencesApply"; text: qsTr("Apply"); showLabel: true; enabled: root.valid; onClicked: root.apply()
                helpText: qsTr("Save these preferences and use them now.")
                helpScope: qsTr("Changed delivery values mark the delivery preset as Custom. Project audio is unchanged.")
                unavailableReason: root.validationMessage
            }
        }
    }
}
