// The port rows below reach the list's own `root` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import QtCore
import "ui"
import "PluginScale.js" as Scale

// One effect's controls, drawn from its port descriptors: the Track Effects panel's
// list for the selected slot, and the same list in the Apply Effect panel.
//
// Controls sit in titled groups, the plugin's own port groups when it names them
// and one "Controls" group when it does not. Each slider has a numbered scale over
// it (spaced by the plugin's own scale: logarithmic where it says so), its value
// and unit on the right in a box you can type into, and a mark where the default
// is; a dot by the value says it has been moved, and a click on the dot (or a
// double-click on the slider) puts the default back. A switch is a switch; two
// or three named values are buttons, more are a menu. The effect's outputs follow
// as meters, and an EQ draws its curve above the rows (EqCurve.qml).
//
// `compact` is the narrow dock form: the name and value on one line with the
// slider under them, only the end values marked, and groups that fold away to a
// one-line summary (remembered per effect).
ListView {
    id: root
    clip: true

    // The service, and which effect: a slot in the chain of `target` (-1 the
    // focused track, the bus's own target for the master).
    property var service: null
    property int selected: -1
    property int target: -1
    // That slot's port descriptors (Backend.pluginPorts, once they are its own).
    property var ports: []
    // Its output controls (Backend.pluginOutputs), drawn as meters below.
    property var outputs: []
    // The effect is off (power or Before): its meters say so.
    property bool bypassed: false
    // "dynamics" | "eq" | "other", and the effect's URI (for folded groups).
    property string kind: "other"
    property string uri: ""
    property bool compact: false
    // Groups fold away to a one-line summary (remembered per effect).
    property bool foldable: true
    // A clip's effect (ClipEffectsPanel): no live instance, so a change is held
    // here while dragging and written on release, one Undo step each, by
    // Backend.setClipEffectControl; a control with a clip curve follows it.
    property bool clipMode: false
    property int clipTrack: -1
    property int clipClip: -1
    property int clipSlot: -1
    property string clipStamp: ""
    property var pending: ({})
    signal clipEdited()
    signal automateRequested(var port)
    // Each row's objectName, for tests.
    property string rowName: "rackPort"
    // The EQ curve, when this app has one and the effect is an EQ.
    readonly property bool showCurve: kind === "eq" && !clipMode && service !== null && service.qmlAvailable("EqCurve.qml")

    // The names' column fits the longest name, up to about a third of the width
    // (long names such as "Delay 3 low-cut frequency" are common).
    TextMetrics { id: nameMetrics; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel + 1 }
    // Measured when the controls change (a binding would loop through the metrics).
    property real longestName: 0
    function measureNames() {
        let most = 0
        for (const row of root.rows || []) {
            nameMetrics.text = row.label || row.name || row.symbol || ""
            most = Math.max(most, nameMetrics.advanceWidth)
        }
        root.longestName = Math.ceil(most)
    }
    Component.onCompleted: measureNames()
    readonly property int labelWidth: Math.round(Math.max(84, Math.min(Math.max(132, width * 0.24), width * 0.38, longestName + 4)))
    readonly property int valueWidth: 92
    readonly property int pad: Theme.s3

    // Folded groups, by "<uri>/<group>": the dock's choice, kept between runs.
    Settings {
        id: folds
        category: "pluginGroups"
        property string folded: "{}"
    }
    property var foldedGroups: {
        try { return JSON.parse(folds.folded) } catch (e) { return ({}) }
    }
    // The Display group starts folded; the others open.
    function isFolded(group) {
        if (!root.foldable) return false
        const v = root.foldedGroups[root.uri + "/" + group]
        return v === undefined ? group === root.displayGroup : v === true
    }
    function setFolded(group, on) {
        const next = Object.assign({}, root.foldedGroups)
        if (group === root.displayGroup) next[root.uri + "/" + group] = on
        else if (on) next[root.uri + "/" + group] = true
        else delete next[root.uri + "/" + group]
        root.foldedGroups = next
        folds.folded = JSON.stringify(next)
    }

    // The rows: the ports grouped (in the order each group first appears), each
    // knowing whether it opens or closes its group's box.
    // An effect's own on/off switch ("Active", "Bypass", "Enabled"): first, not
    // lost at the bottom, when the effect does not group its controls itself.
    function isPower(p) {
        return p.toggled === true && /^(active|enabled?|on|on\/off|bypass|power)$/i.test(String(p.name || "").trim())
    }
    // Controls that only set up the plugin's own drawing (its graph's zoom, an
    // FFT display, which filter it inspects, which tab its window shows) change
    // nothing heard: gathered last
    // in a Display group that starts folded.
    readonly property string displayGroup: qsTr("Display")
    function isDisplay(p) {
        return /graph|\bfft\b|zoom|inspect|display|visual|measuring line|notebook|overlay/i.test(String(p.name || p.symbol || "").replace(/_/g, " "))
    }
    readonly property var rows: {
        const named = root.ports.some(function (p) { return (p.group || "") !== "" })
        const sound = root.ports.filter(function (p) { return !root.isDisplay(p) })
        const ordered = (named ? sound
            : sound.filter(root.isPower).concat(sound.filter(function (p) { return !root.isPower(p) })))
            .concat(root.ports.filter(root.isDisplay))
        // A long effect that names no groups often numbers its parts ("Delay 3
        // high-cut", "Band 2 gain"): those become groups, so they can fold.
        const numbered = function (p) {
            const m = /^(.*?\b\d+)\b/.exec(String(p.name || ""))
            return m && m[1].length <= 24 ? m[1] : ""
        }
        const byNumber = !named && sound.length > 40
            && new Set(sound.map(numbered).filter(function (k) { return k !== "" })).size >= 2
        const order = [], members = ({})
        for (const p of ordered) {
            const g = root.isDisplay(p) ? root.displayGroup
                : named ? (p.group || qsTr("Other"))
                : byNumber ? (numbered(p) || qsTr("Controls")) : qsTr("Controls")
            if (!members[g]) { members[g] = []; order.push(g) }
            members[g].push(p)
        }
        // In a numbered group a name drops the group's part: "Delay 3 high-cut
        // frequency" reads "High-cut frequency" under "Delay 3".
        const label = function (p, g) {
            const name = String(p.name || "")
            if (!byNumber || g === qsTr("Controls") || g === root.displayGroup || !name.startsWith(g + " ")) return name
            const rest = name.slice(g.length + 1).trim()
            return rest.length ? rest.charAt(0).toUpperCase() + rest.slice(1) : name
        }
        const out = []
        for (const g of order) {
            const list = members[g]
            for (let i = 0; i < list.length; ++i)
                out.push(Object.assign({}, list[i], { groupName: g, groupFirst: i === 0, groupLast: i === list.length - 1,
                                                      label: label(list[i], g) }))
        }
        return out
    }
    // A folded group's one-line summary: its first few values.
    function summary(group) {
        const parts = []
        for (const r of root.rows) {
            if (r.groupName !== group || parts.length >= 3) continue
            const v = root.valueOf(r)
            if (r.toggled) parts.push(v >= 0.5 ? qsTr("On") : qsTr("Off"))
            else if ((r.choices || []).length) {
                const c = r.choices.find(function (x) { return x.value === v })
                parts.push(c ? c.label : Scale.valueText(r, v).text)
            } else {
                const t = Scale.valueText(r, v)
                parts.push(t.text + (t.unit ? " " + t.unit : ""))
            }
        }
        return parts.join(" · ")
    }

    model: root.rows
    // A different effect's controls start from the top (the curve, the first
    // group); a value changing in the same effect never moves the view.
    property string shownEffect: ""
    function toStart() { root.positionViewAtBeginning() }
    onRowsChanged: {
        root.measureNames()
        const which = root.uri + "/" + root.selected + "/" + root.clipSlot
        if (which === root.shownEffect) return
        root.shownEffect = which
        Qt.callLater(root.toStart)
    }
    // Virtualised on purpose: one plugin installed here has 1030 of these.
    cacheBuffer: 200
    spacing: 0
    C.ScrollBar.vertical: ScrollBar {}

    header: Loader {
        objectName: "eqCurveLoader"
        width: root.width
        active: root.showCurve
        visible: active
        height: active && item ? item.implicitHeight + Theme.s2 : 0
        source: root.showCurve ? "EqCurve.qml" : ""
        onLoaded: {
            // The curve arrives after the rows: show it, not the first row.
            Qt.callLater(root.toStart)
            item.service = Qt.binding(function () { return root.service })
            item.selected = Qt.binding(function () { return root.selected })
            item.target = Qt.binding(function () { return root.target })
            item.ports = Qt.binding(function () { return root.ports })
        }
    }

    footer: Item {
        width: root.width
        height: root.outputs.length ? meters.implicitHeight + Theme.s3 * 2 + 22 : 0
        visible: root.outputs.length > 0
        Rectangle {
            anchors.fill: parent
            anchors.bottomMargin: Theme.s2
            color: Theme.panelRaised; radius: Theme.rMenu
            border.width: Theme.hairline; border.color: Theme.border
        }
        Text {
            x: root.pad; y: Theme.s2
            text: qsTr("Meters")
            font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Theme.wHeading
            font.capitalization: Font.AllUppercase; font.letterSpacing: 0.8
            color: Theme.textMuted
        }
        PluginMeters {
            id: meters
            x: root.pad; y: 22 + Theme.s1
            width: root.width - root.pad * 2
            service: root.service
            selected: root.selected
            target: root.target
            outputs: root.outputs
            bypassed: root.bypassed
            compact: root.compact
            labelWidth: root.labelWidth
        }
    }

    delegate: Item {
        id: portRow
        objectName: root.rowName
        required property var modelData
        width: root.width

        readonly property bool folded: root.isFolded(modelData.groupName)
        readonly property int headerHeight: modelData.groupFirst ? (root.compact ? 28 : 30) : 0
        readonly property int gap: modelData.groupLast ? Theme.s2 : 0
        readonly property bool sliderKind: !modelData.toggled && !namedChoice
        readonly property int contentHeight: folded ? 0
            : root.compact ? (sliderKind ? 42 : 32) : (sliderKind ? 46 : 36)
        readonly property int contentY: headerHeight
        height: headerHeight + contentHeight + gap + (folded && modelData.groupLast ? Theme.s1 : 0)

        // Values have a separate notification: native edits must not
        // reset this ListView and destroy a slider under the pointer.
        readonly property real currentValue: { void root.pending; return root.valueOf(portRow.modelData) }
        // A control a clip curve drives is read here, set on the curve.
        enabled: !(root.clipMode && portRow.modelData.automated === true)
        readonly property real span: portRow.modelData.max - portRow.modelData.min
        readonly property var choices: portRow.modelData.choices || []
        readonly property bool namedChoice: !portRow.modelData.toggled && choices.length > 0
        readonly property bool fewChoices: namedChoice && choices.length <= 3
        readonly property bool logarithmic: Scale.isLog(portRow.modelData)
        function valueAt(fraction) { return Scale.valueAt(modelData, fraction) }
        // What is drawn. The model is not refreshed during a drag (see
        // Backend::flushPluginPorts), so a pressed slider shows the value
        // under the pointer rather than the last committed one.
        readonly property real shownValue: slideArea.pressed
                                           ? slideArea.dragValue
                                           : portRow.currentValue
        readonly property bool changed: Scale.changed(modelData, shownValue)
        // Text rescans have their own revision, so a plug-in can
        // rename a value without resetting the list or its pointer grab.
        readonly property string nativeValueText: !root.clipMode && root.service && root.service.pluginTextRevision >= 0
            ? root.service.pluginPortText(root.selected, modelData.index, root.target, shownValue) : ""
        readonly property var nativeRangeText: !root.clipMode && root.service && root.service.pluginTextRevision >= 0
            ? [root.service.pluginPortText(root.selected, modelData.index, root.target, modelData.min),
               root.service.pluginPortText(root.selected, modelData.index, root.target, modelData.max),
               root.service.pluginPortText(root.selected, modelData.index, root.target, modelData.defaultValue)] : []
        readonly property string unitText: portRow.modelData.unit ? " " + portRow.modelData.unit : ""
        readonly property string rangeText: nativeRangeText.length === 3 && nativeRangeText.every(function(text) { return text !== "" })
            ? qsTr("Range %1 to %2. Double-click the slider to return to the effect's default, %3.")
              .arg(nativeRangeText[0]).arg(nativeRangeText[1]).arg(nativeRangeText[2])
            : qsTr("Range %1 to %2%3. Double-click the slider to return to the effect's default, %4%3.")
            .arg(Number(portRow.modelData.min).toPrecision(4)).arg(Number(portRow.modelData.max).toPrecision(4))
            .arg(portRow.unitText).arg(Number(portRow.modelData.defaultValue).toPrecision(4))
        readonly property real fraction: Scale.fraction(modelData, shownValue)
        readonly property var reading: Scale.valueText(modelData, shownValue)
        // A plugin that formats its own values (CLAP, VST3) keeps the plain number
        // it always had when its formatter refuses one, so the box never jumps.
        readonly property string valueText: portRow.nativeValueText !== "" ? portRow.nativeValueText
            : portRow.modelData.nativeText === true
              ? (portRow.modelData.integer ? String(Math.round(portRow.shownValue))
                 : portRow.shownValue.toFixed(Math.abs(portRow.shownValue) >= 100 ? 0 : 2)) + portRow.unitText
            : portRow.reading.text + (portRow.reading.unit ? " " + portRow.reading.unit : "")
        function resetToDefault() {
            root.setPort(portRow.modelData, portRow.modelData.defaultValue)
            root.commitPorts()
        }

        // ── the group's box ───────────────────────────────────────────────
        Rectangle {
            id: box
            width: parent.width
            height: parent.height - portRow.gap
            color: Theme.panelRaised
            topLeftRadius: portRow.modelData.groupFirst ? Theme.rMenu : 0
            topRightRadius: topLeftRadius
            bottomLeftRadius: portRow.modelData.groupLast ? Theme.rMenu : 0
            bottomRightRadius: bottomLeftRadius
        }
        Rectangle { width: Theme.hairline; height: box.height; color: Theme.border }
        Rectangle { x: parent.width - width; width: Theme.hairline; height: box.height; color: Theme.border }
        Rectangle { visible: portRow.modelData.groupFirst; width: parent.width; height: Theme.hairline; color: Theme.border }
        Rectangle { visible: portRow.modelData.groupLast; y: box.height - height; width: parent.width; height: Theme.hairline; color: Theme.border }

        Item {
            id: groupHeader
            objectName: "rackGroup"
            visible: portRow.modelData.groupFirst
            width: parent.width; height: portRow.headerHeight
            Icon {
                id: chevron
                visible: root.foldable
                x: root.pad - 2; anchors.verticalCenter: groupTitle.verticalCenter
                name: portRow.folded ? "chevron-right" : "chevron-down"
                size: 12; color: Theme.textMuted
            }
            Text {
                id: groupTitle
                objectName: "rackGroupTitle"
                x: root.foldable ? chevron.x + 16 : root.pad
                y: root.compact ? 8 : 10
                text: portRow.modelData.groupName
                font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Theme.wHeading
                font.capitalization: Font.AllUppercase; font.letterSpacing: 0.8
                color: Theme.textMuted
            }
            Text {
                objectName: "rackGroupSummary"
                visible: portRow.folded
                anchors.left: groupTitle.right; anchors.leftMargin: Theme.s3
                anchors.right: parent.right; anchors.rightMargin: root.pad
                anchors.verticalCenter: groupTitle.verticalCenter
                horizontalAlignment: Text.AlignRight; elide: Text.ElideRight; textFormat: Text.PlainText
                text: portRow.folded && root.service && root.service.pluginValuesRevision >= 0 ? root.summary(portRow.modelData.groupName) : ""
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.features: { "tnum": 1 }
                color: Theme.textSecondary
            }
            MouseArea {
                objectName: "rackGroupFold"
                anchors.fill: parent
                enabled: root.foldable
                cursorShape: root.foldable ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.setFolded(portRow.modelData.groupName, !portRow.folded)
                ControlHelp {
                    title: portRow.modelData.groupName
                    purpose: portRow.folded ? qsTr("Show this group's controls.") : qsTr("Fold this group away; its values stay as they are and show on one line.")
                    scope: qsTr("Remembered for this effect.")
                }
            }
        }

        // ── the control ───────────────────────────────────────────────────
        Text {
            id: portName
            visible: !portRow.folded
            readonly property real automateRoom: root.clipMode && !root.compact ? Theme.szIconHit : 0
            x: root.pad + automateRoom
            width: root.compact ? parent.width - root.pad * 2 - valueBox.width - Theme.s2 - (root.clipMode ? Theme.szIconHit : 0)
                                : root.labelWidth - automateRoom
            y: root.compact ? portRow.contentY + 4
               : portRow.contentY + (portRow.sliderKind ? 24 : portRow.contentHeight / 2) - height / 2
            horizontalAlignment: root.compact ? Text.AlignLeft : Text.AlignRight
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel + (root.compact ? 0 : 1)
            color: portRow.changed ? Theme.textPrimary : Theme.textSecondary
            text: portRow.modelData.label || portRow.modelData.name
        }
        // A clip effect's control can follow a curve drawn across the clip.
        IconButton {
            objectName: "clipEffectAutomate"
            visible: root.clipMode && !portRow.folded
            x: root.compact ? parent.width - root.pad - width : root.pad - Theme.s1
            y: portRow.contentY + (root.compact ? 0 : (portRow.sliderKind ? 24 : portRow.contentHeight / 2) - height / 2)
            iconName: "activity"; text: qsTr("Automate")
            checkable: true; checked: portRow.modelData.automated === true
            iconColor: checked ? Theme.accent : Theme.textMuted
            helpText: qsTr("Draw how this control changes across the clip. Points belong to the clip and move with it.")
            helpScope: portRow.modelData.automated === true ? qsTr("This control follows its clip curve; clear the curve to set one value again.") : ""
            enabled: true
            onClicked: root.automateRequested(portRow.modelData)
        }
        // Where the control itself goes: beside the name, or under it.
        // In the dock the two end values sit on the slider's own line, left and
        // right of it, so they cannot be read as the next row's.
        readonly property var ends: {
            if (!root.compact || !sliderKind) return []
            const all = Scale.ticks(portRow.modelData, 2)
            return all.length ? [all[0], all[all.length - 1]] : []
        }
        TextMetrics { id: lowEnd; font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler; text: portRow.ends.length ? portRow.ends[0].t : "" }
        TextMetrics { id: highEnd; font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler; text: portRow.ends.length ? portRow.ends[1].t : "" }
        readonly property real laneX: root.compact ? root.pad + (ends.length ? Math.ceil(lowEnd.advanceWidth) + Theme.s2 : 0)
                                                   : root.pad + root.labelWidth + Theme.s3
        readonly property real laneRight: root.compact ? width - root.pad - (ends.length ? Math.ceil(highEnd.advanceWidth) + Theme.s2 : 0)
                                                       : width - root.pad - root.valueWidth - 18

        // A toggle is a switch, not a slider that happens to have two ends.
        Toggle {
            objectName: "rackToggle"
            helpTitle: portRow.modelData.name
            helpText: qsTr("Switch this control of the effect on or off, as the plugin describes it.")
            helpScope: qsTr("Applies to this effect in playback and exports. Control changes are not Undo steps; a parameter preset can save and restore them.")
            visible: portRow.modelData.toggled && !portRow.folded
            x: root.compact ? parent.width - root.pad - width : portRow.laneX
            y: portRow.contentY + (portRow.contentHeight - height) / 2
            checked: portRow.currentValue >= 0.5
            onToggled: {
                root.setPort(portRow.modelData, checked ? 1 : 0)
                root.commitPorts()
            }
        }

        // Two or three named values: one click each.
        SegmentedControl {
            objectName: "rackSegments"
            visible: portRow.fewChoices && !portRow.folded
            x: root.compact ? parent.width - root.pad - width : portRow.laneX
            y: portRow.contentY + (portRow.contentHeight - height) / 2
            height: Theme.hControl
            labels: portRow.choices.map(function (choice) { return choice.label })
            helpTitle: portRow.modelData.name
            helpText: qsTr("Choose one of the values the effect offers for this control.")
            helpScope: qsTr("Applies to this effect in playback and exports. Control changes are not Undo steps; a parameter preset can save and restore them.")
            currentIndex: {
                for (let i = 0; i < portRow.choices.length; ++i)
                    if (portRow.choices[i].value === portRow.currentValue) return i
                return -1
            }
            onActivated: function (index) {
                if (index < 0 || index >= portRow.choices.length) return
                root.gesture(portRow.modelData, true)
                root.setPort(portRow.modelData, portRow.choices[index].value)
                root.gesture(portRow.modelData, false)
                root.commitPorts()
            }
        }

        ComboField {
            objectName: "rackChoice"
            helpTitle: portRow.modelData.name
            helpText: qsTr("Choose one of the values the effect offers for this control.")
            helpScope: qsTr("Applies to this effect in playback and exports. Control changes are not Undo steps; a parameter preset can save and restore them.")
            visible: portRow.namedChoice && !portRow.fewChoices && !portRow.folded
            x: root.compact ? parent.width - root.pad - width : portRow.laneX
            width: root.compact ? Math.min(180, parent.width * 0.55) : Math.min(220, portRow.laneRight - portRow.laneX)
            y: portRow.contentY + (portRow.contentHeight - height) / 2
            model: portRow.choices.map(function(choice) { return choice.label })
            currentIndex: {
                for (let i = 0; i < portRow.choices.length; ++i)
                    if (portRow.choices[i].value === portRow.currentValue) return i
                return -1
            }
            displayText: currentIndex >= 0 ? currentText : qsTr("Value %1").arg(Number(portRow.shownValue).toPrecision(6))
            onActivated: {
                if (currentIndex < 0 || currentIndex >= portRow.choices.length) return
                root.gesture(portRow.modelData, true)
                root.setPort(portRow.modelData, portRow.choices[currentIndex].value)
                root.gesture(portRow.modelData, false)
                root.commitPorts()
            }
        }

        // The numbered marks: all of them beside a wide slider, the two ends under
        // a narrow one.
        Item {
            id: scale
            objectName: "rackTicks"
            visible: portRow.sliderKind && !portRow.folded
            x: root.compact ? root.pad : portRow.laneX
            width: root.compact ? parent.width - root.pad * 2 : portRow.laneRight - portRow.laneX
            y: root.compact ? sliderItem.y + (sliderItem.height - height) / 2 : portRow.contentY + 4
            height: 12
            readonly property var marks: {
                if (!visible || width <= 0) return []
                if (root.compact) return portRow.ends.length ? [Object.assign({}, portRow.ends[0], { p: 0 }), Object.assign({}, portRow.ends[1], { p: 1 })] : []
                return Scale.ticks(portRow.modelData, Math.max(2, Math.floor(width / 40)))
            }
            Repeater {
                model: scale.marks
                Item {
                    id: mark
                    required property var modelData
                    x: modelData.p * scale.width
                    Rectangle { visible: !root.compact; x: 0; y: 10; width: 1; height: 3; color: Theme.borderStrong }
                    Text {
                        y: root.compact ? 0 : -1
                        text: mark.modelData.t
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler; font.features: { "tnum": 1 }
                        color: Theme.textMuted
                        x: mark.modelData.p < 0.02 ? 0 : mark.modelData.p > 0.98 ? -implicitWidth + 1 : -implicitWidth / 2
                    }
                }
            }
        }

        Item {
            id: sliderItem
            visible: portRow.sliderKind && !portRow.folded
            x: portRow.laneX
            width: portRow.laneRight - portRow.laneX
            y: root.compact ? portRow.contentY + 22 : portRow.contentY + 17
            height: 14
            Accessible.role: Accessible.Slider
            Accessible.name: portRow.modelData.name
            Accessible.description: sliderHelp.description
            ControlHelp {
                id: sliderHelp
                title: portRow.modelData.name
                purpose: qsTr("Drag to set this control of the effect, as the plugin describes it. Control changes are not Undo steps; a parameter preset can save and restore them.")
                scope: portRow.rangeText
                suppressed: slideArea.pressed
            }

            Rectangle {
                id: groove
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: Theme.controlBg

                readonly property real origin: Scale.origin(portRow.modelData)
                readonly property real at: Math.max(0, Math.min(1, portRow.fraction))
                Rectangle {
                    x: groove.width * Math.min(groove.origin, groove.at)
                    width: groove.width * Math.abs(groove.at - groove.origin)
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                }
                // Zero, on a control that runs either side of it.
                Rectangle {
                    visible: groove.origin > 0 && groove.origin < 1
                    x: groove.width * groove.origin; width: 1; height: 10
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.borderStrong
                }
            }
            // The default, marked under the groove once the value has moved.
            Rectangle {
                objectName: "rackDefaultMark"
                visible: portRow.changed
                x: parent.width * Math.max(0, Math.min(1, Scale.fraction(portRow.modelData, portRow.modelData.defaultValue))) - 1
                y: parent.height - 1; width: 2; height: 4; radius: 1
                color: Theme.textSecondary
            }

            // The handle. The groove alone reads as a meter; a thumb riding
            // the value is what says "pull me".
            Rectangle {
                id: portKnob
                width: 12
                height: 12
                radius: 6
                color: slideArea.pressed ? Theme.accentPressed : Theme.textPrimary
                anchors.verticalCenter: parent.verticalCenter
                x: Math.max(0, Math.min(parent.width - width,
                       Math.max(0, Math.min(1, portRow.fraction))
                           * parent.width - width / 2))
                Rectangle {
                    visible: slideArea.pressed || slideArea.containsMouse
                    anchors.centerIn: parent
                    width: 20; height: 20; radius: 10; z: -1
                    color: Theme.withAlpha(Theme.accent, 0.28)
                }
            }

            MouseArea {
                id: slideArea
                objectName: "rackSlider"
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                // The rack's ListView scrolls vertically and will steal a
                // drag the moment it drifts off-horizontal. This drag is
                // ours until release.
                preventStealing: true

                property real dragValue: portRow.currentValue
                property real grabOffset: 0

                function apply(mouseX) {
                    const f = Math.max(0, Math.min(1, mouseX / Math.max(1, width)))
                    var v = portRow.valueAt(f)
                    if (portRow.modelData.integer)
                        v = Math.round(v)
                    dragValue = v
                    root.setPort(portRow.modelData, v)
                }

                onPressed: function (e) {
                    root.gesture(portRow.modelData, true)
                    dragValue = portRow.currentValue
                    const onKnob = e.x >= portKnob.x && e.x <= portKnob.x + portKnob.width
                    grabOffset = onKnob ? e.x - portRow.fraction * width : 0
                    if (!onKnob) apply(e.x)
                }
                onPositionChanged: function (e) { if (pressed) apply(e.x - grabOffset) }
                onReleased: {
                    root.gesture(portRow.modelData, false)
                    root.commitPorts()
                }
                onCanceled: {
                    if (root && root.service) {
                        root.gesture(portRow.modelData, false)
                        root.commitPorts()
                    }
                }
                // Back to what the plugin shipped with, which is the only value
                // a generic panel can be sure is sensible.
                onDoubleClicked: portRow.resetToDefault()
            }
        }

        // Moved from the default: a dot that puts it back.
        Rectangle {
            objectName: "rackDefaultDot"
            visible: portRow.changed && portRow.sliderKind && !portRow.folded
            width: 6; height: 6; radius: 3
            color: Theme.accent
            x: valueBox.x - 12
            y: valueBox.y + (valueBox.height - height) / 2
            MouseArea {
                objectName: "rackResetDot"
                anchors.fill: parent; anchors.margins: -5
                cursorShape: Qt.PointingHandCursor
                onClicked: portRow.resetToDefault()
                ControlHelp {
                    title: qsTr("Default %1").arg(portRow.nativeRangeText.length === 3 && portRow.nativeRangeText[2] !== ""
                        ? portRow.nativeRangeText[2]
                        : (function () { const t = Scale.valueText(portRow.modelData, portRow.modelData.defaultValue); return t.text + (t.unit ? " " + t.unit : "") })())
                    purpose: qsTr("Click to put this control back to the effect's default.")
                }
            }
        }

        // The value, with its unit: click it to type one.
        Rectangle {
            id: valueBox
            objectName: "rackValueBox"
            visible: portRow.sliderKind && !portRow.folded
            width: root.compact ? Math.max(56, readout.implicitWidth + 12) : root.valueWidth
            height: root.compact ? 18 : 24
            x: parent.width - root.pad - width
            y: root.compact ? portRow.contentY + 1 : portRow.contentY + 12
            radius: Theme.rControl
            color: editor.visible ? Theme.windowBg : root.compact ? "transparent" : Theme.controlBg
            border.width: editor.visible ? 1 : 0
            border.color: Theme.accent

            Text {
                id: readout
                objectName: "rackReadout"
                visible: !editor.visible
                textFormat: Text.PlainText
                elide: Text.ElideRight
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right; anchors.rightMargin: root.compact ? 2 : Theme.s2
                width: Math.min(implicitWidth, parent.width - Theme.s2)
                horizontalAlignment: Text.AlignRight
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel + 1
                font.features: { "tnum": 1 }
                color: Theme.accent
                text: portRow.valueText
            }
            TextInput {
                id: editor
                objectName: "rackValueEdit"
                visible: false
                anchors.fill: parent; anchors.leftMargin: Theme.s2; anchors.rightMargin: Theme.s2
                verticalAlignment: TextInput.AlignVCenter
                horizontalAlignment: TextInput.AlignRight
                font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel + 1
                color: Theme.textPrimary
                selectionColor: Theme.selectionFill
                selectByMouse: true
                clip: true
                function begin() {
                    text = Scale.editText(portRow.modelData, portRow.currentValue)
                    visible = true
                    forceActiveFocus()
                    selectAll()
                }
                function finish(keep) {
                    if (!visible) return
                    visible = false
                    if (!keep) return
                    const v = Scale.parse(portRow.modelData, text)
                    if (isNaN(v)) return
                    root.gesture(portRow.modelData, true)
                    root.setPort(portRow.modelData, v)
                    root.gesture(portRow.modelData, false)
                    root.commitPorts()
                }
                Keys.onReturnPressed: (event) => { finish(true); event.accepted = true }
                Keys.onEnterPressed: (event) => { finish(true); event.accepted = true }
                Keys.onEscapePressed: (event) => { finish(false); event.accepted = true }
                onActiveFocusChanged: if (!activeFocus) finish(true)
            }
            MouseArea {
                objectName: "rackValueClick"
                anchors.fill: parent
                visible: !editor.visible
                cursorShape: Qt.IBeamCursor
                onClicked: editor.begin()
                ControlHelp {
                    title: portRow.modelData.name
                    purpose: qsTr("Click to type a value. Enter sets it, Escape leaves it as it was.")
                    scope: portRow.rangeText
                }
            }
        }
    }

    // Writes the value straight through. The Backend echoes it into the drawn
    // descriptors itself, so the knob follows the pointer without waiting.
    function setPort(port, value) {
        if (!root.service || (root.clipMode ? root.clipSlot < 0 : root.selected < 0))
            return
        var clamped = Math.max(port.min, Math.min(port.max, value))
        if (port.integer)
            clamped = Math.round(clamped)
        if (root.clipMode) {
            const next = Object.assign({}, root.pending)
            next[port.index] = clamped
            root.pending = next
            return
        }
        root.service.setPluginControl(root.selected, port.index, clamped, root.target)
    }
    // A control's value now: what is held for a clip, or what the live effect has.
    function valueOf(port) {
        if (root.clipMode) return root.pending[port.index] !== undefined ? root.pending[port.index] : port.value
        return root.service && root.service.pluginValuesRevision >= 0
            ? root.service.pluginPortValue(root.selected, port.index, root.target, port.value) : port.value
    }
    // A drag of a live effect's control is one automation gesture (Touch, Latch).
    function gesture(port, on) {
        if (root.service && !root.clipMode) root.service.automationGesture(root.target, "fx:" + root.selected + ":" + port.index, on)
    }

    // Makes writes visible to the drawn model. Separate from setPort because a
    // drag calls setPort per tick and must only commit on release -- the model
    // reset destroys the delegate being dragged.
    function commitPorts() {
        if (!root.service) return
        if (root.clipMode) {
            // Each held change is written as its own step; every write moves the
            // project's stamp on, so the next one is read afresh first.
            let stamp = root.clipStamp
            for (const key of Object.keys(root.pending)) {
                if (!root.service.setClipEffectControl(root.clipTrack, root.clipClip, root.clipSlot, Number(key), root.pending[key], stamp)) break
                stamp = root.service.clipEffectsTarget(root.clipTrack, root.clipClip).stamp || ""
            }
            root.pending = ({})
            root.clipEdited()
            return
        }
        root.service.flushPluginPorts()
    }

    // Every control back to the effect's default, as one change.
    function resetAll() {
        if (!root.service || (root.clipMode ? root.clipSlot < 0 : root.selected < 0)) return
        for (const p of root.ports) if (Scale.changed(p, root.valueOf(p))) root.setPort(p, p.defaultValue)
        root.commitPorts()
    }
}
