import QtQuick
import QtQuick.Controls as C

// Thin overlay scrollbar. Stays out of the way in a dense layout: a hairline
// at rest, thickening on hover so it can actually be grabbed.
C.ScrollBar {
    id: root

    padding: 2
    // Nothing to scroll: not there at all. Invisible but present, it took the
    // presses meant for whatever lies under its strip, such as a fade handle
    // or a selection drag at a lane's right edge.
    visible: policy === C.ScrollBar.AlwaysOn || size < 1.0
    // A hairline stays at rest whenever there is more to scroll to, so a list
    // that runs past its panel says so before anyone hovers it.
    property bool showAtRest: true
    // The room a list leaves beside this bar while it is there, so nothing sits
    // under it: its widest, hovered, with a hairline to spare. Lay a list's
    // content out at its width minus this.
    readonly property int gutter: visible ? Theme.s3 : 0
    minimumSize: 0.06

    contentItem: Rectangle {
        implicitWidth: root.hovered || root.pressed ? 8 : 4
        implicitHeight: root.hovered || root.pressed ? 8 : 4
        radius: width / 2
        color: root.pressed ? Theme.textSecondary
             : root.hovered ? Theme.textMuted : Theme.borderStrong
        opacity: root.policy === C.ScrollBar.AlwaysOn || root.active ? 1.0
               : root.showAtRest && root.size < 1.0 ? 0.7 : 0.0

        Behavior on implicitWidth {
            NumberAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
        Behavior on implicitHeight {
            NumberAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
        Behavior on opacity {
            NumberAnimation { duration: Theme.dNormal; easing.type: Theme.easing }
        }
        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    background: Rectangle {
        color: Theme.windowBg
        opacity: root.hovered || root.pressed ? 0.6 : 0.0

        Behavior on opacity {
            NumberAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }
}
