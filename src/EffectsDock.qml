// Delegates below reach the dock's own `root` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import "ui"

// The right dock: the Track Effects panel -- the focused track's effects by
// name, in the row anatomy of the concept (grip, on/off, name, trailing
// actions) -- then Sound, Recipes, the Inspector for the same track and the
// video reference.
//
// The list only: an effect's controls open in the middle of the window
// (EffectPanel) when its name is clicked, from its ⋯ menu, or when it is
// added. That panel is not modal, so the waveform and the keyboard keep working
// beside it.
Rectangle {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int currentTab: 0
    // Track tabs: Effects, Sound, Recipes, Inspector, Video. The master bus has
    // Chain, Bus and Loudness. Repair is the Spectral page (Effects ›
    // Restoration, the Inspector's Repair); track loudness is Effects › Set
    // Track Loudness… and the Sound tab's Match.
    function showInspector() { root.currentTab = root.master ? 1 : 3 }
    function showVideo() { if (!root.master) root.currentTab = 4 }
    function showRecipes() { if (!root.master) root.currentTab = 2 }
    Connections {
        target: root.shell
        function onRackTabRequested(tab) { if (!root.master) root.currentTab = tab }
    }

    // Whose chain: -1 is the focused track, service.masterTarget (-2) is the
    // master bus. The Mastering page hosts this dock pointed at the bus.
    property int target: -1
    readonly property bool master: root.service !== null && root.target === root.service.masterTarget

    // The slot whose panel was opened from here last, highlighted in the list.
    readonly property int selected: root.shell && root.shell.effectPanel.opened && root.track
        && root.shell.effectPanel.mode === "track" && root.shell.effectPanel.chain === root.track.id
        ? root.shell.effectPanel.slot : -1

    color: Theme.panelBg

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.hairline
        color: Theme.border
        z: 2
    }

    readonly property var track: {
        if (!root.service || root.service.tracksRevision < 0)
            return null
        if (root.master) {
            // The bus, in the shape of a track entry, so everything below reads
            // one kind of thing.
            return { id: root.service.masterTarget, name: qsTr("Master Bus"),
                     inserts: root.service.masterInserts, latency: root.service.masterLatency,
                     role: "master", color: "", isMaster: true }
        }
        // One track, by id: indexing `service.tracks` converts the whole list each time.
        const entry = root.service.focusedTrack >= 0 ? root.service.trackById(root.service.focusedTrack) : null
        return entry && entry.id !== undefined ? entry : null
    }

    readonly property var inserts: root.track ? (root.track.inserts || []) : []

    // Opens a slot's panel (and is what the tests call to reach its controls).
    function select(slot) {
        if (!root.shell || !root.track || slot < 0 || slot >= root.inserts.length) return
        root.shell.openEffectPanel(root.track.id, slot)
    }

    DockHeader {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        tabs: root.master ? [qsTr("Chain"), qsTr("Bus"), qsTr("Loudness")]
                          : [qsTr("Effects"), qsTr("Sound"), qsTr("Recipes"), qsTr("Inspector"), qsTr("Video")]
        currentIndex: root.currentTab
        onActivated: function (i) { root.currentTab = i }
        trailing: [
            IconButton {
                iconName: "panel-right"; text: qsTr("Hide Track Effects")
                helpText: qsTr("Hide this panel. Effects keep running.")
                helpScope: qsTr("Show it again with the rack button at the right of the toolbar.")
                shortcut: root.shell ? root.shell.bindingFor("show_rack") : ""
                onClicked: root.shell.runAction("show_rack")
            }
        ]
    }

    // ── Track Effects ────────────────────────────────────────────────────
    Item {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.currentTab === 0

        Column {
            id: rack
            anchors.fill: parent
            anchors.margins: Theme.s2
            spacing: Theme.s2

            // Whose chain this is.
            Row {
                width: parent.width
                spacing: Theme.s2
                Icon {
                    name: "sliders-horizontal"; color: Theme.textSecondary
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - Theme.szIcon - Theme.s2
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsHeading
                    font.weight: Theme.wHeading
                    color: Theme.textPrimary
                    text: root.track ? root.track.name : qsTr("No active track")
                }
            }
            Text {
                visible: root.track !== null
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
                text: {
                    if (!root.track)
                        return ""
                    var line = root.inserts.length === 1 ? qsTr("1 effect")
                             : qsTr("%1 effects").arg(root.inserts.length)
                    if (root.track.latency > 0)
                        line += "  ·  " + qsTr("%1 latency").arg(root.shell ? root.shell.formatLength(root.track.latency) : root.track.latency)
                    return line
                }
            }

            // ── the chain ───────────────────────────────────────────────
            Column {
                width: parent.width
                spacing: Theme.s1

                Repeater {
                    model: root.inserts

                    delegate: Rectangle {
                        id: slotRow
                        objectName: "rackSlot"
                        required property var modelData
                        required property int index
                        readonly property bool current: slotRow.index === root.selected

                        width: parent.width
                        height: Theme.hEffectRow + Theme.s1
                        radius: Theme.rControl
                        color: current ? (slotHover.hovered ? Theme.selectedHover : Theme.selectedFill)
                             : slotHover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
                        border.width: Theme.hairline
                        border.color: current ? Theme.borderStrong : Theme.border

                        HoverHandler { id: slotHover }
                        TapHandler { onTapped: root.select(slotRow.index) }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: slotMenu.popup()
                        }
                        ControlHelp {
                            title: slotRow.modelData.name
                            purpose: qsTr("An effect in this chain, in the order the audio passes through them. Click its name to open its controls in the middle of the window; the buttons switch it off, move and remove it.")
                        }
                        AppMenu {
                            id: slotMenu
                            objectName: "rackSlotMenu"
                            shell: root.shell
                            AppMenuItem {
                                objectName: "rackOpenPanel"
                                text: qsTr("Open Panel")
                                helpText: qsTr("Open this effect's controls in the middle of the window.")
                                helpScope: qsTr("Changes are live. One panel at a time.")
                                onTriggered: root.select(slotRow.index)
                            }
                            AppMenuItem {
                                text: qsTr("Open Effect Window")
                                helpText: qsTr("Open this effect's own window, as its maker designed it.")
                                enabled: windowButton.available
                                onTriggered: root.service.openPluginEditor(slotRow.index, root.target)
                            }
                            MenuSep {}
                            AppMenuItem {
                                text: slotRow.modelData.bypass ? qsTr("Switch On") : qsTr("Switch Off")
                                helpText: qsTr("Switch this effect off without removing it, or back on. Its settings are kept.")
                                onTriggered: root.act("bypass", slotRow.index)
                            }
                            AppMenuItem {
                                text: qsTr("Remove")
                                helpText: qsTr("Take this effect out of the chain. Its settings go with it.")
                                onTriggered: root.act("remove", slotRow.index)
                            }
                        }

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s1
                            anchors.rightMargin: Theme.s1
                            spacing: Theme.s1

                            Icon {
                                name: "grip-vertical"; color: Theme.textMuted
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            // Enable: a bypassed plugin is still there; it just is not
                            // doing anything, and it should look like that rather than gone.
                            IconButton {
                                objectName: "rackBypass"
                                iconName: slotRow.modelData.bypass ? "square" : "circle-check"
                                iconColor: slotRow.modelData.bypass ? Theme.textMuted : Theme.accent
                                text: slotRow.modelData.bypass ? qsTr("Enable") : qsTr("Bypass")
                                helpText: slotRow.modelData.bypass
                                          ? qsTr("Switch this effect back on. Bypass kept its settings and its place in the chain.")
                                          : qsTr("Switch this effect off without removing it. Bypass keeps its settings and its place in the chain.")
                                helpScope: qsTr("Acts on this effect only. Playback and exports skip a bypassed effect.")
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: root.act("bypass", slotRow.index)
                            }
                            Text {
                                objectName: "rackSlotName"
                                width: parent.width - Theme.szIcon - Theme.szIconHit * 6 - Theme.s1 * 7
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fsControl
                                color: !slotRow.modelData.available ? Theme.danger
                                     : slotRow.modelData.bypass ? Theme.textMuted
                                     : slotRow.current ? Theme.textPrimary : Theme.textSecondary
                                text: slotRow.modelData.available
                                      ? slotRow.modelData.name
                                      : qsTr("%1 (not installed)").arg(Theme.shortName(slotRow.modelData.name, 40))
                            }
                            IconButton {
                                id: windowButton
                                objectName: "rackOpenWindow"
                                // Re-read when the chain changes; the effect may have gained or lost its window.
                                readonly property bool available: root.service !== null && root.inserts.length > slotRow.index
                                    && !slotRow.modelData.bypass && root.service.pluginHasEditor(slotRow.index, root.target)
                                iconName: "maximize-2"; text: qsTr("Open effect window")
                                helpText: qsTr("Open this effect's own window, as its maker designed it. Changes there are part of the project like any control.")
                                helpScope: qsTr("Click the effect's name for OmaAudio's own panel instead. The window closes when the effect is removed or replaced.")
                                unavailableReason: slotRow.modelData.bypass ? qsTr("Turn the effect on to open its window.")
                                    : qsTr("This effect has no window of its own; click its name for its controls.")
                                enabled: available
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: root.service.openPluginEditor(slotRow.index, root.target) // a refusal shows in the status line
                            }
                            IconButton {
                                iconName: "chevron-up"; text: qsTr("Move earlier")
                                helpText: qsTr("Move this effect one place earlier, so it processes the audio before the effect above it.")
                                helpScope: qsTr("Effects run in order, so the sound can change. Acts on this chain only.")
                                unavailableReason: qsTr("This effect is already first in the chain.")
                                enabled: slotRow.index > 0
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: root.act("up", slotRow.index)
                            }
                            IconButton {
                                iconName: "chevron-down"; text: qsTr("Move later")
                                helpText: qsTr("Move this effect one place later, so it processes the audio after the effect below it.")
                                helpScope: qsTr("Effects run in order, so the sound can change. Acts on this chain only.")
                                unavailableReason: qsTr("This effect is already last in the chain.")
                                enabled: slotRow.index < root.inserts.length - 1
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: root.act("down", slotRow.index)
                            }
                            IconButton {
                                objectName: "rackSlotMore"
                                iconName: "ellipsis"; text: qsTr("More")
                                helpText: qsTr("Open Panel, the effect's own window, switch it off or remove it.")
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: slotMenu.popup()
                            }
                            IconButton {
                                objectName: "rackRemove"
                                iconName: "x"; text: qsTr("Remove")
                                helpText: qsTr("Take this effect out of the chain. Its settings go with it.")
                                helpScope: qsTr("Acts on this effect only. The source audio is unchanged.")
                                iconColor: hovered ? Theme.danger : Theme.textSecondary
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: root.act("remove", slotRow.index)
                            }
                        }
                    }
                }

                ToolButton {
                    objectName: "rackAdd"
                    iconName: "plus"; text: qsTr("Add Effect"); showLabel: true
                    helpText: qsTr("Choose an installed effect to add at the end of this chain. OmaAudio hosts LV2 and CLAP plugins, the two open formats on Linux.")
                    helpScope: root.master ? qsTr("Adds to the master bus, which processes the whole mix in playback and exports.")
                                           : qsTr("Adds to the active track. Effects run live on playback and in exports.")
                    unavailableReason: root.track === null ? qsTr("Click a track to make it active first.") : qsTr("Open or import audio first.")
                    shortcut: root.shell ? root.shell.bindingFor(root.master ? "show_master_plugins" : "show_plugins") : ""
                    enabled: root.track !== null && (!root.master || root.service.trackCount > 0)
                    onClicked: root.shell.runAction(root.master ? "show_master_plugins" : "show_plugins")
                }

                // The bus's recommended order, one click each: what the brief calls
                // the fixed chain, offered rather than imposed, and only the ones
                // this box has.
                Column {
                    visible: root.master
                    width: parent.width
                    spacing: Theme.s1
                    Text {
                        text: qsTr("MASTERING CHAIN")
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        font.weight: Theme.wHeading
                        color: Theme.textMuted
                    }
                    Repeater {
                        model: [
                            { label: qsTr("Corrective EQ"), uri: "http://calf.sourceforge.net/plugins/Equalizer8Band" },
                            { label: qsTr("Multiband dynamics"), uri: "http://calf.sourceforge.net/plugins/MultibandCompressor" },
                            { label: qsTr("Stereo width"), uri: "http://calf.sourceforge.net/plugins/StereoTools" },
                            { label: qsTr("Saturation"), uri: "http://calf.sourceforge.net/plugins/Saturator" },
                            { label: qsTr("Limiter"), uri: "http://calf.sourceforge.net/plugins/Limiter" }
                        ]
                        delegate: ToolButton {
                            required property var modelData
                            objectName: "masterChainAdd"
                            width: rack.width
                            iconName: "plus"
                            text: modelData.label
                            showLabel: true
                            helpText: qsTr("Add the Calf %1 plugin to the end of the master bus chain.").arg(modelData.label)
                            helpScope: qsTr("The master bus processes the whole mix in playback and exports. Remove the effect to take it out again.")
                            helpTopic: qsTr("These are Calf LV2 plugins, which come with OmaAudio. If one is missing, the message bar says so and nothing is added.")
                            unavailableReason: qsTr("Open or import audio first.")
                            enabled: root.service !== null && root.service.trackCount > 0
                            onClicked: if (root.service) root.service.addPlugin(modelData.uri, root.target)
                        }
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                        text: qsTr("Calf's, in the order a master usually runs. Any LV2 plugin goes on the bus through Add Effect.")
                    }
                }

                Text {
                    objectName: "rackHint"
                    visible: root.inserts.length > 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                    text: qsTr("Click an effect's name to open its controls.")
                }
                Text {
                    visible: root.inserts.length === 0 && !root.master
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                    text: root.track ? qsTr("No effects on this track yet. Effects stack here and run live on playback.")
                                     : qsTr("Open a take first.")
                }
            }

        }
    }

    // ── Sound ───────────────────────────────────────────────────────────
    // Made when the tab is first shown, so a page that is still being built in
    // the background does not also build a tab nobody has opened.
    Loader {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        active: root.currentTab === 1 && !root.master
        visible: active
        sourceComponent: SoundPanel {
            shell: root.shell
            trackId: root.track && !root.master ? root.track.id : -1
        }
    }

    // ── Recipes ─────────────────────────────────────────────────────────
    // Made when the tab is first shown, like Sound, and then kept: a recipe
    // being changed, or steps being recorded while you work in another tab,
    // must still be there when you come back.
    property bool recipesKept: false
    onCurrentTabChanged: if (root.currentTab === 2 && !root.master) root.recipesKept = true
    Loader {
        objectName: "recipesLoader"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        active: !root.master && (root.currentTab === 2 || root.recipesKept)
        visible: root.currentTab === 2 && !root.master
        sourceComponent: RecipesPanel {
            shell: root.shell
        }
    }

    // ── Inspector ───────────────────────────────────────────────────────
    TrackInspector {
        objectName: "trackInspector"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.currentTab === 3 && root.track !== null && !root.master
        shell: root.shell
        track: root.master ? null : root.track
    }
    EmptyState {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.currentTab === 3 && root.track === null && !root.master
        iconName: "info"
        title: qsTr("No active track")
        description: qsTr("Click a lane or a mixer strip to inspect it.")
    }

    // ── the bus itself (master only) ────────────────────────────────────
    Column {
        objectName: "busPanel"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        anchors.margins: Theme.s2
        anchors.topMargin: Theme.s2
        spacing: Theme.s2
        visible: root.currentTab === 1 && root.master

        Text {
            text: qsTr("MASTER BUS")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading; color: Theme.textMuted
        }
        SliderField {
            objectName: "masterTrim"
            width: parent.width
            label: qsTr("Input trim")
            helpText: qsTr("Adjust the mix level before the master effects. Decibels (dB) measure gain: 0 leaves the level unchanged.")
            helpScope: qsTr("Range: −24 to +24 dB. This affects playback and exports.")
            helpTopic: qsTr("Drag the slider or type in its number field. Negative values reduce the level; positive values increase it and may drive effects harder. Monitor volume is a separate listening control.")
            from: -24; to: 24; stepSize: 0.1; decimals: 1; suffix: " dB"
            origin: 0
            value: root.service ? root.service.masterTrimDb : 0
            onEdited: function (v) { if (root.service) root.service.setMasterTrimDb(v) }
            onEditingStarted: if (root.service) root.service.automationGesture(-2, "volume", true)
            onEditingFinished: if (root.service) root.service.automationGesture(-2, "volume", false)
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            text: qsTr("This level changes the sound before the master effects and is included in exports. The monitor level only changes what you hear.")
        }
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
        Text {
            width: parent.width
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
            text: {
                if (!root.service) return ""
                var line = root.inserts.length === 1 ? qsTr("1 effect on the bus") : qsTr("%1 effects on the bus").arg(root.inserts.length)
                if (root.service.masterLatency > 0)
                    line += "  ·  " + qsTr("%1 latency, compensated in playback and export").arg(root.shell ? root.shell.formatLength(root.service.masterLatency) : root.service.masterLatency)
                return line
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            text: qsTr("Dither and loudness normalisation are delivery settings, applied when a file is written, so the bus stays at full precision.")
        }
    }

    VideoPanel {
        objectName: "videoPanel"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.currentTab === 4 && !root.master
        shell: root.shell
    }
    LoudnessPanel {
        objectName: "loudnessPanel"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.leftMargin: Theme.hairline
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.currentTab === 2 && root.master
        shell: root.shell
    }

    // ── actions ─────────────────────────────────────────────────────────

    function act(what, slot) {
        if (!root.service)
            return
        switch (what) {
        case "bypass":
            root.service.setPluginBypass(slot, !root.inserts[slot].bypass, root.target)
            break
        case "up":
            // The selection follows its plugin through onPluginPortsChanged.
            if (slot > 0)
                root.service.movePlugin(slot, slot - 1, root.target)
            break
        case "down":
            if (slot < root.inserts.length - 1)
                root.service.movePlugin(slot, slot + 1, root.target)
            break
        case "remove":
            root.service.removePlugin(slot, root.target)
            break
        }
    }
}
