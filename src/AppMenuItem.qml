import QtQuick
import QtQuick.Controls.Basic
import "ui"

// One menu row: label left, shortcut right in muted tabular type, a check
// glyph for checkable entries, a chevron for submenu titles.
//
// `actionName` is the named action (BRIEF.md section 6). The row reads its own
// binding out of the keymap, so a rebind relabels the menu and a menu can never
// disagree with the keyboard. An item with no action behind it yet is disabled
// and says so in `reason`, which becomes its tooltip -- nothing pretends.
MenuItem {
    id: mi

    // Resolved from the owning menu: a declared item never goes through the Menu's
    // delegate, so nothing else would set it. (Found by the shell suite -- every
    // menu opened with its shortcut column blank.)
    property var shell: mi.menu && mi.menu.shell ? mi.menu.shell : null
    property string actionName: ""
    // Explicit shortcut text, for entries that are not keymap actions.
    property string shortcut: ""
    // Why a disabled item is disabled. Shown as the tooltip.
    property string reason: ""
    // A second, quieter line under the label: what the entry produces, for the
    // few menus whose choices need saying (Export… : full mix or selected tracks).
    property string detail: ""
    property string helpText: ""
    // The help card's title when the entry's text is a shortened name.
    property string helpTitle: ""
    property string helpScope: ""
    property string helpTopic: ""
    readonly property alias controlHelp: help
    Accessible.description: help.description

    readonly property string binding: {
        if (mi.shortcut !== "")
            return mi.shortcut
        if (mi.actionName === "" || !mi.shell || !mi.shell.keymap)
            return ""
        const b = mi.shell.keymap.bindings
        return (b && b[mi.actionName]) ? b[mi.actionName] : ""
    }

    implicitHeight: mi.detail !== "" ? labels.implicitHeight + Theme.s2 * 2 : Theme.hRow + Theme.s1
    padding: 0
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    onTriggered: if (mi.actionName !== "" && mi.shell) mi.shell.runAction(mi.actionName)

    contentItem: Item {
        // Let the menu fit both the complete command and its current shortcut.
        // Without an intrinsic width, longer all-tracks commands are elided.
        implicitWidth: Math.max(label.implicitWidth, detailText.implicitWidth) + hint.implicitWidth + Theme.s3 + Theme.s2
                       + (mi.checkable ? Theme.szIcon : 0)
                       + (mi.subMenu ? Theme.s2 + Theme.szIcon : Theme.s3)
        Column {
            id: labels
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3 + (mi.checkable ? Theme.szIcon : 0)
            anchors.right: hint.left
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                id: label
                width: parent.width
                text: mi.text
                textFormat: Text.PlainText
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl
                color: mi.highlighted ? Theme.textPrimary : Theme.textSecondary
                elide: Text.ElideRight
            }
            Text {
                id: detailText
                width: parent.width
                visible: mi.detail !== ""
                text: mi.detail
                textFormat: Text.PlainText
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
                elide: Text.ElideRight
            }
        }
        Text {
            id: hint
            anchors.right: parent.right
            anchors.rightMargin: mi.subMenu ? Theme.s2 + Theme.szIcon : Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            text: mi.binding
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: Theme.textMuted
        }
        Icon {
            visible: mi.checkable && mi.checked
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            name: "check"; color: Theme.accent
        }
        Icon {
            visible: mi.subMenu !== null
            anchors.right: parent.right
            anchors.rightMargin: Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            name: "chevron-right"; color: Theme.textMuted
        }
    }
    indicator: null
    arrow: null
    background: Rectangle {
        anchors.fill: parent
        anchors.leftMargin: Theme.s1; anchors.rightMargin: Theme.s1
        radius: Theme.rControl
        color: mi.highlighted ? Theme.hoverOverlay : "transparent"
    }

    ControlHelp {
        id: help
        title: mi.helpTitle !== "" ? mi.helpTitle : mi.text
        purpose: mi.helpText
        scope: mi.helpScope
        topic: mi.helpTopic
        shortcut: mi.binding
        unavailableReason: mi.reason
        suppressed: mi.down
    }
}
