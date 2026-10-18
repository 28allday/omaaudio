pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtQuick.Dialogs
import "ui"

C.Popup {
    id: root
    objectName: "collectProjectPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool busy: service ? service.collecting : false
    readonly property var status: service ? service.collectionStatus : ({})
    property alias destination: path.text
    property string message: ""
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 32 : 760)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.panelBg; border.color: Theme.borderStrong; radius: Theme.rPanel }
    function phaseLabel(phase) {
        switch (phase) {
        case "audio": return qsTr("Copying source audio")
        case "video": return qsTr("Copying reference video")
        case "metadata": return qsTr("Writing project")
        case "inventory": return qsTr("Listing required plugins")
        case "publish": return qsTr("Finishing collection")
        default: return qsTr("Preparing collection")
        }
    }
    FileDialog {
        id: chooser; title: qsTr("Collect project to a new folder")
        fileMode: FileDialog.SaveFile; defaultSuffix: "omaaudio"
        nameFilters: [qsTr("OmaAudio projects (*.omaaudio *.omawave)")]
        onAccepted: path.text = Theme.localPath(selectedFile)
    }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Collect Project"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; color: Theme.textPrimary; Layout.fillWidth: true }
            ToolButton { text: qsTr("Close"); showLabel: true; helpText: qsTr("Close this panel. Collection continues while OmaAudio stays open."); onClicked: root.close() }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
            text: qsTr("Copy the current project, all retained source audio and its reference video into a new project folder. Move that whole folder to another machine. The open project and its originals stay as they are.")
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
            text: qsTr("Required plugins are listed in the copy and must be installed separately. Plugin binaries are not included. Collection fails if reference media cannot be read.")
        }
        RowLayout {
            TextEntry { id: path; objectName: "collectDestination"; Layout.fillWidth: true; enabled: !root.busy; placeholderText: qsTr("New project path (.omaaudio)"); helpTitle: qsTr("Collection destination"); helpText: qsTr("Choose a new project name inside an existing local folder. Existing files and folders are never replaced.") }
            ToolButton { text: qsTr("Browse…"); showLabel: true; enabled: !root.busy; helpText: qsTr("Choose the location and name for the collected project."); onClicked: chooser.open() }
        }
        Text {
            objectName: "collectionResult"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: root.status.state === "failed" ? Theme.warning : Theme.textPrimary
            text: root.message || (root.busy ? root.phaseLabel(root.status.phase) + " · " + Math.round((root.status.progress || 0) * 100) + "%\n" + root.status.path
                : root.status.state === "done" ? qsTr("Collection complete · Audio sources: %1 · Plugin uses: %2%3\n%4").arg(root.status.sources).arg(root.status.plugins).arg(root.status.video ? qsTr(" · Video included") : "").arg(root.status.path)
                : root.status.state === "cancelled" ? qsTr("Collection cancelled. No new project was published.")
                : root.status.state === "failed" ? (root.status.published ? qsTr("The copy exists at %1, but its final disk sync failed.\n").arg(root.status.path) : "") + root.status.error : "")
        }
        Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary; text: qsTr("Closing the app cancels unfinished collection. Later edits do not change an accepted copy.") }
        RowLayout {
            Item { Layout.fillWidth: true }
            ToolButton { objectName: "collectCancel"; text: qsTr("Cancel collection"); showLabel: true; enabled: root.busy; helpText: qsTr("Stop copying and remove this operation's unfinished files. A copy that has already finished remains."); onClicked: root.service.cancelCollection() }
            ToolButton {
                objectName: "collectStart"; text: qsTr("Collect project"); showLabel: true
                enabled: root.service && !root.busy && path.text.length > 0
                helpText: qsTr("Take a copy of the current session and start collection. The current project path and Undo history remain unchanged.")
                unavailableReason: root.busy ? qsTr("Collection is already running.") : qsTr("Choose a new project path first.")
                onClicked: root.message = root.service.collectProject(path.text) ? "" : root.service.lastError
            }
        }
    }
}
