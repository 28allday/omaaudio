pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The four workspaces, centred in the title bar. Order is the brief's: the
// single file, the session, the frequency domain, the delivery.
Row {
    id: root

    readonly property var workspaces: ["Waveform", "Multitrack", "Spectral", "Mastering"]
    property string current: "Multitrack"

    signal activated(string name)

    spacing: 0

    Repeater {
        model: root.workspaces

        delegate: TabButton {
            required property string modelData

            objectName: "workspaceTab_" + modelData
            text: Pages.label(modelData)
            Accessible.description: workspaceHelp.description
            ControlHelp {
                id: workspaceHelp
                title: Pages.label(parent.modelData)
                purpose: parent.modelData === "Waveform" ? qsTr("Edit a active track: select, cut, trim, change level and work with its channels.")
                       : parent.modelData === "Multitrack" ? qsTr("Arrange and mix tracks together on the timeline, with recording, effects and automation.")
                       : parent.modelData === "Spectral" ? qsTr("Find and repair noise by selecting where it occurs in time and frequency.")
                       : qsTr("Measure the complete mix, set its delivery level and export an audio file.")
                scope: qsTr("Switching workspace keeps the same project and edits.")
            }
            height: root.height
            checked: root.current === modelData
            fontSize: Theme.fsControl
            // Controlled: the owner decides and binds `current` back, so a
            // programmatic switch (shortcut, restore) and a click agree.
            onClicked: root.activated(modelData)
        }
    }
}
