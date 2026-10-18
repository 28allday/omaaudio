import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// A panel's scrolling column. Its text column stays where a plain layout would
// put it, but the pane reaches past it on both sides, into the panel's margin:
// on the left so a plain labelled button's hover box (ToolButton.flush) is not
// cut off by the clip, on the right so the scrollbar sits in the margin rather
// than over the controls. Put the content at `x: scroll.reachLeft` and
// `width: scroll.columnWidth`. In a layout the pane's reach is taken care of
// here; anchored or as a popup's contentItem, the owner gives it the room.
C.ScrollView {
    id: root
    readonly property int reachLeft: Theme.s2
    readonly property int reachRight: Theme.s3
    readonly property real columnWidth: Math.max(0, availableWidth - reachLeft)

    clip: true
    // The list inside clips at its own edges, so the room on the left is inside
    // it (the content starts at reachLeft) and the room on the right outside.
    leftPadding: 0
    rightPadding: reachRight
    contentWidth: availableWidth
    Layout.leftMargin: -reachLeft
    Layout.rightMargin: -reachRight

    C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
    C.ScrollBar.vertical: ScrollBar {
        parent: root
        x: root.mirrored ? 0 : root.width - width
        y: root.topPadding
        height: root.availableHeight
    }
}
