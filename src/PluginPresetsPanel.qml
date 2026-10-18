pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

C.Popup {
    id: root
    objectName: "pluginPresetsPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int targetTrack: -1
    property int targetSlot: -1
    property var context: ({})
    property string message: ""
    property bool savingComplete: false
    property bool deletePending: false
    readonly property var presets: context.presets || []
    readonly property var selectedPreset: picker.currentIndex > 0 && picker.currentIndex <= presets.length ? presets[picker.currentIndex - 1] : null
    readonly property string unavailableText: context.reason || qsTr("This effect is not available on this system.")
    function reload(preferredId) {
        context = service ? service.pluginPresetContext(targetTrack, targetSlot) : {}
        let index = 0
        for (let i = 0; i < presets.length; ++i) if (presets[i].id === preferredId) index = i + 1
        picker.currentIndex = index
        deletePending = false
    }
    Connections {
        target: root.service
        function onPluginPresetSaved(id, error) {
            if (!root.savingComplete) return
            root.savingComplete = false
            if (id !== "") { root.reload(id); nameEntry.text = ""; root.message = qsTr("Complete preset saved locally, with the effect's own settings.") }
            else root.message = error
        }
    }
    function showFor(track, slot) {
        targetTrack = track; targetSlot = slot; nameEntry.text = ""; message = ""
        reload(""); open()
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(610, parent ? parent.width - 32 : 610)
    height: Math.min(470, parent ? parent.height - 32 : 470)
    modal: true; focus: true; closePolicy: C.Popup.CloseOnEscape
    onClosed: if (shell) shell.restoreFocus()
    padding: Theme.s4
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Layout.fillWidth: true
            Text { text: qsTr("Parameter presets"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton { objectName: "pluginPresetClose"; iconName: "x"; text: qsTr("Close"); helpText: qsTr("Close this panel. The effect keeps its current settings."); onClicked: root.close() }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
            text: root.context.name || qsTr("Effect unavailable")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading; color: Theme.textPrimary
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Save base control values for this plugin and reuse them in other projects. Presets exclude automation, host bypass, routing, plugin files and other plugin state. Applying is one Undo step.")
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
            text: root.context.reason || ""; visible: text !== ""; color: Theme.danger
        }
        RowLayout {
            Layout.fillWidth: true
            TextEntry {
                id: nameEntry; objectName: "pluginPresetName"; Layout.fillWidth: true
                placeholderText: qsTr("Name for the current settings"); maximumLength: 96
                Accessible.name: qsTr("New parameter preset name"); enabled: !!root.context.available
                Accessible.description: nameHelp.description
                ControlHelp {
                    id: nameHelp
                    title: qsTr("Preset name")
                    purpose: qsTr("A name for a new preset made from the effect's current control values.")
                    scope: qsTr("Up to 96 characters. Saved on this computer for use in any project.")
                    unavailableReason: root.unavailableText
                }
            }
            ToolButton {
                objectName: "pluginPresetSave"; text: qsTr("Save new"); showLabel: true
                helpText: qsTr("Store the effect's current control values under the name typed here, for reuse in any project.")
                helpScope: qsTr("Saved on this computer. Automation, bypass, routing and other plugin state are not included.")
                unavailableReason: !root.context.available ? root.unavailableText : qsTr("Type a name first.")
                enabled: !!root.context.available && nameEntry.text.trim() !== ""
                onClicked: {
                    const id = root.service.savePluginPreset(root.targetTrack, root.targetSlot, root.context.stamp, nameEntry.text)
                    if (id !== "") {
                        root.reload(id); nameEntry.text = ""; root.message = qsTr("Parameter preset saved locally.")
                    } else root.message = root.service.lastError
                }
            }
            ToolButton {
                objectName: "pluginPresetSaveComplete"; text: qsTr("Save complete"); showLabel: true
                visible: !!root.context.completeSettings
                helpText: qsTr("Store everything this effect remembers — its controls and its own internal settings, such as loaded files or modes — under the name typed here.")
                helpScope: qsTr("Saved on this computer for any project. Automation, bypass and routing are not included.")
                unavailableReason: !root.context.available ? root.unavailableText : qsTr("Type a name first.")
                enabled: !!root.context.available && nameEntry.text.trim() !== "" && !root.savingComplete
                onClicked: {
                    if (root.service.savePluginPresetWithState(root.targetTrack, root.targetSlot, root.context.stamp, nameEntry.text)) {
                        root.savingComplete = true; root.message = qsTr("Reading the effect's settings…")
                    } else root.message = root.service.lastError
                }
            }
        }
        ComboField {
            id: picker; objectName: "pluginPresetChoice"; Layout.fillWidth: true
            caption: qsTr("Preset"); model: [qsTr("Choose a saved preset…")].concat(root.presets.map(function(p) { return p.name }))
            helpText: qsTr("Choose a saved preset to apply or delete. Choosing one changes nothing until you press Apply.")
            unavailableReason: !root.context.available ? root.unavailableText : qsTr("No presets are saved for this effect yet.")
            enabled: !!root.context.available && root.presets.length > 0
            onActivated: { root.message = ""; root.deletePending = false }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary
            text: root.selectedPreset ? (root.selectedPreset.complete
                    ? qsTr("Complete preset: %1 parameters and the effect's own settings. Applying it reloads the effect.").arg(root.selectedPreset.count)
                    : qsTr("%1 saved parameters. Automated controls must already match or have automation disabled.").arg(root.selectedPreset.count))
                                     : qsTr("Selecting a preset leaves the current settings as they are.")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        }
        Item { Layout.fillHeight: true }
        Text {
            objectName: "pluginPresetMessage"; Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
            text: root.message; visible: text !== ""; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; visible: root.deletePending
            text: qsTr("Delete “%1” from the saved presets? The current insert keeps its settings.").arg(root.selectedPreset ? Theme.shortName(root.selectedPreset.name, 48) : "")
            color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        }
        RowLayout {
            Layout.fillWidth: true
            ToolButton {
                objectName: "pluginPresetReload"; text: qsTr("Reload"); showLabel: true
                helpText: qsTr("Re-read the saved presets and the effect's current values.")
                onClicked: { root.reload(root.selectedPreset ? root.selectedPreset.id : ""); root.message = "" }
            }
            ToolButton {
                objectName: "pluginPresetDelete"; text: qsTr("Delete"); showLabel: true
                helpText: qsTr("Delete the chosen preset from this computer. The effect keeps its current settings. You are asked to confirm.")
                unavailableReason: qsTr("Choose a saved preset first.")
                enabled: root.selectedPreset !== null; visible: !root.deletePending
                onClicked: { root.deletePending = true; root.message = "" }
            }
            ToolButton {
                objectName: "pluginPresetCancelDelete"; text: qsTr("Keep"); showLabel: true; visible: root.deletePending
                helpText: qsTr("Keep the preset. Nothing is deleted.")
                onClicked: root.deletePending = false
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "pluginPresetConfirmDelete"; text: qsTr("Delete preset"); showLabel: true; visible: root.deletePending
                helpText: qsTr("Delete the preset now. This cannot be undone.")
                unavailableReason: qsTr("Choose a saved preset first.")
                enabled: root.selectedPreset !== null
                onClicked: {
                    if (root.service.deletePluginPreset(root.selectedPreset.id)) {
                        root.reload(""); root.message = qsTr("Saved preset deleted.")
                    } else root.message = root.service.lastError
                }
            }
            ToolButton {
                objectName: "pluginPresetApply"; text: qsTr("Apply"); showLabel: true; visible: !root.deletePending
                helpText: qsTr("Set the effect's controls to the preset's values, as one Undo step.")
                helpScope: qsTr("Automated controls must already match the preset or have their automation disabled.")
                unavailableReason: !root.context.available ? root.unavailableText : qsTr("Choose a saved preset first.")
                enabled: !!root.context.available && root.selectedPreset !== null
                onClicked: {
                    const id = root.selectedPreset.id
                    const stamp = root.context.stamp
                    if (root.service.applyPluginPreset(root.targetTrack, root.targetSlot, stamp, id)) {
                        root.message = stamp === root.service.automationStamp ? qsTr("Values already match.") : qsTr("Preset applied. Undo restores the previous values.")
                        root.reload(id)
                    } else root.message = root.service.lastError
                }
            }
        }
    }
}
