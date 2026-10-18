pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric
import "ui/FadeCurve.js" as FadeCurve

C.Popup {
    id: root
    objectName: "clipPropertiesPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var targetClip: ({})
    property string message: ""
    readonly property bool selectedScope: useSelection.checked && targetClip.selectedCount > 1
    readonly property real fadeMaximum: selectedScope && !scaleFades.checked ? targetClip.shortestLength : (targetClip.length || 0)
    readonly property bool fadeShapesValid: fadeInCurve.acceptableInput && fadeOutCurve.acceptableInput
        && fadeInShape.currentIndex >= 0 && fadeOutShape.currentIndex >= 0
    function fadeShapes() {
        return { fadeInShape: fadeInShape.currentIndex, fadeOutShape: fadeOutShape.currentIndex,
            fadeInCurve: Numeric.parse(fadeInCurve.text, "C") / 100,
            fadeOutCurve: Numeric.parse(fadeOutCurve.text, "C") / 100 }
    }
    function resetGainDraft() {
        gain.text = selectedScope && relativeGain.checked ? "0" : Number(targetClip.gainDb || 0).toFixed(6)
    }
    function reload(resetScope) {
        targetClip = service ? service.clipProperties() : {}
        if (resetScope !== false) {
            useSelection.checked = targetClip.selectedCount > 1
            relativeGain.checked = true
            scaleFades.checked = true
        }
        if (!(targetClip.selectedCount > 1)) useSelection.checked = false
        resetGainDraft()
        fadeIn.show(targetClip.fadeIn || 0)
        fadeOut.show(targetClip.fadeOut || 0)
        fadeInShape.currentIndex = targetClip.fadeInShape || 0
        fadeOutShape.currentIndex = targetClip.fadeOutShape || 0
        fadeInCurve.text = Number((targetClip.fadeInCurve || 0) * 100).toFixed(3)
        fadeOutCurve.text = Number((targetClip.fadeOutCurve || 0) * 100).toFixed(3)
        replaceFades.checked = false
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(640, parent ? parent.width - 32 : 640)
    height: Math.min(620, parent ? parent.height - 40 : 620)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: { reload(); message = "" }
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    component Fact: RowLayout {
        property string label: ""
        property string value: ""
        property int elide: Text.ElideNone
        Layout.fillWidth: true
        Text { text: parent.label; Layout.preferredWidth: 132; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        Text { text: parent.value; Layout.fillWidth: true; elide: parent.elide; wrapMode: parent.elide === Text.ElideNone ? Text.WrapAnywhere : Text.NoWrap; textFormat: Text.PlainText; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
    }

    component CurvePreview: Canvas {
        property int shape: 0
        property real bend: 0
        property bool reverse: false
        readonly property color curveColour: Theme.accent
        readonly property color baseColour: Theme.controlBg
        Layout.preferredWidth: 110; Layout.preferredHeight: 38
        onShapeChanged: requestPaint()
        onBendChanged: requestPaint()
        onReverseChanged: requestPaint()
        onCurveColourChanged: requestPaint()
        onBaseColourChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d"); ctx.reset()
            ctx.fillStyle = baseColour; ctx.fillRect(0, 0, width, height)
            ctx.strokeStyle = curveColour; ctx.lineWidth = 1.5; ctx.beginPath()
            for (let i = 0; i <= 64; ++i) {
                const t = i / 64
                const amplitude = FadeCurve.value(reverse ? 1 - t : t, shape, Number.isFinite(bend) ? bend : 0)
                const x = 3 + t * (width - 6), y = 3 + (1 - amplitude) * (height - 6)
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
            ctx.stroke()
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Clip properties"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                objectName: "clipPropertiesEnvelopes"; text: qsTr("Envelopes…"); showLabel: true
                enabled: !!root.targetClip.valid
                helpText: qsTr("Open volume and stereo pan points for the reference clip shown here.")
                helpScope: qsTr("Edits one clip, even when Apply to all selected clips is on. Draft gain and fade values are not applied.")
                unavailableReason: qsTr("Place the playhead or selection start inside a clip first.")
                onClicked: {
                    const track = root.targetClip.track, clip = root.targetClip.clip
                    root.close(); root.shell.openClipEnvelope(track, clip)
                }
            }
            ToolButton {
                objectName: "clipPropertiesEffects"; text: qsTr("Effects…"); showLabel: true
                enabled: !!root.targetClip.valid
                helpText: qsTr("Open the effects that process the reference clip shown here.")
                unavailableReason: qsTr("Place the playhead or selection start inside a clip first.")
                onClicked: {
                    const track = root.targetClip.track, clip = root.targetClip.clip
                    root.close(); root.shell.openClipEffects(track, clip)
                }
            }
            ToolButton {
                iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close this panel without applying the values shown.")
            }
        }
        Toggle {
            id: useSelection; objectName: "clipPropertiesSelectedScope"; Layout.fillWidth: true
            visible: root.targetClip.selectedCount > 1
            label: qsTr("Apply to all %1 selected clips").arg(root.targetClip.selectedCount || 0)
            helpText: qsTr("Edit the level and fades of the selected whole clips together, including linked group members.")
            helpScope: qsTr("One Undo step. Turn off to edit only the reference clip shown below. Time-range processing keeps its own scope.")
            onClicked: { root.resetGainDraft(); root.message = "" }
        }
        ScrollPane {
            id: scroll; objectName: "clipPropertiesScroll"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            contentWidth: availableWidth
            ColumnLayout {
                x: scroll.reachLeft; width: scroll.columnWidth; spacing: Theme.s3
                Text {
                    objectName: "clipPropertiesSourceName"; Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
                    text: root.selectedScope ? qsTr("Reference clip: %1").arg(Theme.shortName(root.targetClip.name, 48))
                        : root.targetClip.name || qsTr("Place the playhead or selection start inside a clip, then reload.")
                    color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                }
                Fact { objectName: "clipPropertiesSourcePath"; label: qsTr("Original file"); value: root.targetClip.path || qsTr("Generated audio"); elide: Text.ElideMiddle }
                Fact { objectName: "clipPropertiesChannels"; label: qsTr("Channels / rate"); value: root.targetClip.valid ? root.targetClip.channels + " / " + root.targetClip.sampleRate + " Hz" : "" }
                Fact { label: qsTr("Original rate"); value: root.targetClip.valid ? root.targetClip.originalSampleRate + " Hz" : "" }
                Fact { label: qsTr("Timeline range"); value: root.targetClip.valid && root.shell ? root.shell.formatFrames(root.targetClip.start) + " – " + root.shell.formatFrames(root.targetClip.end) : "" }
                Fact { label: qsTr("Source offset"); value: root.targetClip.valid && root.shell ? root.shell.formatDuration(root.targetClip.sourceOffset) : "" }
                Fact { label: qsTr("Duration"); objectName: "clipPropertiesDuration"; value: root.targetClip.valid && root.shell ? root.shell.formatDuration(root.targetClip.length) : "" }
                Fact { label: qsTr("Pitch / duration"); value: root.targetClip.valid ? root.targetClip.semitones.toFixed(3) + qsTr(" semitones / ") + (root.targetClip.durationRatio * 100).toFixed(2) + "%" : "" }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Theme.hairline; color: Theme.border }
                Toggle {
                    id: relativeGain; objectName: "clipPropertiesRelativeGain"; Layout.fillWidth: true; visible: root.selectedScope
                    label: qsTr("Keep relative levels")
                    helpText: qsTr("Add the same dB change to each selected clip, preserving their level differences. 0 makes no level change.")
                    helpScope: qsTr("Turn off to set every selected clip to the level shown. An edit that would exceed +48 dB on any clip is refused.")
                    onClicked: { root.resetGainDraft(); root.message = "" }
                }
                RowLayout {
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.selectedScope && relativeGain.checked ? qsTr("Level change (dB)") : qsTr("Clip gain (dB)"); color: Theme.textSecondary; Layout.preferredWidth: 150 }
                    TextEntry {
                        id: gain; objectName: "clipPropertiesGain"; Layout.fillWidth: true; selectByMouse: true
                        enabled: !!root.targetClip.valid
                        helpTitle: qsTr("Clip gain")
                        helpText: root.selectedScope && relativeGain.checked
                            ? qsTr("Add this level change to each selected clip. 0 preserves every clip's current level.")
                            : qsTr("Set the clip gain in decibels (dB). 0 uses the source level; −80 mutes it.")
                        helpScope: qsTr("Between −80 and 48. Applied with the Apply button as one Undo step; the source file is unchanged.")
                        unavailableReason: qsTr("Place the playhead inside a clip first.")
                        onTextEdited: root.message = ""
                        validator: DoubleValidator { bottom: -80; top: 48; decimals: 6; notation: DoubleValidator.StandardNotation; locale: "C" }
                    }
                }
                Toggle {
                    id: replaceFades; objectName: "clipPropertiesReplaceFades"
                    Layout.fillWidth: true
                    label: root.selectedScope ? qsTr("Replace selected clips' fades") : qsTr("Replace this clip's fades"); enabled: !!root.targetClip.valid
                    helpText: qsTr("Use the fade lengths, shapes and curves below in place of the current fades on the chosen clips.")
                    helpScope: qsTr("Off keeps the existing fades, including any curve inherited from an earlier fade.")
                    unavailableReason: qsTr("Place the playhead or selection start inside a clip, then reload.")
                    onClicked: root.message = ""
                }
                Toggle {
                    id: scaleFades; objectName: "clipPropertiesScaleFades"; Layout.fillWidth: true
                    visible: root.selectedScope; enabled: replaceFades.checked
                    label: qsTr("Scale fades to each clip's length")
                    helpText: qsTr("Enter fades for the reference clip. Each selected clip receives the same fraction of its own duration, rounded to a sample.")
                    helpScope: qsTr("Turn off to use the same fade lengths on every clip; they must fit the shortest selected clip.")
                    unavailableReason: qsTr("Turn on Replace fades first.")
                }
                RowLayout {
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Fade in"); color: Theme.textSecondary; Layout.preferredWidth: 150 }
                    TimeField {
                        duration: true
                        id: fadeIn; objectName: "clipPropertiesFadeIn"; Layout.fillWidth: true; selectByMouse: true
                        service: root.service; maximum: root.fadeMaximum
                        enabled: replaceFades.checked
                        helpTitle: qsTr("Fade in")
                        helpText: qsTr("How long the fade at the start of the clip lasts. 0 is no fade. Replaces the clip's fade in when Replace fades is on.")
                        unavailableReason: qsTr("Turn on Replace fades first.")
                        onTextEdited: root.message = ""
                    }
                }
                RowLayout {
                    ComboField {
                        id: fadeInShape; objectName: "clipPropertiesFadeInShape"; Layout.fillWidth: true
                        caption: qsTr("Fade-in shape"); model: [qsTr("Linear"), qsTr("Smooth")]; enabled: replaceFades.checked
                        helpText: qsTr("Linear changes amplitude at a steady rate. Smooth eases into and out of the fade. Curve can bend either shape.")
                        helpScope: qsTr("Applied with Replace fades and Apply. Source audio is retained.")
                        unavailableReason: qsTr("Turn on Replace fades first.")
                        onActivated: root.message = ""
                    }
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Curve %"); color: Theme.textSecondary }
                    TextEntry {
                        id: fadeInCurve; objectName: "clipPropertiesFadeInCurve"; Layout.preferredWidth: 76
                        enabled: replaceFades.checked; selectByMouse: true
                        validator: DoubleValidator { bottom: -100; top: 100; decimals: 3; notation: DoubleValidator.StandardNotation; locale: "C" }
                        helpTitle: qsTr("Fade-in curve")
                        helpText: qsTr("Bend the fade from −100 to +100 percent. Positive values raise its middle level; negative values lower it. 0 keeps the chosen shape.")
                        helpScope: qsTr("The start and end levels stay fixed. The preview shows the curve.")
                        unavailableReason: qsTr("Turn on Replace fades first.")
                        onTextEdited: root.message = ""
                    }
                    CurvePreview { objectName: "clipPropertiesFadeInPreview"; shape: fadeInShape.currentIndex; bend: Numeric.parse(fadeInCurve.text, "C") / 100 }
                }
                RowLayout {
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Fade out"); color: Theme.textSecondary; Layout.preferredWidth: 150 }
                    TimeField {
                        duration: true
                        id: fadeOut; objectName: "clipPropertiesFadeOut"; Layout.fillWidth: true; selectByMouse: true
                        service: root.service; maximum: root.fadeMaximum
                        enabled: replaceFades.checked
                        helpTitle: qsTr("Fade out")
                        helpText: qsTr("How long the fade at the end of the clip lasts. 0 is no fade. Replaces the clip's fade out when Replace fades is on.")
                        unavailableReason: qsTr("Turn on Replace fades first.")
                        onTextEdited: root.message = ""
                    }
                }
                RowLayout {
                    ComboField {
                        id: fadeOutShape; objectName: "clipPropertiesFadeOutShape"; Layout.fillWidth: true
                        caption: qsTr("Fade-out shape"); model: [qsTr("Linear"), qsTr("Smooth")]; enabled: replaceFades.checked
                        helpText: qsTr("Linear changes amplitude at a steady rate. Smooth eases into and out of the fade. Curve can bend either shape.")
                        helpScope: qsTr("Applied with Replace fades and Apply. Source audio is retained.")
                        unavailableReason: qsTr("Turn on Replace fades first.")
                        onActivated: root.message = ""
                    }
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Curve %"); color: Theme.textSecondary }
                    TextEntry {
                        id: fadeOutCurve; objectName: "clipPropertiesFadeOutCurve"; Layout.preferredWidth: 76
                        enabled: replaceFades.checked; selectByMouse: true
                        validator: DoubleValidator { bottom: -100; top: 100; decimals: 3; notation: DoubleValidator.StandardNotation; locale: "C" }
                        helpTitle: qsTr("Fade-out curve")
                        helpText: qsTr("Bend the fade from −100 to +100 percent. Positive values raise its middle level; negative values lower it. 0 keeps the chosen shape.")
                        helpScope: qsTr("The start and end levels stay fixed. The preview shows the curve.")
                        unavailableReason: qsTr("Turn on Replace fades first.")
                        onTextEdited: root.message = ""
                    }
                    CurvePreview { reverse: true; objectName: "clipPropertiesFadeOutPreview"; shape: fadeOutShape.currentIndex; bend: Numeric.parse(fadeOutCurve.text, "C") / 100 }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    text: root.selectedScope ? qsTr("Level and fades affect all channels of the selected clips. Existing fade curves are preserved unless Replace fades is on. Original source audio is retained.")
                        : root.targetClip.inheritedFades
                        ? qsTr("This clip inherits part of an earlier fade. Its curve is preserved unless you choose to replace the fades.")
                        : qsTr("Gain and fades affect the whole clip, on all channels. Original source audio is retained.")
                }
            }
        }
        Text {
            objectName: "clipPropertiesMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            text: root.message; visible: text !== ""; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        }
        RowLayout {
            ToolButton {
                objectName: "clipPropertiesReload"; text: qsTr("Reload"); showLabel: true; onClicked: { root.reload(); root.message = "" }
                helpText: qsTr("Read the clip under the playhead or selection start again, discarding edits here.")
            }
            ToolButton {
                objectName: "clipPropertiesReset"; text: qsTr("Reset level and fades"); showLabel: true; enabled: !!root.targetClip.valid
                helpText: qsTr("Set the level change to 0 dB and remove the fades. Press Apply to use these settings.")
                unavailableReason: qsTr("Place the playhead or selection start inside a clip, then reload.")
                onClicked: { relativeGain.checked = false; gain.text = "0"; fadeIn.text = "0"; fadeOut.text = "0"; fadeInShape.currentIndex = 0; fadeOutShape.currentIndex = 0; fadeInCurve.text = "0"; fadeOutCurve.text = "0"; replaceFades.checked = true; root.message = "" }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "clipPropertiesApply"; text: qsTr("Apply"); showLabel: true
                helpText: root.selectedScope ? qsTr("Apply the level and fade choices to all selected clips.") : qsTr("Apply the gain and fades shown to this clip.")
                helpScope: qsTr("Affects the chosen whole clips on all channels, as one Undo step. Original source audio is retained.")
                unavailableReason: !root.targetClip.valid ? qsTr("Place the playhead or selection start inside a clip, then reload.")
                    : !gain.acceptableInput ? qsTr("Enter a clip gain from −80 to +48 dB.")
                    : replaceFades.checked && !(fadeIn.valid && fadeOut.valid) ? qsTr("Enter fades within the chosen clips' lengths.")
                    : replaceFades.checked && !root.fadeShapesValid ? qsTr("Choose fade shapes and curve values from −100 to +100 percent.")
                    : !root.service ? qsTr("The audio engine is not available.")
                    : root.service.recording ? qsTr("Stop recording first.")
                    : root.service.sampleEditing ? qsTr("Wait for the current sample edit to finish.")
                    : qsTr("Wait for the file to finish loading.")
                enabled: !!root.targetClip.valid && gain.acceptableInput
                    && (!replaceFades.checked || (fadeIn.valid && fadeOut.valid && root.fadeShapesValid))
                    && root.service && !root.service.recording && !root.service.sampleEditing && !root.service.loading
                onClicked: {
                    const stamp = root.targetClip.stamp
                    const ok = root.selectedScope
                        ? root.service.applySelectedClipProperties(root.targetClip.selectionStamp, Numeric.parse(gain.text, "C"),
                            relativeGain.checked, replaceFades.checked, fadeIn.value, fadeOut.value, scaleFades.checked,
                            replaceFades.checked ? root.fadeShapes() : {})
                        : root.service.applyClipProperties(root.targetClip.track, root.targetClip.clip, stamp,
                            Numeric.parse(gain.text, "C"), replaceFades.checked, fadeIn.value, fadeOut.value,
                            replaceFades.checked ? root.fadeShapes() : {})
                    if (ok) {
                        root.message = qsTr("Clip property edit accepted. Undo restores the previous values.")
                        root.reload(false)
                    } else root.message = root.service.lastError
                }
            }
        }
    }
}
