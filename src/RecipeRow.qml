pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// One recipe in the Recipes tab: its name, how many steps and the first few,
// Run, and a ⋯ menu (Edit…, Duplicate, Rename…, Delete…). Rename and Delete
// are answered in the row itself, so nothing covers the list. A built-in
// recipe cannot be renamed or deleted; Edit… changes a copy of it.
Rectangle {
    id: row
    property var panel: null
    property var recipe: ({})
    property bool renaming: false
    property bool confirming: false
    readonly property bool builtIn: recipe.builtIn === true
    readonly property var service: panel ? panel.service : null

    objectName: "recipeRow_" + (recipe.id || "")
    width: parent ? parent.width : 0
    implicitHeight: body.implicitHeight + Theme.s2 * 2
    radius: Theme.rControl
    color: hover.hovered || row.renaming || row.confirming ? Theme.hoverOverlay : "transparent"
    border.width: Theme.hairline
    border.color: hover.hovered || row.renaming || row.confirming ? Theme.borderStrong : "transparent"
    HoverHandler { id: hover }

    Column {
        id: body
        x: Theme.s2; y: Theme.s2
        width: row.width - Theme.s2 * 2
        spacing: Theme.s2

        Item {
            width: parent.width
            height: Math.max(words.implicitHeight, Theme.hControl)
            Column {
                id: words
                anchors.left: parent.left
                anchors.right: actions.left
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Text {
                    width: parent.width
                    text: row.recipe.name || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.weight: Font.Medium
                    color: Theme.textPrimary
                }
                Text {
                    objectName: "recipeRowSteps"
                    width: parent.width
                    text: row.panel ? row.panel.stepsLine(row.recipe) : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                }
            }
            Row {
                id: actions
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s1
                ToolButton {
                    objectName: "recipeRun"
                    text: row.panel && row.panel.target === "files" ? qsTr("Run…") : qsTr("Run")
                    showLabel: true
                    prominent: hover.hovered
                    enabled: row.service !== null && !row.service.recipeRunning
                             && row.panel !== null && row.panel.targetInfo.valid === true
                    helpText: row.panel && row.panel.target === "files"
                              ? qsTr("Open Process Many Files with this recipe chosen.")
                              : qsTr("Run this recipe's steps in order, written into the audio.")
                    helpScope: qsTr("One Undo step. A step that cannot run is shown with the reason; the others still run.")
                    unavailableReason: row.service && row.service.recipeRunning ? qsTr("Another recipe is running.")
                                       : row.panel ? (row.panel.targetInfo.reason || "") : ""
                    onClicked: row.panel.runRecipe(row.recipe.id)
                }
                IconButton {
                    id: more
                    objectName: "recipeMore"
                    iconName: "ellipsis"
                    text: qsTr("More for %1").arg(Theme.shortName(row.recipe.name, 40))
                    helpTitle: qsTr("Recipe actions")
                    helpText: row.builtIn ? qsTr("Edit a copy of this built-in recipe, or duplicate it.")
                                          : qsTr("Edit, duplicate, rename or delete this recipe.")
                    onClicked: menu.popup(more, 0, more.height)
                }
            }
        }

        // Rename…, in the row.
        Row {
            visible: row.renaming
            width: parent.width
            spacing: Theme.s1
            TextEntry {
                id: nameField
                objectName: "recipeRenameField"
                width: parent.width - keepName.width - cancelName.width - Theme.s1 * 2
                helpTitle: qsTr("Recipe name")
                helpText: qsTr("A new name for this recipe. Return keeps it, Escape leaves it as it was.")
                onAccepted: keepName.clicked()
                Keys.onEscapePressed: row.renaming = false
            }
            ToolButton {
                id: keepName
                objectName: "recipeRenameKeep"
                text: qsTr("Rename"); showLabel: true; prominent: true
                enabled: nameField.text.trim() !== ""
                helpText: qsTr("Keep the new name.")
                unavailableReason: qsTr("Type a name first.")
                onClicked: if (row.panel.rename(row.recipe.id, nameField.text)) row.renaming = false
            }
            ToolButton {
                id: cancelName
                text: qsTr("Cancel"); showLabel: true
                helpText: qsTr("Keep the name as it was.")
                onClicked: row.renaming = false
            }
        }
        Timer {
            id: focusName
            interval: 30
            onTriggered: { nameField.forceActiveFocus(); nameField.selectAll() }
        }

        // Delete…, asked in the row.
        Column {
            visible: row.confirming
            width: parent.width
            spacing: Theme.s1
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: qsTr("Delete “%1”? It cannot be brought back. Audio it has already changed stays as it is.").arg(Theme.shortName(row.recipe.name, 48))
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
            }
            Row {
                spacing: Theme.s1
                ToolButton {
                    objectName: "recipeDeleteConfirm"
                    text: qsTr("Delete"); showLabel: true; prominent: true
                    helpText: qsTr("Delete this recipe from this computer.")
                    onClicked: { row.confirming = false; row.panel.remove(row.recipe.id) }
                }
                ToolButton {
                    text: qsTr("Cancel"); showLabel: true
                    helpText: qsTr("Keep the recipe.")
                    onClicked: row.confirming = false
                }
            }
        }
    }

    AppMenu {
        id: menu
        objectName: "recipeMenu"
        shell: row.panel ? row.panel.shell : null
        AppMenuItem {
            objectName: "recipeEdit"
            text: qsTr("Edit…")
            helpText: row.builtIn ? qsTr("Change a copy of this built-in recipe. The built-in recipe stays as it was.")
                                  : qsTr("Change this recipe's name and steps.")
            onTriggered: row.panel.edit(row.recipe.id)
        }
        AppMenuItem {
            objectName: "recipeDuplicate"
            text: qsTr("Duplicate")
            helpText: qsTr("Make a copy of this recipe in Your recipes.")
            onTriggered: row.panel.duplicate(row.recipe.id)
        }
        AppMenuItem {
            objectName: "recipeRename"
            visible: !row.builtIn
            height: visible ? implicitHeight : 0
            text: qsTr("Rename…")
            helpText: qsTr("Give this recipe a new name.")
            onTriggered: {
                row.confirming = false
                nameField.text = row.recipe.name || ""
                row.renaming = true
                focusName.start()
            }
        }
        AppMenuItem {
            objectName: "recipeDelete"
            visible: !row.builtIn
            height: visible ? implicitHeight : 0
            text: qsTr("Delete…")
            helpText: qsTr("Delete this recipe from this computer. You are asked first.")
            onTriggered: { row.renaming = false; row.confirming = true }
        }
    }
}
