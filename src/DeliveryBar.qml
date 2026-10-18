pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The strip along the bottom of the Mastering page: what the delivery is --
// preset, container, depth, rate, channels, dither, how the level is set --
// and the button that renders it. Every field is the shell's, persisted, so a
// render from the File menu uses exactly what is shown here.
Rectangle {
    id: root
    objectName: "deliveryBar"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool loaded: service !== null && service.trackCount > 0

    implicitHeight: content.implicitHeight + Theme.s2 * 2 + Theme.hairline
    color: Theme.panelBg

    Rectangle {
        anchors.top: parent.top
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    readonly property var formats: service ? service.exportFormats : []
    // Built-ins and the user's own, in the shell's one list.
    readonly property var presets: shell ? shell.deliveryAllPresets : (service ? service.deliveryPresets : [])

    // The formats the available encoders can write, by id; the combo lists these.
    readonly property var usableFormats: {
        var out = []
        for (var i = 0; i < root.formats.length; ++i) {
            if (root.formats[i].available)
                out.push(root.formats[i])
        }
        return out
    }
    readonly property var currentFormat: {
        for (var i = 0; i < root.usableFormats.length; ++i) {
            if (root.usableFormats[i].id === (root.shell ? root.shell.deliveryFormat : "wav"))
                return root.usableFormats[i]
        }
        return root.usableFormats.length > 0 ? root.usableFormats[0] : null
    }
    readonly property var depths: root.currentFormat && !root.currentFormat.lossy ? root.currentFormat.bitDepths : []
    readonly property var rates: [0, 44100, 48000, 88200, 96000, 192000]
    // The capability model's verdict on the current choices: ok, or a reason and
    // the nearest choice that works. Re-evaluated as any delivery setting changes.
    readonly property var check: root.service && root.shell
                                 ? root.service.checkDelivery(root.shell.deliverySpec("")) : ({ ok: true, reason: "" })
    readonly property string fixLabel: {
        if (!root.check || root.check.ok) return ""
        if (root.check.suggestedSampleRate > 0) return qsTr("Use %1 kHz").arg(root.check.suggestedSampleRate / 1000)
        if (root.check.suggestedBitDepth > 0) return qsTr("Use %1-bit").arg(root.check.suggestedBitDepth)
        if (root.check.suggestedChannels > 0) return root.check.suggestedChannels === 1 ? qsTr("Use mono") : qsTr("Use stereo")
        if (root.check.suggestedBitrateKbps === 0) return qsTr("Use automatic bitrate")
        return ""
    }
    function applyFix() {
        if (!root.shell || !root.check || root.check.ok) return
        if (root.check.suggestedSampleRate > 0) root.shell.setDelivery("deliverySampleRate", root.check.suggestedSampleRate)
        else if (root.check.suggestedBitDepth > 0) root.shell.setDelivery("deliveryBitDepth", root.check.suggestedBitDepth)
        else if (root.check.suggestedChannels > 0) root.shell.setDelivery("deliveryChannels", root.check.suggestedChannels === 1 ? 1 : 0)
        else if (root.check.suggestedBitrateKbps === 0) root.shell.setDelivery("deliveryBitrateKbps", 0)
    }
    // Formats the installed encoders cannot write, with the engine's reason, for the help.
    readonly property string unavailableFormatsNote: {
        var lines = []
        for (var i = 0; i < root.formats.length; ++i) {
            if (!root.formats[i].available)
                lines.push(qsTr("%1 is not listed: %2").arg(root.formats[i].name).arg(root.formats[i].reason))
        }
        return lines.join("\n")
    }
    readonly property var bitrates: [0, 64, 96, 128, 160, 192, 256, 320]

    function indexOfFormat(id) {
        for (var i = 0; i < root.usableFormats.length; ++i) {
            if (root.usableFormats[i].id === id)
                return i
        }
        return 0
    }
    function indexOfPreset(id) {
        for (var i = 0; i < root.presets.length; ++i) {
            if (root.presets[i].id === id)
                return i
        }
        return root.presets.length        // "Custom"
    }

    // A numeric field with its caption beside it: ValueField carries its label
    // for a screen reader only.
    component Field: Row {
        property alias label: cap.text
        property alias field: box
        spacing: Theme.s1
        Text {
            id: cap
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        ValueField {
            id: box
            width: 112
            anchors.verticalCenter: parent.verticalCenter
            helpTitle: cap.text
        }
    }

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.s2
        anchors.topMargin: Theme.s2 + Theme.hairline
        spacing: Theme.s2

        // ── the delivery, wrapping to the width there is ──────────────
        Flow {
            width: parent.width
            spacing: Theme.s2

            ComboField {
                id: presetCombo
                objectName: "deliveryPreset"
                caption: qsTr("Target")
                helpTitle: qsTr("Delivery target")
                helpText: qsTr("A preset that fills in format, depth, rate, channels and level for a destination.")
                helpScope: qsTr("Editing any field switches this to Custom. Presets change the export settings, not the project.")
                width: 230
                model: {
                    var names = []
                    for (var i = 0; i < root.presets.length; ++i)
                        names.push(root.presets[i].name)
                    names.push(qsTr("Custom"))
                    return names
                }
                currentIndex: root.indexOfPreset(root.shell ? root.shell.deliveryPreset : "")
                onActivated: function (index) {
                    if (!root.shell)
                        return
                    if (index < root.presets.length)
                        root.shell.applyPreset(root.presets[index].id)
                    else
                        root.shell.deliveryPreset = "custom"
                }
            }
            // Keep what the bar shows as a preset of your own; drop one of yours.
            Row {
                spacing: 0
                height: presetCombo.height
                IconButton {
                    objectName: "deliverySavePreset"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "save"; text: qsTr("Save preset")
                    helpText: qsTr("Keep the delivery settings shown here as a preset of your own.")
                    helpScope: qsTr("Stored in your settings and listed under Target.")
                    onClicked: {
                        if (!root.shell)
                            return
                        const current = root.shell.presetById(root.shell.deliveryPreset)
                        const initial = root.shell.deliveryPresetIsUser && current ? current.name : ""
                        root.shell.askName(qsTr("Name the preset"), initial, function (name) {
                            root.shell.saveUserPreset(name)
                        })
                    }
                }
                IconButton {
                    objectName: "deliveryDeletePreset"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "trash-2"; text: qsTr("Delete preset")
                    helpText: qsTr("Remove this preset of yours from the Target list.")
                    helpScope: qsTr("The settings shown stay as they are. Built-in presets cannot be deleted.")
                    unavailableReason: qsTr("Only a preset you saved can be deleted.")
                    enabled: root.shell ? root.shell.deliveryPresetIsUser : false
                    onClicked: if (root.shell) root.shell.deleteUserPreset(root.shell.deliveryPreset)
                }
            }
            ComboField {
                objectName: "deliveryFormat"
                caption: qsTr("Format")
                helpTitle: qsTr("Export format")
                helpText: qsTr("The file type to write. WAV, AIFF and FLAC keep full quality; MP3, Ogg, Opus and AAC compress the audio.")
                helpScope: qsTr("Only formats the installed encoders can write are listed.")
                helpTopic: root.unavailableFormatsNote
                width: 150
                model: {
                    var names = []
                    for (var i = 0; i < root.usableFormats.length; ++i)
                        names.push(root.usableFormats[i].name)
                    return names
                }
                currentIndex: root.indexOfFormat(root.shell ? root.shell.deliveryFormat : "wav")
                onActivated: function (index) {
                    if (!root.shell || index >= root.usableFormats.length)
                        return
                    // Nothing else changes on its own: a depth or rate the new
                    // container cannot hold is reported below, with a one-click fix.
                    root.shell.setDelivery("deliveryFormat", root.usableFormats[index].id)
                }
            }
            ComboField {
                objectName: "deliveryBitrate"; caption: qsTr("Bitrate"); width: 170
                helpText: qsTr("Kilobits per second for the AAC encoder. A higher bitrate keeps more detail in a larger file.")
                helpScope: qsTr("Automatic chooses a bitrate from the sample rate and channel count.")
                visible: root.shell && root.shell.deliveryFormat === "aac"
                model: [qsTr("Automatic"), "64 kb/s", "96 kb/s", "128 kb/s", "160 kb/s", "192 kb/s", "256 kb/s", "320 kb/s"]
                currentIndex: Math.max(0, root.bitrates.indexOf(root.shell ? root.shell.deliveryBitrateKbps : 0))
                onActivated: function(index) { if(root.shell) root.shell.setDelivery("deliveryBitrateKbps",root.bitrates[index]) }
            }
            ComboField {
                objectName: "deliveryDepth"
                caption: qsTr("Depth")
                helpTitle: qsTr("Bit depth")
                helpText: qsTr("Bits per sample in the exported file. 16-bit suits most delivery, 24-bit keeps more headroom and 32-bit float keeps the mix exactly.")
                helpScope: qsTr("Dither applies to 16- and 24-bit exports. The project itself stays 32-bit float.")
                unavailableReason: qsTr("This format sets its own encoding. Bit depth applies to WAV, AIFF and FLAC.")
                width: 130
                enabled: root.depths.length > 0
                model: {
                    if (root.depths.length === 0)
                        return [qsTr("Audio format")]
                    var names = []
                    for (var i = 0; i < root.depths.length; ++i)
                        names.push(root.depths[i] === 32 ? qsTr("32-bit float") : qsTr("%1-bit").arg(root.depths[i]))
                    return names
                }
                currentIndex: Math.max(0, root.depths.indexOf(root.shell ? root.shell.deliveryBitDepth : 24))
                onActivated: function (index) {
                    if (root.shell && index < root.depths.length)
                        root.shell.setDelivery("deliveryBitDepth", root.depths[index])
                }
            }
            ComboField {
                objectName: "deliveryRate"
                caption: qsTr("Rate")
                helpTitle: qsTr("Export sample rate")
                helpText: qsTr("Samples per second in the exported file. A kHz is 1,000 samples per second.")
                helpScope: qsTr("Changing this converts the export; it does not add detail to the recording.")
                helpTopic: qsTr("Project keeps the project sample rate. Choose the rate requested by your delivery destination. This setting does not change the project or the audio device rate. Opus, MP3 and AAC take only their own rates; a choice they cannot write is reported below with a one-click fix.")
                width: 172
                model: [qsTr("Project (%1 kHz)").arg(root.service ? root.service.sampleRate / 1000 : 48),
                        "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "192 kHz"]
                currentIndex: Math.max(0, root.rates.indexOf(root.shell ? root.shell.deliverySampleRate : 0))
                onActivated: function (index) {
                    if (root.shell)
                        root.shell.setDelivery("deliverySampleRate", root.rates[index])
                }
            }
            ComboField {
                objectName: "deliveryChannels"
                caption: qsTr("Channels")
                helpTitle: qsTr("Export channels")
                helpText: qsTr("Stereo writes two channels. Mono sums the mix to one channel.")
                helpScope: qsTr("Mono makes a smaller file and loses stereo placement. The project is unchanged.")
                width: 140
                model: [qsTr("Stereo"), qsTr("Mono")]
                currentIndex: root.shell && root.shell.deliveryChannels === 1 ? 1 : 0
                onActivated: function (index) {
                    if (root.shell)
                        root.shell.setDelivery("deliveryChannels", index === 1 ? 1 : 0)
                }
            }
            Toggle {
                objectName: "deliveryDither"
                label: qsTr("Dither")
                helpText: qsTr("Add very quiet noise when converting to an integer bit depth, helping avoid distortion in quiet audio.")
                helpScope: qsTr("Applied to the exported file. Your source and project audio stay unchanged.")
                unavailableReason: qsTr("Dither is available for integer PCM exports. Choose WAV or AIFF at 16/24-bit, or FLAC, to use it.")
                helpTopic: qsTr("Usually leave dither on for the final integer export. It is not applied to 32-bit float or compressed-codec exports.")
                enabled: root.depths.length > 0 && (root.shell ? root.shell.deliveryBitDepth !== 32 : true)
                checked: root.shell ? root.shell.deliveryDither : true
                onToggled: if (root.shell) root.shell.setDelivery("deliveryDither", checked)
            }

            ComboField {
                objectName: "deliveryNormalise"
                caption: qsTr("Level")
                helpTitle: qsTr("Export level")
                helpText: qsTr("Keep the mix at its current level, set its highest peak, or set its overall loudness.")
                helpScope: qsTr("Adjusts the exported file only.")
                helpTopic: qsTr("Loudness normalise measures the whole mix in LUFS and applies one gain, held below the true-peak ceiling. Peak normalise scales the mix so its loudest true peak reaches the target. As mixed writes the bus output unchanged.")
                width: 230
                model: [qsTr("Keep current level"), qsTr("Set peak level"), qsTr("Set loudness")]
                currentIndex: !root.shell ? 2 : root.shell.deliveryNormalise === "loudness" ? 2
                            : root.shell.deliveryNormalise === "peak" ? 1 : 0
                onActivated: function (index) {
                    if (root.shell)
                        root.shell.setDelivery("deliveryNormalise", index === 2 ? "loudness" : index === 1 ? "peak" : "none")
                }
            }
            Field {
                visible: root.shell ? root.shell.deliveryNormalise === "loudness" : true
                label: qsTr("Target")
                field.objectName: "deliveryTarget"
                field.label: qsTr("Target loudness")
                field.helpText: qsTr("Set the average loudness requested for the export. LUFS measures perceived loudness; a more negative value is quieter.")
                field.helpScope: qsTr("Range: −40 to −5 LUFS. Loudness normalisation adjusts the exported mix, within the true-peak ceiling.")
                field.helpTopic: qsTr("Type an exact value, use Up/Down, or drag the number sideways. Hold Shift while dragging for finer changes.")
                field.suffix: "LUFS"
                field.from: -40; field.to: -5; field.step: 0.5; field.decimals: 1
                field.value: root.shell ? root.shell.deliveryTargetLufs : -16
                field.onEdited: function (v) { if (root.shell) root.shell.setDelivery("deliveryTargetLufs", v) }
            }
            Field {
                visible: root.shell ? root.shell.deliveryNormalise === "loudness" : true
                label: qsTr("Ceiling")
                field.objectName: "deliveryCeiling"
                field.label: qsTr("True-peak ceiling")
                field.helpText: qsTr("The highest true peak allowed in the export. dBTP measures peaks between samples, which can exceed the sample values.")
                field.helpScope: qsTr("Range: −12 to 0 dBTP. Loudness normalisation holds its gain below this ceiling.")
                field.suffix: "dBTP"
                field.from: -12; field.to: 0; field.step: 0.1; field.decimals: 1
                field.value: root.shell ? root.shell.deliveryCeilingDbTp : -1
                field.onEdited: function (v) { if (root.shell) root.shell.setDelivery("deliveryCeilingDbTp", v) }
            }
            Field {
                visible: root.shell ? root.shell.deliveryNormalise === "peak" : false
                label: qsTr("Peak at")
                field.objectName: "deliveryPeak"
                field.label: qsTr("Peak target")
                field.helpText: qsTr("The level the loudest true peak is scaled to.")
                field.helpScope: qsTr("Range: −12 to 0 dBTP. Peak normalisation applies one gain to the whole mix.")
                field.suffix: "dBTP"
                field.from: -12; field.to: 0; field.step: 0.1; field.decimals: 1
                field.value: root.shell ? root.shell.deliveryPeakTargetDb : -0.3
                field.onEdited: function (v) { if (root.shell) root.shell.setDelivery("deliveryPeakTargetDb", v) }
            }
        }

        // ── what cannot be written, and the fix ───────────────────────
        Row {
            width: parent.width
            spacing: Theme.s2
            visible: root.check && !root.check.ok
            Text {
                objectName: "deliveryProblem"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, parent.width - (fixButton.visible ? fixButton.width + Theme.s2 : 0))
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.warning
                text: root.check && !root.check.ok ? root.check.reason : ""
            }
            ToolButton {
                id: fixButton
                objectName: "deliveryFix"
                anchors.verticalCenter: parent.verticalCenter
                visible: root.fixLabel !== ""
                text: root.fixLabel; showLabel: true
                helpText: qsTr("Change the one setting the chosen format cannot take to the nearest value it can.")
                helpScope: qsTr("Only that setting changes; the rest of the delivery stays as it is.")
                onClicked: root.applyFix()
            }
        }

        // ── the sentence, and the button ──────────────────────────────
        Row {
            width: parent.width
            spacing: Theme.s2

            Text {
                id: summary
                objectName: "deliverySummary"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, parent.width - metadataButton.width - renderButton.width - 2 * parent.spacing)
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
                text: {
                    if (!root.shell || !root.currentFormat)
                        return ""
                    var bits = [root.currentFormat.name]
                    if(root.shell.deliveryFormat === "aac")
                        bits.push(qsTr("%1 kb/s target").arg(root.shell.deliveryBitrateKbps || Math.min(root.shell.deliveryChannels === 1 ? 128 : 256,
                            Math.floor(6 * (root.shell.deliverySampleRate || (root.service ? root.service.sampleRate : 48000)) * (root.shell.deliveryChannels === 1 ? 1 : 2) / 1000))))
                    if (!root.currentFormat.lossy)
                        bits.push(root.shell.deliveryBitDepth === 32 ? qsTr("32-bit float") : qsTr("%1-bit").arg(root.shell.deliveryBitDepth)
                                  + (root.shell.deliveryDither && root.shell.deliveryBitDepth !== 32 ? qsTr(" dithered") : ""))
                    bits.push(root.shell.deliverySampleRate === 0
                              ? qsTr("%1 kHz").arg(root.service ? root.service.sampleRate / 1000 : 48)
                              : qsTr("%1 kHz").arg(root.shell.deliverySampleRate / 1000))
                    bits.push(root.shell.deliveryChannels === 1 ? qsTr("mono") : qsTr("stereo"))
                    if (root.shell.deliveryNormalise === "loudness")
                        bits.push(qsTr("%1 LUFS, ceiling %2 dBTP").arg(root.shell.deliveryTargetLufs.toFixed(1)).arg(root.shell.deliveryCeilingDbTp.toFixed(1)))
                    else if (root.shell.deliveryNormalise === "peak")
                        bits.push(qsTr("peak at %1 dBTP").arg(root.shell.deliveryPeakTargetDb.toFixed(1)))
                    else
                        bits.push(qsTr("level as mixed"))
                    const wavFamily = ["wav", "bwf", "rf64"].indexOf(root.shell.deliveryFormat) >= 0
                    if (wavFamily && (root.shell.deliveryIncludeMetadata || root.shell.deliveryFormat === "bwf"))
                        bits.push(qsTr("with metadata"))
                    if (wavFamily && root.shell.deliveryMarkers !== "none")
                        bits.push(root.shell.deliveryMarkers === "project" ? qsTr("project markers") : qsTr("source markers"))
                    return bits.join("  ·  ")
                }
            }

            ToolButton {
                id: metadataButton
                objectName: "deliveryMetadata"
                iconName: "tag"; text: qsTr("Metadata…")
                showLabel: root.width >= 1100
                anchors.verticalCenter: parent.verticalCenter
                helpText: qsTr("The project's broadcast description, the marker choice, and a preview of what the next export writes besides samples.")
                helpScope: qsTr("BWF always carries it; WAV and RF64 on request; compressed formats never.")
                onClicked: root.shell.runAction("show_metadata")
            }
            ToolButton {
                id: renderButton
                objectName: "deliveryRender"
                iconName: "download"
                text: qsTr("Export…")
                prominent: true
                helpText: qsTr("Choose the full mix or the selected tracks, then a file name; the export is queued with these settings.")
                helpScope: qsTr("The full mix is one file; selected tracks give one file per track. You can keep working while it exports.")
                unavailableReason: !root.loaded ? qsTr("Open or import audio before exporting.")
                                  : root.check && !root.check.ok ? root.check.reason : ""
                showLabel: true
                shortcut: root.shell ? root.shell.bindingFor("export") : ""
                enabled: root.loaded && (!root.check || root.check.ok)
                anchors.verticalCenter: parent.verticalCenter
                onClicked: root.shell.runAction("export")
                // The export menu opens from here, wherever Export was asked for.
                Binding { target: root.shell; property: "exportButton"; value: renderButton; when: root.shell !== null }
            }
        }

        // ── what is rendering now ──────────────────────────────────────
        Row {
            width: parent.width
            spacing: Theme.s2
            visible: root.service !== null && root.service.rendering
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Exporting")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
            }
            Rectangle {
                objectName: "renderProgress"
                anchors.verticalCenter: parent.verticalCenter
                width: 220; height: 6; radius: 3
                color: Theme.controlBg
                Rectangle {
                    width: parent.width * (root.service ? root.service.renderProgress : 0)
                    height: parent.height; radius: parent.radius
                    color: Theme.accent
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.service ? Math.round(root.service.renderProgress * 100) + "%" : ""
                font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                font.features: { "tnum": 1 }
                color: Theme.textSecondary
            }
        }
    }
}
