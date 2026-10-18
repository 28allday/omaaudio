pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "essentialProcessorPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property string module: "dynamics"
    property string selectedGroup: ""
    readonly property var groups: Array.from(new Set((descriptor.fields || []).map(function(field) { return field.group }).filter(function(group) { return group !== undefined && group !== "" })))
    property int targetId: -1
    property int selectedSlot: -2
    property int selectedSidechain: -1
    property var descriptor: ({})
    property string message: ""
    property int reloadRevision: 0
    property bool loadingDraft: false
    readonly property bool available: descriptor.available === true
    readonly property string unavailableText: descriptor.reason || qsTr("This processor is not installed. It is an LV2 plugin installed separately from OmaAudio.")
    readonly property bool stale: service !== null && descriptor.stamp !== undefined && descriptor.stamp !== service.automationStamp
    readonly property var choices: (descriptor.slots || []).concat([{slot: -1, label: qsTr("Add a new effect")}])
    readonly property var sidechainChoices: [{track: -1, name: qsTr("Choose a trigger track")}].concat(descriptor.sidechainChoices || [])
    readonly property bool validFields: {
        if (!available || loadingDraft || fields.count === 0) return false
        if (descriptor.requiresSidechain && !sidechainChoices.some(function(choice) { return choice.track >= 0 && choice.track === root.selectedSidechain })) return false
        for (let i = 0; i < fields.count; ++i) {
            const field = (fields.itemAt(i) as ProcessorField)
            if (!field || !field.valid) return false
        }
        return true
    }
    // Explain the audible result of each simplified control, using its stable symbol.
    function fieldPurpose(symbol) {
        if (symbol === "threshold" || symbol === "al")
            return module === "gate" ? qsTr("Sound below this level is turned down. Set it below the quietest speech or music you want to keep.")
                                     : qsTr("Sound above this level is reduced. Lower the threshold to affect more of the recording.")
        if (symbol === "ratio" || symbol === "cr")
            return module === "gate" ? qsTr("How strongly to turn down sound below the threshold. Higher values make the gate stronger.")
                                     : qsTr("How strongly to reduce sound above the threshold. A higher ratio reduces loud passages more.")
        if (symbol === "attack" || symbol === "at")
            return module === "gate" ? qsTr("How quickly sound returns when it rises above the threshold. A shorter time opens the gate faster.")
                                     : qsTr("How quickly the effect starts reducing a loud sound. Shorter times catch sharper sounds.")
        if (symbol === "release" || symbol === "rt")
            return module === "gate" ? qsTr("How quickly the gate turns quiet sound down after it falls below the threshold. Longer times let endings ring out.")
                                     : qsTr("How quickly the level returns after the loud sound ends. Longer times make the return more gradual.")
        if (symbol === "hold") return qsTr("How long to keep reducing the level before it starts to return.")
        if (symbol === "makeup" || symbol === "mk") return qsTr("Adjust the level after the effect. Use this to match the loudness with and without processing.")
        if (symbol === "range") return qsTr("The lowest level the gate can reduce sound to, relative to its original level. More negative values make the closed gate quieter.")
        if (symbol === "mix" || symbol === "amount") return qsTr("How much processed sound to blend in. Lower values keep more of the original sound.")
        if (symbol === "laxity") return qsTr("Fine-tunes how the de-esser responds to harsh s sounds. Start with a preset and adjust while listening.")
        if (symbol === "f1_freq") return qsTr("The frequency where the de-esser separates higher tones from lower ones. Adjust around the harsh s sound.")
        if (symbol === "f2_freq") return qsTr("The frequency the de-esser pays most attention to when detecting harsh s sounds.")
        if (symbol === "freq") return qsTr("The lowest frequency where Brightness adds higher tones. A lower setting affects more of the sound.")
        if (symbol === "drive") return qsTr("How strongly to create extra higher tones. Increase gently to add brightness.")
        if (symbol === "blend") return qsTr("Adjust the character of the added higher tones. Listen as you change it.")
        if (symbol === "level_in") return qsTr("Make the sound louder or quieter before the effect processes it.")
        if (symbol === "level_out") return qsTr("Make the processed sound louder or quieter at the output.")
        if (symbol.endsWith("_active")) return qsTr("Choose which channels this EQ band affects, or turn the band off.")
        if (symbol.endsWith("_freq")) return qsTr("Choose the tone range to adjust. Lower frequencies affect bass; higher frequencies affect brighter tones.")
        if (symbol.endsWith("_level")) return qsTr("Boost or reduce this tone range. Positive dB values add level; negative values reduce it.")
        if (symbol.endsWith("_q")) return symbol.startsWith("p")
                ? qsTr("Choose how narrow the affected tone range is. Higher Q values make the band narrower.")
                : qsTr("Shape the transition around this shelf's frequency. Adjust while listening for a smooth change.")
        return qsTr("Adjust this effect setting while listening to the result.")
    }
    function reload(slot) {
        if (!service) return
        loadingDraft = true
        const revision = ++reloadRevision
        descriptor = service.essentialProcessor(module, targetId, slot === undefined ? selectedSlot : slot)
        selectedSlot = descriptor.slot === undefined ? (slot === undefined ? selectedSlot : slot) : descriptor.slot
        selectedSidechain = descriptor.sidechain === undefined ? -1 : descriptor.sidechain
        if (groups.indexOf(selectedGroup) < 0) selectedGroup = groups.length ? groups[0] : ""
        active.checked = descriptor.enabled !== false
        message = ""
        // An unchanged descriptor keeps the same delegates, whose draft text may
        // have broken its original binding. Reload always restores stored values.
        Qt.callLater(function() {
            if (root.reloadRevision !== revision) return
            for (let i = 0; i < fields.count; ++i) {
                const field = fields.itemAt(i) as ProcessorField
                if (field) field.valueText = Number(field.modelData.value).toFixed(field.modelData.decimals)
            }
            root.loadingDraft = false
        })
    }
    function stage(values, defaults) {
        for (let i = 0; i < fields.count; ++i) {
            const field = (fields.itemAt(i) as ProcessorField)
            if (field && !field.modelData.automated) {
                const value = defaults ? field.modelData.defaultValue : values[field.modelData.symbol]
                if (value !== undefined) field.valueText = Number(value).toFixed(field.modelData.decimals)
            }
        }
        message = qsTr("Preset ready. Press Apply to hear it. Settings controlled by automation stay unchanged.")
    }
    function apply() {
        let values = {}
        for (let i = 0; i < fields.count; ++i) {
            const field = (fields.itemAt(i) as ProcessorField)
            values[field.modelData.symbol] = field.valueText
        }
        const stamp = descriptor.stamp
        if (service.applyEssentialProcessor(module, targetId, selectedSlot, stamp, active.checked, values, selectedSidechain)) {
            reload(selectedSlot === -1 ? service.trackById(targetId).inserts.length - 1 : selectedSlot)
            message = stamp === service.automationStamp ? qsTr("Settings already match.") : qsTr("Settings applied. Undo restores the previous processor settings.")
        } else message = service.lastError
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(640, parent ? parent.width - 32 : 640)
    height: Math.min(620, parent ? parent.height - 40 : 620)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: {
        targetId = service ? service.focusedTrack : -1
        reload(-2)
        if (service && !service.pluginsListed) service.listPlugins()
    }
    onClosed: { if (shell) shell.restoreFocus() }
    Connections {
        target: root.service
        function onPluginsChanged() { if (root.visible && !root.available && !root.service.scanning) root.reload() }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    component Note: Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
        color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
    }
    component ProcessorField: RowLayout {
        id: fieldRow
        required property var modelData
        readonly property var choices: modelData.choices || []
        readonly property bool valid: editor.acceptableInput && (choices.length === 0 || choice.currentIndex >= 0)
        property alias valueText: editor.text
        Layout.fillWidth: true
        visible: root.groups.length === 0 || modelData.group === root.selectedGroup
        readonly property string rangeText: qsTr("Range %1 to %2%3.").arg(Number(fieldRow.modelData.minimum).toFixed(fieldRow.modelData.decimals))
                                            .arg(Number(fieldRow.modelData.maximum).toFixed(fieldRow.modelData.decimals))
                                            .arg(fieldRow.modelData.unit ? " " + fieldRow.modelData.unit : "")
        Text { text: fieldRow.modelData.label; Layout.preferredWidth: 160; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
        TextEntry {
            id: editor; objectName: "essentialProcessor_" + fieldRow.modelData.symbol
            visible: fieldRow.choices.length === 0
            Layout.fillWidth: true; selectByMouse: true; readOnly: fieldRow.modelData.automated
            text: Number(fieldRow.modelData.value).toFixed(fieldRow.modelData.decimals)
            validator: DoubleValidator { bottom: fieldRow.modelData.minimum; top: fieldRow.modelData.maximum; decimals: fieldRow.modelData.decimals; notation: DoubleValidator.StandardNotation; locale: "C" }
            onTextEdited: root.message = ""
            Accessible.name: editorHelp.title
            Accessible.description: editorHelp.description
            ControlHelp {
                id: editorHelp
                title: fieldRow.modelData.label
                purpose: fieldRow.modelData.automated
                         ? qsTr("This control is automated, so it is read-only here. Edit or disable its automation in the Track Effects panel.")
                         : root.fieldPurpose(fieldRow.modelData.symbol) + " " + qsTr("Press Apply to hear the change.")
                scope: fieldRow.rangeText
            }
        }
        ComboField {
            id: choice; objectName: "essentialProcessorChoice_" + fieldRow.modelData.symbol
            helpTitle: fieldRow.modelData.label
            helpText: root.fieldPurpose(fieldRow.modelData.symbol) + " " + qsTr("Press Apply to use this choice.")
            unavailableReason: qsTr("This control is automated. Edit or disable its automation in the Track Effects panel.")
            visible: fieldRow.choices.length > 0; Layout.fillWidth: true; enabled: !fieldRow.modelData.automated
            model: fieldRow.choices.map(function(entry) { return entry.label })
            currentIndex: {
                for (let i = 0; i < fieldRow.choices.length; ++i)
                    if (fieldRow.choices[i].value === Numeric.parse(editor.text, "C")) return i
                return -1
            }
            onActivated: { if (currentIndex < 0 || currentIndex >= fieldRow.choices.length) return; editor.text = String(fieldRow.choices[currentIndex].value); root.message = "" }
        }
        Text { text: fieldRow.modelData.automated ? qsTr("Automated") : fieldRow.modelData.unit; Layout.preferredWidth: 90; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
    }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Quick Fixes · %1").arg(root.descriptor.title || ""); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton { text: qsTr("Close"); iconName: "x"; helpText: qsTr("Close and discard changes you have not applied."); onClicked: root.close() }
        }
        ScrollPane {
            id: scroll; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
            ColumnLayout {
                x: scroll.reachLeft; width: scroll.columnWidth; spacing: Theme.s3; enabled: !root.loadingDraft
                Note { text: root.descriptor.trackName || qsTr("Click a track before opening this processor."); color: Theme.textPrimary }
                Note { text: root.available ? qsTr("Uses %1 from your installed effects. Changes take effect when you press Apply.").arg(root.descriptor.processor) : root.descriptor.reason || "" }
                Note { objectName: "essentialProcessorDescription"; visible: root.available && text !== ""; text: root.descriptor.description || "" }
                ComboField {
                    objectName: "essentialProcessorSlot"; Layout.fillWidth: true
                    visible: root.available || (root.descriptor.slots || []).length > 0
                    caption: qsTr("Processor"); model: root.choices.map(function(choice) { return choice.label })
                    helpText: qsTr("Choose an existing effect to adjust, or add another copy after the track's other effects.")
                    helpScope: qsTr("Choosing another effect discards changes you have not applied.")
                    unavailableReason: qsTr("Wait for the fields to load.")
                    currentIndex: { for (let i = 0; i < root.choices.length; ++i) if (root.choices[i].slot === root.selectedSlot) return i; return -1 }
                    onActivated: root.reload(root.choices[currentIndex].slot)
                }
                ComboField {
                    objectName: "essentialProcessorSidechain"; Layout.fillWidth: true
                    visible: root.available && root.descriptor.requiresSidechain === true
                    caption: qsTr("Trigger track"); model: root.sidechainChoices.map(function(choice) { return choice.name })
                    helpText: qsTr("The track whose signal triggers ducking. While it plays, this track is lowered.")
                    helpScope: qsTr("For example, use a voice track to turn the music down while someone speaks. Press Apply to use your choice.")
                    unavailableReason: qsTr("Wait for the fields to load.")
                    currentIndex: root.sidechainChoices.findIndex(function(choice) { return choice.track === root.selectedSidechain })
                    onActivated: {
                        if (currentIndex < 0 || currentIndex >= root.sidechainChoices.length) return
                        root.selectedSidechain = root.sidechainChoices[currentIndex].track
                        root.message = qsTr("Press Apply to use the chosen trigger track.")
                    }
                }
                Note {
                    objectName: "essentialProcessorSharedKey"; color: Theme.warning
                    visible: root.available && root.descriptor.otherSidechainInserts > 0
                    text: qsTr("Other effects on this track also use a sidechain. The trigger chosen here will control those effects too.")
                }
                RowLayout {
                    visible: root.available; Layout.fillWidth: true
                    Toggle { id: active; objectName: "essentialProcessorEnabled"; label: qsTr("Processor enabled")
                        helpText: qsTr("Turn the effect on or off without removing it. Press Apply to hear the change.")
                        helpScope: qsTr("Bypass and Apply to compare with and without.")
                        unavailableReason: qsTr("Wait for the fields to load.")
                        onClicked: root.message = qsTr("Press Apply to turn the effect on or off.") }
                    Item { Layout.fillWidth: true }
                    ToolButton { objectName: "essentialProcessorDefaults"; text: qsTr("Factory defaults"); showLabel: true
                        helpText: qsTr("Load the effect's original settings. Settings controlled by automation stay unchanged.")
                        helpScope: qsTr("Nothing changes until you press Apply.")
                        unavailableReason: qsTr("Wait for the fields to load.")
                        onClicked: root.stage({}, true) }
                }
                RowLayout {
                    visible: root.available; spacing: Theme.s2
                    Text { text: qsTr("Starting values"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
                    Repeater {
                        model: root.descriptor.presets || []
                        ToolButton {
                            required property var modelData
                            required property int index
                            objectName: "essentialProcessorPreset" + index
                            text: modelData.name; showLabel: true
                            helpText: qsTr("Try the %1 preset. Listen after pressing Apply, then adjust it to suit your audio.").arg(modelData.name)
                            helpScope: qsTr("Automated controls are left alone. Nothing changes until you press Apply.")
                            unavailableReason: qsTr("Wait for the fields to load.")
                            onClicked: root.stage(modelData.values, false)
                        }
                    }
                }
                ComboField {
                    objectName: "essentialProcessorGroup"; Layout.fillWidth: true; visible: root.groups.length > 1
                    caption: qsTr("Band"); model: root.groups; currentIndex: root.groups.indexOf(root.selectedGroup)
                    helpText: qsTr("Choose a frequency band to adjust. Your changes in the other bands are kept until you apply or close.")
                    unavailableReason: qsTr("Wait for the fields to load.")
                    onActivated: root.selectedGroup = root.groups[currentIndex]
                }
                Repeater {
                    id: fields; model: root.descriptor.fields || []
                    ProcessorField {}
                }
                Note { visible: root.available; text: qsTr("These are starting values, not automatic analysis. Adjust them for the recording; bypass and Apply to compare. Other controls remain available in the Track Effects panel.") }
                Note { visible: root.descriptor.bypassAutomated === true; color: Theme.warning; text: qsTr("Internal enable/bypass has automation. Use the rack’s Automation editor before enabling this processor here.") }
                Note { objectName: "essentialProcessorSetupAutomated"; visible: root.descriptor.configurationAutomated === true; color: Theme.warning; text: qsTr("Ducking setup controls have automation. Edit or disable it in the Track Effects panel before enabling Ducking.") }
                Note { objectName: "essentialProcessorStale"; visible: root.stale; color: Theme.warning; text: qsTr("The track or effect changed. Reload before applying these settings.") }
            }
        }
        Note { objectName: "essentialProcessorMessage"; text: root.message; visible: text !== "" }
        RowLayout {
            ToolButton {
                objectName: "essentialProcessorReload"; text: root.service && root.service.scanning ? qsTr("Checking effects…") : qsTr("Reload"); showLabel: true
                helpTitle: qsTr("Reload")
                helpText: qsTr("Discard your unapplied changes and reload the effect's current settings. Also checks that the effect is still installed.")
                unavailableReason: qsTr("Wait for the effect scan to finish.")
                enabled: root.service && !root.service.scanning
                onClicked: { root.reload(); if (!root.service.pluginsListed) root.service.listPlugins() }
            }
            ToolButton {
                objectName: "essentialProcessorAdvanced"; text: qsTr("Rack controls…"); showLabel: true
                helpText: qsTr("Close this panel and open the same insert in the Track Effects panel, where every control of the plugin is available.")
                unavailableReason: !root.available ? root.unavailableText
                                 : root.selectedSlot < 0 ? qsTr("Add and apply the processor first.")
                                 : qsTr("The track or effect changed. Reload first.")
                enabled: root.available && root.selectedSlot >= 0 && !root.stale
                onClicked: { root.close(); root.shell.openTrackEffect(root.targetId, root.selectedSlot) }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "essentialProcessorApply"; text: root.selectedSlot === -1 ? qsTr("Add and apply") : qsTr("Apply"); showLabel: true
                helpText: root.selectedSlot === -1 ? qsTr("Add this effect after the track's other effects, using the settings shown.")
                                                   : qsTr("Use the settings shown for this effect.")
                helpScope: qsTr("One Undo step. The processor then runs live on playback and in exports.")
                unavailableReason: !root.available ? root.unavailableText
                                 : root.stale ? qsTr("The track or effect changed. Reload first.")
                                 : root.service && root.service.scanning ? qsTr("Wait for the effect scan to finish.")
                                 : root.descriptor.requiresSidechain && root.selectedSidechain < 0 ? qsTr("Choose a trigger track first.")
                                 : qsTr("Enter a value within range in every field first.")
                enabled: root.validFields && !root.stale && root.service && !root.service.scanning
                onClicked: root.apply()
            }
        }
    }
}
