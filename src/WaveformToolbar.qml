pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The strip over the Waveform page's lanes: the brief's selection tools (only
// the time selection exists, and says so), the single-file edit set, the
// processes, then zoom. Snapping, ripple and the rack live in the fixed
// right-hand group as they do on the Multitrack page.
Rectangle {
    id: root
    objectName: "waveformToolbar"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool loaded: service !== null && service.loaded

    implicitHeight: Theme.hToolbar
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    component Action: IconButton {
        property string actionName: ""
        objectName: "wtool_" + actionName
        shortcut: root.shell ? root.shell.bindingFor(actionName) : ""
        enabled: root.loaded
        unavailableReason: qsTr("Open or import audio before using this editing action.")
        anchors.verticalCenter: parent.verticalCenter
        onClicked: root.shell.runAction(actionName)
    }
    component Sep: Rectangle {
        width: Theme.hairline; height: 16
        color: Theme.border
        anchors.verticalCenter: parent.verticalCenter
    }

    Flickable {
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        anchors.right: rightGroup.left
        anchors.rightMargin: Theme.s2
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        contentWidth: tools.implicitWidth
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick

        Row {
            id: tools
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1

            IconButton {
                iconName: "text-cursor-input"; text: qsTr("Time selection")
                helpText: qsTr("Drag across a lane to select a time range. It is the only selection tool on this page; frequency selection is on the Spectral page.")
                helpScope: qsTr("Editing actions act on the selection on the active track, on the channels chosen beside this tool.")
                checkable: true; checked: true
                anchors.verticalCenter: parent.verticalCenter
                onClicked: checked = true
            }
            ComboField {
                objectName: "waveformSelectionChannel"
                helpTitle: qsTr("Selection channels")
                helpText: qsTr("Choose whether edits apply to every channel of the active track or to one channel only.")
                helpScope: qsTr("Whole-track edits such as channel conversion need All channels. A take that is not stereo offers All channels only.")
                unavailableReason: qsTr("Open or import audio first.")
                anchors.verticalCenter: parent.verticalCenter
                width: 110
                model: {
                    const take = root.service ? root.service.trackById(root.service.focusedTrack) : null
                    const names = [qsTr("All channels")]
                    const n = take && take.channels ? take.channels : 0
                    for (let i = 0; i < n; ++i) names.push(n === 2 ? (i === 0 ? qsTr("Left") : qsTr("Right")) : qsTr("Channel %1").arg(i + 1))
                    return names
                }
                currentIndex: root.service ? root.service.selectionChannel + 1 : 0
                enabled: root.loaded
                onActivated: function(i) { root.service.selectionChannel = i - 1 }
            }
            Action {
                iconName: "lasso"; text: qsTr("Spectral tools"); actionName: "workspace_spectral"
                helpText: qsTr("Switch to the Spectral workspace, where you select by time and frequency and repair noise, clicks and hum.")
                helpScope: qsTr("Changes the view only. The project and its audio are unchanged.")
            }
            Sep {}
            Action {
                iconName: "scissors"; text: qsTr("Cut"); actionName: "cut"
                helpText: qsTr("Copy the selection to the clipboard and remove it from the active track.")
                helpScope: qsTr("With Ripple editing off the removed range leaves a gap; with it on, later audio moves earlier. Source audio is unchanged; Undo restores it.")
            }
            Action {
                iconName: "copy"; text: qsTr("Copy"); actionName: "copy"
                helpText: qsTr("Copy the selection on the active track to the clipboard.")
                helpScope: qsTr("Nothing changes until you paste.")
            }
            Action {
                iconName: "corner-down-right"; text: qsTr("Paste at playhead"); actionName: "paste"
                helpText: qsTr("Place the clipboard on the active track, starting at the playhead.")
                helpScope: qsTr("With Ripple editing on, later audio on the track moves along to make room. Undo removes the pasted audio.")
            }
            Action {
                iconName: "file-plus"; text: qsTr("Paste to a new track"); actionName: "paste_to_new"
                helpText: qsTr("Place the clipboard on a new track, starting at the beginning of the timeline.")
                helpScope: qsTr("Existing tracks are unchanged. Undo removes the new track.")
            }
            Action {
                iconName: "trash-2"; text: qsTr("Delete"); actionName: "delete_selection"
                helpText: qsTr("Remove the selection from the active track.")
                helpScope: qsTr("With Ripple editing off the gap stays where the audio was; with it on, later audio moves earlier to close it. Source audio is unchanged; Undo restores it.")
            }
            Action {
                iconName: "volume-x"; text: qsTr("Silence"); actionName: "silence"
                helpText: qsTr("Replace the selection with silence.")
                helpScope: qsTr("The gap stays in place whatever Ripple editing says. Source audio is unchanged; Undo restores the sound.")
            }
            Action {
                iconName: "crop"; text: qsTr("Trim to selection"); actionName: "trim"
                helpText: qsTr("Keep only the selected range on the active track and remove the audio outside it.")
                helpScope: qsTr("Source audio is unchanged; Undo restores what was removed.")
            }
            Action {
                iconName: "volume-x"; text: qsTr("Insert silence…"); actionName: "show_insert_silence"
                helpText: qsTr("Open a panel to insert a chosen length of silence at the playhead on the active track.")
                helpScope: qsTr("Later clips move along by that length; automation and markers keep their positions. Undo restores the previous audio.")
            }
            Action {
                iconName: "audio-lines"; text: qsTr("Paste and Mix…"); actionName: "show_mix_paste"
                helpText: qsTr("Open a panel to add the copied audio into the existing audio at the playhead, with a gain for each, instead of replacing it.")
                helpScope: qsTr("Respects the channel selection. A mono copy feeds each chosen channel; a multichannel copy is averaged for a mono destination. Undo restores the previous audio.")
            }
            Action {
                iconName: "shuffle"; text: qsTr("Map track channels…"); actionName: "show_channel_map"
                helpText: qsTr("Open a panel to route the active track's channels: keep, omit, swap or invert each input in every output.")
                helpScope: qsTr("Clip timing, gain and fades stay in place. Undo restores the previous mapping.")
            }
            Action {
                iconName: "split"; text: qsTr("Split at playhead"); actionName: "split"
                helpText: qsTr("Divide the clip at the playhead so you can move or edit each part.")
                helpScope: qsTr("Acts on the active track. No audio is removed; Undo joins the parts again.")
            }
            Sep {}
            Action {
                iconName: "trending-up"; text: qsTr("Fade in"); actionName: "fade_in"
                helpText: qsTr("Fade the selection in from silence, rising over the selected range.")
                helpScope: qsTr("Applied as a clip fade on the active track. Source audio is unchanged; Undo removes the fade.")
            }
            Action {
                iconName: "arrow-down-to-line"; text: qsTr("Fade out"); actionName: "fade_out"
                helpText: qsTr("Fade the selection out to silence, falling over the selected range.")
                helpScope: qsTr("Applied as a clip fade on the active track. Source audio is unchanged; Undo removes the fade.")
            }
            Action {
                iconName: "arrow-up"; text: qsTr("Gain +1 dB"); actionName: "gain_up"
                helpText: qsTr("Raise the level of the selection by 1 dB (decibel), about a small, just-noticeable step.")
                helpScope: qsTr("Applied through clip gain; source audio is unchanged. Undo reverses it.")
            }
            Action {
                iconName: "arrow-down"; text: qsTr("Gain −1 dB"); actionName: "gain_down"
                helpText: qsTr("Lower the level of the selection by 1 dB (decibel), about a small, just-noticeable step.")
                helpScope: qsTr("Applied through clip gain; source audio is unchanged. Undo reverses it.")
            }
            Action {
                iconName: "gauge"; text: qsTr("Normalize"); actionName: "normalize"
                helpText: qsTr("Raise or lower the selection so its loudest sample reaches −1 dBFS (decibels below full scale, where 0 is the loudest a file can hold).")
                helpScope: qsTr("A peak normalise through clip gain; it changes level, not loudness balance. Source audio is unchanged; Undo reverses it.")
            }
            Action {
                iconName: "flip-horizontal"; text: qsTr("Reverse"); actionName: "reverse"
                helpText: qsTr("Play the selection backwards.")
                helpScope: qsTr("Changes the selected part of the active track. Undo restores it; your original file stays unchanged.")
                enabled: root.loaded && !root.service.selectionEmpty && !root.service.sampleEditing
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.")
                                 : root.service.selectionEmpty ? qsTr("Select a time range first.")
                                 : qsTr("Wait for the current audio edit to finish.")
            }
            Sep {}
            Action {
                iconName: "bookmark"; text: qsTr("Add marker"); actionName: "add_marker"
                helpText: qsTr("Add a marker at the playhead, or a range marker over the selection when there is one.")
                helpScope: qsTr("Markers are saved with the project and do not change the audio.")
            }
            Sep {}
            Action {
                iconName: "zoom-out"; text: qsTr("Zoom out"); actionName: "zoom_out"
                helpText: qsTr("Show more of the timeline, keeping the centre of the view in place.")
                helpScope: qsTr("Changes the view only.")
            }
            Action {
                iconName: "zoom-in"; text: qsTr("Zoom in"); actionName: "zoom_in"
                helpText: qsTr("Show less of the timeline in more detail, keeping the centre of the view in place.")
                helpScope: qsTr("Changes the view only.")
            }
            Action {
                iconName: "expand"; text: qsTr("Zoom to fit"); actionName: "zoom_fit"
                helpText: qsTr("Fit the whole project into the view.")
                helpScope: qsTr("Changes the view only.")
            }
            Action {
                iconName: "focus"; text: qsTr("Zoom to selection"); actionName: "zoom_selection"
                helpText: qsTr("Fill the view with the selected time range.")
                helpScope: qsTr("Needs a selection; without one the view is unchanged.")
            }
        }
    }

    Row {
        id: rightGroup
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1

        IconButton {
            objectName: "wtool_toggle_snap"
            iconName: "magnet"
            text: qsTr("Snapping")
            helpText: root.shell && root.shell.service ? root.shell.service.snapHint : ""
            helpScope: qsTr("Choose an optional musical grid under Edit › Snap Grid. The project tempo and meter set its spacing.")
            shortcut: root.shell ? root.shell.bindingFor("toggle_snap") : ""
            checkable: true
            checked: root.shell ? root.shell.snapping : true
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("toggle_snap")
        }
        IconButton {
            objectName: "wtool_toggle_ripple"
            iconName: "chevrons-left-right"
            text: qsTr("Ripple editing")
            helpText: qsTr("Delete and cut close the gap. Paste pushes later audio along.")
            helpScope: qsTr("Later audio on the edited track changes position. Check timing after an edit.")
            shortcut: root.shell ? root.shell.bindingFor("toggle_ripple") : ""
            checkable: true
            checked: root.shell ? root.shell.ripple : false
            iconColor: checked ? Theme.warning : Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("toggle_ripple")
        }
        Sep {}
        ToolButton {
            iconName: "sparkles"; text: qsTr("Add Effect"); showLabel: root.width >= 900
            helpText: qsTr("Open the effect browser to add an installed plugin (LV2 or CLAP) to the active track.")
            helpScope: qsTr("Effects run during playback and export. Source audio is unchanged.")
            unavailableReason: qsTr("Open or import audio first.")
            shortcut: root.shell ? root.shell.bindingFor("show_plugins") : ""
            enabled: root.loaded
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("show_plugins")
        }
        IconButton {
            iconName: "panel-right"; text: qsTr("Track Effects")
            helpText: qsTr("Show or hide the right-hand dock: track effects, the inspector, repair, loudness and the video reference for the active track.")
            helpScope: qsTr("Changes the layout only.")
            shortcut: root.shell ? root.shell.bindingFor("show_rack") : ""
            checkable: true
            checked: root.shell ? root.shell.effectsDockVisible : false
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("show_rack")
        }
    }
}
