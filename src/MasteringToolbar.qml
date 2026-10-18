pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The strip over the Mastering page's mix: analyse, the live meter's reset,
// markers, zoom; and in the fixed right group what state the analysis is in,
// the bus browser, and the rack toggle.
Rectangle {
    id: root
    objectName: "masteringToolbar"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool loaded: service !== null && service.trackCount > 0

    implicitHeight: Theme.hToolbar
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    component Action: IconButton {
        property string actionName: ""
        objectName: "mtool_" + actionName
        shortcut: root.shell ? root.shell.bindingFor(actionName) : ""
        enabled: root.loaded
        unavailableReason: qsTr("Open or import audio first.")
        anchors.verticalCenter: parent.verticalCenter
        onClicked: root.shell.runAction(actionName)
    }
    component Sep: Rectangle {
        width: Theme.hairline; height: 16
        color: Theme.border
        anchors.verticalCenter: parent.verticalCenter
    }

    // The strip's margin is inside it, so a labelled button's hover box, which
    // reaches a step to the left of its label, is not cut off by the clip.
    Flickable {
        anchors.left: parent.left
        anchors.right: rightGroup.left
        anchors.rightMargin: Theme.s2
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        contentWidth: tools.implicitWidth + Theme.s2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick

        Row {
            id: tools
            x: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1

            ToolButton {
                objectName: "mtool_analyse"
                iconName: "refresh-cw"
                text: root.service && root.service.analysing ? qsTr("Analysing…")
                    : root.service && root.service.analysisStale && root.service.analysed ? qsTr("Re-analyse")
                    : qsTr("Analyse mix")
                showLabel: true
                helpTitle: qsTr("Analyse mix")
                helpText: qsTr("Measure the finished mix: its average loudness, changes in loudness and highest peaks.")
                helpScope: qsTr("Nothing is exported. Analyse again after the mix changes to refresh the figures and the level-matched Before preview.")
                helpTopic: qsTr("Integrated loudness in LUFS is the average perceived loudness of the whole programme. Loudness range in LU shows how much it varies. True peak in dBTP estimates the highest level after conversion to analogue, which can exceed the sample peaks.")
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.") : qsTr("The analysis is already running.")
                shortcut: root.shell ? root.shell.bindingFor("analyse_master") : ""
                enabled: root.loaded && root.service !== null && !root.service.analysing
                checkable: false
                anchors.verticalCenter: parent.verticalCenter
                onClicked: root.shell.runAction("analyse_master")
            }
            Action {
                iconName: "rotate-ccw"; text: qsTr("Reset live meter"); actionName: "reset_loudness"
                helpText: qsTr("Start the live loudness measurement again from zero.")
                helpScope: qsTr("Affects the live meter only; the analysis report and the audio are unchanged.")
            }
            Sep {}
            Action {
                iconName: "bookmark"; text: qsTr("Add marker"); actionName: "add_marker"
                helpText: qsTr("Add a marker at the playhead, or a range marker over the selection when there is one.")
                helpScope: qsTr("Markers are saved with the project and do not change the audio.")
            }
            Action {
                iconName: "chevron-left"; text: qsTr("Previous marker"); actionName: "prev_marker"
                helpText: qsTr("Move the playhead to the previous marker.")
                helpScope: qsTr("Changes the playhead only.")
            }
            Action {
                iconName: "chevron-right"; text: qsTr("Next marker"); actionName: "next_marker"
                helpText: qsTr("Move the playhead to the next marker.")
                helpScope: qsTr("Changes the playhead only.")
            }
            Sep {}
            Action {
                iconName: "zoom-out"; text: qsTr("Zoom out"); actionName: "zoom_out"
                helpText: qsTr("Show more of the mix, keeping the centre of the view in place.")
                helpScope: qsTr("Changes the view only.")
            }
            Action {
                iconName: "zoom-in"; text: qsTr("Zoom in"); actionName: "zoom_in"
                helpText: qsTr("Show less of the mix in more detail, keeping the centre of the view in place.")
                helpScope: qsTr("Changes the view only.")
            }
            Action {
                iconName: "expand"; text: qsTr("Zoom to fit"); actionName: "zoom_fit"
                helpText: qsTr("Fit the whole mix into the view.")
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
        spacing: Theme.s2

        // Where the analysis stands, in words, always reachable.
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1
            readonly property alias controlHelp: analysisHelp
            // A positioner must not position the adapter: zero size, hover on the row.
            ControlHelp {
                id: analysisHelp
                anchors.fill: undefined
                width: 0; height: 0
                title: qsTr("Analysis state")
                purpose: qsTr("Whether the loudness figures describe the current mix. Mix changed means the mix was edited after the last analysis, so analyse again before trusting them.")
                scope: qsTr("Green is up to date, amber is running or stale, grey means no analysis yet.")
            }
            HoverHandler { onHoveredChanged: hovered ? Help.request(analysisHelp) : Help.leave(analysisHelp) }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 8; height: 8; radius: 4
                color: !root.service || !root.loaded ? Theme.textMuted
                     : root.service.analysing ? Theme.warning
                     : !root.service.analysed ? Theme.textMuted
                     : root.service.analysisStale ? Theme.warning
                     : Theme.success
            }
            Text {
                objectName: "analysisState"
                anchors.verticalCenter: parent.verticalCenter
                text: !root.service || !root.loaded ? qsTr("No project")
                    : root.service.analysing ? qsTr("Analysing")
                    : !root.service.analysed ? qsTr("Not analysed")
                    : root.service.analysisStale ? qsTr("Mix changed")
                    : qsTr("Analysed")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
            }
        }
        Sep {}
        IconButton {
            objectName: "mtool_preview_before"
            iconName: "headphones"
            text: qsTr("Hear before the bus")
            helpText: root.service && root.service.previewMatched
                  ? qsTr("Play the mix before the master bus effects, level-matched by %1 dB so you compare tone rather than loudness.").arg(root.service.previewMatchDb.toFixed(1))
                  : qsTr("Play the mix before the master bus effects. Until the mix is analysed this is a plain bypass, not level-matched, so it will also sound quieter or louder.")
            helpScope: qsTr("Playback only. Exports always include the master bus.")
            unavailableReason: qsTr("Open or import audio first.")
            shortcut: root.shell ? root.shell.bindingFor("preview_before") : ""
            checkable: true
            checked: root.service ? root.service.previewBefore : false
            iconColor: checked ? Theme.warning : Theme.textSecondary
            enabled: root.loaded
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("preview_before")
        }
        Sep {}
        ToolButton {
            iconName: "sparkles"; text: qsTr("Add to Bus"); showLabel: root.width >= 900
            helpText: qsTr("Open the effect browser to add an installed plugin (LV2 or CLAP) to the master bus, the chain the whole mix passes through.")
            helpScope: qsTr("Bus effects run during playback and export. Source audio is unchanged.")
            unavailableReason: qsTr("Open or import audio first.")
            shortcut: root.shell ? root.shell.bindingFor("show_master_plugins") : ""
            enabled: root.loaded
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("show_master_plugins")
        }
        IconButton {
            iconName: "panel-right"; text: qsTr("Master Chain")
            helpText: qsTr("Show or hide the right-hand dock: the master chain, the bus settings and the loudness meters.")
            helpScope: qsTr("Changes the layout only.")
            shortcut: root.shell ? root.shell.bindingFor("show_rack") : ""
            checkable: true
            checked: root.shell ? root.shell.effectsDockVisible : false
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("show_rack")
        }
    }
}
