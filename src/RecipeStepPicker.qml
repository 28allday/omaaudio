pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// + Add step… in the recipe editor: every kind of step, by group, with a
// field to find one by name, and Installed effects for any effect from the
// effect browser. A kind this computer cannot run is shown, greyed, with why.
Rectangle {
    id: root
    property var editor: null
    readonly property var kinds: editor ? editor.kinds : []
    readonly property string needle: find.text.trim().toLowerCase()
    readonly property var shown: kinds.filter(k => needle === "" || k.title.toLowerCase().indexOf(needle) >= 0
                                                  || (k.groupTitle || "").toLowerCase().indexOf(needle) >= 0)
    readonly property var groups: {
        const out = []
        for (const k of shown) {
            let g = out.find(x => x.key === k.group)
            if (!g) { g = { key: k.group, title: k.groupTitle, kinds: [] }; out.push(g) }
            g.kinds.push(k)
        }
        return out
    }
    readonly property bool effectsShown: needle === "" || qsTr("Installed effects").toLowerCase().indexOf(needle) >= 0
                                         || qsTr("effect").indexOf(needle) >= 0

    objectName: "recipeStepPicker"
    implicitHeight: col.implicitHeight + Theme.s2 * 2
    radius: Theme.rControl
    color: Theme.panelRaised
    border.width: Theme.hairline
    border.color: Theme.border
    onVisibleChanged: if (visible) find.text = ""

    Column {
        id: col
        x: Theme.s2; y: Theme.s2
        width: root.width - Theme.s2 * 2
        spacing: Theme.s2

        SearchField {
            id: find
            objectName: "recipeStepFind"
            width: parent.width
            placeholder: qsTr("Find a step")
            helpTitle: qsTr("Find a step")
            helpText: qsTr("Type part of a step's name to narrow the list.")
        }

        Repeater {
            model: root.groups
            Column {
                id: group
                required property var modelData
                width: col.width
                spacing: 2
                Text {
                    text: group.modelData.title
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading
                    color: Theme.textPrimary
                }
                Flow {
                    width: parent.width
                    spacing: 0
                    Repeater {
                        model: group.modelData.kinds
                        RecipeLink {
                            id: choice
                            required property var modelData
                            required property int index
                            objectName: "recipeAddKind_" + modelData.kind
                            chevron: false
                            quiet: true
                            text: modelData.title
                            trailing: index < group.modelData.kinds.length - 1 ? " · " : ""
                            enabled: modelData.available === true
                            helpText: qsTr("Add %1 as the last step, at its usual settings. Change… adjusts them.").arg(modelData.title)
                            helpScope: modelData.available ? "" : modelData.reason
                            onClicked: root.editor.addKind(modelData.kind)
                        }
                    }
                }
            }
        }

        Column {
            visible: root.effectsShown
            width: parent.width
            spacing: 2
            Text {
                text: qsTr("Installed effects")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading
                color: Theme.textPrimary
            }
            RecipeLink {
                objectName: "recipeAddEffect"
                quiet: true
                text: qsTr("Any effect from Add Effect…, with its settings")
                helpText: qsTr("Open the effect browser and choose an installed effect to add as the last step.")
                helpScope: qsTr("The step runs the effect at its default settings. Record steps keeps the settings you apply.")
                onClicked: if (root.editor && root.editor.shell)
                               root.editor.shell.pickEffect(function (uri) { root.editor.addEffect(uri) })
            }
        }

        Text {
            visible: root.groups.length === 0 && !root.effectsShown
            text: qsTr("No step matches.")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
        }
    }
}
