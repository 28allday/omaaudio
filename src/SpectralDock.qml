pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The Spectral page's right dock. Repair is what can be done to the selection
// and its channel scope, masks, gain, healing and restoration.
// Display is how the spectrum is analysed (rebuilds the cache) and how it is
// drawn (does not).
Rectangle {
    id: root
    property real declipThresholdDb: -1
    property real dereverbAmountDb: 12
    property real dereverbDecay: 0.6
    property real dereverbStrength: 1
    objectName: "spectralDock"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int currentTab: 0
    readonly property real nyquist: service ? service.sampleRate / 2 : 24000
    readonly property var spectral: service ? service.spectral : null

    color: Theme.panelBg

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.hairline
        color: Theme.border
        z: 2
    }

    DockHeader {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        tabs: [qsTr("Repair"), qsTr("Display")]
        currentIndex: root.currentTab
        onActivated: function (i) { root.currentTab = i }
        trailing: [
            IconButton {
                iconName: "panel-right"; text: qsTr("Hide the panel")
                helpText: qsTr("Hide this Repair and Display panel to give the spectral view more room.")
                helpScope: qsTr("Changes the layout only. The View menu or the toolbar button brings it back.")
                onClicked: if (root.shell) root.shell.effectsDockVisible = false
            }
        ]
    }

    component Heading: Text {
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        font.weight: Theme.wHeading
        color: Theme.textMuted
    }
    component Body: Text {
        width: parent.width
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
    }
    component Fact: Row {
        property string label: ""
        property string value: ""
        width: parent.width
        spacing: Theme.s2
        Text {
            width: 72
            text: parent.label
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Text {
            width: parent.width - 72 - Theme.s2
            text: parent.value
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: Theme.textPrimary
        }
    }


    Flickable {
        id: flick
        objectName: "spectralDockFlick"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // As ScrollPane: the list reaches past the text column on both sides, on
        // the left for a labelled button's hover box, on the right for the bar.
        readonly property int reachLeft: Theme.s2
        readonly property int reachRight: Theme.s3
        anchors.leftMargin: Theme.s3 - reachLeft
        anchors.rightMargin: Theme.s3 - reachRight
        contentHeight: pages.implicitHeight + Theme.s3 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}

        Item {
            id: pages
            x: flick.reachLeft
            width: flick.width - flick.reachLeft - flick.reachRight
            implicitHeight: root.currentTab === 0 ? repair.implicitHeight : display.implicitHeight
            y: Theme.s3

            // ── Repair ────────────────────────────────────────────────────
            Column {
                id: repair
                objectName: "repairPanel"
                visible: root.currentTab === 0
                width: parent.width
                spacing: Theme.s2

                Heading { text: qsTr("SELECTION") }
                ComboField {
                    objectName: "repairChannel"
                    caption: qsTr("Process")
                    helpTitle: qsTr("Channels to process")
                    helpText: qsTr("Choose which channels a repair changes: every channel, Left or Right alone, or the Mid (centre) or Side (stereo difference) component of a stereo take.")
                    helpScope: qsTr("Applies to every repair on this panel. The other channels or components stay exactly as they were. This is separate from how the display draws channels.")
                    unavailableReason: qsTr("Wait for the current repair to finish.")
                    width: parent.width
                    model: [qsTr("All channels"), qsTr("Left"), qsTr("Right"), qsTr("Mid"), qsTr("Side")]
                    currentIndex: root.service ? Math.max(0, ["all", "left", "right", "mid", "side"].indexOf(root.service.repairChannel)) : 0
                    enabled: root.service && !root.service.repairing
                    onActivated: function(i) { root.service.repairChannel = ["all", "left", "right", "mid", "side"][i] }
                }
                Body { text: qsTr("Processing scope is independent of the display. Mid = (L + R) / 2; Side = (L − R) / 2.") }
                Fact {
                    objectName: "repairSelectionTime"
                    label: qsTr("Time")
                    value: !root.service || root.service.selectionEmpty ? qsTr("none")
                         : root.shell.formatFrames(root.service.selectionStart) + " – "
                           + root.shell.formatFrames(root.service.selectionEnd)
                }
                Fact {
                    objectName: "repairSelectionBand"
                    label: qsTr("Band")
                    value: !root.service || root.service.bandEmpty ? qsTr("full spectrum")
                         : Theme.formatHz(root.service.bandLowHz) + " – " + Theme.formatHz(root.service.bandHighHz)
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    Text { width: 72; text: qsTr("Low"); anchors.verticalCenter: parent.verticalCenter
                           font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    ValueField {
                        objectName: "repairBandLow"
                        width: 112
                        label: qsTr("Band low")
                        helpText: qsTr("The lowest frequency in hertz (Hz) that a repair touches. Below it the audio is left alone.")
                        helpScope: qsTr("Sets the frequency band with Band high; an empty band means the full spectrum. A drawn mask replaces the band while it holds anything. Type a value, use Up and Down, or drag the number.")
                        value: root.service && !root.service.bandEmpty ? root.service.bandLowHz : 0
                        from: 0; to: root.nyquist
                        step: 10; decimals: 0; suffix: " Hz"
                        onEdited: function (v) {
                            if (root.service)
                                root.service.setBand(v, root.service.bandEmpty ? root.nyquist : root.service.bandHighHz)
                        }
                    }
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    Text { width: 72; text: qsTr("High"); anchors.verticalCenter: parent.verticalCenter
                           font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    ValueField {
                        objectName: "repairBandHigh"
                        width: 112
                        label: qsTr("Band high")
                        helpText: qsTr("The highest frequency in hertz (Hz) that a repair touches. The top of the range is half the sample rate, the highest frequency the audio can hold.")
                        helpScope: qsTr("Sets the frequency band with Band low; an empty band means the full spectrum. A drawn mask replaces the band while it holds anything.")
                        value: root.service && !root.service.bandEmpty ? root.service.bandHighHz : root.nyquist
                        from: 0; to: root.nyquist
                        step: 10; decimals: 0; suffix: " Hz"
                        onEdited: function (v) {
                            if (root.service)
                                root.service.setBand(root.service.bandEmpty ? 0 : root.service.bandLowHz, v)
                        }
                    }
                    ToolButton {
                        objectName: "repairClearBand"
                        iconName: "eraser"; text: qsTr("Full spectrum")
                        helpText: qsTr("Clear the frequency band so repairs act on every frequency in the time selection.")
                        helpScope: qsTr("Changes the selection only; no audio changes.")
                        unavailableReason: qsTr("There is no frequency band to clear; the full spectrum is already selected.")
                        enabled: root.service && !root.service.bandEmpty
                        onClicked: if (root.service) root.service.clearBand()
                    }
                }

                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("MASK") }
                Body { text: qsTr("Draw what to repair with the lasso or the brush. While the mask holds anything, the repairs take it instead of the band; the selection grows to hold it. Cut shapes take away from what came before them; a Keep shape narrows the mask to what it holds inside.") }
                Fact {
                    objectName: "repairMaskFact"
                    label: qsTr("Mask")
                    value: !root.service || root.service.maskShapeCount === 0 ? qsTr("none")
                         : root.service.maskEmpty ? qsTr("%n cut(s), nothing added", "", root.service.maskShapeCount)
                         : qsTr("%n shape(s)", "", root.service.maskShapeCount)
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    Text { width: 72; text: qsTr("Mode"); anchors.verticalCenter: parent.verticalCenter
                           font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    SegmentedControl {
                        objectName: "repairMaskMode"
                        helpTitle: qsTr("Mask mode")
                        helpText: qsTr("What a new shape does to the repair mask.")
                        helpScope: qsTr("Holding Alt while drawing flips Add and Cut for that shape.")
                        labels: [qsTr("Add"), qsTr("Cut"), qsTr("Keep")]
                        names: [qsTr("New shapes add to the mask"), qsTr("New shapes cut from the mask"),
                                qsTr("New shapes keep only what the mask already had inside them")]
                        currentIndex: root.shell && root.shell.spectralMaskSubtract ? 1
                                    : root.shell && root.shell.spectralMaskIntersect ? 2 : 0
                        onActivated: function (i) { if (root.shell) root.shell.spectralMaskMode = i === 1 ? "cut" : i === 2 ? "keep" : "add" }
                    }
                }
                SliderField {
                    objectName: "repairBrushSize"
                    width: parent.width
                    label: qsTr("Brush")
                    helpTitle: qsTr("Brush size")
                    helpText: qsTr("How wide the brush paints on the spectral view, in screen pixels (px), from 2 to 200.")
                    helpScope: qsTr("Applies to the Brush and Repair brush tools. A painted shape covers the time and frequency under the stroke; changing the size does not alter existing shapes.")
                    from: 2; to: 200; stepSize: 1; decimals: 0; suffix: " px"
                    readoutWidth: 84
                    value: root.shell ? root.shell.spectralBrushSize : 24
                    onEdited: function (v) { if (root.shell) root.shell.spectralBrushSize = v }
                }
                SliderField {
                    objectName: "repairFeather"
                    width: parent.width
                    label: qsTr("Feather")
                    helpText: qsTr("Soften the edge of the mask. Feather widens each shape by this many analysis cells (time steps and frequency bins) with a gradual fade, so a repair blends in instead of stopping sharply. 0 is a hard edge.")
                    helpScope: qsTr("Applies to the next repair that uses the mask. Existing audio is unchanged until you repair.")
                    from: 0; to: 16; stepSize: 1; decimals: 0; suffix: qsTr(" cells")
                    readoutWidth: 84
                    value: root.shell ? root.shell.spectralFeather : 2
                    onEdited: function (v) { if (root.shell) root.shell.spectralFeather = Math.round(v) }
                }
                Heading { text: qsTr("HARMONICS") }
                Toggle {
                    objectName: "repairFollowPitch"
                    label: qsTr("Follow pitch")
                    helpText: qsTr("When the Harmonics tool picks a tone, trace how its pitch drifts over the selection instead of assuming a fixed frequency. Each harmonic band then follows the traced pitch.")
                    helpScope: qsTr("Applies to harmonic shapes made after this is switched on. Tracing runs in the background and can be cancelled; it holds the last frequency through quiet gaps.")
                    checked: root.shell ? root.shell.spectralFollowPitch : false
                    onClicked: if (root.shell) root.shell.spectralFollowPitch = checked
                }
                Body { text: qsTr("Follow pitch traces a clear picked tone in both directions. It holds the last frequency through quiet gaps. For drifting hum, use a larger FFT and narrow Hz bands.") }
                Row {
                    visible: root.service !== null && root.service.pitchTracking
                    width: parent.width
                    spacing: Theme.s2
                    Text {
                        text: qsTr("Following pitch…")
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                        color: Theme.textSecondary
                    }
                    ToolButton {
                        objectName: "cancelPitchTracking"
                        text: qsTr("Cancel")
                        helpTitle: qsTr("Cancel pitch tracing")
                        helpText: qsTr("Stop tracing the picked tone. No harmonic shape is added.")
                        helpScope: qsTr("No audio changes.")
                        onClicked: if (root.service) root.service.cancelPitchTracking()
                    }
                }
                Body { text: qsTr("With the tool (O), click a partial for its series over the selection, or drag along it for a span. Cents make bands proportional to each partial's frequency; Hz gives every band the same width.") }
                SliderField {
                    objectName: "repairHarmonicPartials"
                    width: parent.width
                    label: qsTr("Partials")
                    helpText: qsTr("How many harmonics a harmonic shape covers, from 1 to 48. A harmonic (partial) is a multiple of the picked tone's fundamental frequency: 2×, 3× and so on.")
                    helpScope: qsTr("Applies to the next harmonic shape you pick. Bands above half the sample rate are dropped.")
                    from: 1; to: 48; stepSize: 1; decimals: 0; suffix: ""
                    readoutWidth: 84
                    value: root.shell ? root.shell.spectralHarmonicPartials : 16
                    onEdited: function (v) { if (root.shell) root.shell.spectralHarmonicPartials = Math.round(v) }
                }
                SliderField {
                    objectName: "repairHarmonicWidth"
                    width: parent.width
                    label: qsTr("Width")
                    helpTitle: qsTr("Harmonic band width")
                    helpText: qsTr("How far either side of each harmonic a band reaches. In cents (¢, hundredths of a semitone) every band scales with its harmonic's frequency; in hertz (Hz) every band is the same width.")
                    helpScope: qsTr("Applies to the next harmonic shape you pick. Narrow bands take less of the surrounding sound; wider bands tolerate a drifting pitch.")
                    from: root.shell && root.shell.spectralHarmonicWidthInHz ? 0.5 : 1
                    to: 200
                    stepSize: root.shell && root.shell.spectralHarmonicWidthInHz ? 0.5 : 1
                    decimals: root.shell && root.shell.spectralHarmonicWidthInHz ? 1 : 0
                    suffix: root.shell && root.shell.spectralHarmonicWidthInHz ? qsTr(" Hz") : qsTr(" ¢")
                    readoutWidth: 84
                    value: root.shell ? root.shell.spectralHarmonicWidth : 30
                    onEdited: function (v) { if (root.shell) root.shell.spectralHarmonicWidth = v }
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    Text { width: 72; text: qsTr("Width in"); anchors.verticalCenter: parent.verticalCenter
                           font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    SegmentedControl {
                        objectName: "repairHarmonicUnit"
                        helpTitle: qsTr("Harmonic band width unit")
                        helpText: qsTr("How the width of each harmonic band is measured.")
                        labels: [qsTr("Cents"), qsTr("Hz")]
                        names: [qsTr("Each band scales with its partial"), qsTr("Every band the same width")]
                        currentIndex: root.shell && root.shell.spectralHarmonicWidthInHz ? 1 : 0
                        onActivated: function (i) {
                            if (!root.shell)
                                return
                            root.shell.spectralHarmonicWidthInHz = i === 1
                            root.shell.spectralHarmonicWidth = Math.max(i === 1 ? 0.5 : 1, Math.min(200, root.shell.spectralHarmonicWidth))
                        }
                    }
                }
                Toggle {
                    objectName: "repairHarmonicFundamental"
                    label: qsTr("Include the fundamental (off: the overtones alone)")
                    helpTitle: qsTr("Include the fundamental")
                    helpText: qsTr("Include the picked tone itself in a harmonic shape. Switched off, the shape covers only the overtones above it.")
                    helpScope: qsTr("Applies to the next harmonic shape you pick. Leaving the fundamental out keeps the body of a note while thinning its upper harmonics.")
                    checked: root.shell ? root.shell.spectralHarmonicFundamental : true
                    onClicked: if (root.shell) root.shell.spectralHarmonicFundamental = checked
                }
                Fact {
                    objectName: "repairHarmonicFact"
                    label: qsTr("Fundamental")
                    value: {
                        if (!root.service)
                            return qsTr("—")
                        const shapes = root.service.maskShapes
                        for (var i = shapes.length - 1; i >= 0; --i) {
                            if (shapes[i].kind === "comb")
                                return Theme.formatHz(shapes[i].fundamentalHz) + qsTr(" · %n band(s)", "", shapes[i].bands.length)
                        }
                        return qsTr("—")
                    }
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "repairHum50"
                        iconName: "audio-lines"; text: qsTr("Hum 50 Hz"); showLabel: true
                        helpText: qsTr("Add a harmonic shape for mains hum at 50 Hz and its first forty harmonics, each band 3 Hz either side, to the mask.")
                        helpScope: qsTr("Covers the time selection, or the whole take without one. Nothing is repaired yet: follow it with Change level. 50 Hz is the mains frequency in the UK, Europe and most of the world.")
                        unavailableReason: qsTr("Open or import audio first.")
                        shortcut: root.shell ? root.shell.bindingFor("spectral_hum_50") : ""
                        enabled: root.service && root.service.trackCount > 0
                        onClicked: root.shell.runAction("spectral_hum_50")
                    }
                    ToolButton {
                        objectName: "repairHum60"
                        iconName: "audio-lines"; text: qsTr("Hum 60 Hz"); showLabel: true
                        helpText: qsTr("Add a harmonic shape for mains hum at 60 Hz and its first forty harmonics, each band 3 Hz either side, to the mask.")
                        helpScope: qsTr("Covers the time selection, or the whole take without one. Nothing is repaired yet: follow it with Change level. 60 Hz is the mains frequency in North America and parts of Asia and South America.")
                        unavailableReason: qsTr("Open or import audio first.")
                        shortcut: root.shell ? root.shell.bindingFor("spectral_hum_60") : ""
                        enabled: root.service && root.service.trackCount > 0
                        onClicked: root.shell.runAction("spectral_hum_60")
                    }
                }
                Body { text: qsTr("Selects the hum and 39 higher tones related to it, with a narrow band around each. Use Change level to reduce them.") }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "repairMaskUndo"
                        iconName: "undo-2"; text: qsTr("Undo last shape"); showLabel: true
                        helpText: qsTr("Remove the most recent shape from the mask: a lasso, a brush stroke or a harmonic series.")
                        helpScope: qsTr("Changes the mask only; no audio changes. This is separate from Edit › Undo, which reverses repairs.")
                        unavailableReason: qsTr("The mask holds no shapes.")
                        shortcut: root.shell ? root.shell.bindingFor("spectral_mask_undo") : ""
                        enabled: root.service && root.service.maskShapeCount > 0
                        onClicked: root.shell.runAction("spectral_mask_undo")
                    }
                    ToolButton {
                        objectName: "repairMaskClear"
                        iconName: "eraser"; text: qsTr("Clear mask"); showLabel: true
                        helpText: qsTr("Remove every shape from the mask. Repairs go back to using the frequency band and the time selection.")
                        helpScope: qsTr("Changes the mask only; no audio changes.")
                        unavailableReason: qsTr("The mask holds no shapes.")
                        shortcut: root.shell ? root.shell.bindingFor("spectral_clear_mask") : ""
                        enabled: root.service && root.service.maskShapeCount > 0
                        onClicked: root.shell.runAction("spectral_clear_mask")
                    }
                }
                Body { text: qsTr("The Repair brush is the brush and Repair in one: what you paint is repaired the moment you let go, without touching the mask.") }

                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("CHANGE LEVEL") }
                Body { text: qsTr("Turns the band — or the mask — inside the selection up or down, feathered at its edges. −60 dB removes it.") }
                SliderField {
                    objectName: "attenuateAmount"
                    width: parent.width
                    label: qsTr("Gain")
                    helpTitle: qsTr("Level change")
                    helpText: qsTr("How much louder or quieter the selected area becomes, from −60 to +24 dB. Negative values reduce it; 0 leaves it unchanged. Start with a small reduction and listen.")
                    helpScope: qsTr("Press Apply level change to use this amount. The selection edges are softened to blend the change into the surrounding sound.")
                    from: -60; to: 24; stepSize: 0.5; decimals: 1; suffix: " dB"
                    readoutWidth: 84
                    origin: 0
                    value: root.shell ? root.shell.spectralAttenuateDb : -24
                    onEdited: function (v) { if (root.shell) root.shell.spectralAttenuateDb = v }
                }
                ToolButton {
                    objectName: "attenuateButton"
                    iconName: "volume-1"; text: qsTr("Apply level change"); showLabel: true
                    helpText: qsTr("Turn the frequency band, or the mask, inside the time selection up or down by the Gain above.")
                    helpScope: qsTr("Changes only the selected area and channels on the active track. Your original audio is kept, and Undo restores the previous sound.")
                    unavailableReason: !root.service || root.service.selectionEmpty ? qsTr("Select a time range first.") : root.service && root.service.repairing ? qsTr("Wait for the current repair to finish.") : ""
                    shortcut: root.shell ? root.shell.bindingFor("spectral_attenuate") : ""
                    enabled: root.service && !root.service.selectionEmpty && !root.service.repairing
                    onClicked: root.shell.runAction("spectral_attenuate")
                }

                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("HEAL") }
                Body { text: qsTr("Estimates a replacement from the sound just before and after the selected area. Keep the selection short, then listen and compare with Undo.") }
                ToolButton {
                    objectName: "healButton"
                    iconName: "wand"; text: qsTr("Repair the selection"); showLabel: true
                    helpText: qsTr("Try to repair a short unwanted sound using the audio around it. Select a small area first, then listen to the result. Undo restores the previous sound.")
                    helpScope: qsTr("Changes only the selected area and channels on the active track. Your original audio is kept, and Undo restores the previous sound.")
                    helpTopic: qsTr("Keep the selection tight around the sound you want gone: the heal is estimated from the audio on either side, so a long selection has less to borrow from. If a repair is already running, a new one waits and then starts.")
                    unavailableReason: !root.service || root.service.selectionEmpty ? qsTr("Select a time range first.") : root.service && root.service.repairing ? qsTr("Wait for the current repair to finish.") : ""
                    shortcut: root.shell ? root.shell.bindingFor("spectral_heal") : ""
                    enabled: root.service && !root.service.selectionEmpty && !root.service.repairing
                    onClicked: root.shell.runAction("spectral_heal")
                }
                Text {
                    objectName: "repairState"
                    width: parent.width
                    visible: root.service && root.service.repairing
                    text: root.service && root.service.repairQueued ? qsTr("Waiting for the engine, then repairing…") : qsTr("Repairing…")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    color: Theme.warning
                }
                ToolButton {
                    objectName: "repairCancel"
                    iconName: "x"; text: qsTr("Cancel repair"); showLabel: true
                    helpText: qsTr("Stop the repair that is running or waiting.")
                    helpScope: qsTr("A cancelled repair writes nothing; the audio stays as it was.")
                    visible: root.service && root.service.repairing
                    onClicked: root.service.cancelRepair()
                }
                Body { text: qsTr("Every repair is one undo step: the result is a new source laid over the selection, and the original audio is still in the project.") }

                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("REDUCE NOISE") }
                Body { text: qsTr("Select a stretch of nothing but the noise — room tone, hiss, hum — and capture it. Then select what to clean and reduce: every bin loses the print, never by more than the amount.") }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "denoiseCapture"
                        iconName: "pipette"; text: qsTr("Learn noise"); showLabel: true
                        helpText: qsTr("Measure the selection and keep its spectrum as the noise sample: a picture of the noise that Reduce noise later subtracts.")
                        helpScope: qsTr("Select a stretch of nothing but the noise first (room tone, hiss, hum). The print is kept with the project. No audio changes.")
                        unavailableReason: !root.service || root.service.selectionEmpty ? qsTr("Select a time range first.")
                                           : root.service.noisePrintBuilding ? qsTr("Wait for the noise sample being measured.") : ""
                        shortcut: root.shell ? root.shell.bindingFor("spectral_capture_print") : ""
                        enabled: root.service && !root.service.selectionEmpty && !root.service.noisePrintBuilding
                        onClicked: root.shell.runAction("spectral_capture_print")
                    }
                    IconButton {
                        objectName: "denoiseClearPrint"
                        iconName: "eraser"; text: qsTr("Forget the noise sample")
                        helpText: qsTr("Discard the noise sample, or stop measuring one.")
                        helpScope: qsTr("Reduce noise needs a print, so capture another before using it again. No audio changes.")
                        unavailableReason: qsTr("There is no noise sample to forget.")
                        enabled: root.service && (root.service.noisePrintReady || root.service.noisePrintBuilding)
                        anchors.verticalCenter: parent.verticalCenter
                        onClicked: if (root.service) root.service.clearNoisePrint()
                    }
                }
                Text {
                    objectName: "noisePrintStatus"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.service && root.service.noisePrintBuilding ? qsTr("Measuring noise sample…") : root.service && root.service.noisePrintReady ? root.service.noisePrintSummary
                                                                       : qsTr("No noise sample yet.")
                    textFormat: Text.PlainText
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                    font.features: { "tnum": 1 }
                    color: root.service && root.service.noisePrintReady ? Theme.textPrimary : Theme.textMuted
                }
                SliderField {
                    objectName: "denoiseAmount"
                    width: parent.width
                    label: qsTr("Amount")
                    helpTitle: qsTr("Noise reduction amount")
                    helpText: qsTr("The most any frequency can be reduced, in decibels (dB), from 0 to 40. This is the floor: no frequency loses more than this, which keeps a voice from turning watery.")
                    helpScope: qsTr("Applies to the next Reduce noise. Start around 12 dB and listen; a large amount can leave a hollow sound.")
                    from: 0; to: 40; stepSize: 1; decimals: 0; suffix: " dB"
                    readoutWidth: 84
                    value: root.shell ? root.shell.spectralDenoiseDb : 12
                    onEdited: function (v) { if (root.shell) root.shell.spectralDenoiseDb = v }
                }
                SliderField {
                    objectName: "denoiseStrength"
                    width: parent.width
                    label: qsTr("Strength")
                    helpTitle: qsTr("Noise reduction strength")
                    helpText: qsTr("How much of the noise sample is subtracted from each frequency, as a multiple from 0.5× to 3×. 1× subtracts the measured noise; more than 1× over-subtracts to catch noise that varies; less leaves some behind.")
                    helpScope: qsTr("Applies to the next Reduce noise, always limited by the Amount above.")
                    from: 0.5; to: 3; stepSize: 0.1; decimals: 1; suffix: " ×"
                    readoutWidth: 84
                    value: root.shell ? root.shell.spectralDenoiseStrength : 1.0
                    onEdited: function (v) { if (root.shell) root.shell.spectralDenoiseStrength = v }
                }
                ToolButton {
                    objectName: "denoiseButton"
                    iconName: "sparkles"; text: qsTr("Reduce noise in the selection"); showLabel: true
                    helpText: qsTr("Subtract the noise sample from the selection, frequency by frequency, by up to the Amount, at the Strength set above.")
                    helpScope: qsTr("Changes the selected channels inside the chosen band or drawn area. Your original audio is kept, and Undo restores the previous sound.")
                    unavailableReason: !root.service || root.service.selectionEmpty ? qsTr("Select a time range first.")
                                       : !root.service.noisePrintReady ? qsTr("Learn the noise first: select some noise on its own.")
                                       : root.service.repairing ? qsTr("Wait for the current repair to finish.") : ""
                    shortcut: root.shell ? root.shell.bindingFor("spectral_denoise") : ""
                    enabled: root.service && !root.service.selectionEmpty && root.service.noisePrintReady && !root.service.repairing
                    onClicked: root.shell.runAction("spectral_denoise")
                }

                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("CLICKS AND POPS") }
                Body { text: qsTr("Find what stands out from the waveform around it — in the selection, or the whole take without one. Each is marked on the display and listed here: a row puts the playhead on it, its own button heals it alone. Heal them all in one step, or raise the sensitivity and look again.") }
                SliderField {
                    objectName: "clickSensitivity"
                    width: parent.width
                    label: qsTr("Sensitivity")
                    helpTitle: qsTr("Click sensitivity")
                    helpText: qsTr("How readily Find clicks reports a click, from 0 to 1. Higher values find quieter clicks but can also flag sharp sounds that belong, such as drum hits or consonants.")
                    helpScope: qsTr("Applies to the next Find clicks or Find and Repair. Nothing changes until you repair.")
                    from: 0; to: 1; stepSize: 0.05; decimals: 2; suffix: ""
                    readoutWidth: 84
                    value: root.shell ? root.shell.spectralClickSensitivity : 0.5
                    onEdited: function (v) { if (root.shell) root.shell.spectralClickSensitivity = v }
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "clickFind"
                        iconName: "search"; text: qsTr("Find clicks"); showLabel: true
                        helpText: qsTr("Look for clicks and pops: brief spikes that stand out from the waveform around them. Each one found is marked on the display and listed below.")
                        helpScope: qsTr("Searches the time selection, or the whole active track without one. Finding changes no audio.")
                        unavailableReason: !root.service || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                                           : root.service.detecting ? qsTr("Wait for the current search to finish.") : ""
                        shortcut: root.shell ? root.shell.bindingFor("spectral_find_clicks") : ""
                        enabled: root.service && root.service.trackCount > 0 && !root.service.detecting
                        onClicked: root.shell.runAction("spectral_find_clicks")
                    }
                    IconButton {
                        objectName: "clickClear"
                        iconName: "eraser"; text: qsTr("Forget the clicks found")
                        helpText: qsTr("Clear the list of clicks and their marks on the display.")
                        helpScope: qsTr("No audio changes. Find clicks again to rebuild the list.")
                        unavailableReason: qsTr("No clicks are listed.")
                        enabled: root.service && root.service.clickCount > 0
                        anchors.verticalCenter: parent.verticalCenter
                        onClicked: if (root.service) root.service.clearClicks()
                    }
                }
                Text {
                    objectName: "clickFact"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.service && root.service.detecting ? qsTr("Looking…")
                        : root.service && root.service.clickSummary !== "" ? root.service.clickSummary
                        : qsTr("Nothing looked for yet.")
                    textFormat: Text.PlainText
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                    font.features: { "tnum": 1 }
                    color: root.service && root.service.clickCount > 0 ? Theme.textPrimary : Theme.textMuted
                }
                // The list itself: each click a row, a click on it puts the
                // playhead there (and scrolls it into view); its own heal is
                // the one repair, with the rest of the list kept.
                ListView {
                    id: clickList
                    objectName: "clickList"
                    width: parent.width
                    height: Math.min(6, count) * (Theme.hRow + Theme.s1)
                    visible: count > 0
                    clip: true
                    spacing: Theme.s1
                    model: root.service ? root.service.clicks : []
                    boundsBehavior: Flickable.StopAtBounds
                    C.ScrollBar.vertical: ScrollBar {}
                    function jumpTo(frame) {
                        if (!root.service)
                            return
                        root.service.seekFrames(frame)
                        const axis = root.shell ? root.shell.timeAxis : null
                        if (axis && (frame < axis.viewStart || frame > axis.viewEnd)) {
                            axis.autoFit = false
                            axis.viewStart = frame - axis.viewFrames / 2
                            axis.clampView()
                        }
                    }
                    delegate: Rectangle {
                        id: clickRow
                        objectName: "clickRow"
                        required property var modelData
                        required property int index
                        readonly property real ms: root.service && root.service.sampleRate > 0
                                                   ? modelData.length * 1000 / root.service.sampleRate : 0
                        width: clickList.width
                        height: Theme.hRow
                        radius: Theme.rControl
                        color: rowMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: clickList.jumpTo(clickRow.modelData.frame)
                            ControlHelp {
                                title: qsTr("Found click")
                                purpose: qsTr("A click the detector found, with its time and length. Click to bring the view and the playhead to it.")
                            }
                        }
                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.s2
                            anchors.right: healOne.left
                            anchors.rightMargin: Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.s2
                            Text {
                                objectName: "clickRowTime"
                                text: root.shell ? root.shell.formatFrames(clickRow.modelData.frame) : ""
                                font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                                font.features: { "tnum": 1 }
                                color: Theme.textPrimary
                            }
                            Text {
                                text: clickRow.ms < 1 ? qsTr("<1 ms") : qsTr("%1 ms").arg(Math.round(clickRow.ms))
                                font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                                font.features: { "tnum": 1 }
                                color: Theme.textMuted
                            }
                            Text {
                                text: "×" + Math.round(clickRow.modelData.strength)
                                font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                                font.features: { "tnum": 1 }
                                color: Theme.textMuted
                            }
                        }
                        IconButton {
                            id: healOne
                            objectName: "clickRowHeal"
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.s1
                            anchors.verticalCenter: parent.verticalCenter
                            iconName: "bandage"; text: qsTr("Repair this click alone")
                            helpText: qsTr("Repair just this click by redrawing the waveform across it from the samples either side. The rest of the list is kept.")
                            helpScope: qsTr("One Undo step. The original audio stays in the project.")
                            unavailableReason: qsTr("Wait for the current repair to finish.")
                            enabled: root.service && !root.service.repairing
                            onClicked: if (root.service) root.service.healClickAt(clickRow.index)
                        }
                    }
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "clickHeal"
                        iconName: "bandage"; text: qsTr("Repair them all"); showLabel: true
                        helpText: qsTr("Repair every click in the list by redrawing the waveform across each one from the samples either side.")
                        helpScope: qsTr("One Undo step for the whole list. The original audio stays in the project.")
                        unavailableReason: !root.service || root.service.clickCount === 0 ? qsTr("Find clicks first.") : root.service && root.service.repairing ? qsTr("Wait for the current repair to finish.") : ""
                        shortcut: root.shell ? root.shell.bindingFor("spectral_heal_clicks") : ""
                        enabled: root.service && root.service.clickCount > 0 && !root.service.repairing
                        onClicked: root.shell.runAction("spectral_heal_clicks")
                    }
                    ToolButton {
                        objectName: "clickAutoHeal"
                        iconName: "wand-sparkles"; text: qsTr("Find and Repair"); showLabel: true
                        helpText: qsTr("Find clicks at the current sensitivity and heal every one found, in one go.")
                        helpScope: qsTr("Searches the time selection, or the whole active track without one. One Undo step; the original audio stays in the project.")
                        helpTopic: qsTr("Listen afterwards. At high sensitivity the search can flag sharp sounds that belong, such as drum hits or hard consonants, and healing them softens them. Undo and lower the sensitivity if so.")
                        unavailableReason: !root.service || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                                           : root.service.detecting ? qsTr("Wait for the current search to finish.")
                                           : root.service.repairing ? qsTr("Wait for the current repair to finish.") : ""
                        shortcut: root.shell ? root.shell.bindingFor("spectral_auto_heal") : ""
                        enabled: root.service && root.service.trackCount > 0 && !root.service.detecting && !root.service.repairing
                        onClicked: root.shell.runAction("spectral_auto_heal")
                    }
                }

                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("RESTORE CLIPPED PEAKS") }
                Body { text: qsTr("Rebuilds short, flat clipped peaks from unclipped audio around them. Set the threshold just below the flat tops. Uncertain peaks and long plateaus stay unchanged; soft distortion cannot be restored this way.") }
                SliderField {
                    objectName: "declipThreshold"
                    width: parent.width; label: qsTr("Threshold")
                    helpTitle: qsTr("Clipping threshold")
                    helpText: qsTr("The level above which a flat-topped peak counts as clipped, in decibels relative to full scale (dBFS, where 0 is the loudest a file can hold), from −40 to +12. Set it just below the flat tops.")
                    helpScope: qsTr("Applies to the next Restore clipped peaks. Values above 0 dBFS suit audio that was clipped before being turned down.")
                    from: -40; to: 12; stepSize: 0.1; decimals: 1; suffix: " dBFS"; readoutWidth: 96
                    value: root.declipThresholdDb
                    onEdited: function(v) { root.declipThresholdDb = v }
                }
                ToolButton {
                    objectName: "declipButton"
                    iconName: "wand"; text: qsTr("Restore clipped peaks"); showLabel: true
                    helpText: qsTr("Rebuild short, flat clipped peaks in the selection from the unclipped audio around them.")
                    helpScope: qsTr("Acts on the selected channels of the active track. Uncertain peaks and long flat stretches are left alone; soft distortion cannot be restored. One Undo step; the original audio stays in the project.")
                    unavailableReason: !root.service || root.service.selectionEmpty ? qsTr("Select a time range first.") : root.service && root.service.repairing ? qsTr("Wait for the current repair to finish.") : ""
                    enabled: root.service && !root.service.selectionEmpty && !root.service.repairing
                    onClicked: root.service.declipSpectral(root.declipThresholdDb)
                }
                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("REDUCE LATE REVERB") }
                Body { text: qsTr("Reduces lingering room sound after words or notes. It cannot remove all echo or recreate a recording made without reverb. Start gently and compare with Undo.") }
                SliderField {
                    objectName: "dereverbAmount"
                    width: parent.width; label: qsTr("Amount")
                    helpTitle: qsTr("Reverb reduction amount")
                    helpText: qsTr("The most a decaying reverb tail can be turned down, in decibels (dB), from 0 to 40.")
                    helpScope: qsTr("Applies to the next Reduce late reverb. Start gently and compare with Undo.")
                    from: 0; to: 40; stepSize: 1; decimals: 0; suffix: " dB"; readoutWidth: 84
                    value: root.dereverbAmountDb
                    onEdited: function(v) { root.dereverbAmountDb = v }
                }
                SliderField {
                    objectName: "dereverbDecay"
                    width: parent.width; label: qsTr("Decay")
                    helpTitle: qsTr("Reverb decay time")
                    helpText: qsTr("How long the room's reverb takes to die away, in seconds, from 0.05 to 3. Match it to the recording: a small room is well under a second, a hall is longer.")
                    helpScope: qsTr("Applies to the next Reduce late reverb. Too short leaves tails; too long eats into sustained sounds.")
                    from: 0.05; to: 3; stepSize: 0.05; decimals: 2; suffix: " s"; readoutWidth: 84
                    value: root.dereverbDecay
                    onEdited: function(v) { root.dereverbDecay = v }
                }
                SliderField {
                    objectName: "dereverbStrength"
                    width: parent.width; label: qsTr("Strength")
                    helpTitle: qsTr("Reverb reduction strength")
                    helpText: qsTr("How aggressively the estimated tail is subtracted, as a multiple from 0× to 2×. 1× removes the estimate; less is gentler; more also takes some of the sound that follows.")
                    helpScope: qsTr("Applies to the next Reduce late reverb, always limited by the Amount above.")
                    from: 0; to: 2; stepSize: 0.05; decimals: 2; suffix: " ×"; readoutWidth: 84
                    value: root.dereverbStrength
                    onEdited: function(v) { root.dereverbStrength = v }
                }
                ToolButton {
                    objectName: "dereverbButton"
                    iconName: "sparkles"; text: qsTr("Reduce late reverb"); showLabel: true
                    helpText: qsTr("Turn down the decaying tails of reverb in the selection while keeping attacks and steady tones.")
                    helpScope: qsTr("Acts on the selected channels of the active track. It cannot make a dry recording or remove early reflections. One Undo step; the original audio stays in the project.")
                    unavailableReason: !root.service || root.service.selectionEmpty ? qsTr("Select a time range first.") : root.service && root.service.repairing ? qsTr("Wait for the current repair to finish.") : ""
                    enabled: root.service && !root.service.selectionEmpty && !root.service.repairing
                    onClicked: root.service.dereverbSpectral(root.dereverbAmountDb, root.dereverbDecay, root.dereverbStrength)
                }
                Text {
                    objectName: "restorationSummary"
                    width: parent.width; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    text: root.service ? root.service.repairSummary : ""
                    visible: text !== ""
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary
                }
            }

            // ── Display ───────────────────────────────────────────────────
            Column {
                id: display
                objectName: "displayPanel"
                visible: root.currentTab === 1
                width: parent.width
                spacing: Theme.s2

                Heading { text: qsTr("ANALYSIS") }
                Body { text: qsTr("These settings change how the sound is analysed for the display and repairs. The display updates in the background.") }
                ComboField {
                    objectName: "spectralFft"
                    caption: qsTr("FFT")
                    helpTitle: qsTr("FFT size")
                    helpText: qsTr("How many samples each analysis frame holds. The FFT (fast Fourier transform) turns a frame into frequencies: a larger size resolves frequencies more finely but blurs time; a smaller size shows time sharply but blurs frequency.")
                    helpScope: qsTr("Rebuilds the spectral display in the background and sets the frame the repairs use. No audio changes.")
                    helpTopic: qsTr("Use a large FFT for hum, whistles and other steady tones; a small one for clicks and other short events. The Rebuild status below shows the resulting frequency bins and time columns.")
                    width: parent.width
                    model: {
                        var names = []
                        const sizes = root.service ? root.service.spectralFftSizes : []
                        for (var i = 0; i < sizes.length; ++i)
                            names.push(String(sizes[i]))
                        return names
                    }
                    currentIndex: root.service ? Math.max(0, root.service.spectralFftSizes.indexOf(root.service.spectralFftSize)) : 2
                    onActivated: function (i) {
                        if (root.shell)
                            root.shell.setSpectralAnalysis(root.service.spectralFftSizes[i], root.service.spectralWindow, root.service.spectralOverlap)
                    }
                }
                ComboField {
                    objectName: "spectralWindow"
                    caption: qsTr("Window")
                    helpTitle: qsTr("Analysis window")
                    helpText: qsTr("The shape that fades each analysis frame in and out before it is analysed. Hann is the usual choice; Hamming keeps close frequencies slightly more distinct; Blackman-Harris hides faint sounds near loud ones best but is less precise in frequency.")
                    helpScope: qsTr("Rebuilds the spectral display in the background and applies to the repairs. No audio changes.")
                    width: parent.width
                    model: [qsTr("Hann"), qsTr("Hamming"), qsTr("Blackman-Harris")]
                    currentIndex: root.service ? Math.max(0, root.service.spectralWindows.indexOf(root.service.spectralWindow)) : 0
                    onActivated: function (i) {
                        if (root.shell)
                            root.shell.setSpectralAnalysis(root.service.spectralFftSize, root.service.spectralWindows[i], root.service.spectralOverlap)
                    }
                }
                ComboField {
                    objectName: "spectralOverlap"
                    caption: qsTr("Overlap")
                    helpTitle: qsTr("Frame overlap")
                    helpText: qsTr("How much each analysis frame overlaps the next. More overlap gives more time columns and smoother repairs, and costs more processing and cache.")
                    helpScope: qsTr("Rebuilds the spectral display in the background and applies to the repairs. No audio changes.")
                    width: parent.width
                    model: ["50 %", "75 %", "87.5 %"]
                    currentIndex: root.service ? Math.max(0, root.service.spectralOverlaps.indexOf(root.service.spectralOverlap)) : 1
                    onActivated: function (i) {
                        if (root.shell)
                            root.shell.setSpectralAnalysis(root.service.spectralFftSize, root.service.spectralWindow, root.service.spectralOverlaps[i])
                    }
                }
                Text {
                    objectName: "spectralStatus"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: {
                        if (!root.service)
                            return ""
                        if (root.service.spectralBuilding)
                            return qsTr("Building… %1 %").arg(Math.round(root.service.spectralProgress * 100))
                        if (root.service.spectralStale)
                            return qsTr("Out of date — rebuilds shortly")
                        if (root.spectral && root.spectral.ready) {
                            var detail = qsTr("%1-point %2 · hop %3 · %4 bins · %5 columns")
                                .arg(root.spectral.fftSize).arg(root.service.spectralWindow)
                                .arg(root.spectral.hop).arg(root.spectral.bins).arg(root.spectral.columns)
                            if (root.spectral.analysisHop && root.spectral.hop > root.spectral.analysisHop)
                                detail += "\n" + qsTr("Display combines %1 time steps to limit cache size. Repairs keep full resolution.")
                                    .arg(root.spectral.hop / root.spectral.analysisHop)
                            return detail
                        }
                        return qsTr("Nothing built")
                    }
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                    font.features: { "tnum": 1 }
                    color: root.service && (root.service.spectralBuilding || root.service.spectralStale) ? Theme.warning : Theme.textSecondary
                }
                ToolButton {
                    objectName: "spectralRebuild"
                    iconName: "refresh-cw"; text: qsTr("Rebuild now"); showLabel: true
                    helpText: qsTr("Rebuild the spectral display straight away instead of waiting for the automatic rebuild after a change.")
                    helpScope: qsTr("Runs in the background; the display only. No audio changes.")
                    unavailableReason: !root.service || !root.service.loaded ? qsTr("Open or import audio first.")
                                       : root.service.spectralBuilding ? qsTr("The display is already being built.") : ""
                    shortcut: root.shell ? root.shell.bindingFor("spectral_rebuild") : ""
                    enabled: root.service && !root.service.spectralBuilding && root.service.loaded
                    onClicked: root.shell.runAction("spectral_rebuild")
                }

                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Heading { text: qsTr("DRAWING") }
                Body { text: qsTr("Scale, brightness and palette redraw the same cache. Changing to or from mid/side rebuilds the channel analysis.") }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    Text { width: 72; text: qsTr("Scale"); anchors.verticalCenter: parent.verticalCenter
                           font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    SegmentedControl {
                        objectName: "spectralScale"
                        helpTitle: qsTr("Frequency scale")
                        helpText: qsTr("How frequency is spread up the display. Linear gives every hertz the same height; logarithmic gives each octave the same height, which is closer to how pitch is heard.")
                        helpScope: qsTr("Display only.")
                        labels: [qsTr("Linear"), qsTr("Logarithmic")]
                        names: [qsTr("Linear frequency scale"), qsTr("Logarithmic frequency scale")]
                        currentIndex: root.shell && root.shell.spectralScale === "linear" ? 0 : 1
                        onActivated: function (i) { if (root.shell) root.shell.spectralScale = i === 0 ? "linear" : "log" }
                    }
                }
                ComboField {
                    objectName: "spectralRange"
                    caption: qsTr("Range")
                    helpTitle: qsTr("Display range")
                    helpText: qsTr("How many decibels (dB) below the loudest level the display shows before fading to black. A narrow range shows loud detail clearly; a wide range also shows the quiet noise floor.")
                    helpScope: qsTr("Redraws the display only. No audio changes.")
                    width: parent.width
                    model: ["60 dB", "90 dB", "120 dB"]
                    currentIndex: root.shell ? Math.max(0, [60, 90, 120].indexOf(root.shell.spectralRange)) : 0
                    onActivated: function (i) { if (root.shell) root.shell.spectralRange = [60, 90, 120][i] }
                }
                SliderField {
                    objectName: "spectralGain"
                    width: parent.width
                    label: qsTr("Brightness")
                    helpText: qsTr("Shift the whole display brighter or darker, in decibels (dB), from −24 to +24. Turn it up to see quiet sounds; turn it down when loud sounds wash out.")
                    helpScope: qsTr("Redraws the display only. No audio changes.")
                    from: -24; to: 24; stepSize: 1; decimals: 0; suffix: " dB"
                    readoutWidth: 84
                    origin: 0
                    value: root.shell ? root.shell.spectralGain : 0
                    onEdited: function (v) { if (root.shell) root.shell.spectralGain = v }
                }
                ComboField {
                    objectName: "spectralPalette"
                    caption: qsTr("Palette")
                    helpTitle: qsTr("Colour palette")
                    helpText: qsTr("The colours used to draw level, from quiet to loud.")
                    helpScope: qsTr("Redraws the display only. No audio changes.")
                    width: parent.width
                    model: {
                        var names = []
                        for (var i = 0; i < Theme.spectralPalettes.length; ++i)
                            names.push(Theme.spectralPaletteLabel(Theme.spectralPalettes[i]))
                        return names
                    }
                    currentIndex: root.shell ? Math.max(0, Theme.spectralPalettes.indexOf(root.shell.spectralPalette)) : 0
                    onActivated: function (i) { if (root.shell) root.shell.spectralPalette = Theme.spectralPalettes[i] }
                }
                ComboField {
                    objectName: "spectralView"
                    caption: qsTr("Channels")
                    helpTitle: qsTr("Channel display")
                    helpText: qsTr("How a stereo take is drawn: Left and Right in their own lanes, both overlaid in one lane (cyan for Left, amber for Right), or as Mid (centre) and Side (stereo difference).")
                    helpScope: qsTr("Changes the display only; Mid/Side rebuilds it in the background. Which channels a repair changes is set by Process on the Repair tab.")
                    width: parent.width
                    model: [qsTr("Separate L / R"), qsTr("Overlaid L + R"), qsTr("Mid / Side")]
                    currentIndex: root.service ? Math.max(0, ["separate", "overlay", "mid-side"].indexOf(root.service.spectralView)) : 0
                    onActivated: function(i) { root.service.spectralView = ["separate", "overlay", "mid-side"][i] }
                }
                Body { text: qsTr("Stereo overlay uses cyan for Left and amber for Right. Mid/side rebuilds the display only. Mono and surround retain their own channel lanes.") }
            }
        }
    }
}
