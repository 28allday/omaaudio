pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// A recipe's run, in the Recipes tab (Backend::recipeRun): while it runs, each
// step with its state and Stop; when it is over, what ran, what could not and
// why, and Undo for the whole run. It stays until Done.
Item {
    id: root
    objectName: "recipeRunCard"
    property var panel: null
    readonly property var service: panel ? panel.service : null
    readonly property var run: service ? service.recipeRun : ({})
    readonly property bool running: run.running === true
    readonly property bool stopped: run.stopped === true
    readonly property bool finished: run.finished === true && !stopped
    readonly property var steps: run.steps || []
    readonly property int failed: run.failed || 0

    function seconds(s) { return qsTr("%1 s").arg(Number(s || 0).toFixed(1)) }
    // Steps that have run, plus how far the current one has got.
    readonly property real fraction: {
        const total = run.total || 0
        if (total <= 0) return 0
        let progress = (run.done || 0) + failed
        for (const s of steps) if (s.state === "running") progress += Math.max(0, Math.min(1, s.progress || 0))
        return Math.min(1, progress / total)
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: body.implicitHeight + Theme.s3 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: body
            x: Theme.s3; y: Theme.s3
            width: flick.width - Theme.s3 * 2
            spacing: Theme.s3

            Column {
                width: parent.width
                spacing: 2
                Text {
                    objectName: "recipeRunName"
                    width: parent.width
                    text: root.run.name || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading
                    color: Theme.textPrimary
                }
                Text {
                    width: parent.width
                    text: root.run.targetText || ""
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                }
            }

            Rectangle {
                width: parent.width
                height: card.implicitHeight + Theme.s3 * 2
                radius: Theme.rControl
                color: Theme.panelRaised
                border.width: Theme.hairline
                border.color: Theme.border

                Column {
                    id: card
                    x: Theme.s3; y: Theme.s3
                    width: parent.width - Theme.s3 * 2
                    spacing: Theme.s2

                    Item {
                        width: parent.width
                        height: Theme.hRow
                        Text {
                            objectName: "recipeRunState"
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.running ? qsTr("Running") : root.stopped ? qsTr("Stopped") : qsTr("Finished")
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.weight: Theme.wHeading
                            color: Theme.textPrimary
                        }
                        Text {
                            objectName: "recipeRunCount"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.running ? qsTr("Step %1 of %2").arg(Math.max(1, root.run.current || 0)).arg(root.run.total || 0)
                                : root.finished ? qsTr("%1 of %2 steps run · %3").arg(root.run.done || 0).arg(root.run.total || 0).arg(root.seconds(root.run.seconds))
                                : ""
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                        }
                    }
                    // Progress, under the heading.
                    Rectangle {
                        visible: root.running
                        width: parent.width
                        height: 3
                        radius: 1
                        color: Theme.controlBg
                        Rectangle {
                            width: parent.width * root.fraction
                            height: parent.height
                            radius: 1
                            color: Theme.accent
                            Behavior on width { NumberAnimation { duration: Theme.dFast } }
                        }
                    }

                    // One step could not run: said plainly, in amber.
                    Rectangle {
                        objectName: "recipeRunNotice"
                        visible: root.finished && root.failed > 0
                        width: parent.width
                        height: visible ? notice.implicitHeight + Theme.s2 * 2 : 0
                        radius: Theme.rControl
                        color: Theme.withAlpha(Theme.warning, 0.12)
                        border.width: Theme.hairline
                        border.color: Theme.withAlpha(Theme.warning, 0.6)
                        Row {
                            id: notice
                            x: Theme.s2; y: Theme.s2
                            width: parent.width - Theme.s2 * 2
                            spacing: Theme.s2
                            Icon { name: "circle-alert"; color: Theme.warning; size: Theme.szIcon }
                            Text {
                                width: parent.width - Theme.szIcon - Theme.s2
                                wrapMode: Text.WordWrap
                                text: (root.run.done || 0) === 0 ? qsTr("No step could run.")
                                    : root.failed === 1 ? qsTr("One step could not run. The others did.")
                                    : qsTr("%1 steps could not run. The others did.").arg(root.failed)
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary
                            }
                        }
                    }

                    Text {
                        visible: root.stopped
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: qsTr("Stopped. The audio is as it was.")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                    }

                    Column {
                        objectName: "recipeRunSteps"
                        width: parent.width
                        spacing: Theme.s1
                        Repeater {
                            model: root.steps
                            Item {
                                id: stepRow
                                required property var modelData
                                required property int index
                                readonly property string stepState: modelData.state || "waiting"
                                objectName: "recipeRunStep_" + index
                                width: parent.width
                                height: Math.max(Theme.hRow, stepWords.implicitHeight)

                                Item {
                                    id: mark
                                    width: Theme.szIcon + 2
                                    height: Theme.hRow
                                    Icon {
                                        id: markIcon
                                        anchors.centerIn: parent
                                        size: Theme.szIcon
                                        name: stepRow.stepState === "done" ? "circle-check"
                                            : stepRow.stepState === "running" ? "loader"
                                            : stepRow.stepState === "failed" ? "circle-alert"
                                            : stepRow.stepState === "off" ? "circle-dashed" : "circle"
                                        color: stepRow.stepState === "done" ? Theme.success
                                             : stepRow.stepState === "running" ? Theme.accent
                                             : stepRow.stepState === "failed" ? Theme.warning : Theme.textMuted
                                        RotationAnimator on rotation {
                                            running: stepRow.stepState === "running" && root.visible
                                            from: 0; to: 360; duration: 1200; loops: Animation.Infinite
                                        }
                                    }
                                }
                                Column {
                                    id: stepWords
                                    anchors.left: mark.right
                                    anchors.leftMargin: Theme.s2
                                    anchors.right: stepRight.left
                                    anchors.rightMargin: Theme.s2
                                    spacing: 1
                                    Text {
                                        width: parent.width
                                        height: Math.max(implicitHeight, Theme.hRow)
                                        verticalAlignment: Text.AlignVCenter
                                        text: stepRow.modelData.title || ""
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                                        font.weight: stepRow.stepState === "running" ? Font.Medium : Font.Normal
                                        color: stepRow.stepState === "waiting" || stepRow.stepState === "off" ? Theme.textMuted : Theme.textPrimary
                                    }
                                    // Why it could not run, in full.
                                    Text {
                                        objectName: "recipeRunStepReason"
                                        visible: stepRow.stepState === "failed" && (stepRow.modelData.reason || "") !== ""
                                        width: parent.width
                                        wrapMode: Text.WordWrap
                                        text: stepRow.modelData.reason || ""
                                        textFormat: Text.PlainText
                                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                                    }
                                }
                                Text {
                                    id: stepRight
                                    anchors.right: parent.right
                                    height: Theme.hRow
                                    verticalAlignment: Text.AlignVCenter
                                    text: stepRow.stepState === "running" ? qsTr("%1 %").arg(Math.round((stepRow.modelData.progress || 0) * 100))
                                        : stepRow.stepState === "done" ? ((stepRow.modelData.result || "") !== "" ? stepRow.modelData.result : root.seconds(stepRow.modelData.seconds))
                                        : stepRow.stepState === "off" ? qsTr("Off") : ""
                                    textFormat: Text.PlainText
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                                }
                            }
                        }
                    }

                    // Running: Stop.
                    Item {
                        visible: root.running
                        width: parent.width
                        height: Theme.hControl
                        Text {
                            anchors.left: parent.left
                            anchors.right: stop.left
                            anchors.rightMargin: Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Stop puts the audio back as it was.")
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                        }
                        ToolButton {
                            id: stop
                            objectName: "recipeStop"
                            anchors.right: parent.right
                            text: qsTr("Stop"); showLabel: true
                            helpText: qsTr("Stop the recipe and put the audio back as it was before the run.")
                            onClicked: if (root.service) root.service.stopRecipe()
                        }
                    }

                    // Over: Undo and Done.
                    Text {
                        objectName: "recipeRunUndoNote"
                        visible: root.finished && (root.run.undoLabel || "") !== ""
                        width: parent.width
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: qsTr("Undo takes back the whole run in one step (“%1”).").arg(root.run.undoLabel || "")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                    }
                    Row {
                        visible: !root.running
                        anchors.right: parent.right
                        spacing: Theme.s2
                        ToolButton {
                            objectName: "recipeRunUndo"
                            visible: root.finished && (root.run.undoLabel || "") !== ""
                            text: qsTr("Undo"); showLabel: true
                            enabled: root.service !== null && root.service.canUndo && root.service.undoLabel === root.run.undoLabel
                            helpText: qsTr("Take back the whole run in one step and close this report.")
                            unavailableReason: qsTr("Something else has been done since; use Edit › Undo to step back through it.")
                            onClicked: { root.service.undo(); root.service.clearRecipeRun() }
                        }
                        ToolButton {
                            objectName: "recipeRunDone"
                            text: qsTr("Done"); showLabel: true; prominent: true
                            helpText: qsTr("Close this report. The audio stays as it is; Undo still takes back the run.")
                            onClicked: if (root.service) root.service.clearRecipeRun()
                        }
                    }
                }
            }
        }
    }
}
