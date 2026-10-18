pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The Inspector: everything about the focused track that is not a plugin --
// name, role, colour, height, level, pan, mute and solo -- then the Quick
// Fixes modules the brief lists, and the source the take came from.
//
// Available modules open their own controls. Unavailable processors keep an
// explicit reason; the Track Effects panel remains available for installed LV2 effects.
Flickable {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var track: null
    readonly property int trackId: track ? track.id : -1
    readonly property var provenance: root.service ? root.service.sourceMetadata(root.track ? root.track.id : -1) : ({})
    readonly property var sourceInfo: {
        const revision = root.service ? root.service.tracksRevision : 0   // re-read when tracks change
        return root.service && root.track ? root.service.trackSourceInfo(root.trackId) : ({})
    }
    function rates(values) { return values && values.length ? values.join(" Hz, ") + " Hz" : "—" }

    contentHeight: column.implicitHeight + Theme.s3 * 2
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    C.ScrollBar.vertical: ScrollBar {}

    Column {
        id: column
        x: Theme.s2
        y: Theme.s2
        width: root.width - Theme.s2 * 2 - Theme.wScrollBar
        spacing: Theme.s2

        component SectionTitle: Text {
            width: parent.width
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            font.letterSpacing: 0.4
            color: Theme.textMuted
            topPadding: Theme.s1
        }
        component Caption: Text {
            width: 64
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
        }
        component Fact: Row {
            property string label: ""
            property string value: ""
            width: parent.width
            Text { width: 84; text: parent.label; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
            Text {
                width: parent.width - 84; text: parent.value; elide: Text.ElideMiddle; textFormat: Text.PlainText
                font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; font.features: { "tnum": 1 }
                color: Theme.textSecondary
            }
        }

        // ── Track ────────────────────────────────────────────────────────
        SectionTitle { text: qsTr("TRACK") }

        Row {
            width: parent.width
            spacing: Theme.s2
            Caption { text: qsTr("Name") }
            Rectangle {
                width: parent.width - 64 - Theme.s2
                height: Theme.hControl
                radius: Theme.rControl
                color: Theme.controlBg
                border.width: Theme.hairline
                border.color: nameInput.activeFocus ? Theme.accent : Theme.border
                TextInput {
                    id: nameInput
                    objectName: "inspectorName"
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s2
                    anchors.rightMargin: Theme.s2
                    verticalAlignment: Text.AlignVCenter
                    text: root.track ? root.track.name : ""
                    // A long name stays inside the box and shows its start until
                    // it is being edited.
                    clip: true
                    onTextChanged: if (!activeFocus) cursorPosition = 0
                    onActiveFocusChanged: if (!activeFocus) cursorPosition = 0
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    selectByMouse: true
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentText
                    onAccepted: {
                        if (root.service && root.track && text.trim() !== "")
                            root.service.renameTrack(root.track.id, text.trim())
                        focus = false
                        if (root.shell) root.shell.restoreFocus()
                    }
                    Keys.onEscapePressed: {
                        text = root.track ? root.track.name : ""
                        focus = false
                        if (root.shell) root.shell.restoreFocus()
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: Theme.s2
            Caption { text: qsTr("Role") }
            ComboField {
                id: roleCombo
                objectName: "inspectorRole"
                helpTitle: qsTr("Role")
                helpText: qsTr("Label this track by what it carries: dialogue, music, effects, a bus and so on. The role sets the track's colour and label.")
                helpScope: qsTr("Changes nothing in the audio. Where the track's signal goes is set in Routing.")
                width: parent.width - 64 - Theme.s2
                readonly property var roles: root.service ? root.service.trackRoles : []
                model: {
                    var out = []
                    for (var i = 0; i < roles.length; ++i)
                        out.push(Theme.roleLabel(roles[i]))
                    return out
                }
                currentIndex: root.track ? Math.max(0, roles.indexOf(root.track.role)) : 0
                onActivated: function (i) {
                    if (root.service && root.track && i >= 0 && i < roles.length)
                        root.service.setTrackRole(root.track.id, roles[i])
                    if (root.shell) root.shell.restoreFocus()
                }
            }
        }
        Text {
            objectName: "inspectorRoleNote"
            width: parent.width
            wrapMode: Text.WordWrap
            visible: root.track !== null && (root.track.role === "bus" || root.track.role === "aux" || root.track.role === "master")
            text: qsTr("A role sets the label and colour. Routing chooses this track’s output, aux sends and external sidechain.")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsRuler + 1
            color: Theme.textMuted
        }
        ToolButton {
            objectName: "inspectorRouting"; text: qsTr("Routing…"); showLabel: true; iconName: "git-branch"
            helpText: qsTr("Choose where this track's output goes, add aux sends and pick an external sidechain.")
            helpScope: qsTr("A bus sums the tracks routed into it. An aux send feeds a copy of this track to another track. A sidechain lets an effect on this track react to another track's signal.")
            unavailableReason: qsTr("Click a track to make it active first.")
            enabled: root.track !== null && root.service !== null
            onClicked: { root.service.setFocusedTrack(root.track.id); root.shell.runAction("show_routing") }
        }

        Row {
            width: parent.width
            spacing: Theme.s2
            Caption { text: qsTr("Colour") }
            Row {
                spacing: Theme.s1
                anchors.verticalCenter: parent.verticalCenter

                // "Auto": the role's own colour.
                Rectangle {
                    objectName: "swatchAuto"
                    width: 18; height: 18; radius: 9
                    color: Theme.roleColor(root.track ? root.track.role : "dialogue")
                    border.width: root.track && root.track.color === "" ? 2 : Theme.hairline
                    border.color: root.track && root.track.color === "" ? Theme.textPrimary : Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: "A"
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler; font.weight: Theme.wHeading
                        color: Theme.accentText
                    }
                    TapHandler { onTapped: if (root.service && root.track) root.service.setTrackColor(root.track.id, "") }
                    Accessible.role: Accessible.Button
                    Accessible.name: autoHelp.title
                    Accessible.description: autoHelp.description
                    ControlHelp {
                        id: autoHelp
                        title: qsTr("Automatic colour")
                        purpose: qsTr("Colour this track by its role, so it changes when the role does.")
                        scope: qsTr("Display only: the lane, mixer strip and clips use this colour.")
                    }
                }
                Repeater {
                    model: Theme.swatches
                    Rectangle {
                        id: swatch
                        objectName: "swatch"
                        required property color modelData
                        readonly property string hex: String(modelData).toUpperCase()
                        readonly property bool current: root.track !== null && root.track.color === hex
                        width: 18; height: 18; radius: 9
                        color: modelData
                        border.width: current ? 2 : Theme.hairline
                        border.color: current ? Theme.textPrimary : Theme.withAlpha(Theme.textPrimary, 0.15)
                        TapHandler { onTapped: if (root.service && root.track) root.service.setTrackColor(root.track.id, swatch.hex) }
                        Accessible.role: Accessible.Button
                        Accessible.name: swatchHelp.title
                        Accessible.description: swatchHelp.description
                        ControlHelp {
                            id: swatchHelp
                            title: qsTr("Track colour %1").arg(swatch.hex)
                            purpose: qsTr("Give this track a fixed colour instead of its role's colour.")
                            scope: qsTr("Display only: the lane, mixer strip and clips use this colour.")
                        }
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: Theme.s2
            Caption { text: qsTr("Height") }
            SegmentedControl {
                objectName: "inspectorHeight"
                readonly property var heights: root.service ? root.service.trackHeights : []
                Accessible.description: heightHelp.description
                ControlHelp {
                    id: heightHelp
                    title: qsTr("Lane height")
                    purpose: qsTr("Choose how tall this track's lane is in the timeline, from minimized to expanded.")
                    scope: qsTr("Display only. A taller lane shows the waveform in more detail.")
                }
                labels: [qsTr("Min"), qsTr("Compact"), qsTr("Normal"), qsTr("Tall")]
                names: [qsTr("Minimized"), qsTr("Compact"), qsTr("Normal"), qsTr("Expanded")]
                currentIndex: root.track ? Math.max(0, heights.indexOf(root.track.height)) : 2
                onActivated: function (i) {
                    if (root.service && root.track && i < heights.length)
                        root.service.setTrackHeight(root.track.id, heights[i])
                }
            }
        }

        SliderField {
            objectName: "inspectorLevel"
            width: parent.width
            label: qsTr("Level")
            helpText: qsTr("Set this track's level in decibels (dB). 0 dB leaves the source level unchanged.")
            helpScope: qsTr("Range −60 to +12 dB. Applies in playback and exports; the source audio is unchanged. Volume automation, when enabled, multiplies this level.")
            labelWidth: 64
            from: -60; to: 12
            decimals: 1
            suffix: "dB"
            origin: 0
            value: root.track ? root.track.gainDb : 0
            onEdited: function (v) { if (root.service && root.track) root.service.setTrackGainDb(root.track.id, v) }
        }
        SliderField {
            objectName: "inspectorPan"
            width: parent.width
            label: qsTr("Pan")
            helpText: qsTr("Place this track between left (−100%) and right (+100%). 0 is the centre.")
            helpScope: qsTr("Applies to this track's output in playback and exports. If pan automation is enabled, the curve overrides this control.")
            labelWidth: 64
            from: -100; to: 100
            decimals: 0
            suffix: "%"
            origin: 0
            value: root.track ? Math.round(root.track.pan * 100) : 0
            onEdited: function (v) { if (root.service && root.track) root.service.setTrackPan(root.track.id, v / 100) }
        }

        Row {
            width: parent.width
            spacing: Theme.s3
            Toggle {
                objectName: "inspectorMute"
                label: qsTr("Mute")
                helpText: qsTr("Silence this track. Mute also silences its sends.")
                helpScope: qsTr("Acts on this track only. A soloed track elsewhere silences this one as well.")
                checked: root.track ? root.track.muted === true : false
                onToggled: if (root.service && root.track) root.service.setTrackMute(root.track.id, checked)
            }
            Toggle {
                objectName: "inspectorSolo"
                label: qsTr("Solo")
                helpText: qsTr("Hear this track on its own. While any track is soloed, tracks that are not soloed stay silent.")
                helpScope: qsTr("Solo several tracks to hear them together. The status bar shows SOLO while it is on.")
                checked: root.track ? root.track.soloed === true : false
                onToggled: if (root.service && root.track) root.service.setTrackSolo(root.track.id, checked)
            }
            ToolButton {
                objectName: "inspectorFx"
                iconName: "sparkles"
                text: root.track && root.track.inserts && root.track.inserts.length > 0
                      ? qsTr("%1 FX").arg(root.track.inserts.length) : qsTr("Add FX")
                helpTitle: qsTr("Effects")
                helpText: qsTr("Open the effect browser to add an installed plugin (LV2 or CLAP) to this track. The count is how many effects it already has.")
                helpScope: qsTr("Effects run live on playback and in exports. Manage them in the Track Effects panel.")
                showLabel: true
                shortcut: root.shell ? root.shell.bindingFor("show_plugins") : ""
                onClicked: root.shell.runAction("show_plugins")
            }
        }

        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

        // ── Quick Fixes ──────────────────────────────────────────────
        SectionTitle { text: qsTr("QUICK FIXES") }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Clean speech, measure loudness, open spectral repair, or adjust installed effects.")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }

        component Module: Rectangle {
            id: mod
            property string name: ""
            property string reason: ""
            property bool ai: false
            property bool available: false
            property string actionName: ""
            width: parent.width
            height: Theme.hRow + Theme.s1
            radius: Theme.rControl
            color: Theme.panelRaised
            border.width: Theme.hairline
            border.color: Theme.border
            opacity: available ? 1 : Theme.disabledOpacity + 0.2
            Row {
                anchors.left: parent.left
                anchors.leftMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2
                Icon { name: "chevron-right"; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: mod.name
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    color: Theme.textSecondary
                    anchors.verticalCenter: parent.verticalCenter
                }
                Rectangle {
                    visible: mod.ai
                    width: aiText.implicitWidth + Theme.s1 * 2
                    height: 14
                    radius: Theme.rControl
                    color: Theme.aiAccent
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        id: aiText
                        anchors.centerIn: parent
                        text: "AI"
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler; font.weight: Theme.wHeading
                        color: Theme.aiAccentText
                    }
                }
            }
            Toggle {
                visible: !mod.available
                anchors.right: parent.right
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                helpTitle: mod.name
                unavailableReason: mod.reason
                enabled: false
                checked: false
            }
            ToolButton {
                objectName: mod.objectName + "Open"
                visible: mod.available
                anchors.right: parent.right; anchors.rightMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Open"); showLabel: true
                helpTitle: qsTr("Open %1").arg(mod.name)
                helpText: mod.reason
                helpScope: qsTr("Acts on this track.")
                unavailableReason: qsTr("Click a track to make it active first.")
                enabled: root.track !== null && root.service !== null
                onClicked: { root.service.focusTrack(root.track.id); root.shell.runAction(mod.actionName) }
            }
            Accessible.role: Accessible.StaticText
            Accessible.name: mod.name
            Accessible.description: modHelp.description
            ControlHelp {
                id: modHelp
                title: mod.name
                purpose: mod.reason
            }
        }

        Module { objectName: "essentialLoudness"; name: qsTr("Loudness"); available: true; actionName: "show_track_loudness"; reason: qsTr("Measure this track’s output and apply an undoable loudness match with a peak ceiling.") }
        Module { objectName: "essentialVoiceCleanup"; name: qsTr("Voice Cleanup"); ai: true; available: true; actionName: "show_voice_cleanup"; reason: qsTr("Reduce noise in selected speech with local RNNoise processing, adjustable amount and Undo.") }
        Module { objectName: "essentialGate"; name: qsTr("Noise Gate"); available: true; actionName: "show_essential_gate"; reason: qsTr("Turn down quiet background sound between words using Calf Gate. Choose settings, then press Apply.") }
        Module { objectName: "essentialRepair"; name: qsTr("Repair"); available: true; actionName: "workspace_spectral"; reason: qsTr("Open the Spectral page: reduce noise, reduce late reverb, restore clipped peaks and repair a selection.") }
        Module { objectName: "essentialClarity"; name: qsTr("Brightness"); available: true; actionName: "show_essential_clarity"; reason: qsTr("Add brightness to the sound using Calf Exciter. Choose settings, then press Apply.") }
        Module { objectName: "essentialDeEsser"; name: qsTr("De-Esser"); available: true; actionName: "show_essential_deesser"; reason: qsTr("Soften harsh s and sh sounds using Calf Deesser. Choose settings, then press Apply.") }
        Module { objectName: "essentialDynamics"; name: qsTr("Dynamics"); available: true; actionName: "show_essential_dynamics"; reason: qsTr("Even out loud and quiet parts using Calf Compressor. Choose settings, then press Apply.") }
        Module { objectName: "essentialEq"; name: qsTr("EQ"); available: true; actionName: "show_essential_eq"; reason: qsTr("Adjust bass, middle and high tones using Calf Equalizer 5 Band. Choose settings, then press Apply.") }
        Module { objectName: "essentialDucking"; name: qsTr("Ducking"); available: true; actionName: "show_essential_ducking"; reason: qsTr("Reduce this track while a chosen trigger produces audio, using the LSP sidechain compressor.") }

        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

        // ── Source ───────────────────────────────────────────────────────
        SectionTitle { text: qsTr("SOURCE") }
        Fact { label: qsTr("Clips"); value: root.track ? String(root.track.clipCount) : "" }
        Fact { label: qsTr("Length"); value: root.track && root.shell ? root.shell.formatDuration(root.track.frames) : "" }
        Fact { objectName: "inspectorSourceCount"; label: qsTr("Sources"); value: root.sourceInfo.valid ? String(root.sourceInfo.sourceCount) : "" }
        Fact { objectName: "inspectorSourceName"; label: qsTr("Source"); value: !root.sourceInfo.valid ? "" : root.sourceInfo.sourceCount === 0 ? qsTr("No direct source audio") : root.sourceInfo.sourceCount > 1 ? qsTr("Multiple sources") : root.sourceInfo.name || qsTr("Missing source") }
        Fact { objectName: "inspectorSourcePath"; label: qsTr("Path"); value: root.sourceInfo.path || "—" }
        Fact { objectName: "inspectorSourceChannels"; label: qsTr("Channels"); value: (root.sourceInfo.channels || []).join(", ") || "—" }
        Fact { objectName: "inspectorSourceRate"; label: qsTr("Audio rate"); value: root.rates(root.sourceInfo.sampleRates) }
        Fact { objectName: "inspectorOriginalRate"; label: qsTr("Original rate"); value: root.rates(root.sourceInfo.originalRates) }
        Fact { objectName: "inspectorResampled"; label: qsTr("Resampled"); value: !root.sourceInfo.sourceCount ? "—" : (root.sourceInfo.resampledFrom || []).length ? qsTr("from %1").arg(root.rates(root.sourceInfo.resampledFrom)) : qsTr("no") }
        Fact { label: qsTr("Project rate"); value: root.service ? root.service.sampleRate + " Hz" : "" }
        Fact { objectName: "inspectorOriginator"; label: qsTr("Originator"); visible: root.provenance.hasBext === true; value: (root.provenance.originator || "—") + (root.provenance.originatorReference ? " · " + root.provenance.originatorReference : "") }
        Fact { objectName: "inspectorRecorded"; label: qsTr("Recorded"); visible: root.provenance.hasBext === true; value: root.provenance.recorded || "—" }
        Fact { objectName: "inspectorTimeReference"; label: qsTr("Time reference"); visible: root.provenance.hasBext === true; value: root.provenance.timeReferenceText || qsTr("none") }
        Fact { objectName: "inspectorFileMarkers"; label: qsTr("File markers"); visible: root.provenance.present === true; value: root.provenance.markerCount > 0 ? qsTr("%1: %2").arg(root.provenance.markerCount).arg((root.provenance.markers || []).slice(0, 3).map(m => m.label || "?").join(", ")) : qsTr("none") }
        Fact { objectName: "inspectorIxml"; label: qsTr("iXML"); visible: root.provenance.present === true; value: root.provenance.hasIxml ? qsTr("kept with the source") : qsTr("none") }
        Fact { objectName: "inspectorFileLoudness"; label: qsTr("File loudness"); visible: !!root.provenance.loudness; value: root.provenance.loudness || "" }
        Text {
            visible: root.sourceInfo.missingSources > 0; width: parent.width; wrapMode: Text.WordWrap
            text: qsTr("%1 source(s) are missing from this track.").arg(root.sourceInfo.missingSources || 0)
            color: Theme.warning; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        }
        ToolButton {
            objectName: "inspectorSourceProperties"; text: qsTr("Selected clip properties…"); showLabel: true
            helpText: qsTr("Show the selected clip's source file and edit its gain and fades.")
            helpScope: qsTr("Acts on the selected clip. Each change is one Undo step; the source file is unchanged.")
            unavailableReason: qsTr("This track has no source audio to inspect.")
            enabled: root.sourceInfo.sourceCount > 0
            onClicked: root.shell.runAction("show_clip_properties")
        }
        Fact { label: qsTr("Project"); value: root.service && root.service.projectPath !== "" ? root.service.projectPath : qsTr("unsaved") }
    }
}
