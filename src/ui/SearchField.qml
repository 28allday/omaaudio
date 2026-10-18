import QtQuick
import QtQuick.Controls as C

// Search input with a leading glyph and a clear affordance that only appears
// once there is something to clear.
FocusScope {
    id: root
    activeFocusOnTab: true
    // Escape may have left the inner field unfocused on the previous opening.
    onActiveFocusChanged: if (activeFocus && !input.activeFocus) input.forceActiveFocus(Qt.TabFocusReason)

    property alias text: input.text
    property string placeholder: qsTr("Search…")
    // Search-as-you-type is the default; set false where a query is expensive
    // and should wait for Return.
    property bool live: true

    // True while the field owns the keyboard — owners use it to stand the
    // editing shortcuts down so typing a name never blades a clip.
    readonly property bool active: input.activeFocus

    signal accepted(string text)
    signal cleared()

    property string helpTitle: placeholder.replace("…", "")
    property string helpText: ""
    property string helpScope: ""
    ControlHelp {
        title: root.helpTitle
        purpose: root.helpText
        scope: root.helpScope
        suppressed: input.activeFocus
    }

    implicitWidth: 200
    implicitHeight: Theme.hControl
    Rectangle {
        anchors.fill: parent
        color: Theme.controlBg
        radius: Theme.rControl
        border.width: Theme.hairline
        border.color: input.activeFocus ? Theme.accent : Theme.border
        Behavior on border.color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    Icon {
        id: glyph
        name: "search"
        size: Theme.szIcon
        color: Theme.textMuted
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
    }

    C.TextField {
        id: input
        focus: true
        anchors.left: glyph.right
        anchors.leftMargin: Theme.s2
        anchors.right: clear.visible ? clear.left : parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height

        placeholderText: root.placeholder
        placeholderTextColor: Theme.textMuted
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsControl
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        verticalAlignment: Text.AlignVCenter
        padding: 0
        background: null

        Accessible.role: Accessible.EditableText
        Accessible.name: root.placeholder

        onTextChanged: if (root.live) root.accepted(text)
        onAccepted: root.accepted(text)
        Keys.onEscapePressed: function (event) {
            if (text !== "") {
                text = ""
                root.cleared()
            } else {
                focus = false
                event.accepted = false
            }
        }
    }

    IconButton {
        id: clear
        iconName: "x"
        text: qsTr("Clear search")
        visible: input.text !== ""
        implicitWidth: Theme.szIcon + Theme.s1 * 2
        implicitHeight: Theme.szIcon + Theme.s1 * 2
        anchors.right: parent.right
        anchors.rightMargin: Theme.s1
        anchors.verticalCenter: parent.verticalCenter
        onClicked: {
            input.text = ""
            input.forceActiveFocus()
            root.cleared()
        }
    }
}
