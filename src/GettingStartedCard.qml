import QtQuick
import "ui"

// The welcome card over an empty Multitrack session: the three ways a session
// begins, the guides, and a way to never see it again. Help › Getting Started
// brings it back. Quiet like EmptyState, but it offers the next useful action
// rather than describing the absence of one.
Item {
    id: root
    objectName: "gettingStartedCard"

    property var shell: null

    implicitWidth: 320
    implicitHeight: column.implicitHeight

    Column {
        id: column
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 440)
        spacing: Theme.s3

        Icon {
            name: "audio-lines"
            size: 28
            color: Theme.textMuted
            opacity: 0.7
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Text {
            objectName: "gettingStartedTitle"
            text: qsTr("Welcome to OmaAudio")
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
        }

        Text {
            text: qsTr("Open an audio file, add one to your project, or record a new sound. Your original files stay unchanged. Undo takes back edits. Hold the pointer over a control for help.")
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            wrapMode: Text.WordWrap
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.s2

            ToolButton {
                objectName: "startOpenAudio"
                iconName: "folder-open"; text: qsTr("Open Audio…"); showLabel: true
                shortcut: root.shell ? root.shell.bindingFor("open_file") : ""
                helpText: qsTr("Choose an audio file to begin a new project from.")
                helpScope: qsTr("The file is read into the project; the file itself is not changed.")
                onClicked: if (root.shell) root.shell.runAction("open_file")
            }
            ToolButton {
                objectName: "startImportTrack"
                iconName: "file-plus"; text: qsTr("Import as Track…"); showLabel: true
                shortcut: root.shell ? root.shell.bindingFor("import_file") : ""
                helpText: qsTr("Choose an audio file to add to this project as a new track.")
                helpScope: qsTr("Anything already in the project stays where it is.")
                onClicked: if (root.shell) root.shell.runAction("import_file")
            }
            ToolButton {
                objectName: "startRecord"
                iconName: "circle"; text: qsTr("Record…"); showLabel: true
                shortcut: root.shell ? root.shell.bindingFor("record") : ""
                helpText: qsTr("Open the Recording panel to capture a take from an input device.")
                helpScope: qsTr("Recording starts only when you press Record in the panel.")
                onClicked: if (root.shell) root.shell.runAction("record")
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.s2

            ToolButton {
                objectName: "startGuides"
                iconName: "sparkles"; text: qsTr("Guides"); showLabel: true
                helpText: qsTr("Short local walkthroughs: importing, recording, editing, effects, spectral repair, saving and exporting.")
                onClicked: if (root.shell) root.shell.showGettingStarted()
            }
            ToolButton {
                objectName: "startDismiss"
                text: qsTr("Don't show this again"); showLabel: true
                helpText: qsTr("Hide this card. Help › Getting Started shows it again.")
                onClicked: if (root.shell) root.shell.gettingStartedDismissed = true
            }
        }
    }
}
