pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

C.Popup {
    id: root
    objectName: "storagePanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var usage: ({})
    readonly property bool busy: service && service.storageBusy
    function reload() { usage = service ? service.storageUsage() : ({}) }
    function size(bytes) { return (Number(bytes || 0) / 1048576).toFixed(2) + " MiB" }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(740, parent ? parent.width - 32 : 740)
    height: Math.min(700, parent ? parent.height - 32 : 700)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onOpened: reload()
    onClosed: if (shell) shell.restoreFocus()
    Connections {
        target: root.service
        function onStorageChanged() { if (root.visible) root.reload() }
        function onTimelineChanged() { if (root.visible) root.reload() }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Storage and cleanup"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                iconName: "x"; text: qsTr("Close"); onClicked: root.close()
                helpText: qsTr("Close this panel. A storage task in progress carries on.")
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "storageUsageSummary"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textPrimary
            text: qsTr("Sources: %1 · Streamed: %2 · Retained for undo or clipboard: %3").arg(root.usage.sources || 0).arg(root.usage.diskSources || 0).arg(root.usage.retained || 0)
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: qsTr("Source memory: %1 · Disk backing: %2 · Playback cache: %3").arg(root.size(root.usage.memoryBytes)).arg(root.size(root.usage.backingBytes)).arg(root.size(root.usage.cacheBytes))
        }
        ScrollPane {
            id: usageScroll
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 90
            clip: true; contentWidth: availableWidth
            Column {
                x: usageScroll.reachLeft; width: usageScroll.columnWidth; spacing: Theme.s2
                Repeater {
                    model: root.usage.rows || []
                    delegate: RowLayout {
                        required property var modelData
                        width: parent.width; spacing: Theme.s3
                        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; Layout.fillWidth: true; elide: Text.ElideMiddle; textFormat: Text.PlainText; text: parent.modelData.name; color: Theme.textPrimary }
                        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: parent.modelData.use; color: parent.modelData.use === "unused" ? Theme.warning : Theme.textMuted }
                        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.size(parent.modelData.bytes); color: Theme.textSecondary; Layout.preferredWidth: 90; horizontalAlignment: Text.AlignRight }
                    }
                }
            }
        }
        RowLayout {
            Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; text: qsTr("%1 unused sources (%2) can be released. Undo and clipboard audio stay available.").arg(root.usage.unused || 0).arg(root.size(root.usage.unusedBytes)) }
            ToolButton {
                objectName: "storagePrune"; text: qsTr("Release unused sources"); showLabel: true
                helpText: qsTr("Free the memory and disk backing held by audio that no clip uses any more.")
                helpScope: qsTr("Undo history and the clipboard keep their audio. Files on disk are not deleted.")
                unavailableReason: !root.service ? qsTr("The audio engine is not available.")
                    : root.busy ? qsTr("Wait for the current storage task to finish.")
                    : root.service.recording ? qsTr("Stop recording first.")
                    : qsTr("There are no unused sources to release.")
                enabled: root.service && !root.busy && !root.service.recording && root.usage.unused > 0
                onClicked: root.service.pruneUnusedSources(root.usage.stamp)
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
            text: qsTr("Save after releasing sources, then inspect the saved bundle. Only the files listed below will be removed. Saved and recovery audio are protected; original imports are kept.")
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; elide: Text.ElideMiddle; textFormat: Text.PlainText; color: Theme.textMuted
            text: root.service && root.service.projectPath ? root.service.projectPath : qsTr("Save this project to inspect its bundle.")
        }
        ScrollPane {
            id: unusedScroll
            Layout.fillWidth: true; Layout.preferredHeight: 120
            clip: true; contentWidth: availableWidth
            Column {
                x: unusedScroll.reachLeft; width: unusedScroll.columnWidth; spacing: Theme.s2
                Repeater {
                    model: root.service ? root.service.unusedAudioFiles : []
                    delegate: RowLayout {
                        required property var modelData
                        width: parent.width
                        Text { Layout.fillWidth: true; elide: Text.ElideMiddle; textFormat: Text.PlainText; text: parent.modelData.name; color: Theme.textPrimary; font.family: Theme.monoFamily }
                        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.size(parent.modelData.bytes); color: Theme.textSecondary }
                    }
                }
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "storageMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap
            textFormat: Text.PlainText; color: Theme.textPrimary
            text: root.service ? root.service.storageMessage : ""
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.warning
            visible: root.service && (root.service.streamCacheMisses > 0 || root.service.streamReadErrors > 0)
            text: root.service ? qsTr("Playback: %1 missing pages · %2 disk read errors. Missing audio plays as silence. Seek and allow the cache to warm before replaying.").arg(root.service.streamCacheMisses).arg(root.service.streamReadErrors) : ""
        }
        RowLayout {
            ToolButton {
                text: qsTr("Refresh"); enabled: !root.busy; onClicked: root.reload()
                helpText: qsTr("Measure the project's storage use again.")
                unavailableReason: qsTr("Wait for the current storage task to finish.")
            }
            ToolButton {
                objectName: "storageInspect"; text: qsTr("Inspect saved bundle"); enabled: root.service && !!root.service.projectPath && !root.busy && !root.service.recording; onClicked: root.service.inspectUnusedAudio()
                helpText: qsTr("List the audio files in the saved project bundle that no clip, undo step or recovery layer uses.")
                helpScope: qsTr("Nothing is removed until you press Remove.")
                unavailableReason: !root.service ? qsTr("The audio engine is not available.")
                    : !root.service.projectPath ? qsTr("Save the project first; only a saved bundle can be inspected.")
                    : root.busy ? qsTr("Wait for the current storage task to finish.")
                    : qsTr("Stop recording first.")
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Cancel"); visible: root.busy; onClicked: root.service.cancelStorage()
                helpTitle: qsTr("Cancel storage task")
                helpText: qsTr("Stop the storage task in progress.")
            }
            ToolButton {
                objectName: "storageRemove"; text: root.service && root.service.unusedAudioFiles.length === 1 ? qsTr("Remove 1 file") : qsTr("Remove %1 files").arg(root.service ? root.service.unusedAudioFiles.length : 0)
                helpTitle: qsTr("Remove unused files")
                helpText: qsTr("Delete the listed files from the saved project bundle.")
                helpScope: qsTr("Permanent: the files are removed from disk. Original imports, saved audio and recovery audio are never listed.")
                unavailableReason: !root.service ? qsTr("The audio engine is not available.")
                    : root.busy ? qsTr("Wait for the current storage task to finish.")
                    : root.service.recording ? qsTr("Stop recording first.")
                    : root.service.dirty ? qsTr("Save your changes first.")
                    : root.service.unusedAudioFiles.length === 0 ? qsTr("Inspect the saved bundle to list removable files.")
                    : qsTr("The project changed since the inspection. Inspect the saved bundle again.")
                enabled: root.service && !root.busy && !root.service.recording && !root.service.dirty && root.service.unusedAudioFiles.length > 0 && root.service.storagePreviewStamp === root.service.automationStamp
                onClicked: root.service.removeUnusedAudio(root.service.storagePreviewStamp)
            }
        }
    }
}
