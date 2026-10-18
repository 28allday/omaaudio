pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C

// The existing shared tooltip view, retained for simple-label callers too.
C.ToolTip {
    id: root
    property string shortcut: ""
    property string purpose: ""
    property string scope: ""
    property string reason: ""
    property string helpHint: ""
    property real maximumWidth: 380
    property real textScale: Help.textScale
    delay: Theme.tooltipDelay
    padding: Theme.s2
    margins: Theme.s2
    focus: false
    closePolicy: C.Popup.NoAutoClose
    width: Math.min(maximumWidth, 380)

    contentItem: Column {
        id: body
        spacing: Theme.s1
        Repeater {
            model: [root.text, root.purpose, root.scope, root.reason,
                    root.shortcut ? qsTr("Shortcut: %1").arg(root.shortcut) : "", root.helpHint]
            delegate: Text {
                required property string modelData
                required property int index
                width: body.width
                text: modelData
                textFormat: Text.PlainText
                visible: text !== ""
                wrapMode: Text.Wrap
                maximumLineCount: index === 0 ? 3 : 4
                elide: Text.ElideRight
                font.family: index === 4 ? Theme.monoFamily : Theme.fontFamily
                font.pixelSize: Theme.fsControl * root.textScale
                font.bold: index === 0
                color: index === 3 ? Theme.warning : index > 3 ? Theme.textSecondary : Theme.textPrimary
            }
        }
    }
    background: Rectangle {
        color: Theme.panelRaised
        radius: Theme.rMenu
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dFast } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.dFast } }
}
