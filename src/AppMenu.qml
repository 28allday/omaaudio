import QtQuick
import QtQuick.Controls.Basic
import "ui"

// A dropdown menu in the Oma chrome. Entries are AppMenuItems, which resolve
// their shortcut from the keymap; the delegate here only styles submenu titles.
//
// A closing menu leaves keyboard focus with the menu bar, and a focused
// MenuBarItem eats Space to open itself -- so one trip through File and the
// keymap went deaf, starting with play. The shell's editor keys live on one
// item; every menu hands focus back there on close.
Menu {
    id: m

    // The shell (Main.qml): supplies the keymap and the focus target.
    property var shell: null

    delegate: AppMenuItem { shell: m.shell; helpText: qsTr("Open the %1 submenu.").arg(text) }
    padding: Theme.s1
    // Basic's ListView has no intrinsic width. Size the popup from the rows so
    // full labels and re-bound shortcuts fit instead of using the background alone.
    implicitWidth: {
        let widest = 236
        for (let i = 0; i < count; ++i) {
            const row = itemAt(i)
            if (row)
                widest = Math.max(widest, row.implicitWidth + leftPadding + rightPadding)
        }
        return widest
    }

    background: Rectangle {
        implicitWidth: 236
        color: Theme.panelRaised
        border.width: Theme.hairline
        border.color: Theme.border
        radius: Theme.rMenu
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dFast; easing.type: Theme.easing }
    }

    onClosed: if (shell) shell.restoreFocus()
}
