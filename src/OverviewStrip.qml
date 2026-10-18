pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The whole take in one strip under the channel lanes, with the current view
// framed on it. Press or drag to put the view there; the wheel zooms the main
// view. It keeps its own axis, always fitted, so the main axis can go anywhere
// without the overview following it.
Rectangle {
    id: root
    objectName: "overviewStrip"

    property var service: null
    property var mainAxis: null
    property int trackId: -1
    property var levels: []
    property real trackFrames: 0
    property string shaderUrl: ""
    property color tint: Theme.audioPrimary
    property real originX: 0

    implicitHeight: Theme.hOverview
    color: Theme.panelBg

    Rectangle {
        anchors.top: parent.top
        width: parent.width; height: Theme.hairline
        color: Theme.border
    }

    TimeAxis {
        id: fitAxis
        totalFrames: root.service ? root.service.takeFrames : 0
        viewWidth: Math.max(0, field.width)
    }
    // The fit is the whole point; anything that unfits it is undone here.
    Connections {
        target: fitAxis
        function onAutoFitChanged() { if (!fitAxis.autoFit) fitAxis.zoomToFit() }
    }

    Text {
        x: Theme.s1
        anchors.verticalCenter: parent.verticalCenter
        width: root.originX - Theme.s1 * 2
        text: qsTr("View")
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }

    Item {
        id: field
        x: root.originX
        y: Theme.hairline
        width: root.width - root.originX
        height: root.height - Theme.hairline
        clip: true

        Waveform {
            anchors.fill: parent
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            service: root.service
            axis: fitAxis
            shaderUrl: root.shaderUrl
            trackId: -1                 // never holds the selection; it only shows it
            levels: root.levels
            trackFrames: root.trackFrames
            waveColor: Theme.withAlpha(root.tint, 0.55)
            opacity: 0.9
            // Its own mouse is off: the strip's handler below owns every press.
            enabled: false
        }

        // The selection, dimly, so a range reads on the overview too.
        Rectangle {
            visible: root.service !== null && !root.service.selectionEmpty
                     && root.service.selectionTrack === root.trackId
            x: fitAxis.frameToX(root.service ? root.service.selectionStart : 0)
            width: Math.max(1, fitAxis.frameToX(root.service ? root.service.selectionEnd : 0) - x)
            y: 0; height: parent.height
            color: Theme.selectionFill
        }

        // The window onto the take.
        Rectangle {
            id: window
            objectName: "overviewWindow"
            x: root.mainAxis ? fitAxis.frameToX(root.mainAxis.viewStart) : 0
            width: root.mainAxis ? Math.max(4, fitAxis.frameToX(root.mainAxis.viewEnd) - x) : 0
            y: 0; height: parent.height
            color: Theme.withAlpha(Theme.textPrimary, 0.06)
            border.width: Theme.hairline
            border.color: Theme.withAlpha(Theme.accent, 0.8)
        }

        Rectangle {
            x: fitAxis.frameToX(root.service ? root.service.playhead : 0)
            width: Theme.playheadWidth; y: 0; height: parent.height
            color: Theme.playhead
            visible: root.service !== null && root.service.trackCount > 0
        }

        MouseArea {
            id: overviewArea
            objectName: "overviewMouse"
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            ControlHelp {
                title: qsTr("Overview")
                purpose: qsTr("The whole project in one strip, with the part on screen framed. Drag the frame to move the view. Click outside it to jump there; the wheel zooms.")
                scope: qsTr("The view only; nothing in the project changes.")
                suppressed: overviewArea.pressed
            }
            function place(x) {
                if (!root.mainAxis)
                    return
                // Centre the view on the pointer, clamped by the axis.
                const frame = fitAxis.xToFrame(x)
                root.mainAxis.autoFit = false
                root.mainAxis.viewStart = frame - root.mainAxis.viewFrames / 2
                root.mainAxis.clampView()
            }
            preventStealing: true
            property real grabOffset: 0
            onPressed: function (e) {
                // Keep the point grabbed inside the visible window under the
                // pointer. Clicking elsewhere still centres the view there.
                grabOffset = e.x >= window.x && e.x <= window.x + window.width
                    ? e.x - (window.x + window.width / 2) : 0
                place(e.x - grabOffset)
            }
            onPositionChanged: function (e) { if (pressed) place(e.x - grabOffset) }
        }
        WheelHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                if (!root.mainAxis)
                    return
                root.mainAxis.applyWheel("zoom", root.mainAxis.wheelDelta(event), root.mainAxis.viewWidth / 2)
                event.accepted = true
            }
        }
    }
}
