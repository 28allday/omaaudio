import QtQuick
import "ui"

// One section of the Sound tab: a title, a short summary on the right of it,
// and its rows underneath.
Column {
    id: root
    property string title: ""
    property string summary: ""
    default property alias rows: content.data

    width: parent ? parent.width : 0
    spacing: Theme.s2

    Item {
        width: parent.width
        height: Theme.hRow
        Text {
            id: heading
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        Text {
            anchors.left: heading.right; anchors.leftMargin: Theme.s3
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignRight
            text: root.summary
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }
    Column {
        id: content
        width: parent.width
        spacing: Theme.s3
    }
    Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
}
