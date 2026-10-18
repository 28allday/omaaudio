pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// One effect's controls, in the middle of the window: an effect on a track, on
// the master bus or on a clip, opened by clicking its name in the side panel
// (or its ⋯ menu's Open Panel, or the mixer strip's FX chip). Changes are live:
// what you set is what plays, with no Apply. It is not modal, so the transport,
// the waveform and the keyboard keep working behind it; drag it by its title,
// resize it from the corner. One at a time: opening another effect's panel
// moves this one over to it.
//
// The body is the same as the Apply panel's: groups with numbered scales, knobs
// for dynamics, the curve for an EQ, the effect's meters, presets. A clip's
// effect has no live instance, so a change to it is written when you let go
// (one Undo step) and its sound is prepared again; a control that follows a
// clip curve is read here and set on the curve.
C.Popup {
    id: root
    objectName: "effectPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null

    // What it shows: "track" (a track's or the bus's chain, by id) or "clip".
    property string mode: "track"
    property int chain: -1
    property int slot: -1
    property int clipTrack: -1
    property int clipClip: -1
    property var clipTarget: ({})

    // Where it sat and how big it was, for the rest of this session.
    property real savedX: -1
    property real savedY: -1
    property real savedWidth: 760
    property real savedHeight: -1
    readonly property real minWidth: 520
    readonly property real minHeight: 300

    // Playback this panel began, so closing it stops only that.
    property bool panelPlaying: false
    property bool loopBefore: false

    readonly property bool forClip: root.mode === "clip"
    readonly property bool master: root.service !== null && !root.forClip && root.chain === root.service.masterTarget
    readonly property var trackEntry: root.opened && root.service && !root.master && root.service.tracksRevision >= 0
        ? root.service.trackById(root.forClip ? root.clipTrack : root.chain) : null
    readonly property var inserts: root.forClip ? (root.clipTarget.effects || [])
        : root.master ? (root.service.masterInserts || [])
        : root.trackEntry && root.trackEntry.inserts ? root.trackEntry.inserts : []
    readonly property var insert: root.slot >= 0 && root.slot < root.inserts.length ? root.inserts[root.slot] : null
    readonly property bool bypassed: root.insert !== null && root.insert.bypass === true
    readonly property bool portsOwn: !root.forClip && root.service !== null && root.slot >= 0
        && root.service.pluginPortsSlot === root.slot && root.service.pluginPortsTarget === root.chain
    readonly property var ports: root.forClip ? (root.insert ? (root.insert.controls || []) : [])
        : root.portsOwn ? (root.service.pluginPorts || []) : []
    readonly property var outputs: root.portsOwn ? (root.service.pluginOutputs || []) : []
    readonly property string kind: root.portsOwn ? root.service.pluginPortsKind : "other"
    readonly property bool knobs: !root.forClip && root.kind === "dynamics" && root.ports.length > 0 && root.ports.length <= 16
    readonly property bool reading: !root.forClip && root.service !== null && !(root.portsOwn && root.service.pluginPortsComplete)
    readonly property string ownerName: root.forClip ? qsTr("%1 · clip %2").arg(Theme.shortName(root.clipTarget.trackName || "", 32))
                                                       .arg(Theme.shortName(root.clipTarget.name || "", 32))
        : root.master ? qsTr("Master Bus") : root.trackEntry ? Theme.shortName(root.trackEntry.name || "", 40) : ""
    readonly property string fullOwnerName: root.forClip ? (root.clipTarget.trackName || "") + " · " + (root.clipTarget.name || "")
        : root.master ? qsTr("Master Bus") : root.trackEntry ? (root.trackEntry.name || "") : ""

    // ── opening ──────────────────────────────────────────────────────────
    function openFor(chain, slot) {
        root.mode = "track"; root.chain = chain; root.slot = slot
        root.clipTarget = ({})
        if (root.service && slot >= 0) root.service.requestPluginPorts(slot, chain)
        if (!root.opened) root.open()
        else body.forceActiveFocus()
    }
    function openForClip(track, clip, slot) {
        root.mode = "clip"; root.clipTrack = track; root.clipClip = clip; root.slot = slot
        root.reloadClip()
        if (!root.clipTarget.valid) { if (root.shell) root.shell.say(root.clipTarget.reason || qsTr("That clip is gone.")); return }
        if (!root.opened) root.open()
        else body.forceActiveFocus()
    }
    function reloadClip() {
        if (!root.service || !root.forClip) return
        root.clipTarget = root.service.clipEffectsTarget(root.clipTrack, root.clipClip)
        if (root.opened && (!root.clipTarget.valid || root.slot >= (root.clipTarget.effects || []).length)) root.close()
    }

    parent: C.Overlay.overlay
    width: Math.max(root.minWidth, Math.min(root.savedWidth, parent ? parent.width - 16 : root.savedWidth))
    height: root.savedHeight > 0 ? Math.max(root.minHeight, Math.min(root.savedHeight, parent ? parent.height - 16 : root.savedHeight))
                                 : Math.min(parent ? parent.height - 64 : 720, Math.max(root.minHeight, implicitHeight))
    padding: Theme.s4
    modal: false; dim: false; focus: true
    closePolicy: C.Popup.CloseOnEscape

    // In the middle until it is moved; then where it was put, kept on screen.
    property bool userPlaced: false
    function place() {
        if (!parent) return
        const w = parent.width, h = parent.height
        const x = root.userPlaced ? root.savedX : (w - root.width) / 2
        const y = root.userPlaced ? root.savedY : (h - root.height) / 2
        root.x = Math.round(Math.max(0, Math.min(w - root.width, x)))
        root.y = Math.round(Math.max(8, Math.min(h - root.height - 8, y)))
    }
    onAboutToShow: root.place()
    onHeightChanged: if (root.visible) root.place()
    onWidthChanged: if (root.visible) root.place()
    onOpened: {
        root.panelPlaying = false
        root.maybeAutoStart()
        body.forceActiveFocus()
    }
    onClosed: {
        if (root.panelPlaying && root.service && root.service.playing) root.service.stop()
        if (root.panelPlaying && root.service && root.service.loopEnabled !== root.loopBefore) root.service.setLoopEnabled(root.loopBefore)
        root.panelPlaying = false
        if (root.shell) root.shell.restoreFocus()
    }

    // Following the effect: it can move in its chain, or go.
    Connections {
        target: root.service
        function onPluginPortsChanged() {
            if (!root.opened || root.forClip || root.service.pluginPortsTarget !== root.chain) return
            const at = root.service.pluginPortsSlot
            if (at === root.slot) return
            if (at >= 0) root.slot = at
            else if (root.slot >= root.inserts.length) root.close()
        }
        // The Apply panel takes over the effect controls while it is open.
        function onEffectPreviewChanged() { if (root.opened && root.service.effectPreview.active === true) root.close() }
        function onClipEffectsChanged() { if (root.opened && root.forClip) root.reloadClip() }
    }
    // A chain is rebuilt after an edit, and an effect only just added is not in
    // the engine yet: the controls are asked for again once it is.
    // Acted on after the bindings settle: closing from inside the change would
    // re-read trackEntry while it is still being worked out (a binding loop).
    onInsertsChanged: Qt.callLater(root.followInserts)
    function followInserts() {
        if (!root.opened || root.forClip) return
        if (root.slot >= root.inserts.length) root.close()
        else if (!root.portsOwn && root.service) root.service.requestPluginPorts(root.slot, root.chain)
    }

    // ── hearing it ──────────────────────────────────────────────────────
    // Starts by itself only when nothing is playing and there is an output.
    function maybeAutoStart() {
        if (!root.service || root.service.playing || !root.service.audioOutputRunning()) return
        root.play()
    }
    function play() {
        const s = root.service
        if (!s) return
        root.loopBefore = s.loopEnabled
        // The selection on this effect's track, over and over; otherwise from the playhead.
        const owner = root.forClip ? root.clipTrack : root.master ? -99 : root.chain
        if (!s.selectionEmpty && s.selectionTrack === owner) {
            if (!s.loopEnabled) s.setLoopEnabled(true)
            if (s.playhead < s.selectionStart || s.playhead >= s.selectionEnd) s.seekFrames(s.selectionStart)
        }
        s.play()
        root.panelPlaying = true
    }
    function togglePlay() {
        if (!root.service) return
        if (root.service.playing) {
            root.service.stop()
            if (root.panelPlaying && root.service.loopEnabled !== root.loopBefore) root.service.setLoopEnabled(root.loopBefore)
            root.panelPlaying = false
            return
        }
        root.play()
    }
    function setBypass(off) {
        if (!root.service || root.slot < 0) return
        if (root.forClip) {
            if (!root.service.setClipEffectBypass(root.clipTrack, root.clipClip, root.slot, off, root.clipTarget.stamp || "") && root.shell)
                root.shell.say(root.service.lastError)
            root.reloadClip()
        } else root.service.setPluginBypass(root.slot, off, root.chain)
    }
    function resetAll() {
        if (root.knobs) knobPanel.resetAll()
        else controls.resetAll()
    }

    background: Rectangle {
        color: Theme.panelBg; radius: Theme.rPanel
        border.width: Theme.hairline; border.color: Theme.borderStrong
        // Resize from the corner.
        MouseArea {
            objectName: "effectPanelResize"
            width: 18; height: 18
            anchors.right: parent.right; anchors.bottom: parent.bottom
            cursorShape: Qt.SizeFDiagCursor
            property point start
            property real startWidth
            property real startHeight
            onPressed: function (e) { start = mapToItem(null, e.x, e.y); startWidth = root.width; startHeight = root.height }
            onPositionChanged: function (e) {
                if (!pressed) return
                const at = mapToItem(null, e.x, e.y)
                root.savedWidth = Math.max(root.minWidth, startWidth + at.x - start.x)
                root.savedHeight = Math.max(root.minHeight, startHeight + at.y - start.y)
            }
            Icon { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 2; name: "move-diagonal"; size: 10; color: Theme.textMuted }
            ControlHelp { title: qsTr("Resize"); purpose: qsTr("Drag to make this panel bigger or smaller. It keeps its size until OmaAudio closes.") }
        }
    }

    contentItem: ColumnLayout {
        id: body
        spacing: Theme.s3
        focus: true
        // Space plays or pauses, as it does everywhere else.
        Keys.onSpacePressed: (event) => { root.togglePlay(); event.accepted = true }
        // Every other shortcut works as if the window had the keyboard; Escape
        // closes this panel.
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape || !root.shell) return
            if (root.shell.handleKey(event)) event.accepted = true
        }

        // ── the title: drag it to move the panel ──────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: titleRow.implicitHeight
            MouseArea {
                objectName: "effectPanelDrag"
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                property point start
                property point origin
                onPressed: function (e) { start = mapToItem(null, e.x, e.y); origin = Qt.point(root.x, root.y) }
                onPositionChanged: function (e) {
                    if (!pressed || !root.parent) return
                    const at = mapToItem(null, e.x, e.y)
                    root.x = Math.max(0, Math.min(root.parent.width - 80, origin.x + at.x - start.x))
                    root.y = Math.max(0, Math.min(root.parent.height - 40, origin.y + at.y - start.y))
                    root.savedX = root.x; root.savedY = root.y; root.userPlaced = true
                }
            }
            RowLayout {
                id: titleRow
                anchors.fill: parent
                spacing: Theme.s2
                Toggle {
                    objectName: "effectPanelPower"
                    checked: !root.bypassed
                    helpTitle: qsTr("Effect on")
                    helpText: qsTr("Switch this effect on or off. Off keeps its settings and its place in the chain.")
                    helpScope: qsTr("Playback and exports skip an effect that is off.")
                    enabled: root.insert !== null
                    onToggled: root.setBypass(!checked)
                }
                Text {
                    objectName: "effectPanelTitle"
                    Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText
                    text: root.insert ? Theme.shortName(root.insert.name, 48) : ""
                    color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading
                    ControlHelp {
                        title: root.insert ? root.insert.name : ""
                        purpose: qsTr("On %1. Drag the title to move this panel.").arg(root.fullOwnerName)
                    }
                }
                Text {
                    objectName: "effectPanelOwner"
                    Layout.maximumWidth: root.width * 0.4
                    elide: Text.ElideMiddle; textFormat: Text.PlainText
                    text: root.ownerName
                    color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                }
                ToolButton {
                    objectName: "effectPanelClose"
                    iconName: "x"; text: qsTr("Close")
                    helpText: qsTr("Close this panel. The effect stays as you set it; click its name in the side panel to open it again.")
                    helpScope: qsTr("Escape does the same.")
                    onClicked: root.close()
                }
            }
        }

        // ── hearing it ─────────────────────────────────────────────────
        RowLayout {
            objectName: "rackPreview"
            spacing: Theme.s2
            ToolButton {
                objectName: "rackPreviewPlay"
                iconName: root.service && root.service.playing ? "pause" : "play"
                text: root.service && root.service.playing ? qsTr("Pause") : qsTr("Play")
                showLabel: true
                helpText: qsTr("Play through this effect while you change it: the selection on this track over and over, or from the playhead.")
                helpScope: qsTr("Space does the same while this panel has the keyboard.")
                unavailableReason: qsTr("Open or import audio first.")
                enabled: root.service !== null && root.service.trackCount > 0
                onClicked: root.togglePlay()
            }
            SegmentedControl {
                objectName: "rackCompare"
                labels: [qsTr("Before"), qsTr("After")]
                currentIndex: root.bypassed ? 0 : 1
                helpTitle: qsTr("Before and after")
                helpText: qsTr("Before is the sound without this effect, After is with it. Switches straight away, without stopping.")
                helpScope: root.forClip ? qsTr("The same as switching the effect off. On a clip, each switch is an Undo step and the clip's sound is prepared again.")
                                     : qsTr("The same as switching the effect off.")
                enabled: root.insert !== null
                onActivated: function (index) { root.setBypass(index === 0) }
            }
            Item { Layout.fillWidth: true }
            Row {
                spacing: Theme.s1
                visible: root.service !== null && root.service.playing && !root.forClip
                Rectangle { width: 6; height: 6; radius: 3; color: Theme.success; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    objectName: "effectPanelLive"
                    text: root.bypassed ? qsTr("Before · the effect is off") : qsTr("Live · changes are heard as you move them")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                }
            }
        }

        // ── the effect's window, its defaults and presets ─────────────────
        RowLayout {
            spacing: Theme.s2
            ToolButton {
                objectName: "effectPanelWindow"
                visible: !root.forClip
                iconName: "maximize-2"; text: qsTr("Effect window"); showLabel: true
                readonly property bool available: root.service !== null && root.insert !== null && !root.bypassed
                    && root.service.tracksRevision >= 0 && root.service.pluginHasEditor(root.slot, root.chain)
                helpText: qsTr("Open the effect's own window, as its maker designed it. What you set there is part of the project too.")
                unavailableReason: root.bypassed ? qsTr("Switch the effect on to open its window.")
                                   : qsTr("This effect has no window of its own; use its controls below.")
                enabled: available
                onClicked: root.service.openPluginEditor(root.slot, root.chain)
            }
            ToolButton {
                objectName: "effectPanelReset"
                iconName: "rotate-ccw"; text: qsTr("Reset all"); showLabel: true
                helpText: qsTr("Put every control back to the effect's default.")
                helpScope: root.forClip ? qsTr("Each control put back is an Undo step.") : qsTr("Control changes are not Undo steps; a preset can save and restore them.")
                enabled: root.insert !== null && root.ports.length > 0
                onClicked: root.resetAll()
            }
            Item { Layout.fillWidth: true }
            Text {
                text: qsTr("Preset")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }
            PresetChoice {
                id: presetChoice
                objectName: "effectPanelPreset"
                Layout.preferredWidth: 200
                context: !root.opened || !root.service || root.insert === null ? ({})
                    : root.forClip ? (void root.clipTarget, root.service.clipEffectPresetContext(root.clipTrack, root.clipClip, root.slot))
                    : root.portsOwn ? root.service.pluginPresetContext(root.chain, root.slot) : ({})
                onDefaultChosen: root.resetAll()
                onPresetChosen: function (id) {
                    const ok = root.forClip
                        ? root.service.applyClipEffectPreset(root.clipTrack, root.clipClip, root.slot, id, context.stamp || "")
                        : root.service.applyPluginPreset(root.chain, root.slot, context.stamp, id)
                    if (!ok && root.shell) root.shell.say(root.service.lastError)
                    if (root.forClip) root.reloadClip()
                }
            }
            ToolButton {
                objectName: "rackPresets"
                visible: !root.forClip
                text: qsTr("Save…"); showLabel: true
                helpText: qsTr("Save these settings as a preset for this effect, or apply one you saved before.")
                helpScope: qsTr("Applying a preset is one Undo step.")
                enabled: root.insert !== null && root.shell !== null
                onClicked: root.shell.openPluginPresets(root.chain, root.slot)
            }
        }

        Text {
            visible: root.reading && root.insert !== null
            color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Reading controls…")
        }
        EmptyState {
            objectName: "effectPanelEmpty"
            Layout.fillWidth: true
            visible: root.insert !== null && !root.reading && root.ports.length === 0 && root.outputs.length === 0
            title: qsTr("No controls of its own")
            description: root.forClip ? qsTr("This effect has no settings to change here.")
                                   : qsTr("This effect keeps its settings in its own window: use Effect window above.")
        }
        PluginControlList {
            id: controls
            objectName: "rackControls"
            visible: !root.knobs && (root.ports.length > 0 || root.outputs.length > 0)
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: visible ? Theme.hControl * 3 : 0
            Layout.preferredHeight: visible ? Math.min(contentHeight, 480) : 0
            rowName: "rackPort"
            service: root.service
            selected: root.forClip ? -1 : root.slot
            target: root.chain
            ports: root.knobs ? [] : root.ports
            outputs: []  // the meters have their own box below
            bypassed: root.bypassed
            kind: root.kind
            uri: root.insert ? (root.insert.uri || "") : ""
            clipMode: root.forClip
            clipTrack: root.clipTrack
            clipClip: root.clipClip
            clipSlot: root.forClip ? root.slot : -1
            clipStamp: root.clipTarget.stamp || ""
            onClipEdited: root.reloadClip()
            onAutomateRequested: function (port) {
                if (root.shell) root.shell.openClipEffectAutomation(root.clipTrack, root.clipClip, root.slot, port)
            }
        }
        PluginKnobs {
            id: knobPanel
            objectName: "effectPanelKnobs"
            visible: root.knobs
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: visible ? 160 : 0
            Layout.preferredHeight: visible ? Math.min(contentHeight, 480) : 0
            service: root.service
            selected: root.slot
            target: root.chain
            ports: root.knobs ? root.ports : []
            outputs: []
            bypassed: root.bypassed
        }
        // The meters stay in view under the controls, however long they are.
        PluginMeterBox {
            objectName: "effectMeterBox"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            service: root.service
            selected: root.forClip ? -1 : root.slot
            target: root.chain
            outputs: root.outputs
            bypassed: root.bypassed
            labelWidth: root.knobs ? 110 : controls.labelWidth
        }
        // How this effect's automation is timed: what its format can take.
        Text {
            objectName: "rackTiming"
            Layout.fillWidth: true
            visible: !root.forClip && text !== ""
            wrapMode: Text.WordWrap; textFormat: Text.PlainText
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            readonly property string timing: root.insert ? (root.insert.timing || "") : ""
            text: timing === "sample" ? qsTr("Automation: exact point timing, with small steps between points.")
                : timing === "block" ? qsTr("Automation: exact point timing, with 64-sample steps between points.")
                : ""
            ControlHelp {
                title: qsTr("Automation timing")
                purpose: parent.timing === "sample"
                          ? qsTr("Saved points reach the effect at their exact time. Changes between them normally update every audio sample. If many settings change together, the app uses larger steps between points; the points themselves stay exact.")
                          : qsTr("Saved points reach the effect at their exact time. Between points, changes update every 64 audio samples. The effect may smooth the changes further, making its response slower.")
                scope: qsTr("Movements recorded with Write, Touch or Latch are timed to the audio processing blocks.")
            }
        }
        Text {
            objectName: "effectPanelClipNote"
            Layout.fillWidth: true
            visible: root.forClip
            wrapMode: Text.WordWrap; textFormat: Text.PlainText
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            text: qsTr("A clip's effect: each change is written when you let go, as one Undo step, and the clip's sound is prepared again.")
        }
    }
}
