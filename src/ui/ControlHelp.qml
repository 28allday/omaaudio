pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Passive adapter, also usable on a meter. Never changes enabled or accepts
// a click. The owner exposes this as controlHelp for keyboard focus lookup.
//
// Declared inside an ordinary Item it fills its owner and watches hover
// itself. Declared inside a positioner (Row, Column, Grid, Flow) it must not
// take anchors or size, so it stays zero-sized and puts its hover handler on
// the owner instead. Either way the owner is `parent` unless told otherwise.
Item {
    id: root
    property Item owner: parent
    readonly property bool positioned: parent !== null
        && (parent instanceof Row || parent instanceof Column || parent instanceof Grid || parent instanceof Flow
            || parent instanceof RowLayout || parent instanceof ColumnLayout || parent instanceof GridLayout)
    anchors.fill: positioned ? undefined : parent
    width: positioned ? 0 : implicitWidth
    height: positioned ? 0 : implicitHeight
    property string title: ""
    property string purpose: ""
    property string scope: ""
    property string topic: ""
    property string shortcut: ""
    property string unavailableReason: ""
    property bool suppressed: false
    readonly property bool available: owner !== null && owner.visible && owner.width > 0 && owner.height > 0 && title !== ""
    readonly property string reason: owner && !owner.enabled ? unavailableReason : ""
    readonly property string description: [purpose, scope, reason,
        shortcut ? qsTr("Shortcut: %1").arg(shortcut) : ""].filter(part => part !== "").join("\n\n")
    onSuppressedChanged: if (suppressed) Help.leave(root)
    onAvailableChanged: if (!available) Help.leave(root)
    Component.onCompleted: {
        Help.registerControl(root)
        if (positioned && owner) ownerHover.createObject(owner)
    }
    Component.onDestruction: Help.unregisterControl(root)
    HoverHandler {
        // Qt 6 observes hover even when the owning action is disabled.
        enabled: !root.positioned
        onHoveredChanged: {
            if (hovered) Help.request(root)
            else Help.leave(root)
        }
    }
    Component {
        id: ownerHover
        HoverHandler {
            onHoveredChanged: {
                if (hovered) Help.request(root)
                else Help.leave(root)
            }
        }
    }
}
