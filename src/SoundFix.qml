import QtQuick
import "ui"

// One fix in the Sound tab: a switch, its name, a line of plain words and one
// slider (or, for mains hum, the mains frequency). The panel owns the
// settings; this row only shows one of them and asks the panel to change it.
Column {
    id: root
    property var panel: null
    property string fixId: ""
    property string hint: ""
    property string lowText: ""
    property string highText: ""
    readonly property var fix: panel ? panel.fix(fixId) : null
    readonly property bool available: fix !== null && fix.available === true
    readonly property bool on: fix !== null && fix.on === true
    readonly property real percent: fix && fix.scale ? fix.scale : 100

    objectName: "soundFix_" + fixId
    width: parent ? parent.width : 0
    spacing: Theme.s1
    visible: fix !== null

    Item {
        width: parent.width
        height: Theme.hRow
        Toggle {
            id: toggle
            objectName: "soundFixToggle_" + root.fixId
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            checked: root.on
            enabled: root.available
            helpTitle: root.fix ? root.fix.title : ""
            helpText: root.hint
            unavailableReason: root.fix && !root.available ? (root.fix.reason || "") : ""
            onToggled: { root.panel.change({ [root.fixId]: { on: checked } }); checked = Qt.binding(() => root.on) }
        }
        Text {
            anchors.left: toggle.right; anchors.leftMargin: Theme.s2
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: root.fix ? root.fix.title : ""
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.weight: Font.Medium
            color: root.available ? Theme.textPrimary : Theme.textMuted
        }
    }
    Text {
        x: toggle.width + Theme.s2
        width: parent.width - x
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        text: root.fix && !root.available && root.fix.reason ? root.fix.reason : root.hint
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
    // Away from 48 kHz the noise fix is applied to a selection instead.
    ToolButton {
        x: toggle.width + Theme.s2
        visible: root.fixId === "noise" && root.fix !== null && root.fix.live === false
        text: qsTr("Voice Cleanup…")
        iconName: "sparkles"; showLabel: true
        helpText: qsTr("Reduce background noise in the selected audio. Written into the audio, one Undo step.")
        onClicked: if (root.panel.shell) root.panel.shell.runAction("show_voice_cleanup")
    }
    SegmentedControl {
        objectName: "soundFixMains_" + root.fixId
        x: toggle.width + Theme.s2
        visible: root.fix !== null && root.fix.choice === true
        enabled: root.available && root.on
        labels: [qsTr("50 Hz"), qsTr("60 Hz")]
        currentIndex: root.fix && root.fix.mains === 60 ? 1 : 0
        helpTitle: qsTr("Mains frequency")
        helpText: qsTr("50 Hz in Europe, Africa, Asia and Australia; 60 Hz in the Americas.")
        onActivated: function (i) { root.panel.change({ [root.fixId]: { mains: i === 1 ? 60 : 50 } }) }
    }
    Item {
        x: toggle.width + Theme.s2
        width: parent.width - x
        height: slider.height + (root.lowText !== "" ? Theme.fsLabel + Theme.s1 : 0)
        visible: root.fix !== null && root.fix.choice !== true
        SliderField {
            id: slider
            objectName: "soundFixAmount_" + root.fixId
            width: parent.width
            labelWidth: 0
            enabled: root.available && root.on
            from: 0; to: root.percent; decimals: 0; suffix: "%"
            value: root.fix ? Math.round(root.fix.amount * root.percent) : 0
            helpTitle: root.fix ? root.fix.title : ""
            helpText: qsTr("How much. Takes effect when you let go; each change is one Undo step.")
            onEditingFinished: function (v) {
                if (root.fix && Math.abs(v / root.percent - root.fix.amount) > 0.4 / root.percent)
                    root.panel.change({ [root.fixId]: { amount: v / root.percent } })
            }
        }
        Text {
            anchors.top: slider.bottom
            visible: root.lowText !== ""
            text: root.lowText
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
        }
        Text {
            anchors.top: slider.bottom
            anchors.right: parent.right
            anchors.rightMargin: slider.readoutWidth + Theme.s2
            visible: root.highText !== ""
            text: root.highText
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
        }
    }
}
