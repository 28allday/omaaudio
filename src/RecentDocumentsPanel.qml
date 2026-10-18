pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

C.Popup {
    id: root
    objectName: "recentDocumentsPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var selectedDocument: null
    readonly property var documents: service ? service.recentDocuments : []
    property string message: ""
    function openSelected(importing) {
        if (!selectedDocument || !shell) return
        const document = {path: selectedDocument.path, project: selectedDocument.project}
        close(); shell.openRecentDocument(document, importing)
    }
    onDocumentsChanged: {
        if (!selectedDocument) return
        if (!documents.some(function(d) { return d.path === root.selectedDocument.path && d.project === root.selectedDocument.project }))
            selectedDocument = null
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(720, parent ? parent.width - 32 : 720)
    height: Math.min(560, parent ? parent.height - 32 : 560)
    modal: true; focus: true; closePolicy: C.Popup.CloseOnEscape
    padding: Theme.s4
    onAboutToShow: { selectedDocument = null; message = ""; if (service) service.reloadRecentDocuments() }
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Layout.fillWidth: true
            Text { text: qsTr("Recent documents"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                objectName: "recentDocumentsClose"; iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close this panel without opening anything.")
            }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Up to 20 saved projects and opened or imported audio files, kept locally. Double-click an entry to reopen it, or select it to import its audio as a track.")
        }
        ListView {
            id: list; objectName: "recentDocumentsList"; Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; spacing: Theme.s1; model: root.documents
            C.ScrollBar.vertical: ScrollBar { id: recentBar }
            delegate: C.ItemDelegate {
                id: row; objectName: "recentDocumentRow"
                required property var modelData
                width: list.width - recentBar.gutter; height: 56
                highlighted: root.selectedDocument !== null && root.selectedDocument.path === modelData.path && root.selectedDocument.project === modelData.project
                onClicked: { root.selectedDocument = {path: modelData.path, project: modelData.project, name: modelData.name}; root.message = "" }
                // A double-click opens it, as Open would.
                onDoubleClicked: {
                    root.selectedDocument = {path: modelData.path, project: modelData.project, name: modelData.name}
                    if (root.service && !root.service.loading && !root.service.saving) root.openSelected(false)
                }
                background: Rectangle { color: row.highlighted ? Theme.selectedFill : row.hovered ? Theme.hoverOverlay : "transparent"; radius: Theme.rControl }
                contentItem: Column {
                    spacing: Theme.s1
                    Text {
                        width: parent.width; text: (row.modelData.project ? qsTr("Project · ") : qsTr("Audio · ")) + row.modelData.name
                        textFormat: Text.PlainText; elide: Text.ElideMiddle; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    }
                    Text {
                        width: parent.width; text: row.modelData.path; textFormat: Text.PlainText; elide: Text.ElideMiddle
                        color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    }
                }
                Accessible.name: (modelData.project ? qsTr("Project: ") : qsTr("Audio: ")) + modelData.path
            }
            Text {
                anchors.centerIn: parent; visible: root.documents.length === 0
                text: qsTr("No recent documents yet."); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            }
        }
        Text {
            objectName: "recentDocumentPath"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
            text: root.selectedDocument ? root.selectedDocument.path : ""; visible: text !== ""
            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            maximumLineCount: 4; elide: Text.ElideMiddle
        }
        Text {
            objectName: "recentDocumentsMessage"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
            text: root.service && root.service.recentDocumentsError !== "" ? root.service.recentDocumentsError : root.message
            visible: text !== ""; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        }
        RowLayout {
            Layout.fillWidth: true
            ToolButton {
                objectName: "recentDocumentsReload"; text: qsTr("Reload"); showLabel: true
                helpText: qsTr("Read the recent list again from disk.")
                onClicked: { root.service.reloadRecentDocuments(); root.message = "" }
            }
            ToolButton {
                objectName: "recentDocumentsForget"; text: qsTr("Forget"); showLabel: true; enabled: root.selectedDocument !== null
                helpText: qsTr("Remove the selected entry from the list.")
                helpScope: qsTr("The file or project itself is untouched.")
                unavailableReason: qsTr("Select an entry first.")
                onClicked: {
                    if (root.service.forgetRecentDocument(root.selectedDocument.path, root.selectedDocument.project)) root.message = qsTr("Entry removed from recent documents.")
                }
            }
            ToolButton {
                objectName: "recentDocumentsClear"; text: qsTr("Clear list"); showLabel: true; enabled: root.documents.length > 0
                helpText: qsTr("Empty the recent list.")
                helpScope: qsTr("Files and projects are untouched.")
                unavailableReason: qsTr("The list is already empty.")
                onClicked: { if (root.service.clearRecentDocuments()) root.message = qsTr("Recent document history cleared.") }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "recentDocumentsImport"; text: qsTr("Import as track"); showLabel: true
                helpText: qsTr("Add the selected audio file to the current project as a new track.")
                helpScope: qsTr("The project is kept. Audio at another sample rate is converted to the project rate.")
                unavailableReason: root.selectedDocument === null ? qsTr("Select an entry first.")
                    : root.selectedDocument.project ? qsTr("A project cannot be imported as a track. Open it instead.")
                    : !root.service ? qsTr("The audio engine is not available.")
                    : root.service.loading ? qsTr("Wait for the current file to finish loading.")
                    : root.service.saving ? qsTr("Wait for the save to finish.")
                    : qsTr("Stop recording first.")
                enabled: root.selectedDocument !== null && !root.selectedDocument.project && root.service && !root.service.loading && !root.service.saving && !root.service.recording
                onClicked: root.openSelected(true)
            }
            ToolButton {
                objectName: "recentDocumentsOpen"; text: qsTr("Open"); showLabel: true
                helpText: qsTr("Open the selected project, or the selected audio file as a new project.")
                helpScope: qsTr("Replaces the current project. You are asked to save unsaved changes first.")
                unavailableReason: root.selectedDocument === null ? qsTr("Select an entry first.")
                    : !root.service ? qsTr("The audio engine is not available.")
                    : root.service.loading ? qsTr("Wait for the current file to finish loading.")
                    : qsTr("Wait for the save to finish.")
                enabled: root.selectedDocument !== null && root.service && !root.service.loading && !root.service.saving
                onClicked: root.openSelected(false)
            }
        }
    }
}
