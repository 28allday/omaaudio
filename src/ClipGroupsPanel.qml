pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

C.Popup {
    id: root
    objectName: "clipGroupsPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var groups: []
    property string stamp: ""
    property string message: ""
    readonly property var currentGroup: chooser.currentIndex > 0 ? groups[chooser.currentIndex - 1] : null
    readonly property int selectedCount: service ? service.selectedClips.length : 0
    readonly property bool editable: service && !service.recording && !service.loading && !service.sampleEditing && !service.clipMoving
    function showGroup() {
        name.text = currentGroup ? currentGroup.name : ""
        colour.text = currentGroup ? currentGroup.colour : ""
        message = ""
    }
    function reload() {
        groups = service ? service.clipGroups : []
        stamp = service ? service.automationStamp : ""
        chooser.currentIndex = 0
        const selected = service ? service.selectedClips : []
        if (selected.length && selected[0].group >= 0) {
            for (let i = 0; i < groups.length; ++i)
                if (groups[i].id === selected[0].group) { chooser.currentIndex = i + 1; break }
        }
        showGroup()
    }
    function complete(ok) {
        if (ok) close()
        else message = service.lastError
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(600, parent ? parent.width - 32 : 600)
    height: Math.min(460, parent ? parent.height - 40 : 460)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: reload()
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Clip groups"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton { iconName: "x"; text: qsTr("Close"); onClicked: root.close(); helpText: qsTr("Close without applying the fields shown.") }
        }
        ScrollPane {
            id: scroll; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
            ColumnLayout {
                x: scroll.reachLeft; width: scroll.columnWidth; spacing: Theme.s3
                Toggle {
                    objectName: "clipGroupsLink"; Layout.fillWidth: true
                    label: qsTr("Link group selection"); checked: root.service ? root.service.clipGroupsLinked : true
                    onClicked: if (root.service) root.service.setClipGroupsLinked(!root.service.clipGroupsLinked)
                    helpText: qsTr("Select group members together for moving, copying, cutting and deleting whole clips.")
                    helpScope: qsTr("Turn off to select individual members. Membership stays saved; New and Open turn linking back on. Time-range edits and individual clip properties keep their own scope.")
                }
                Text {
                    objectName: "clipGroupsSelection"; Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: root.selectedCount === 1 ? qsTr("1 whole clip selected. Ctrl-click clip labels to change the selection.")
                        : qsTr("%1 whole clips selected. Ctrl-click clip labels to change the selection.").arg(root.selectedCount)
                    color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                }
                ComboField {
                    id: chooser; objectName: "clipGroupsChooser"; Layout.fillWidth: true
                    model: [qsTr("New group from selected clips")].concat(root.groups.map(group => group.name + " · " + group.memberCount))
                    helpTitle: qsTr("Clip group"); helpText: qsTr("Create a group from the selected clips, or choose a saved group to rename, recolour or dissolve.")
                    helpScope: qsTr("Empty groups are kept until dissolved. Choosing a group here does not change your timeline selection.")
                    onActivated: root.showGroup()
                }
                RowLayout {
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Name"); color: Theme.textSecondary; Layout.preferredWidth: 80 }
                    TextEntry {
                        id: name; objectName: "clipGroupsName"; Layout.fillWidth: true; selectByMouse: true
                        helpTitle: qsTr("Group name"); helpText: qsTr("A name for related clips, shown in their existing labels. Up to 512 UTF-8 bytes.")
                    }
                }
                RowLayout {
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Colour"); color: Theme.textSecondary; Layout.preferredWidth: 80 }
                    TextEntry {
                        id: colour; objectName: "clipGroupsColour"; Layout.fillWidth: true; selectByMouse: true
                        placeholderText: qsTr("Track colour, or #RRGGBB")
                        validator: RegularExpressionValidator { regularExpression: /^(#[0-9a-fA-F]{6})?$/ }
                        helpTitle: qsTr("Group colour"); helpText: qsTr("Optional label and outline colour as #RRGGBB. Leave empty to keep each track's colour.")
                        helpScope: qsTr("Display only. Original audio is unchanged.")
                    }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    text: root.currentGroup ? qsTr("%1 members. Dissolving removes their link and the saved group; the audio stays in place.").arg(root.currentGroup.memberCount)
                        : qsTr("Create from at least two whole clips. Selected clips leave any previous group; unselected members keep their existing link.")
                }
                Text {
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    objectName: "clipGroupsMessage"; Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
                    visible: text !== ""; text: root.message; color: Theme.warning
                }
                RowLayout {
                    ToolButton {
                        objectName: "clipGroupsReload"; text: qsTr("Reload"); showLabel: true; onClicked: root.reload()
                        helpText: qsTr("Read the current groups again and discard the fields shown.")
                    }
                    Item { Layout.fillWidth: true }
                    ToolButton {
                        objectName: "clipGroupsApply"; text: root.currentGroup ? qsTr("Apply properties") : qsTr("Create group"); showLabel: true
                        enabled: root.editable && name.text.length > 0 && colour.acceptableInput && (root.currentGroup !== null || root.selectedCount >= 2)
                        helpText: qsTr("Save the name, colour and membership as one Undo step.")
                        unavailableReason: qsTr("Enter a name and valid colour, select at least two clips for a new group, and finish any current edit.")
                        onClicked: root.complete(root.currentGroup
                            ? root.service.updateClipGroup(root.currentGroup.id, name.text, colour.text, root.stamp)
                            : root.service.createClipGroup(name.text, colour.text, root.stamp))
                    }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Theme.hairline; color: Theme.border }
                ToolButton {
                    objectName: "clipGroupsDetach"; text: qsTr("Detach selected clips"); showLabel: true
                    enabled: root.editable && root.service.selectedClips.some(clip => clip.group >= 0)
                    helpText: qsTr("Remove just the selected clips from their groups. Audio stays in place. One Undo step.")
                    helpScope: qsTr("To choose individual members, close this panel, turn off Link group selection, then select the clips.")
                    onClicked: root.complete(root.service.detachSelectedClips(root.stamp))
                }
                ToolButton {
                    objectName: "clipGroupsDissolve"; text: qsTr("Dissolve this group"); showLabel: true
                    enabled: root.editable && root.currentGroup !== null
                    helpText: qsTr("Remove the chosen group and all its links. Keep every clip and its audio. One Undo step.")
                    onClicked: root.complete(root.service.dissolveClipGroup(root.currentGroup.id, root.stamp))
                }
            }
        }
    }
}
