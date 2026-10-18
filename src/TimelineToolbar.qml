pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// Timeline tools and editing actions; snap and ripple stay in the fixed group.
Rectangle {
    id: root
    objectName: "timelineToolbar"

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
        objectName: "tool_" + actionName
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

    // The tools scroll under the fixed right-hand group rather than run into it:
    // at the 1280 px minimum the timeline is 650 px wide and the full strip is
    // not, and a toolbar that overlaps itself is worse than one that clips.
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

        // Tool modes (brief 6, "Timeline toolbar").
        Action {
            iconName: "mouse-pointer-2"; text: qsTr("Selection tool"); actionName: "timeline_select"
            helpText: qsTr("Drag across a lane to select a time range on that track.")
            helpScope: qsTr("Editing actions act on the selection on the active track. Choose Move or Slip to change clips instead.")
            checkable: !checked; checked: root.shell ? !root.shell.timelineMoveTool && !root.shell.timelineSlipTool : true
        }
        Action {
            iconName: "move"; text: qsTr("Move clip"); actionName: "timeline_move"
            helpText: qsTr("Drag a clip to an empty position or another track.")
            helpScope: qsTr("Changes where the clip plays. Its source audio is unchanged.")
            checkable: !checked; checked: root.shell ? root.shell.timelineMoveTool : false
        }
        Action {
            iconName: "move-horizontal"; text: qsTr("Slip clip"); actionName: "timeline_slip"
            helpText: qsTr("Change which part of the source plays inside this clip.")
            helpScope: qsTr("The clip stays in the same position and keeps its length.")
            helpTopic: qsTr("Choose Slip, then drag inside a clip. You can only slip into audio that exists in its source. Undo restores the previous source position.")
            checkable: !checked; checked: root.shell ? root.shell.timelineSlipTool : false
        }
        Action {
            iconName: "stretch-horizontal"; text: qsTr("Stretch clip duration"); actionName: "show_stretch"
            helpText: qsTr("Open Pitch and Duration to change the active clip's length or pitch.")
            helpScope: qsTr("Acts on the active clip. Its source audio is unchanged; Undo restores the original duration.")
        }
        Action {
            iconName: "brush"; text: qsTr("Spectral tools"); actionName: "workspace_spectral"
            helpText: qsTr("Switch to the Spectral workspace, where you select by time and frequency and repair noise, clicks and hum.")
            helpScope: qsTr("Changes the view only. The project and its audio are unchanged.")
        }

        Sep {}

        // Edit commands.
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
            iconName: "trash-2"; text: qsTr("Delete"); actionName: "delete_selection"
            helpText: qsTr("Remove the selection from the active track.")
            helpScope: qsTr("With Ripple editing off the gap stays where the audio was; with it on, later audio moves earlier to close it. Source audio is unchanged; Undo restores it.")
        }
        Sep {}
        Action {
            iconName: "split"; text: qsTr("Split at playhead"); actionName: "split"
            helpText: qsTr("Divide the clip at the playhead so you can move or edit each part.")
            helpScope: qsTr("Acts on the selected track. The original source file stays unchanged.")
            helpTopic: qsTr("Select the track and place the playhead inside a clip, then split. No audio is removed. Undo joins the parts again.")
        }
        Action {
            iconName: "bookmark"; text: qsTr("Add marker"); actionName: "add_marker"
            helpText: qsTr("Add a marker at the playhead, or a range marker over the selection when there is one.")
            helpScope: qsTr("Markers are saved with the project and do not change the audio.")
        }
        Action {
            iconName: "crop"; text: qsTr("Trim to selection"); actionName: "trim"
            helpText: qsTr("Keep only the selected range on the active track and remove the audio outside it.")
            helpScope: qsTr("Source audio is unchanged; Undo restores what was removed.")
        }
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
            iconName: "activity"; text: qsTr("Edit automation"); actionName: "show_automation"
            helpText: qsTr("Open the Automation panel to draw or record changes to volume, pan or effect settings over time.")
            helpScope: qsTr("Automation is saved with the project and plays back in the mix and in exports.")
        }
        Action {
            iconName: "blend"; text: qsTr("Crossfade"); actionName: "crossfade_linear"
            helpText: qsTr("Blend two adjacent clips across the selected join so the change between them is smooth.")
            helpScope: qsTr("Select a range across one join between adjacent clips. Both clips need unused source audio for the blend. One Undo step.")
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
        // Labels only when the strip is wide enough to afford them.
        readonly property bool labels: root.width >= 900

        IconButton {
            objectName: "tool_toggle_snap"
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
            objectName: "tool_toggle_ripple"
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
            iconName: "plus"; text: qsTr("Add Track"); showLabel: rightGroup.labels
            helpText: qsTr("Add an empty track below the others.")
            helpScope: qsTr("A project holds as many tracks as the engine can export; adding one past that limit is refused with a message.")
            shortcut: root.shell ? root.shell.bindingFor("add_track") : ""
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("add_track")
        }
        ToolButton {
            iconName: "sparkles"; text: qsTr("Add Effect"); showLabel: rightGroup.labels
            helpText: qsTr("Open the effect browser to add an installed plugin (LV2 or CLAP) to the active track.")
            helpScope: qsTr("Effects run during playback and export. Source audio is unchanged.")
            unavailableReason: qsTr("Open or import audio first.")
            shortcut: root.shell ? root.shell.bindingFor("show_plugins") : ""
            enabled: root.loaded
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("show_plugins")
        }
        IconButton {
            objectName: "tool_show_mixer"
            iconName: "sliders-vertical"; text: qsTr("Mixer")
            helpText: qsTr("Show or hide the mixer, with a channel strip for every track.")
            helpScope: qsTr("Changes the layout only.")
            shortcut: root.shell ? root.shell.bindingFor("show_mixer") : ""
            checkable: true
            checked: root.shell ? root.shell.mixerVisible : false
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("show_mixer")
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
