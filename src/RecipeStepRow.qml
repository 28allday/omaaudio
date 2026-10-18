pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// One step in the recipe editor: its dots (drag to move), its switch, its
// number and title, a line saying what its settings are, Change… to open the
// settings under it, and a ⋯ menu to move or remove it.
Item {
    id: row
    property var editor: null
    property var step: ({})
    property int stepIndex: 0
    property Item column: null        // the steps column, for dragging
    readonly property var service: editor ? editor.service : null
    readonly property var kind: editor ? editor.kindFor(step.kind) : null
    // Read for the step itself: an installed effect's are its own controls.
    readonly property var settings: service ? service.stepSettings(step) : []
    readonly property bool open: editor !== null && editor.expanded === stepIndex
    readonly property bool on: step.on !== false

    objectName: "recipeStep_" + stepIndex
    width: parent ? parent.width : 0
    implicitHeight: col.implicitHeight
    opacity: editor && editor.dragFrom === stepIndex ? 0.5 : 1

    function decimalsFor(step) {
        const s = String(step)
        return s.indexOf(".") < 0 ? 0 : s.length - s.indexOf(".") - 1
    }

    Column {
        id: col
        width: parent.width
        spacing: Theme.s1

        Item {
            width: parent.width
            height: Math.max(words.implicitHeight, Theme.hControl) + Theme.s1

            // The dots: drag to move the step.
            Item {
                id: grip
                width: 14
                height: parent.height
                Icon {
                    anchors.centerIn: parent
                    name: "grip-vertical"
                    size: Theme.szIcon
                    color: dragArea.containsMouse || dragArea.pressed ? Theme.textPrimary : Theme.textMuted
                }
                ControlHelp {
                    title: qsTr("Move step")
                    purpose: qsTr("Drag up or down to change where this step runs. Steps run from top to bottom.")
                    scope: qsTr("Move Up and Move Down in the step's ⋯ menu do the same from the keyboard.")
                }
                MouseArea {
                    id: dragArea
                    objectName: "recipeStepGrip"
                    anchors.fill: parent
                    anchors.margins: -2
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    onPressed: { row.editor.dragFrom = row.stepIndex; row.editor.dragSlot = -1 }
                    onPositionChanged: function (mouse) {
                        if (!pressed || !row.column) return
                        const p = mapToItem(row.column, mouse.x, mouse.y)
                        const slot = row.editor.slotAt(p.y)
                        // Its own place, above or below itself, is no move.
                        row.editor.dragSlot = slot === row.stepIndex || slot === row.stepIndex + 1 ? -1 : slot
                    }
                    onReleased: row.editor.finishDrag()
                    onCanceled: { row.editor.dragFrom = -1; row.editor.dragSlot = -1 }
                }
            }
            Toggle {
                id: toggle
                objectName: "recipeStepOn"
                anchors.left: grip.right
                anchors.leftMargin: Theme.s1
                anchors.top: parent.top
                anchors.topMargin: (Theme.hControl - height) / 2
                checked: row.on
                helpTitle: qsTr("Step on or off")
                helpText: qsTr("Turn this step off to skip it when the recipe runs, without removing it.")
                onToggled: row.editor.setStep(row.stepIndex, "on", checked)
            }
            Column {
                id: words
                anchors.left: toggle.right
                anchors.leftMargin: Theme.s2
                anchors.right: buttons.left
                anchors.rightMargin: Theme.s2
                anchors.top: parent.top
                anchors.topMargin: 2
                spacing: 1
                Text {
                    width: parent.width
                    text: (row.stepIndex + 1) + "  " + (row.service ? row.service.stepTitle(row.step) : "")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.weight: Font.Medium
                    color: row.on ? Theme.textPrimary : Theme.textMuted
                }
                Text {
                    objectName: "recipeStepSummary"
                    width: parent.width
                    leftPadding: Theme.s3
                    text: row.service ? row.service.stepSummary(row.step) : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                }
                // A step this computer cannot run says why, before the run does.
                Text {
                    visible: row.kind !== null && row.kind.available === false
                    width: parent.width
                    leftPadding: Theme.s3
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: row.kind ? row.kind.reason || "" : ""
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    color: Theme.warning
                }
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: 0
                ToolButton {
                    objectName: "recipeStepChange"
                    visible: row.settings.length > 0
                    text: row.open ? qsTr("Done") : qsTr("Change…")
                    showLabel: true
                    checked: row.open
                    helpText: row.open ? qsTr("Close this step's settings.") : qsTr("Open this step's settings under it.")
                    onClicked: row.editor.expanded = row.open ? -1 : row.stepIndex
                }
                IconButton {
                    id: more
                    objectName: "recipeStepMore"
                    iconName: "ellipsis"
                    text: qsTr("More for step %1").arg(row.stepIndex + 1)
                    helpTitle: qsTr("Step actions")
                    helpText: qsTr("Move this step up or down, or remove it from the recipe.")
                    onClicked: menu.popup(more, 0, more.height)
                }
            }
        }

        // The step's settings, under it.
        Rectangle {
            visible: row.open
            x: grip.width + Theme.s1 + toggle.width + Theme.s2
            width: parent.width - x
            height: visible ? settingsColumn.implicitHeight + Theme.s2 * 2 : 0
            radius: Theme.rControl
            color: Theme.panelRaised
            border.width: Theme.hairline
            border.color: Theme.border
            Column {
                id: settingsColumn
                x: Theme.s2; y: Theme.s2
                width: parent.width - Theme.s2 * 2
                spacing: Theme.s2
                Repeater {
                    model: row.open ? row.settings : []
                    Column {
                        id: setting
                        required property var modelData
                        readonly property var current: row.step[modelData.key] !== undefined ? row.step[modelData.key] : modelData["default"]
                        width: settingsColumn.width
                        spacing: Theme.s1
                        Text {
                            visible: (setting.modelData.choices || []).length > 0
                            text: setting.modelData.label
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                        }
                        SegmentedControl {
                            objectName: "recipeStepChoice_" + setting.modelData.key
                            visible: (setting.modelData.choices || []).length > 0
                            labels: (setting.modelData.choices || []).map(c => c.label)
                            currentIndex: (setting.modelData.choices || []).findIndex(c => c.value == setting.current)
                            helpTitle: setting.modelData.label
                            helpText: qsTr("Choose how this step runs. The recipe keeps the choice when you Save.")
                            onActivated: function (i) { row.editor.setStep(row.stepIndex, setting.modelData.key, setting.modelData.choices[i].value) }
                        }
                        SliderField {
                            objectName: "recipeStepSetting_" + setting.modelData.key
                            visible: (setting.modelData.choices || []).length === 0
                            width: parent.width
                            label: setting.modelData.label
                            labelWidth: 96
                            readoutWidth: 76
                            from: setting.modelData.from; to: setting.modelData.to
                            stepSize: setting.modelData.step || 0
                            decimals: setting.modelData.step ? row.decimalsFor(setting.modelData.step) : 2
                            suffix: setting.modelData.unit ? " " + setting.modelData.unit : ""
                            value: Number(setting.current)
                            helpTitle: setting.modelData.label
                            helpText: qsTr("This step's setting, from %1 to %2 %3. The recipe keeps it when you Save.")
                                      .arg(setting.modelData.from).arg(setting.modelData.to).arg(setting.modelData.unit || "")
                            onEditingFinished: function (v) { row.editor.setStep(row.stepIndex, setting.modelData.key, v) }
                        }
                        // What the two ends mean, where words say it better than numbers.
                        Item {
                            visible: (setting.modelData.choices || []).length === 0 && !!setting.modelData.lowText
                            width: parent.width
                            height: visible ? lowWord.implicitHeight : 0
                            Text {
                                id: lowWord
                                x: 96
                                text: setting.modelData.lowText || ""
                                textFormat: Text.PlainText
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 76
                                text: setting.modelData.highText || ""
                                textFormat: Text.PlainText
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                            }
                        }
                    }
                }
            }
        }
    }

    AppMenu {
        id: menu
        objectName: "recipeStepMenu"
        shell: row.editor ? row.editor.shell : null
        AppMenuItem {
            objectName: "recipeStepUp"
            text: qsTr("Move Up")
            enabled: row.stepIndex > 0
            helpText: qsTr("Run this step one place earlier.")
            reason: qsTr("This step already runs first.")
            onTriggered: row.editor.moveStep(row.stepIndex, row.stepIndex - 1)
        }
        AppMenuItem {
            objectName: "recipeStepDown"
            text: qsTr("Move Down")
            enabled: row.editor !== null && row.stepIndex < row.editor.steps.length - 1
            helpText: qsTr("Run this step one place later.")
            reason: qsTr("This step already runs last.")
            onTriggered: row.editor.moveStep(row.stepIndex, row.stepIndex + 1)
        }
        MenuSep {}
        AppMenuItem {
            objectName: "recipeStepRemove"
            text: qsTr("Remove Step")
            helpText: qsTr("Take this step out of the recipe. Nothing is kept until you Save.")
            onTriggered: row.editor.removeStep(row.stepIndex)
        }
    }
}
