pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The strip between the ruler and the lanes: one flag per session marker on the
// shared axis, a span for a range. Click a flag to go there, double-click to
// rename it in place, drag it to move it, right-click for the rest.
//
// Markers are drawn here and not on the lanes because they belong to the
// session -- a chapter point does not move when a clip does.
Rectangle {
    id: root
    objectName: "markerLane"

    property var axis: null
    property var service: null
    property var shell: null
    // Pixel offset of the axis origin inside this item (the track header column).
    property real originX: 0

    signal seekRequested(real frame)   // a frame count: past 2^31 on a long take, so not an int

    implicitHeight: Theme.hMarkerLane
    color: Theme.panelBg
    clip: true

    readonly property var markers: service ? service.markers : []

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    // The header column carries the lane's name, like a track header would.
    Item {
        width: root.originX
        height: parent.height
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Theme.hairline
            color: Theme.border
        }
        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2 + 3
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1
            Icon { name: "bookmark"; size: 12; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
            Text {
                // The Waveform page's column is only an amplitude ruler wide: the
                // icon alone says what the lane is there.
                visible: root.originX >= 100
                text: root.markers.length === 0 ? qsTr("Markers")
                    : root.markers.length === 1 ? qsTr("1 marker")
                    : qsTr("%1 markers").arg(root.markers.length)
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    Item {
        id: field
        x: root.originX
        width: root.width - root.originX
        height: root.height
        clip: true

        Rectangle {
            objectName: "loopRange"
            visible: root.service !== null && root.service.loopEnabled
            x: root.axis && root.service ? root.axis.frameToX(root.service.loopStart) : 0
            width: root.axis && root.service ? Math.max(1, (root.service.loopEnd - root.service.loopStart) / root.axis.framesPerPixel) : 0
            height: parent.height
            color: Theme.withAlpha(Theme.accent, 0.22)
            border.color: Theme.accent; border.width: 1
            Text {
                anchors.centerIn: parent
                visible: parent.width > 40
                text: qsTr("Loop")
                color: Theme.accent; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            }
        }

        // A hint where the flags will go, until there is one.
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            visible: root.markers.length === 0 && root.service !== null && root.service.loaded && !root.service.loopEnabled
            text: root.shell ? qsTr("%1 adds a marker at the playhead").arg(root.shell.bindingFor("add_marker")) : ""
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            opacity: 0.7
        }

        Repeater {
            model: root.markers

            delegate: Item {
                id: flag
                objectName: "markerFlag"
                required property var modelData
                required property int index

                readonly property int markerId: modelData.id
                readonly property bool isRange: modelData.type === "range" && modelData.length > 0
                readonly property color tint: modelData.color !== "" ? modelData.color
                                                                     : Theme.markerColor(modelData.type)
                // While dragging the flag follows the pointer; the model catches up
                // on release.
                property real dragOffset: 0
                readonly property real anchorX: root.axis ? root.axis.frameToX(modelData.position) : 0

                x: anchorX + dragOffset
                y: 0
                height: field.height
                width: isRange && root.axis
                       ? Math.max(6, root.axis.frameToX(modelData.end) - anchorX)
                       : labelBox.width + 8
                visible: x + width >= 0 && x <= field.width

                // A range is a translucent span with its name at the left edge.
                Rectangle {
                    visible: flag.isRange
                    anchors.fill: parent
                    anchors.topMargin: 3
                    anchors.bottomMargin: Theme.hairline + 3
                    color: Theme.withAlpha(flag.tint, 0.22)
                    border.width: Theme.hairline
                    border.color: Theme.withAlpha(flag.tint, 0.7)
                    radius: Theme.rControl
                }

                // The flag pole.
                Rectangle {
                    x: 0
                    width: 2
                    height: parent.height
                    color: flag.tint
                }

                Rectangle {
                    id: labelBox
                    x: 2
                    anchors.verticalCenter: parent.verticalCenter
                    height: Theme.hMarkerLane - 6
                    width: labelRow.implicitWidth + Theme.s1 * 2
                    radius: Theme.rControl
                    color: flagHover.hovered || nameField.visible ? Theme.hovered(flag.tint) : flag.tint
                    visible: !nameField.visible

                    Row {
                        id: labelRow
                        anchors.centerIn: parent
                        spacing: 3
                        Icon {
                            name: Theme.markerIcon(flag.modelData.type)
                            size: 10
                            color: Theme.accentText
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            objectName: "markerName"
                            text: flag.modelData.name
                            // A long name ends in "…" inside its range, or after
                            // 220 px for a point; the help card shows it whole.
                            width: Math.min(implicitWidth, flag.isRange ? Math.max(24, flag.width - 30) : 220)
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsRuler + 1
                            font.weight: Theme.wHeading
                            color: Theme.accentText
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }

                // Rename in place, like a track.
                Rectangle {
                    anchors.fill: nameField
                    anchors.margins: -2
                    visible: nameField.visible
                    color: Theme.controlBg
                    radius: Theme.rControl
                    border.width: Theme.hairline
                    border.color: Theme.accent
                }
                TextInput {
                    id: nameField
                    objectName: "markerNameField"
                    x: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(80, contentWidth + Theme.s2)
                    visible: false
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    selectByMouse: true
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentText
                    Keys.onEscapePressed: {
                        nameField.visible = false
                        if (root.shell) root.shell.restoreFocus()
                    }
                    onAccepted: {
                        if (root.service)
                            root.service.renameMarker(flag.markerId, nameField.text)
                        nameField.visible = false
                        if (root.shell) root.shell.restoreFocus()
                    }
                    onActiveFocusChanged: if (!activeFocus) nameField.visible = false
                }

                HoverHandler { id: flagHover }

                MouseArea {
                    id: flagMouse
                    objectName: "markerMouse"
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.PointingHandCursor
                    enabled: !nameField.visible
                    preventStealing: true
                    ControlHelp {
                        title: flag.modelData.name || qsTr("Marker")
                        purpose: qsTr("Click to move the playhead here. Drag to move the marker. Double-click to rename it. Right-click for its menu.")
                        suppressed: flagMouse.pressed
                    }

                    property real pressX: 0
                    property bool moved: false

                    // Measured in the lane's coordinates, not the flag's: the flag
                    // moves under the pointer during the drag, so its own x is a
                    // moving target and a drag measured against it travels at half
                    // speed. Found by the shell suite.
                    onPressed: function (e) {
                        if (e.button === Qt.RightButton) {
                            if (root.shell)
                                root.shell.markerContextMenu.popupFor(flag.markerId)
                            return
                        }
                        pressX = flagMouse.mapToItem(field, e.x, e.y).x
                        moved = false
                    }
                    onPositionChanged: function (e) {
                        if (!pressed || !(e.buttons & Qt.LeftButton))
                            return
                        const dx = flagMouse.mapToItem(field, e.x, e.y).x - pressX
                        if (!moved && Math.abs(dx) < Theme.dragThreshold)
                            return
                        moved = true
                        flag.dragOffset = dx
                    }
                    onCanceled: { flag.dragOffset = 0; moved = false }
                    onReleased: function (e) {
                        if (e.button !== Qt.LeftButton)
                            return
                        if (moved) {
                            const frame = root.axis
                                ? Math.max(0, Math.round(root.axis.xToFrame(flag.anchorX + flag.dragOffset)))
                                : flag.modelData.position
                            flag.dragOffset = 0
                            if (root.service)
                                root.service.moveMarker(flag.markerId, frame)
                        } else {
                            root.seekRequested(flag.modelData.position)
                        }
                    }
                    onDoubleClicked: function (e) {
                        if (e.button !== Qt.LeftButton)
                            return
                        nameField.text = flag.modelData.name
                        nameField.visible = true
                        nameField.forceActiveFocus()
                        nameField.selectAll()
                    }
                }

                Tooltip {
                    text: Theme.markerLabel(flag.modelData.type)
                          + (flag.modelData.notes !== "" ? " — " + flag.modelData.notes : "")
                    visible: flagHover.hovered && !flagMouse.pressed && !nameField.visible
                }
            }
        }

        // Double-clicking empty lane space makes a marker there.
        MouseArea {
            objectName: "markerLaneField"
            anchors.fill: parent
            z: -1
            acceptedButtons: Qt.LeftButton
            ControlHelp {
                title: qsTr("Marker lane")
                purpose: qsTr("Double-click to add a marker at that time. Click to move the playhead.")
            }
            onDoubleClicked: function (e) {
                if (root.service && root.axis)
                    root.service.addMarker(Math.max(0, Math.round(root.axis.xToFrame(e.x))))
            }
            onClicked: function (e) {
                if (root.axis)
                    root.seekRequested(Math.max(0, Math.round(root.axis.xToFrame(e.x))))
            }
        }
    }
}
