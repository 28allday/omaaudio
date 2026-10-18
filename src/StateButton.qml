import QtQuick
import "ui"

// The small square M / S / R toggle a lane header and a mixer strip share. One
// definition, so the two can never drift apart in size or colour.
Rectangle {
    id: root

    property string label: ""
    property bool active: false
    property color activeColor: Theme.accent
    property color activeText: Theme.accentText
    property string tip: ""
    property string helpText: ""
    property string helpScope: ""

    signal clicked()

    width: 22
    height: 18
    radius: Theme.rControl
    color: active ? activeColor
         : hover.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
    border.width: Theme.hairline
    border.color: active ? activeColor : Theme.border
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Text {
        anchors.centerIn: parent
        text: root.label
        color: root.active ? root.activeText : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        font.weight: Theme.wHeading
    }
    HoverHandler { id: hover }
    TapHandler { enabled: root.enabled; onTapped: root.clicked() }
    Accessible.role: Accessible.Button
    Accessible.name: root.tip
    ControlHelp {
        title: root.tip
        purpose: root.helpText
        scope: root.helpScope
    }
}
