pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The strip over the Spectral page's lanes: selection tools (time, rectangle,
// band; lasso, brush, spot, harmonics), the band or mask readout with its eraser, the
// four repairs; zoom, snapping and the repair panel toggle in the fixed
// right-hand group, reachable at any width. What the concept also shows here
// lives on the dock instead, because the strip has to fit at 1280 px (trap
// 47): the Lin/Log switch and Rebuild on the Display tab (Ctrl+Alt+F,
// Ctrl+Alt+R), Add/Cut/Keep (X, Ctrl+Alt+X; the readout says which) and undo-last-shape
// (Ctrl+Backspace) on the Repair tab.
Rectangle {
    id: root
    objectName: "spectralToolbar"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property bool loaded: service !== null && service.loaded
    readonly property string tool: shell ? shell.spectralTool : "rect"

    implicitHeight: Theme.hToolbar
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    component Action: IconButton {
        property string actionName: ""
        objectName: "stool_" + actionName
        shortcut: root.shell ? root.shell.bindingFor(actionName) : ""
        enabled: root.loaded
        unavailableReason: qsTr("Open or import audio first.")
        anchors.verticalCenter: parent.verticalCenter
        onClicked: root.shell.runAction(actionName)
    }
    component Tool: IconButton {
        property string actionName: ""
        property string toolName: ""
        objectName: "stool_" + actionName
        shortcut: root.shell ? root.shell.bindingFor(actionName) : ""
        // One of a set: the current tool is not switched off by clicking it again.
        checkable: !checked
        checked: root.tool === toolName
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
            objectName: "spectralToolRow"
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1

            Tool {
                iconName: "text-cursor-input"; text: qsTr("Time selection"); actionName: "spectral_tool_time"; toolName: "time"
                helpText: qsTr("Drag across the display to select a time range over the whole spectrum, every frequency included.")
                helpScope: qsTr("Repairs act inside the selection on the active track.")
            }
            Tool {
                iconName: "square-dashed"; text: qsTr("Rectangular selection"); actionName: "spectral_tool_rect"; toolName: "rect"
                helpText: qsTr("Drag a box to select a time range and a frequency band together.")
                helpScope: qsTr("Repairs act inside the box on the active track. The readout beside the tools shows the band.")
            }
            Tool {
                iconName: "stretch-horizontal"; text: qsTr("Frequency band"); actionName: "spectral_tool_band"; toolName: "band"
                helpText: qsTr("Drag up or down to choose a band of frequencies. It applies over the time selection, or the whole take when nothing is selected.")
                helpScope: qsTr("Repairs act inside that band. Clear it to work on the full spectrum again.")
            }
            Tool {
                iconName: "lasso"; text: qsTr("Lasso"); actionName: "spectral_tool_lasso"; toolName: "lasso"
                helpText: qsTr("Draw round the sound you want to repair. The drawn shape becomes a mask, which replaces the band while it holds anything.")
                helpScope: qsTr("Repairs act inside the mask on the active track. The mask is saved with the project and does not change the audio by itself.")
                helpTopic: qsTr("A lasso needs at least three points. On the Repair tab, the Add, Cut and Keep modes set whether the next shape adds to the mask, cuts a hole in it, or keeps only where they overlap. Feather softens the edge of the mask so a repair blends in.")
            }
            Tool {
                iconName: "paintbrush"; text: qsTr("Brush"); actionName: "spectral_tool_brush"; toolName: "brush"
                helpText: qsTr("Paint over the sound you want to repair. Each stroke adds to the mask.")
                helpScope: qsTr("Repairs act inside the mask on the active track. Holding Alt while painting cuts from the mask instead.")
                helpTopic: qsTr("The Repair tab sets the brush size and the feather. A stroke narrower than one analysis frame covers nothing, so widen the brush for fine detail rather than painting very quickly.")
            }
            Tool {
                iconName: "bandage"; text: qsTr("Repair brush"); actionName: "spectral_tool_spot"; toolName: "spot"
                helpText: qsTr("Click or paint over a small blemish and it is healed straight away from the sound around it.")
                helpScope: qsTr("The mask and selection stay as they are. Each stroke is one Undo step; the source file is unchanged.")
            }
            Tool {
                iconName: "rows-3"; text: qsTr("Harmonics"); actionName: "spectral_tool_harmonic"; toolName: "harmonic"
                helpText: qsTr("Click a partial to select its whole harmonic series over the selection, or drag along it to cover a span of time.")
                helpScope: qsTr("The Repair tab sets how many partials and how wide each band is. The shapes join the mask like a lasso.")
                helpTopic: qsTr("A harmonic series is a fundamental frequency and its multiples: the overtones of a voice, a note or mains hum. The pick snaps to the nearest steady peak; if none is found, choose a clearer partial or a larger FFT size on the Display tab.")
            }
            Sep {}
            Text {
                objectName: "bandReadoutText"
                readonly property alias controlHelp: readoutHelp
                ControlHelp {
                    id: readoutHelp
                    title: qsTr("Selection band")
                    purpose: qsTr("Shows what the next repair acts on: the frequency band, the mask with its shape count, or the full spectrum.")
                    scope: qsTr("Cut or keep after the mask shows the mode the next shape uses.")
                }
                anchors.verticalCenter: parent.verticalCenter
                // Short on purpose: the toolbar has to fit at 1280 px (trap 47).
                text: {
                    if (!root.service)
                        return qsTr("Full spectrum")
                    const cutting = root.shell && root.shell.spectralMaskSubtract ? qsTr(" · cut")
                                  : root.shell && root.shell.spectralMaskIntersect ? qsTr(" · keep") : ""
                    if (!root.service.maskEmpty)
                        return qsTr("Mask %1").arg(root.service.maskShapeCount) + cutting
                    if (cutting !== "")
                        return qsTr("Mask") + cutting
                    if (root.service.bandEmpty)
                        return qsTr("Full spectrum")
                    return Theme.formatHz(root.service.bandLowHz) + " – " + Theme.formatHz(root.service.bandHighHz)
                }
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
                font.features: { "tnum": 1 }
                color: !root.service || (root.service.bandEmpty && root.service.maskEmpty) ? Theme.textMuted : Theme.textPrimary
            }
            Action {
                iconName: "eraser"; text: qsTr("Clear mask"); actionName: "spectral_clear_mask"
                helpText: qsTr("Remove every mask shape, so the frequency band, or the full spectrum, applies again.")
                helpScope: qsTr("The audio is unchanged; only what the next repair acts on changes.")
                visible: root.service && root.service.maskShapeCount > 0
                enabled: root.loaded && root.service && root.service.maskShapeCount > 0
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.") : qsTr("There is no mask to clear.")
            }
            Action {
                iconName: "eraser"; text: qsTr("Clear band"); actionName: "spectral_clear_band"
                helpText: qsTr("Remove the frequency band, so repairs act on the full spectrum.")
                helpScope: qsTr("The audio is unchanged; only what the next repair acts on changes.")
                visible: !root.service || root.service.maskShapeCount === 0
                enabled: root.loaded && root.service && !root.service.bandEmpty
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.") : qsTr("There is no frequency band to clear.")
            }
            Sep {}
            Action {
                iconName: "volume-1"; text: qsTr("Change level"); actionName: "spectral_attenuate"
                helpText: qsTr("Turn the selected sound down by the amount set on the Repair tab, inside the selection's time and its band or mask.")
                helpScope: qsTr("Changes the selected part of the active track. Undo restores it; your original file stays unchanged.")
                enabled: root.loaded && root.service && !root.service.selectionEmpty && !root.service.repairing
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.")
                                 : root.service.selectionEmpty ? qsTr("Select a time range first.")
                                 : qsTr("Wait for the current repair to finish.")
            }
            Action {
                iconName: "wand"; text: qsTr("Repair"); actionName: "spectral_heal"
                helpText: qsTr("Replace the selected sound with what surrounds it in time and frequency, so a click, bump or gap disappears.")
                helpScope: qsTr("Changes the selected part of the active track. Undo restores it; your original file stays unchanged.")
                enabled: root.loaded && root.service && !root.service.selectionEmpty && !root.service.repairing
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.")
                                 : root.service.selectionEmpty ? qsTr("Select a time range first.")
                                 : qsTr("Wait for the current repair to finish.")
            }
            Action {
                iconName: "pipette"; text: qsTr("Learn noise"); actionName: "spectral_capture_print"
                helpText: qsTr("Measure the selection as a noise sample: the average spectrum of the noise on its own, used by Reduce noise.")
                helpScope: qsTr("Select a part that holds nothing but the noise. The print is a measurement of this take, not a setting, and does not change the audio.")
                enabled: root.loaded && root.service && !root.service.selectionEmpty
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.") : qsTr("Select a time range that holds only the noise.")
            }
            Action {
                iconName: "sparkles"; text: qsTr("Reduce noise"); actionName: "spectral_denoise"
                helpText: qsTr("Subtract the noise sample from the selection, up to the amount and with the strength set on the Repair tab.")
                helpScope: qsTr("Acts inside the selection's band or mask on the active track, in one Undo step. The source file is unchanged.")
                enabled: root.loaded && root.service && !root.service.selectionEmpty && root.service.noisePrintReady && !root.service.repairing
                unavailableReason: !root.loaded ? qsTr("Open or import audio first.")
                                 : root.service.selectionEmpty ? qsTr("Select a time range first.")
                                 : !root.service.noisePrintReady ? qsTr("Learn the noise first: select some noise on its own.")
                                 : qsTr("Wait for the current repair to finish.")
            }
        }
    }

    // Fixed, so the view controls are reachable at any width (trap 47): the six
    // selection tools left no room for them in the strip at 1280 px.
    Row {
        id: rightGroup
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1

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
            helpText: qsTr("Fit the whole take into the view.")
            helpScope: qsTr("Changes the view only.")
        }
        Action {
            iconName: "focus"; text: qsTr("Zoom to selection"); actionName: "zoom_selection"
            helpText: qsTr("Fill the view with the selected time range.")
            helpScope: qsTr("Needs a selection; without one the view is unchanged.")
        }
        Sep {}
        IconButton {
            objectName: "stool_toggle_snap"
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
        Sep {}
        IconButton {
            objectName: "stool_show_rack"
            iconName: "panel-right"; text: qsTr("Repair panel")
            helpText: qsTr("Show or hide the Repair panel: repair amounts, the mask and harmonics settings, the display settings and the clicks list.")
            helpScope: qsTr("Changes the layout only.")
            shortcut: root.shell ? root.shell.bindingFor("show_rack") : ""
            checkable: true
            checked: root.shell ? root.shell.effectsDockVisible : false
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.shell.runAction("show_rack")
        }
    }
}
