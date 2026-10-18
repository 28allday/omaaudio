pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

C.Popup {
    id: root
    objectName: "routingPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int target: -1
    property int destination: -2
    property int keySource: -1
    property var sends: []
    property var descriptor: ({})
    property string message: ""
    // Track data only while shown: a closed panel would otherwise re-evaluate its
    // per-track controls on every edit, which adds up in large sessions.
    property bool showing: false
    readonly property var tracks: showing && service ? service.trackSummaries : []
    readonly property var destinations: tracks.filter(t => t.id !== target)
    readonly property var outputIds: [-2, -3].concat(destinations.map(t => t.id))
    readonly property var keyIds: [-1].concat(destinations.map(t => t.id))
    readonly property bool editable: service && target >= 0 && !service.recording && !service.loading
    readonly property string editReason: !service || target < 0 ? qsTr("Add a track or click an existing one first.")
                                       : service.recording ? qsTr("Stop recording first.")
                                       : service.loading ? qsTr("Wait for the file to finish loading.") : ""
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(710, parent ? parent.width - 32 : 710)
    height: Math.min(body.implicitHeight + padding * 2, parent ? parent.height - 40 : 800)
    padding: Theme.s4; modal: true; focus: true
    // The scrolling column reaches into this margin (ScrollPane).
    leftPadding: Theme.s4 - Theme.s2; rightPadding: Theme.s4 - Theme.s3
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    function refresh() {
        descriptor = service ? service.routingTarget(target) : {}
        destination = descriptor.output === undefined ? -2 : descriptor.output
        keySource = descriptor.sidechain === undefined ? -1 : descriptor.sidechain
        sends = (descriptor.sends || []).map(s => ({target: s.target, db: s.db, pre: s.pre}))
    }
    function updateSend(index, key, value) {
        const next = sends.map(s => ({target: s.target, db: s.db, pre: s.pre}))
        next[index][key] = value; sends = next
    }
    function addSend() {
        const track = destinations.find(t => !sends.some(s => s.target === t.id))
        if (track) sends = sends.concat([{target: track.id, db: -12, pre: false}])
    }
    onAboutToShow: showing = true
    onOpened: { target = service ? service.focusedTrack : -1; message = ""; refresh() }
    onClosed: { showing = false; if (shell) shell.restoreFocus() }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }
    contentItem: ScrollPane {
        id: routingScroll
        clip: true; contentWidth: availableWidth
        ColumnLayout {
            id: body; x: routingScroll.reachLeft; width: routingScroll.columnWidth; spacing: Theme.s3
            RowLayout {
                Layout.fillWidth: true
                Text { text: qsTr("Routing"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
                ToolButton { text: qsTr("Close routing"); iconName: "x"; helpText: qsTr("Close without applying. Route changes not yet applied are dropped."); onClicked: root.close() }
            }
            ComboField {
                objectName: "routingTrack"; caption: qsTr("Track"); Layout.fillWidth: true
                helpText: qsTr("Choose which track's routes to view and edit.")
                helpScope: qsTr("Switching tracks drops route changes not yet applied.")
                model: root.tracks.map(t => t.name); currentIndex: root.tracks.findIndex(t => t.id === root.target)
                onActivated: index => { root.target = root.tracks[index].id; root.message = ""; root.refresh() }
            }
            ComboField {
                objectName: "routingOutput"; caption: qsTr("Main output"); Layout.fillWidth: true; enabled: root.editable
                helpText: qsTr("Where this track's signal goes after its effects, fader and pan: the Master bus, another track acting as a bus, or nowhere.")
                helpScope: qsTr("A bus is a track that receives other tracks, processes their sum and passes it on. None keeps the sends and sidechain feeds working.")
                unavailableReason: root.editReason
                model: [qsTr("Master"), qsTr("None")].concat(root.destinations.map(t => t.name))
                currentIndex: root.outputIds.indexOf(root.destination)
                onActivated: index => root.destination = root.outputIds[index]
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: qsTr("A bus combines several tracks so you can control them together. Main output chooses where the sound goes. None turns off that output; sends and trigger tracks can still work.")
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: qsTr("Sends to other tracks"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; Layout.fillWidth: true }
                ToolButton { objectName: "routingAddSend"; text: qsTr("Add send"); iconName: "plus"; showLabel: true
                    helpText: qsTr("Send a copy of this track to another track at its own level, for example to a reverb bus.")
                    helpScope: qsTr("A send is an extra copy; it leaves the main output unchanged. Add up to eight, with one per destination.")
                    unavailableReason: !root.editable ? root.editReason
                                     : root.sends.length >= 8 ? qsTr("A track can have at most eight sends.")
                                     : qsTr("Every other track already receives a send from this one.")
                    enabled: root.editable && root.sends.length < 8 && root.sends.length < root.destinations.length; onClicked: root.addSend() }
            }
            Repeater {
                // Counted, not listed: editing a send replaces the array, and a
                // Repeater over the array itself rebuilt every row on each step,
                // taking the field being dragged (and its focus) with it.
                model: root.sends.length
                RowLayout {
                    id: sendRow
                    required property int index
                    readonly property var modelData: root.sends[index] || ({target: -1, db: 0, pre: false})
                    Layout.fillWidth: true; enabled: root.editable
                    ComboField {
                        objectName: "routingSendTarget" + sendRow.index; Layout.fillWidth: true; caption: qsTr("To")
                        helpTitle: qsTr("Send destination")
                        helpText: qsTr("The track that receives this send.")
                        unavailableReason: root.editReason
                        model: root.destinations.map(t => t.name); currentIndex: root.destinations.findIndex(t => t.id === sendRow.modelData.target)
                        onActivated: index => root.updateSend(sendRow.index, "target", root.destinations[index].id)
                    }
                    ValueField {
                        objectName: "routingSendLevel" + sendRow.index; Layout.preferredWidth: 90
                        helpTitle: qsTr("Send level")
                        helpText: qsTr("How loud the copy is, in decibels (dB). 0 dB matches the track; −120 dB is effectively silent.")
                        helpScope: qsTr("Range −120 to +12 dB. Type a value, use Up/Down, or drag the number sideways.")
                        unavailableReason: root.editReason
                        from: -120; to: 12; decimals: 1; step: 0.5; suffix: "dB"; value: sendRow.modelData.db
                        onEdited: value => root.updateSend(sendRow.index, "db", value)
                    }
                    ComboField {
                        objectName: "routingSendMode" + sendRow.index; Layout.preferredWidth: 120
                        helpTitle: qsTr("Send point")
                        helpText: qsTr("Pre-fader takes the signal after the effects but before the fader and pan. Post-fader includes the fader and pan.")
                        helpScope: qsTr("Mute silences the send either way.")
                        unavailableReason: root.editReason
                        model: [qsTr("Post-fader"), qsTr("Pre-fader")]; currentIndex: sendRow.modelData.pre ? 1 : 0
                        onActivated: index => root.updateSend(sendRow.index, "pre", index === 1)
                    }
                    ToolButton { objectName: "routingRemoveSend" + sendRow.index; text: qsTr("Remove send"); iconName: "x"
                        helpText: qsTr("Drop this send from the list. It is only removed from the project when you apply.")
                        unavailableReason: root.editReason
                        onClicked: root.sends = root.sends.filter((s, i) => i !== sendRow.index) }
                }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: qsTr("Pre-fader: the copy includes effects but ignores this track's volume and pan. Post-fader: it follows both. Mute silences either type.")
            }
            ComboField {
                objectName: "routingSidechain"; caption: qsTr("Trigger track"); Layout.fillWidth: true; enabled: root.editable
                helpText: qsTr("Choose another track whose signal drives effects on this track that accept a sidechain, such as a ducking compressor.")
                helpScope: qsTr("Only effects with a sidechain input can use a trigger track. Turn on their external-input mode. The trigger follows its source track's volume and pan.")
                unavailableReason: root.editReason
                model: [qsTr("None")].concat(root.destinations.map(t => t.name)); currentIndex: root.keyIds.indexOf(root.keySource)
                onActivated: index => root.keySource = root.keyIds[index]
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: root.descriptor.keySlots > 0
                    ? qsTr("%1 effect(s) accept an external sidechain. Enable their external-key mode in the effect controls; the key follows its source’s fader and pan.").arg(root.descriptor.keySlots)
                    : qsTr("Add an effect with a sidechain input to use a trigger track. You can choose the track before adding the effect.")
            }
            Text { objectName: "routingMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                text: root.message || (root.target < 0 ? qsTr("Add a track or click an existing one to set its routes.") : qsTr("Press Apply to use these routes. Undo restores the previous routes. A route cannot send sound back into itself."))
                color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
            RowLayout {
                ToolButton { objectName: "routingReload"; text: qsTr("Reload"); showLabel: true
                    helpText: qsTr("Discard route changes not yet applied and show the routes currently in use.")
                    onClicked: { root.refresh(); root.message = "" } }
                Item { Layout.fillWidth: true }
                ToolButton {
                    objectName: "routingApply"; text: qsTr("Apply routing"); showLabel: true; enabled: root.editable && root.descriptor.track !== undefined
                    helpText: qsTr("Apply this track's output, sends and trigger track. Undo restores the previous routes.")
                    helpScope: qsTr("Routes that would feed a track back into itself are refused and the message below says why.")
                    unavailableReason: root.editReason || qsTr("Routing information is not available for this track. Reload.")
                    onClicked: {
                        if (root.service.setTrackRouting(root.target, root.destination, root.keySource, root.sends, root.descriptor.stamp)) {
                            root.refresh(); root.message = qsTr("Routing updated.")
                        } else root.message = root.service.lastError
                    }
                }
            }
        }
    }
}
