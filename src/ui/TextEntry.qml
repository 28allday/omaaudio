import QtQuick
import QtQuick.Controls as C

C.TextField {
    id: root
    // Help, as every control carries it: the title names the field, the text
    // says what to type and what it does.
    property string helpTitle: ""
    property string helpText: ""
    property string helpScope: ""
    property string helpTopic: ""
    property string unavailableReason: ""
    Accessible.name: helpTitle
    Accessible.description: help.description
    ControlHelp {
        id: help
        title: root.helpTitle
        purpose: root.helpText
        scope: root.helpScope
        topic: root.helpTopic
        unavailableReason: root.unavailableReason
    }
    implicitWidth: 160
    implicitHeight: Theme.hControl
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    color: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    verticalAlignment: Text.AlignVCenter
    selectByMouse: true
    padding: 0
    leftPadding: Theme.s2
    rightPadding: Theme.s2
    background: Rectangle {
        color: Theme.controlBg
        radius: Theme.rControl
        border.width: Theme.hairline
        border.color: root.activeFocus ? Theme.accent : Theme.border
    }
}
