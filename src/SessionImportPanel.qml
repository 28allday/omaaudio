pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import QtQuick.Dialogs as D
import "ui"

C.Popup {
    id: root
    objectName: "sessionImportPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property string filePath: ""
    property string mediaFolder: ""
    property bool attempted: false
    function showFile(path) { filePath = Theme.localPath(path); mediaFolder = ""; attempted = false; open() }
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(640, parent ? parent.width - 32 : 640)
    height: Math.min(490, parent ? parent.height - 32 : 490)
    modal: true
    focus: true
    padding: Theme.s3
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rMenu; border.color: Theme.border }

    D.FileDialog {
        id: chooser
        title: qsTr("Choose an AAF or OMF session")
        nameFilters: [qsTr("AAF and OMF sessions (*.aaf *.AAF *.omf *.OMF)")]
        onAccepted: { root.filePath = Theme.localPath(selectedFile); root.attempted = false }
    }
    D.FolderDialog {
        id: mediaChooser
        title: qsTr("Choose the exported audio folder")
        onAccepted: { root.mediaFolder = Theme.localPath(selectedFolder); root.attempted = false }
    }
    component Note: Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
        color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
    }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Text { text: qsTr("Import Session"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl * 1.3 }
        Note {
            Layout.fillWidth: true
            text: qsTr("Add tracks and clips from an AAF (such as Resolve’s Pro Tools export) or an OMF 2.0 file. Clips keep their timeline positions, and one Undo removes the import.")
        }
        RowLayout {
            Layout.fillWidth: true
            ToolButton {
                objectName: "sessionImportChoose"; text: qsTr("Choose session…"); showLabel: true
                enabled: root.service && !root.service.loading
                helpText: qsTr("Choose the AAF or OMF file exported with the audio tracks from another editor.")
                helpScope: qsTr("The current project is kept. Imported audio is copied into OmaAudio’s own storage.")
                onClicked: chooser.open()
            }
            Text { Layout.fillWidth: true; text: root.filePath || qsTr("No session file selected"); textFormat: Text.PlainText; elide: Text.ElideMiddle; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        }
        RowLayout {
            Layout.fillWidth: true
            ToolButton {
                objectName: "sessionImportMedia"; text: qsTr("Audio folder…"); showLabel: true
                enabled: root.service && !root.service.loading
                helpText: qsTr("Choose the folder containing the exported audio if it was moved or could not be found.")
                helpScope: qsTr("Audio beside the session file and embedded WAV, PCM or AIFC audio are found automatically. Files with ambiguous names must be resolved before import.")
                onClicked: mediaChooser.open()
            }
            Text { Layout.fillWidth: true; text: root.mediaFolder || qsTr("Use audio beside the session file"); textFormat: Text.PlainText; elide: Text.ElideMiddle; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        }
        Note {
            Layout.fillWidth: true
            text: qsTr("Keeps clip positions, the part of each file used, gaps, fixed volume changes and supported straight fades. In Resolve, turn speed changes, time effects and changing volume into finished audio before export. The result below explains anything that cannot transfer.")
        }
        ScrollPane {
            id: pane1
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth; clip: true
            C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
            Text {
                objectName: "sessionImportResult"
                x: pane1.reachLeft; width: pane1.columnWidth
                text: root.attempted && root.service ? root.service.sessionImportMessage : ""
                textFormat: Text.PlainText; wrapMode: Text.WordWrap
                color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl * Help.textScale
            }
        }
        RowLayout {
            Layout.fillWidth: true
            ToolButton {
                objectName: "sessionImportCancel"; text: qsTr("Cancel import"); showLabel: true
                visible: root.attempted && root.service && root.service.loading
                helpText: qsTr("Stop reading the session file and keep the current session unchanged.")
                onClicked: root.service.cancelLoad()
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "sessionImportClose"; text: qsTr("Close"); showLabel: true
                helpText: qsTr("Close this panel. Any import already running continues; Cancel is also available in the status bar.")
                onClicked: root.close()
            }
            ToolButton {
                objectName: "sessionImportApply"; text: qsTr("Import tracks"); showLabel: true
                enabled: root.filePath !== "" && root.service && !root.service.loading && !root.service.saving && !root.service.recording
                helpText: qsTr("Read the AAF and add all its audio tracks to this project.")
                helpScope: qsTr("Missing audio or an unsupported timeline stops the import without changing the project. One Undo removes a successful import.")
                unavailableReason: root.filePath === "" ? qsTr("Choose an AAF file first.") : qsTr("Wait for the current recording, save or import to finish.")
                onClicked: { root.attempted = true; root.service.importSession(root.filePath, root.mediaFolder) }
            }
        }
    }
}
