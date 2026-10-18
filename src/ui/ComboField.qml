pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import "NumericDraft.js" as NumericDraft
import QtQuick.Controls as C

// Dropdown. Styled rather than themed, because the Material combo brings its
// own ripple, elevation and radius that all fight the brief.
C.ComboBox {
    id: root
    onPressedChanged: if (pressed) NumericDraft.commit(root.Window.window)

    // Optional leading caption rendered inside the control, for the dense
    // Render Settings rows where a separate label column would waste width.
    property string caption: ""

    property string helpTitle: caption
    property string helpText: ""
    property string helpScope: ""
    property string helpTopic: ""
    property string unavailableReason: ""
    property string shortcut: ""
    readonly property alias controlHelp: help

    // A click never takes the keyboard from the editing shortcuts; Tab still reaches it.
    focusPolicy: Qt.TabFocus

    implicitWidth: 140
    implicitHeight: Theme.hControl
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.ComboBox
    Accessible.name: helpTitle
    Accessible.description: help.description

    background: Rectangle {
        color: root.pressed ? Theme.pressedOn(Theme.controlBg)
             : root.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
        radius: Theme.rControl
        border.width: root.visualFocus ? Theme.focusRing : Theme.hairline
        border.color: root.visualFocus ? Theme.accent : Theme.border

        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    contentItem: Row {
        leftPadding: Theme.s2
        spacing: Theme.s2

        Text {
            id: captionLabel
            text: root.caption
            textFormat: Text.PlainText
            visible: root.caption !== ""
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.displayText
            textFormat: Text.PlainText
            font: root.font
            color: Theme.textPrimary
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, root.width - root.indicator.width - Theme.s2 * 3
                   - (captionLabel.visible ? captionLabel.implicitWidth + Theme.s2 : 0))
        }
    }

    indicator: Icon {
        name: "chevron-down"
        size: Theme.szIcon
        color: Theme.textMuted
        x: root.width - width - Theme.s2
        y: root.topPadding + (root.availableHeight - height) / 2
    }

    delegate: C.ItemDelegate {
        id: option
        required property int index
        required property var modelData

        // A model of objects ({text, header}) can carry section headings: shown
        // small and muted, never chosen. A model of strings works as before.
        readonly property bool heading: modelData !== null && typeof modelData === "object" && modelData.header === true
        readonly property string label: modelData !== null && typeof modelData === "object"
                                        ? (modelData[root.textRole || "text"] || "") : String(modelData)
        objectName: heading ? "comboHeading" : "comboOption"
        enabled: !heading
        opacity: 1
        width: root.width
        height: heading ? Theme.hRow + Theme.s1 : Theme.hRow + Theme.s1 * 2
        topPadding: heading ? Theme.s2 : undefined
        bottomPadding: heading ? 0 : undefined
        highlighted: !heading && root.highlightedIndex === index

        contentItem: Text {
            text: option.heading ? option.label.toUpperCase() : option.label
            textFormat: Text.PlainText
            font.family: Theme.fontFamily
            font.pixelSize: option.heading ? Theme.fsLabel - 1 : Theme.fsControl
            font.letterSpacing: option.heading ? 0.6 : 0
            color: option.heading ? Theme.textMuted : option.highlighted ? Theme.textPrimary : Theme.textSecondary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: option.highlighted ? Theme.hoverOverlay : "transparent"
        }
    }

    popup: C.Popup {
        y: root.height
        width: root.width
        implicitHeight: Math.min(contentItem.implicitHeight + Theme.s1 * 2, 320)
        padding: Theme.s1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollIndicator.vertical: C.ScrollIndicator {}
        }

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.rMenu
            border.width: Theme.hairline
            border.color: Theme.border
        }

        enter: Transition {
            NumberAnimation {
                property: "opacity"; from: 0; to: 1
                duration: Theme.dFast; easing.type: Theme.easing
            }
        }
        exit: Transition {
            NumberAnimation {
                property: "opacity"; from: 1; to: 0
                duration: Theme.dFast; easing.type: Theme.easing
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
        suppressed: root.pressed || root.popup.visible
    }
}
