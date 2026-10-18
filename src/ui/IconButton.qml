import QtQuick
import QtQuick.Window
import "NumericDraft.js" as NumericDraft
import QtQuick.Controls as C

// Icon-only button. Because it carries no visible label it MUST be given a
// `text` — that is what the tooltip shows and what a screen reader announces.
C.AbstractButton {
    id: root
    onPressed: NumericDraft.commit(root.Window.window)

    // `text` is the accessible name and tooltip title; `shortcut` is appended
    // to the tooltip as the brief requires ("Blade Tool  B").
    property string iconName: ""
    property string shortcut: ""

    property string helpTitle: text
    property string helpText: ""
    property string helpScope: ""
    property string helpTopic: ""
    property string unavailableReason: ""
    readonly property alias controlHelp: help

    property color iconColor: checked ? Theme.accent : Theme.textSecondary
    // Flat buttons carry no chrome until hovered — used in dense toolbars
    // where a grid of filled boxes would read as noise.
    property bool flat: true

    implicitWidth: Theme.szIconHit
    implicitHeight: Theme.szIconHit
    hoverEnabled: true
    // Tab reaches it; a click never takes focus — the keyboard belongs to the
    // editing shortcuts (Space, JKL) and must survive a toolbar click.
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: help.description

    background: Rectangle {
        radius: Theme.rControl
        color: {
            if (root.checked)
                return root.hovered && !root.down ? Theme.selectedHover : Theme.selectedFill
            if (root.flat)
                return root.down ? Theme.pressOverlay : root.hovered ? Theme.hoverOverlay : "transparent"
            return root.down ? Theme.pressedOn(Theme.controlBg) : root.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
        }
        border.width: root.visualFocus ? Theme.focusRing : 0
        border.color: Theme.accent

        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    // AbstractButton stretches contentItem to fill, so the glyph is centred
    // inside a filling wrapper rather than being the contentItem itself.
    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: root.iconName
            size: Theme.szIcon
            color: root.iconColor
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
