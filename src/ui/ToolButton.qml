import QtQuick
import QtQuick.Window
import "NumericDraft.js" as NumericDraft
import QtQuick.Controls as C

// Toolbar button that may carry an icon, a label, or both. Checkable when it
// represents a mode (the timeline's select/blade/slip tools) rather than an
// action; a checked tool shows a filled well plus a cyan glyph so the active
// mode is readable at a glance across a 38 px strip.
C.AbstractButton {
    id: root
    onPressed: NumericDraft.commit(root.Window.window)

    property string iconName: ""
    property string shortcut: ""

    property string helpTitle: text
    property string helpText: ""
    property string helpScope: ""
    property string helpTopic: ""
    property string unavailableReason: ""
    readonly property alias controlHelp: help

    property bool showLabel: text !== "" && iconName === ""
    // The one action a page exists for (Mastering's Export…): filled with the
    // accent so it reads as the way out, not as another toolbar setting.
    property bool prominent: false

    // A plain labelled button keeps its label on the text column it sits in
    // (the docks line their buttons up with their headings): its hover box
    // reaches out one step to the left instead, so the label is still in the
    // middle of it. Prominent and icon-only buttons centre their content.
    readonly property bool flush: !prominent && showLabel
    // A flush button's box is its width plus the step it reaches left, so the
    // icon-button minimum would only push its label off the middle of the box.
    implicitWidth: flush ? row.implicitWidth + Theme.s2
                         : Math.max(Theme.szIconHit, row.implicitWidth + (prominent ? Theme.s3 * 2 : Theme.s2 * 2))
    leftInset: flush ? -Theme.s2 : 0
    implicitHeight: Theme.hControl
    hoverEnabled: true
    // Tab reaches it; a click never takes focus — the keyboard belongs to the
    // editing shortcuts (Space, JKL) and must survive a toolbar click.
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: checkable ? Accessible.CheckBox : Accessible.Button
    Accessible.name: text
    Accessible.description: help.description

    background: Rectangle {
        radius: Theme.rControl
        color: {
            if (root.prominent)
                return root.down ? Theme.accentPressed
                     : root.hovered ? Theme.hovered(Theme.accent) : Theme.accent
            if (root.checked)
                return root.down ? Theme.selectedFill : root.hovered ? Theme.selectedHover : Theme.selectedFill
            if (root.down)
                return Theme.pressOverlay
            if (root.hovered)
                return Theme.hoverOverlay
            return "transparent"
        }
        border.width: root.visualFocus ? Theme.focusRing : 0
        border.color: root.prominent ? Theme.textPrimary : Theme.accent

        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    // A prominent button has its label in the middle however wide a layout
    // makes it, and an icon-only one its icon; a plain labelled one starts at
    // its left edge (see `flush`).
    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            spacing: Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            x: root.flush ? 0 : Math.max(0, Math.round((parent.width - width) / 2))

            Icon {
                name: root.iconName
                visible: root.iconName !== ""
                size: Theme.szIcon
                color: root.prominent ? Theme.accentText : root.checked ? Theme.accent : Theme.textSecondary
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                text: root.text
                visible: root.showLabel
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl
                font.weight: root.prominent ? Font.DemiBold : Font.Normal
                color: root.prominent ? Theme.accentText : root.checked ? Theme.accent : Theme.textSecondary
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    ControlHelp {
        id: help
        title: root.helpTitle
        purpose: root.helpText
        scope: root.helpScope
        topic: root.helpTopic
        shortcut: root.shortcut
        unavailableReason: root.unavailableReason
        suppressed: root.down
    }
}
