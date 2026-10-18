import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic as C
import "ui"

// A line of accent text that goes somewhere else ("Process many files with a
// recipe ›"): quieter than a button, for a way out of the tab.
C.AbstractButton {
    id: root
    property string helpText: ""
    property string helpScope: ""
    property bool chevron: true
    // Quiet: secondary text that turns to the accent under the pointer, for a
    // list of choices (the step picker) rather than one way out.
    property bool quiet: false
    // Muted text after the label, such as the " · " between choices.
    property string trailing: ""

    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    implicitWidth: line.implicitWidth + leftPadding + rightPadding
    implicitHeight: Theme.hRow
    opacity: enabled ? 1.0 : Theme.disabledOpacity
    Accessible.role: Accessible.Link
    Accessible.name: text
    Accessible.description: help.description

    background: null
    contentItem: Row {
        id: line
        Text {
            height: root.availableHeight
            text: root.text + (root.chevron ? "  ›" : "")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignVCenter
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            font.underline: (root.hovered || root.visualFocus) && root.enabled
            color: !root.quiet || (root.hovered && root.enabled) ? Theme.accent : Theme.textSecondary
        }
        Text {
            height: root.availableHeight
            visible: root.trailing !== ""
            text: root.trailing
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignVCenter
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }
    ControlHelp {
        id: help
        title: root.text
        purpose: root.helpText
        scope: root.helpScope
        suppressed: root.down
    }
}
