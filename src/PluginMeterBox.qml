pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import "ui"

// An effect's meters in a box of their own, kept in view under its controls
// instead of below them, where a long list of controls scrolled them away.
// Many meters scroll inside the box.
Rectangle {
    id: root
    objectName: "pluginMeterBox"

    property var service: null
    property int selected: -1
    property int target: -1
    property var outputs: []
    property bool bypassed: false
    property int labelWidth: 110
    readonly property int maxHeight: 150

    visible: outputs.length > 0
    implicitHeight: visible ? Math.min(meters.implicitHeight, maxHeight) + 22 + Theme.s3 * 2 : 0
    color: Theme.panelRaised; radius: Theme.rMenu
    border.width: Theme.hairline; border.color: Theme.border

    Text {
        x: Theme.s3; y: Theme.s2
        text: qsTr("Meters")
        font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Theme.wHeading
        font.capitalization: Font.AllUppercase; font.letterSpacing: 0.8
        color: Theme.textMuted
    }
    Flickable {
        id: flick
        x: Theme.s3; y: 22 + Theme.s1
        width: root.width - Theme.s3 * 2
        height: Math.min(meters.implicitHeight, root.maxHeight)
        contentWidth: width; contentHeight: meters.implicitHeight
        clip: contentHeight > height
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar { id: meterBar }
        PluginMeters {
            id: meters
            width: flick.width - meterBar.gutter
            service: root.service
            selected: root.selected
            target: root.target
            outputs: root.outputs
            bypassed: root.bypassed
            labelWidth: root.labelWidth
        }
    }
}
