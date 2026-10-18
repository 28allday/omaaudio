pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import "ui"
import "PluginScale.js" as Scale

// A dynamics effect's controls as knobs, in the Apply panel: one column per
// group (the plugin's own port groups, or one "Controls" group), switches and
// named values under the knobs, and the effect's meters beneath. The same
// values, defaults and typing as the slider rows (PluginControlList).
Flickable {
    id: root
    objectName: "pluginKnobs"
    clip: true
    contentWidth: width
    contentHeight: body.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    C.ScrollBar.vertical: ScrollBar {}

    property var service: null
    property int selected: -1
    property int target: -1
    property var ports: []
    property var outputs: []
    // The effect is off (power or Before): its meters say so.
    property bool bypassed: false
    property string knobName: "effectApplyKnob"

    readonly property var groups: {
        const named = root.ports.some(function (p) { return (p.group || "") !== "" })
        const order = [], members = ({})
        for (const p of root.ports) {
            const g = named ? (p.group || qsTr("Other")) : qsTr("Controls")
            if (!members[g]) { members[g] = []; order.push(g) }
            members[g].push(p)
        }
        return order.map(function (g) {
            return { name: g,
                     knobs: members[g].filter(function (p) { return !p.toggled && !(p.choices || []).length }),
                     others: members[g].filter(function (p) { return p.toggled || (p.choices || []).length }) }
        })
    }

    function valueOf(port) {
        return root.service && root.service.pluginValuesRevision >= 0
            ? root.service.pluginPortValue(root.selected, port.index, root.target, port.value) : port.value
    }
    function textOf(port, value) {
        return root.service && root.service.pluginTextRevision >= 0
            ? root.service.pluginPortText(root.selected, port.index, root.target, value) : ""
    }
    function setPort(port, value) {
        if (!root.service || root.selected < 0) return
        let v = Math.max(port.min, Math.min(port.max, value))
        if (port.integer) v = Math.round(v)
        root.service.setPluginControl(root.selected, port.index, v, root.target)
    }
    function gesture(port, begin) {
        if (root.service) root.service.automationGesture(root.target, "fx:" + root.selected + ":" + port.index, begin)
    }
    function commitPorts() { if (root.service) root.service.flushPluginPorts() }
    function resetAll() {
        if (!root.service || root.selected < 0) return
        for (const p of root.ports) root.service.setPluginControl(root.selected, p.index, p.defaultValue, root.target)
        root.commitPorts()
    }

    Column {
        id: body
        width: root.width
        spacing: Theme.s2

        Flow {
            width: parent.width
            spacing: Theme.s2
            Repeater {
                model: root.groups
                delegate: Rectangle {
                    id: group
                    objectName: "knobGroup"
                    required property var modelData
                    readonly property int columns: Math.max(1, Math.min(4, modelData.knobs.length))
                    // One group fills the panel; several share it, each as wide as its knobs.
                    width: root.groups.length === 1 ? body.width
                           : Math.min(body.width, Math.max(200, columns * 96 + Theme.s3 * 2))
                    height: content.implicitHeight + Theme.s3 * 2
                    color: Theme.panelRaised; radius: Theme.rMenu
                    border.width: Theme.hairline; border.color: Theme.border
                    Column {
                        id: content
                        x: Theme.s3; y: Theme.s2
                        width: parent.width - Theme.s3 * 2
                        spacing: Theme.s2
                        Text {
                            objectName: "knobGroupTitle"
                            text: group.modelData.name
                            font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Theme.wHeading
                            font.capitalization: Font.AllUppercase; font.letterSpacing: 0.8
                            color: Theme.textMuted
                        }
                        Grid {
                            columns: group.columns
                            spacing: Theme.s1
                            Repeater {
                                model: group.modelData.knobs
                                delegate: Knob {
                                    id: knob
                                    required property var modelData
                                    objectName: root.knobName
                                    width: 92
                                    port: knob.modelData
                                    value: root.valueOf(knob.modelData)
                                    nativeText: root.textOf(knob.modelData, knob.shown)
                                    onMoved: function (v) { root.setPort(knob.modelData, v) }
                                    onCommitted: root.commitPorts()
                                    onGesture: function (begin) { root.gesture(knob.modelData, begin) }
                                }
                            }
                        }
                        Repeater {
                            model: group.modelData.others
                            delegate: Item {
                                id: other
                                required property var modelData
                                width: content.width
                                height: Theme.hControl + 4
                                readonly property real current: root.valueOf(modelData)
                                readonly property var choices: modelData.choices || []
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 90; elide: Text.ElideRight; textFormat: Text.PlainText
                                    text: other.modelData.name
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                                    color: Theme.textSecondary
                                }
                                Toggle {
                                    objectName: "knobToggle"
                                    visible: other.modelData.toggled
                                    x: 96; anchors.verticalCenter: parent.verticalCenter
                                    helpTitle: other.modelData.name
                                    helpText: qsTr("Switch this control of the effect on or off, as the plugin describes it.")
                                    checked: other.current >= 0.5
                                    onToggled: { root.setPort(other.modelData, checked ? 1 : 0); root.commitPorts() }
                                }
                                SegmentedControl {
                                    objectName: "knobSegments"
                                    visible: !other.modelData.toggled && other.choices.length <= 3
                                    x: 96; anchors.verticalCenter: parent.verticalCenter
                                    height: Theme.hControl
                                    labels: other.choices.map(function (c) { return c.label })
                                    helpTitle: other.modelData.name
                                    helpText: qsTr("Choose one of the values the effect offers for this control.")
                                    currentIndex: other.choices.findIndex(function (c) { return c.value === other.current })
                                    onActivated: function (i) {
                                        root.gesture(other.modelData, true)
                                        root.setPort(other.modelData, other.choices[i].value)
                                        root.gesture(other.modelData, false)
                                        root.commitPorts()
                                    }
                                }
                                ComboField {
                                    objectName: "knobChoice"
                                    visible: !other.modelData.toggled && other.choices.length > 3
                                    x: 96; width: Math.max(120, other.width - 96)
                                    anchors.verticalCenter: parent.verticalCenter
                                    helpTitle: other.modelData.name
                                    helpText: qsTr("Choose one of the values the effect offers for this control.")
                                    model: other.choices.map(function (c) { return c.label })
                                    currentIndex: other.choices.findIndex(function (c) { return c.value === other.current })
                                    onActivated: {
                                        if (currentIndex < 0) return
                                        root.gesture(other.modelData, true)
                                        root.setPort(other.modelData, other.choices[currentIndex].value)
                                        root.gesture(other.modelData, false)
                                        root.commitPorts()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // The effect's own readings.
        Rectangle {
            visible: root.outputs.length > 0
            width: parent.width
            height: visible ? meters.implicitHeight + 22 + Theme.s3 * 2 : 0
            color: Theme.panelRaised; radius: Theme.rMenu
            border.width: Theme.hairline; border.color: Theme.border
            Text {
                x: Theme.s3; y: Theme.s2
                text: qsTr("Meters")
                font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Theme.wHeading
                font.capitalization: Font.AllUppercase; font.letterSpacing: 0.8
                color: Theme.textMuted
            }
            PluginMeters {
                id: meters
                x: Theme.s3; y: 22 + Theme.s1
                width: parent.width - Theme.s3 * 2
                service: root.service
                selected: root.selected
                target: root.target
                outputs: root.outputs
                bypassed: root.bypassed
            }
        }
    }
}
