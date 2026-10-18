import QtQuick
import "ui"

// The Preset menu of an effect's panels: Default, then OmaAudio's starting points,
// the effect's own presets and the person's own, each under a heading. `context`
// is Backend.pluginPresetContext / clipEffectPresetContext; the panel applies what
// is chosen (one Undo step) and puts every control back for Default.
ComboField {
    id: root

    property var context: ({})
    signal defaultChosen()
    signal presetChosen(string id)

    textRole: "text"
    helpTitle: qsTr("Preset")
    helpText: qsTr("Default puts every control back to the effect's default. Starting points are settings for common jobs; the effect's own presets and the ones you saved are listed below them.")
    helpScope: qsTr("Applying a preset is one Undo step.")
    enabled: context.available === true

    readonly property var entries: {
        const out = [{ text: qsTr("Default"), id: "" }]
        const section = function (title, list) {
            if (!list || list.length === 0) return
            out.push({ text: title, header: true })
            for (const p of list) out.push({ text: p.name, id: p.id })
        }
        section(qsTr("Starting points"), context.builtIn)
        section(qsTr("From the effect"), context.pluginPresets)
        section(qsTr("Your presets"), context.presets)
        return out
    }
    model: entries
    currentIndex: 0

    property int lastIndex: 0
    onActivated: function (index) {
        const entry = root.entries[index]
        if (!entry || entry.header) { root.currentIndex = root.lastIndex; return }
        root.lastIndex = index
        if (entry.id === "") root.defaultChosen()
        else root.presetChosen(entry.id)
    }
}
