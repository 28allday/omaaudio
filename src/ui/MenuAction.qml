import QtQuick
import QtQuick.Controls as C

// One menu row: optional leading check or icon, label, right-aligned shortcut.
// The shortcut column is what makes a menu teach its own keyboard equivalents,
// so it is never omitted where one exists.
C.MenuItem {
    id: root

    property string iconName: ""
    property string shortcut: ""

    implicitHeight: Theme.hRow + Theme.s1 * 2
    implicitWidth: Theme.s3 * 2 + Theme.szIcon + Theme.s2
                   + label.implicitWidth + Theme.s5 + hint.implicitWidth
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.name: text

    indicator: Item {
        width: Theme.szIcon
        height: parent.height
        x: Theme.s2

        Icon {
            anchors.centerIn: parent
            name: root.checkable ? (root.checked ? "check" : "") : root.iconName
            size: Theme.szIcon
            color: root.checked ? Theme.accent : Theme.textSecondary
        }
    }

    contentItem: Item {
        anchors.fill: parent

        Text {
            id: label
            text: root.text
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2 + Theme.szIcon + Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: root.highlighted ? Theme.textPrimary : Theme.textSecondary
            elide: Text.ElideRight
        }

        Text {
            id: hint
            text: root.shortcut
            visible: root.shortcut !== ""
            anchors.right: parent.right
            anchors.rightMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }

    background: Rectangle {
        radius: Theme.rControl
        color: root.highlighted ? Theme.hoverOverlay : "transparent"
    }
}
