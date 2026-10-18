import QtQuick
import "ui"

// A named slider in the Sound tab, with a line of plain words and words for
// its two ends. `committed` fires when a drag ends or a value is typed.
Column {
    id: root
    property string title: ""
    property string hint: ""
    property string lowText: ""
    property string highText: ""
    property real from: 0
    property real to: 100
    property int decimals: 0
    property string suffix: ""
    property real value: 0
    signal committed(real value)

    width: parent ? parent.width : 0
    spacing: Theme.s1
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Text {
        text: root.title
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.weight: Font.Medium
        color: Theme.textPrimary
    }
    Text {
        width: parent.width
        wrapMode: Text.WordWrap
        text: root.hint
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
    }
    SliderField {
        id: slider
        objectName: root.objectName + "Slider"
        width: parent.width
        labelWidth: 0
        readoutWidth: 76
        enabled: root.enabled
        from: root.from; to: root.to; decimals: root.decimals; suffix: root.suffix
        value: root.value
        helpTitle: root.title
        helpText: root.hint
        onEditingFinished: function (v) { if (Math.abs(v - root.value) > Math.pow(10, -root.decimals) / 2) root.committed(v) }
    }
    Item {
        width: parent.width
        height: Theme.fsLabel + Theme.s1
        visible: root.lowText !== "" || root.highText !== ""
        Text { text: root.lowText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        Text {
            anchors.right: parent.right; anchors.rightMargin: slider.readoutWidth + Theme.s2
            text: root.highText
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
        }
    }
}
