import QtQuick
import QtQuick.Window
import "NumericDraft.js" as NumericDraft
import QtQuick.Controls as C

// Panel tab: uppercase label, cyan text plus a 2 px underline when active.
// Used for both the panel tab strips (PROJECT / EFFECTS / TRANSITIONS) and
// the workspace bar, which differ only in type size.
C.AbstractButton {
    id: root
    onPressed: NumericDraft.commit(root.Window.window)

    property bool uppercase: true
    property int fontSize: Theme.fsControl
    // Optional trailing count, as on the bin rows and smart collections.
    property string badge: ""
    // Room either side of the label; a crowded strip narrows it (DockHeader).
    property int sidePadding: Theme.s3
    // Where a label too long for its tab is cut (the track tabs keep both ends).
    property int elide: Text.ElideRight
    readonly property real labelWidth: label.implicitWidth

    implicitWidth: label.implicitWidth + sidePadding * 2
                   + (badge ? count.implicitWidth + Theme.s2 : 0)
    implicitHeight: Theme.hTab
    hoverEnabled: true
    // Tab reaches it; a click never takes focus — the keyboard belongs to the
    // editing shortcuts (Space, JKL) and must survive a toolbar click.
    focusPolicy: Qt.TabFocus
    // Checkable only while unchecked: a click on the current tab must not
    // switch its highlight off behind the owner's binding.
    checkable: !checked
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.PageTab
    Accessible.name: text

    background: Rectangle {
        color: root.hovered && !root.checked
               ? Theme.hoverOverlay : "transparent"

        // The active marker. Sits flush to the bottom edge of the strip so a
        // row of tabs reads as one ruled line with one lit segment.
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: Theme.activeUnderline
            color: Theme.accent
            visible: root.checked
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: root.visualFocus ? Theme.focusRing : 0
            border.color: Theme.accent
        }
    }

    // The control sizes its content to the whole tab, so the label and its count
    // are centred inside it rather than anchored: an anchor here was overridden
    // and left the label at the tab's left edge, off its hover box.
    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            spacing: Theme.s2
            anchors.centerIn: parent

            Text {
                id: label
                // Given less than it needs, the label elides rather than overlapping
                // its neighbour; its natural width is what the strip measures.
                width: root.width - root.sidePadding * 2 >= implicitWidth - 0.5 ? implicitWidth
                     : Math.max(0, root.width - root.sidePadding * 2)
                elide: root.elide
                text: root.uppercase ? root.text.toUpperCase() : root.text
                font.family: Theme.fontFamily
                font.pixelSize: root.fontSize
                font.weight: root.checked ? Theme.wHeading : Theme.wNormal
                font.letterSpacing: root.uppercase ? 0.4 : 0
                color: root.checked ? Theme.accent
                     : root.hovered ? Theme.textPrimary : Theme.textSecondary
                anchors.verticalCenter: parent.verticalCenter

                Behavior on color {
                    ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
                }
            }

            Text {
                id: count
                text: root.badge
                visible: root.badge !== ""
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
}
