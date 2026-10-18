pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// Changing a recipe, in the Recipes tab: its name and its steps, top to
// bottom. A step can be turned off, moved by its dots, changed in place
// (Change… opens its settings under it) or removed. Steps come from the
// picker (+ Add step…) or from Record steps, which turns the effects you apply
// while it is on into steps. Nothing is kept until Save.
//
// A step is a map {kind, on, ...settings} (Backend::recipeStepKinds); the
// editor copies the recipe's steps and hands the new list to saveRecipe.
Item {
    id: root
    objectName: "recipeEditor"

    property var panel: null
    readonly property var service: panel ? panel.service : null
    readonly property var shell: panel ? panel.shell : null

    property string recipeId: ""        // "" makes a new recipe on Save
    property bool fromBuiltIn: false    // a copy of a built-in recipe
    property var steps: []
    property int expanded: -1           // whose settings are open
    property bool picking: false
    property bool awaitingRecorded: false
    property string message: ""
    property var kinds: []
    // Moving a step by its dots: where it came from and the gap it would go to.
    property int dragFrom: -1
    property int dragSlot: -1

    signal finished()

    function begin(id, name, recipeSteps, builtIn) {
        recipeId = id
        fromBuiltIn = builtIn
        nameField.text = name
        steps = JSON.parse(JSON.stringify(recipeSteps || []))
        expanded = -1
        picking = false
        message = ""
        dragFrom = -1; dragSlot = -1
        reloadKinds()
        if (service && !service.pluginsListed && !service.scanning) service.listPlugins()
        flick.contentY = 0
    }
    function reloadKinds() { kinds = service ? service.recipeStepKinds() : [] }
    Connections {
        target: root.service
        function onPluginsChanged() { if (root.visible) root.reloadKinds() }
        // Stop and keep, from the strip along the bottom of the window: the
        // recorded steps join this recipe.
        function onRecordedStepsChanged() {
            if (!root.awaitingRecorded || root.service.recordingSteps) return
            root.awaitingRecorded = false
            const recorded = root.service.recordedSteps || []
            if (recorded.length) root.steps = root.steps.concat(JSON.parse(JSON.stringify(recorded)))
        }
    }

    function kindFor(kind) {
        for (const k of kinds) if (k.kind === kind) return k
        return null
    }
    function newStep(kind) {
        const k = kindFor(kind)
        const step = { kind: kind, on: true }
        if (k) for (const s of k.settings || []) step[s.key] = s["default"]
        return step
    }
    function addStep(step) {
        if (!step || step.kind === undefined) return
        steps = steps.concat([step])
        expanded = -1
        picking = false
        message = ""
        Qt.callLater(() => flick.contentY = Math.max(0, flick.contentHeight - flick.height))
    }
    function addKind(kind) { addStep(newStep(kind)) }
    function addEffect(uri) {
        if (!service) return
        const step = service.effectStep(uri)
        if (!step || step.kind === undefined) { message = service.lastError; return }
        addStep(step)
    }
    function setStep(index, key, value) {
        if (index < 0 || index >= steps.length) return
        const next = steps.slice()
        next[index] = Object.assign({}, next[index])
        next[index][key] = value
        steps = next
    }
    function moveStep(from, to) {
        if (from < 0 || from >= steps.length) return
        to = Math.max(0, Math.min(steps.length - 1, to))
        if (from === to) return
        const next = steps.slice()
        const moved = next.splice(from, 1)[0]
        next.splice(to, 0, moved)
        if (expanded === from) expanded = to
        else if (expanded >= 0) {
            if (from < expanded && to >= expanded) expanded--
            else if (from > expanded && to <= expanded) expanded++
        }
        steps = next
    }
    function removeStep(index) {
        if (index < 0 || index >= steps.length) return
        const next = steps.slice()
        next.splice(index, 1)
        if (expanded === index) expanded = -1
        else if (expanded > index) expanded--
        steps = next
    }

    // The gap a drag at `y` (in the steps column) points to: 0 is above the
    // first step, steps.length below the last.
    function slotAt(y) {
        let slot = 0
        for (let i = 0; i < stepRepeater.count; ++i) {
            const it = stepRepeater.itemAt(i)
            if (it && y > it.y + it.height / 2) slot = i + 1
        }
        return slot
    }
    function finishDrag() {
        if (dragFrom >= 0 && dragSlot >= 0)
            moveStep(dragFrom, dragSlot > dragFrom ? dragSlot - 1 : dragSlot)
        dragFrom = -1; dragSlot = -1
    }

    function startRecording() {
        if (!service) return
        message = ""
        awaitingRecorded = true
        service.startRecordingSteps()
    }
    function save() {
        if (!service) return
        const id = service.saveRecipe({ id: recipeId, name: nameField.text.trim(), steps: steps })
        if (id === "") { message = service.lastError; return }
        if (awaitingRecorded) { awaitingRecorded = false; service.stopRecordingSteps(false) }
        recipeId = id
        finished()
    }
    function cancel() {
        if (awaitingRecorded) { awaitingRecorded = false; if (service && service.recordingSteps) service.stopRecordingSteps(false) }
        message = ""
        finished()
    }

    Flickable {
        id: flick
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        contentHeight: body.implicitHeight + Theme.s3 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: root.dragFrom < 0
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: body
            x: Theme.s3; y: Theme.s3
            width: flick.width - Theme.s3 * 2
            spacing: Theme.s3

            RecipeLink {
                objectName: "recipeEditorBack"
                text: qsTr("‹ Recipes")
                chevron: false
                helpText: qsTr("Back to the list of recipes without keeping these changes.")
                onClicked: root.cancel()
            }

            Row {
                width: parent.width
                spacing: Theme.s2
                Text {
                    width: 52; height: Theme.hControl
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Name")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                }
                TextEntry {
                    id: nameField
                    objectName: "recipeName"
                    width: parent.width - 52 - Theme.s2
                    placeholderText: qsTr("Name, for example Interview tidy")
                    helpTitle: qsTr("Recipe name")
                    helpText: qsTr("What the recipe is called in this tab and in Effects › Recipes.")
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Steps run from top to bottom. Drag a step by its dots to move it.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }
            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            Text {
                visible: root.steps.length === 0
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("No steps yet. Add one, or record the effects you apply.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }

            Column {
                id: stepsColumn
                objectName: "recipeSteps"
                width: parent.width
                spacing: Theme.s1
                Repeater {
                    id: stepRepeater
                    model: root.steps
                    RecipeStepRow {
                        required property var modelData
                        required property int index
                        editor: root
                        step: modelData
                        stepIndex: index
                        column: stepsColumn
                    }
                }
            }

            Row {
                spacing: Theme.s2
                ToolButton {
                    objectName: "recipeAddStep"
                    text: qsTr("Add step…"); iconName: "plus"; showLabel: true
                    checkable: true
                    checked: root.picking
                    helpText: qsTr("Choose a step to add at the end: a clean-up, a level or tone step, or any installed effect.")
                    onClicked: { root.picking = !root.picking; if (root.picking) root.reloadKinds() }
                }
                ToolButton {
                    objectName: "recipeRecordSteps"
                    text: root.service && root.service.recordingSteps ? qsTr("Recording steps…") : qsTr("Record steps")
                    iconName: "circle-dot"; showLabel: true
                    enabled: root.service !== null && !root.service.recordingSteps
                    helpText: qsTr("Work as usual: each effect you apply is added as a step, with its settings. The audio is changed as normal.")
                    helpScope: qsTr("Stop it from the strip along the bottom of the window: Stop and keep adds the steps here.")
                    unavailableReason: qsTr("Steps are being recorded. Use the strip along the bottom of the window to stop.")
                    onClicked: root.startRecording()
                }
            }

            RecipeStepPicker {
                id: picker
                visible: root.picking
                width: parent.width
                editor: root
            }

            Text {
                objectName: "recipeEditorMessage"
                visible: root.message !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: root.message
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.warning
            }
        }

        // Where a dragged step would land.
        Rectangle {
            visible: root.dragFrom >= 0 && root.dragSlot >= 0
            x: body.x
            width: body.width
            height: Theme.selectionRing
            color: Theme.accent
            y: {
                if (root.dragSlot < 0) return 0
                const top = body.y + stepsColumn.y
                if (root.dragSlot < stepRepeater.count) {
                    const it = stepRepeater.itemAt(root.dragSlot)
                    return top + (it ? it.y : 0) - Theme.s1 / 2 - height / 2
                }
                return top + stepsColumn.height + Theme.s1 / 2 - height / 2
            }
        }
    }

    Rectangle {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hControl + Theme.s3 * 2
        color: Theme.panelBg
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3
            anchors.right: footerButtons.left
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            visible: root.fromBuiltIn
            text: qsTr("The built-in recipe stays as it was.")
            elide: Text.ElideRight
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
        }
        Row {
            id: footerButtons
            anchors.right: parent.right
            anchors.rightMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s2
            ToolButton {
                objectName: "recipeCancel"
                text: qsTr("Cancel"); showLabel: true
                helpText: qsTr("Leave without keeping these changes.")
                onClicked: root.cancel()
            }
            ToolButton {
                objectName: "recipeSave"
                text: qsTr("Save"); showLabel: true; prominent: true
                enabled: nameField.text.trim() !== "" && root.steps.length > 0
                helpText: root.fromBuiltIn ? qsTr("Keep this as a recipe of your own. The built-in recipe stays as it was.")
                                           : qsTr("Keep this recipe's name and steps.")
                helpScope: qsTr("Kept on this computer for every project.")
                unavailableReason: nameField.text.trim() === "" ? qsTr("Give the recipe a name first.") : qsTr("Add a step first.")
                onClicked: root.save()
            }
        }
    }
}
