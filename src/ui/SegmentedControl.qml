pragma ComponentBehavior: Bound
import QtQuick

// Exclusive choice shown as one welded strip — the view-mode and split-screen
// switches in the concepts. Segments may be icons or short labels; mixing the
// two inside one control is deliberately not supported, because the two align
// on different optical centres.
Rectangle {
    id: root

    // Either a list of icon names or a list of labels.
    property var icons: []
    property var labels: []
    property int currentIndex: 0
    // Per-segment accessible names; required when using `icons`.
    property var names: []

    signal activated(int index)

    // One explanation for the whole strip; the per-segment `names` are what a
    // screen reader gets and what the help lists as the choices.
    property string helpTitle: ""
    property string helpText: ""
    property string helpScope: ""
    ControlHelp {
        title: root.helpTitle
        purpose: root.helpText + (root.names.length ? (root.helpText ? " " : "") + root.names.join(". ") + "." : "")
        scope: root.helpScope
    }

    readonly property var model: icons.length ? icons : labels
    readonly property bool iconMode: icons.length > 0

    implicitWidth: row.implicitWidth + Theme.hairline * 2
    implicitHeight: Theme.hControl
    color: Theme.controlBg
    radius: Theme.rControl
    border.width: Theme.hairline
    border.color: Theme.border

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 0

        Repeater {
            model: root.model

            delegate: Item {
                id: seg
                required property int index
                required property var modelData

                width: root.iconMode
                       ? Theme.szIconHit
                       : segLabel.implicitWidth + Theme.s3 * 2
                height: root.height - Theme.hairline * 2

                readonly property bool current: index === root.currentIndex

                Rectangle {
                    anchors.fill: parent
                    // Only the ends get the control's radius, so the strip
                    // reads as a single object rather than adjacent pills.
                    topLeftRadius: seg.index === 0 ? Theme.rControl : 0
                    bottomLeftRadius: seg.index === 0 ? Theme.rControl : 0
                    topRightRadius: seg.index === root.model.length - 1 ? Theme.rControl : 0
                    bottomRightRadius: seg.index === root.model.length - 1 ? Theme.rControl : 0
                    color: seg.current ? Theme.accent
                         : hover.hovered ? Theme.hovered(Theme.controlBg)
                         : "transparent"

                    Behavior on color {
                        ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
                    }
                }

                // Separator between unlit segments only — a divider beside the
                // lit segment would cut into its fill.
                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.hairline
                    height: parent.height - Theme.s1 * 2
                    color: Theme.border
                    visible: seg.index < root.model.length - 1
                             && !seg.current
                             && seg.index + 1 !== root.currentIndex
                }

                Icon {
                    anchors.centerIn: parent
                    visible: root.iconMode
                    name: root.iconMode ? seg.modelData : ""
                    size: Theme.szIcon
                    color: seg.current ? Theme.accentText : Theme.textSecondary
                }

                Text {
                    id: segLabel
                    anchors.centerIn: parent
                    visible: !root.iconMode
                    text: root.iconMode ? "" : seg.modelData
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    color: seg.current ? Theme.accentText : Theme.textSecondary
                }

                HoverHandler { id: hover }

                TapHandler {
                    onTapped: {
                        // The owner hears first. One that binds currentIndex to its
                        // own state has moved it by now, and writing here would
                        // replace that binding with a constant for good.
                        root.activated(seg.index)
                        if (root.currentIndex !== seg.index)
                            root.currentIndex = seg.index
                    }
                }

                Accessible.role: Accessible.RadioButton
                Accessible.name: seg.index < root.names.length ? root.names[seg.index]
                               : root.iconMode ? seg.modelData : String(seg.modelData)
                Accessible.checked: seg.current
            }
        }
    }
}
