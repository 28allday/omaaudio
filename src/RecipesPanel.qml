pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The Recipes tab: saved lists of steps that run in order, in one click, and
// are written into the audio (Backend recipes, backend_recipes.cpp). Three
// faces in one place: the list (built in, then your own), the editor for one
// recipe, and the card for a run, which stays until it is dismissed.
//
// A run is one Undo step. A step that cannot run is reported and the others
// carry on; Stop puts the audio back as it was.
Item {
    id: root
    objectName: "recipesPanel"

    property var shell: null
    readonly property var service: shell ? shell.service : null

    // Where a Run goes: the selection, the whole focused track, or many files
    // (Process Many Files opens with the recipe chosen).
    property string target: "selection"
    property string message: ""
    property bool editing: false

    readonly property var recipes: service ? service.recipes : []
    readonly property var builtIns: recipes.filter(r => r.builtIn === true)
    readonly property var own: recipes.filter(r => r.builtIn !== true)
    readonly property var run: service ? service.recipeRun : ({})
    readonly property bool showingRun: run.active === true

    readonly property var targetInfo: {
        if (!service)
            return { valid: false, text: "", reason: "" }
        if (target === "files")
            return { valid: true, text: qsTr("Process Many Files opens with the recipe chosen."), reason: "" }
        // Read again whenever what it describes can change.
        void service.selectionStart; void service.selectionEnd; void service.selectionEmpty
        void service.focusedTrack; void service.tracksRevision; void service.historyCursor
        return service.recipeTarget(target)
    }

    // Opening the tab with nothing selected starts on the track.
    function chooseStartingTarget() {
        if (!service || target === "files") return
        if (target === "selection" && !service.recipeTarget("selection").valid && service.recipeTarget("track").valid)
            target = "track"
    }
    onVisibleChanged: if (visible) chooseStartingTarget()
    Component.onCompleted: if (visible) chooseStartingTarget()

    // "7 steps · Low cut, Mains hum, Voice Cleanup…", made by the backend.
    function stepsLine(recipe) {
        if (recipe.summary) return recipe.summary
        const steps = recipe.steps || []
        return steps.length === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(steps.length)
    }

    function runRecipe(id) {
        if (!service) return
        message = ""
        if (target === "files") {
            if (shell) shell.processFilesWithRecipe(id)
            return
        }
        if (!service.runRecipe(id, target))
            message = service.lastError
    }
    function newRecipe() {
        message = ""
        editor.begin("", qsTr("New recipe"), [], false)
        editing = true
    }
    function edit(id) {
        if (!service) return
        message = ""
        const recipe = service.recipe(id)
        if (!recipe || recipe.id === undefined) { message = service.lastError; return }
        const builtIn = recipe.builtIn === true
        // A built-in recipe opens as a copy to change; the original stays as it was.
        editor.begin(builtIn ? "" : recipe.id,
                     builtIn ? qsTr("%1 (my copy)").arg(recipe.name) : recipe.name,
                     recipe.steps || [], builtIn)
        editing = true
    }
    function duplicate(id) {
        if (!service) return
        message = service.duplicateRecipe(id) !== "" ? "" : service.lastError
    }
    function rename(id, name) {
        if (!service) return false
        const ok = service.renameRecipe(id, String(name).trim())
        message = ok ? "" : service.lastError
        return ok
    }
    function remove(id) {
        if (!service) return
        message = service.removeRecipe(id) ? "" : service.lastError
    }

    // ── the list ──────────────────────────────────────────────────────────
    Flickable {
        id: flick
        objectName: "recipesList"
        anchors.fill: parent
        visible: !root.editing && !root.showingRun
        contentHeight: body.implicitHeight + Theme.s3 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: body
            x: Theme.s3; y: Theme.s3
            width: flick.width - Theme.s3 * 2
            spacing: Theme.s3

            Item {
                width: parent.width
                height: Theme.hControl
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Recipes")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading
                    color: Theme.textPrimary
                }
                ToolButton {
                    objectName: "recipeNew"
                    anchors.right: parent.right
                    text: qsTr("New recipe"); iconName: "plus"; showLabel: true
                    helpText: qsTr("Make a recipe of your own: name it, then add the steps it runs.")
                    helpScope: qsTr("Kept on this computer for every project.")
                    onClicked: root.newRecipe()
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("A recipe is a list of steps that runs in order, in one click.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }

            // Where Run goes, and what that covers now.
            Column {
                width: parent.width
                spacing: Theme.s1
                Row {
                    spacing: Theme.s2
                    Text {
                        width: 52; height: Theme.hControl
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Run on")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                    }
                    SegmentedControl {
                        objectName: "recipeTarget"
                        labels: [qsTr("Selection"), qsTr("This track"), qsTr("Files…")]
                        currentIndex: ["selection", "track", "files"].indexOf(root.target)
                        helpTitle: qsTr("Run on")
                        helpText: qsTr("Where Run writes the recipe: the selected audio, the whole active track, or many files through Process Many Files.")
                        helpScope: qsTr("A run on the selection or the track is one Undo step. Files are written as new files; the project does not change.")
                        onActivated: function (i) { root.target = ["selection", "track", "files"][i]; root.message = "" }
                    }
                }
                Text {
                    objectName: "recipeTargetText"
                    x: 52 + Theme.s2
                    width: parent.width - x
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: root.targetInfo.valid ? (root.targetInfo.text || "") : (root.targetInfo.reason || "")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    color: root.targetInfo.valid ? Theme.textMuted : Theme.warning
                }
            }

            Text {
                objectName: "recipesMessage"
                visible: root.message !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: root.message
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.warning
            }

            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            RecipeListHeading { width: parent.width; title: qsTr("Built in"); count: root.builtIns.length }
            Column {
                width: parent.width
                spacing: Theme.s1
                Repeater {
                    model: root.builtIns
                    RecipeRow {
                        required property var modelData
                        panel: root
                        recipe: modelData
                    }
                }
            }

            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            RecipeListHeading { width: parent.width; title: qsTr("Your recipes"); count: root.own.length }
            Text {
                visible: root.own.length === 0
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("None yet. Make one with New recipe, or choose Edit… on a built-in recipe to change a copy of it.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }
            Column {
                width: parent.width
                spacing: Theme.s1
                Repeater {
                    model: root.own
                    RecipeRow {
                        required property var modelData
                        panel: root
                        recipe: modelData
                    }
                }
            }

            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Each run is one Undo step. Recipes are kept on this computer and work in every project. A built-in recipe opens as a copy to change.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }
            RecipeLink {
                objectName: "recipeProcessFiles"
                text: qsTr("Process many files with a recipe")
                helpText: qsTr("Open Process Many Files with Recipe chosen as its effects, to run a recipe on many files at once.")
                helpScope: qsTr("Each file is written as a new file. The project and its Undo history do not change.")
                onClicked: if (root.shell) root.shell.processFilesWithRecipe("")
            }
        }
    }

    // ── the editor ────────────────────────────────────────────────────────
    RecipeEditor {
        id: editor
        anchors.fill: parent
        visible: root.editing && !root.showingRun
        panel: root
        onFinished: root.editing = false
    }

    // ── a run ─────────────────────────────────────────────────────────────
    RecipeRunCard {
        anchors.fill: parent
        visible: root.showingRun
        panel: root
    }
}
