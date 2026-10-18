pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// What the audio load is made of, and what to do when it is high.
C.Popup {
    id: root
    objectName: "audioLoadPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var load: ({})

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 32 : 560)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    Timer {
        interval: 250; repeat: true; running: root.opened; triggeredOnStart: true
        onTriggered: if (root.service) root.load = root.service.audioLoad()
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Audio load"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton { text: qsTr("Close"); iconName: "x"; onClicked: root.close(); helpText: qsTr("Close this panel.") }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("How much of each audio block OmaAudio needs to prepare the sound, while playing. Above 100% the block is late and you hear a dropout.")
        }
        RowLayout {
            spacing: Theme.s4
            ColumnLayout {
                Text { text: qsTr("Now"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
                Text { objectName: "audioLoadNow"; text: Math.round(root.load.percent || 0) + "%"; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            }
            ColumnLayout {
                Text { text: qsTr("Recent peak"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
                Text { text: Math.round(root.load.peak || 0) + "%"; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            }
            ColumnLayout {
                Text { text: qsTr("Dropouts"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
                Text { objectName: "audioLoadDropouts"; text: String(root.load.dropouts || 0); color: (root.load.dropouts || 0) > 0 ? Theme.danger : Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            }
            ColumnLayout {
                Text { text: qsTr("Buffer"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
                Text { text: qsTr("%1 samples").arg(root.load.blockFrames || 0); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading }
            }
        }
        Text {
            visible: (root.load.heaviest || []).length > 0
            text: qsTr("Effects using the most time"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        }
        Repeater {
            model: root.load.heaviest || []
            delegate: Text {
                required property var modelData
                Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: qsTr("%1 — %2% of each block").arg(modelData.name).arg(modelData.percent.toFixed(1))
            }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("If the load is high or you hear dropouts: choose a larger buffer in Audio Setup (smoother playback, a little more delay), bake clip effects you have finished with, bypass or remove heavy effects, or mute tracks you are not using.")
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        ComputerCheck { service: root.service; Layout.fillWidth: true }
    }
}
