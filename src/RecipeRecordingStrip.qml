import QtQuick
import "ui"

// Along the bottom of the window while Record steps is on: how many steps so
// far, Discard, and Stop and keep (the steps join the recipe being changed in
// the Recipes tab). Work goes on as usual meanwhile; the audio changes as normal.
Rectangle {
    id: root
    objectName: "recipeRecordingStrip"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool recording: service !== null && service.recordingSteps
    readonly property int count: service && service.recordedSteps ? service.recordedSteps.length : 0

    visible: recording
    height: recording ? Theme.hControl + Theme.s1 * 2 : 0
    color: Theme.panelRaised
    Rectangle { width: parent.width; height: Theme.hairline; color: Theme.accent }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 8; height: 8; radius: 4
            color: Theme.accent
            SequentialAnimation on opacity {
                running: root.recording
                loops: Animation.Infinite
                NumberAnimation { to: 0.35; duration: 700 }
                NumberAnimation { to: 1; duration: 700 }
            }
        }
        Text {
            objectName: "recipeRecordingText"
            anchors.verticalCenter: parent.verticalCenter
            text: root.count === 0 ? qsTr("Recording steps · none yet")
                                   : qsTr("Recording steps · %1 so far").arg(root.count)
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; color: Theme.textPrimary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Each effect you apply is added as a step, with its settings.")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
        }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2
        ToolButton {
            objectName: "recipeRecordingDiscard"
            text: qsTr("Discard"); showLabel: true
            helpText: qsTr("Stop recording and forget the recorded steps. The audio keeps the effects you applied.")
            onClicked: if (root.service) root.service.stopRecordingSteps(false)
        }
        ToolButton {
            objectName: "recipeRecordingKeep"
            text: qsTr("Stop and keep"); showLabel: true; prominent: true
            helpText: qsTr("Stop recording and add the recorded steps to the recipe being changed in the Recipes tab.")
            onClicked: {
                if (!root.service) return
                root.service.stopRecordingSteps(true)
                if (root.shell) root.shell.showRecipesTab()
            }
        }
    }
}
