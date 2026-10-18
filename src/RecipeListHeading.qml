import QtQuick
import "ui"

// A list heading in the Recipes tab: its title, and how many at the right.
Item {
    id: root
    property string title: ""
    property int count: 0
    implicitHeight: Theme.hRow
    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: root.title
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading
        color: Theme.textPrimary
    }
    Text {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: root.count
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
}
