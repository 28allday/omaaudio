pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import QtQuick.Layouts
import QtQuick.Window
import OmaAudio.Video
import "ui"

// The Video tab of the right dock: the reference picture at the playhead, its
// timecode, what the file is (rate as a ratio, size, length, timecode tag),
// where it sits on the timeline, and the state of the decoder in words. The
// picture follows the audio clock; nothing here can slow the sound down.
Item {
    id: root
    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var info: service ? service.videoInfo : ({present: false, status: "none"})
    readonly property bool present: !!info.present
    readonly property bool ready: info.status === "ready"
    readonly property var stats: service ? service.videoStats : ({})
    property bool detached: false

    function statusLine() {
        if (!root.present) return ""
        if (root.info.status === "probing") return root.info.message
        if (root.info.status === "missing" || root.info.status === "unsupported") return root.info.message
        var line = qsTr("%1 × %2, %3 fps (%4)%5").arg(root.info.width).arg(root.info.height)
                       .arg(root.info.fps).arg(root.info.rateRatio)
                       .arg(root.info.variableRate ? qsTr(", variable") : "")
        line += "  ·  " + qsTr("%1 frames, %2").arg(root.info.frameCount).arg(root.info.durationText)
        if (root.info.timecode) line += "  ·  " + qsTr("timecode %1").arg(root.info.timecode)
        return line
    }
    function decoderLine() {
        if (!root.ready || !root.stats) return ""
        if (root.stats.error) return root.stats.error
        var parts = []
        if (root.stats.late > 0) parts.push(qsTr("%1 moments the picture lagged the sound").arg(root.stats.late))
        if (root.stats.dropped > 0) parts.push(qsTr("%1 decoded frames passed over at seeks and jumps").arg(root.stats.dropped))
        if (root.stats.restarts > 1) parts.push(qsTr("%1 restarts").arg(root.stats.restarts - 1))
        return parts.length ? parts.join("  ·  ") : qsTr("Video playback is keeping up")
    }

    component Line: Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
    }

    // Anchored by width, not filled: the rows below wrap to the dock's width
    // instead of pushing the column wider than the dock.
    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.s2
        spacing: Theme.s2

        // ── the picture ──────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.maximumWidth: root.width - 2 * Theme.s2
            Layout.preferredHeight: root.ready && root.info.width > 0 ? Math.min(width * root.info.height / root.info.width, root.height * 0.5) : 120
            color: "black"; radius: Theme.rControl; clip: true
            VideoFrame {
                id: frame
                objectName: "videoFrame"
                anchors.fill: parent
                service: root.service
                visible: root.ready && !root.detached
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - Theme.s4
                horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                visible: !frame.visible || !frame.hasFrame
                text: !root.present ? qsTr("No reference picture. File › Import Video Reference… chooses one.")
                    : root.detached ? qsTr("Shown in its own window.")
                    : root.info.status === "probing" ? qsTr("Reading the video…")
                    : root.info.status === "missing" ? qsTr("The video file is missing.")
                    : root.info.status === "unsupported" ? qsTr("This video cannot be shown.")
                    : root.service && root.service.playhead < root.info.offset ? qsTr("Before the picture starts.")
                    : qsTr("Waiting for the first frame…")
            }
            Text {
                objectName: "videoTimecode"
                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: Theme.s1
                textFormat: Text.PlainText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                color: "white"; style: Text.Outline; styleColor: "black"
                visible: root.present && text !== ""
                text: root.service ? root.service.videoTimecode : ""
                ControlHelp {
                    title: qsTr("Video timecode")
                    purpose: qsTr("The picture's timecode at the playhead: the file's own timecode when it carries one, otherwise counted from its start. A ';' before the frames means drop-frame counting.")
                    scope: qsTr("Follows the playhead. Choose drop-frame or non-drop below; it changes only how the numbers are written.")
                }
            }
        }

        Line { objectName: "videoName"; visible: root.present; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.present ? root.info.name : "" }
        Line { objectName: "videoStatus"; visible: root.present; text: root.statusLine()
               color: root.info.status === "missing" || root.info.status === "unsupported" ? Theme.warning : Theme.textSecondary }
        Line { objectName: "videoMessage"; visible: root.ready && root.info.message !== ""; text: root.ready ? root.info.message : "" }
        Line { objectName: "videoDecoder"; visible: root.ready; color: Theme.textMuted; text: root.decoderLine() }
        Line { visible: !root.present && root.info.supportReason; color: Theme.warning; text: root.info.supportReason || "" }

        // ── placement ────────────────────────────────────────────────
        RowLayout {
            visible: root.present
            Layout.fillWidth: true
            spacing: Theme.s1
            Text { text: qsTr("Starts at"); color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
            TextEntry {
                id: offsetField
                objectName: "videoOffset"
                Layout.fillWidth: true
                Layout.minimumWidth: 60
                text: root.present ? root.info.offsetText : ""
                selectByMouse: true
                onAccepted: if (root.service) root.service.setVideoOffsetText(text)
                helpTitle: qsTr("Picture start")
                helpText: qsTr("Where the picture's first moment sits on the timeline, as hh:mm:ss.mmm, seconds or a sample count; a leading minus puts the start before the project.")
                helpScope: qsTr("One Undo step. Press Return to apply.")
            }
            ToolButton {
                objectName: "videoStartHere"; iconName: "pin"; text: qsTr("At playhead"); showLabel: true
                helpText: qsTr("Place the picture's start at the playhead.")
                helpScope: qsTr("One Undo step.")
                enabled: root.present
                onClicked: root.service.setVideoStartAtPlayhead()
            }
        }
        Flow {
            visible: root.present
            Layout.fillWidth: true
            spacing: Theme.s1
            ToolButton {
                objectName: "videoAlignTimecode"; iconName: "clock"; text: qsTr("Place by timecode"); showLabel: true
                helpText: qsTr("Match the video's recorded timecode to the project time reference in File › Metadata…. If the project has no reference, it uses the video's and places the video at the start. Timecode is converted to its actual clock time.")
                helpScope: qsTr("One Undo step. Needs a video that carries timecode.")
                unavailableReason: qsTr("This video carries no timecode.")
                enabled: root.ready && root.info.timecodeStart >= 0
                onClicked: root.service.alignVideoToTimeReference()
            }
            Toggle {
                objectName: "videoDropFrame"
                text: qsTr("Drop-frame")
                helpText: qsTr("Keep timecode close to clock time at 29.97 or 59.94 fps. It skips 2 or 4 frame numbers each minute, except every tenth minute. No video frames are removed.")
                helpScope: qsTr("Display only, saved with the project. Only 29.97 and 59.94 pictures can be counted this way.")
                unavailableReason: qsTr("Drop-frame counting is defined only for 29.97 and 59.94 fps.")
                enabled: root.ready && root.info.dropFrameCapable
                checked: root.present && !!root.info.dropFrame
                onClicked: root.service.setVideoDropFrame(!root.info.dropFrame)
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.s1
            ToolButton {
                objectName: "videoImport"; iconName: "film"; text: root.present ? qsTr("Replace…") : qsTr("Import…"); showLabel: true
                helpText: qsTr("Choose the video to cut the sound against. One per project; it replaces any current one.")
                helpScope: qsTr("One Undo step. No sound is taken from the video.")
                onClicked: if (root.shell) root.shell.importVideo()
            }
            ToolButton {
                objectName: "videoRelink"; iconName: "link"; text: qsTr("Relink…"); showLabel: true
                visible: root.present
                helpText: qsTr("Point the reference at the file's new location. Its offset and timecode display are kept.")
                helpScope: qsTr("One Undo step.")
                onClicked: if (root.shell) root.shell.relinkVideo()
            }
            ToolButton {
                objectName: "videoStepBack"; iconName: "chevron-left"; text: qsTr("Previous frame")
                visible: root.present; enabled: root.ready
                helpText: qsTr("Move the playhead to the previous picture frame.")
                shortcut: root.shell ? root.shell.bindingFor("video_step_back") : ""
                onClicked: root.service.videoStep(-1)
            }
            ToolButton {
                objectName: "videoStepForward"; iconName: "chevron-right"; text: qsTr("Next frame")
                visible: root.present; enabled: root.ready
                helpText: qsTr("Move the playhead to the next picture frame.")
                shortcut: root.shell ? root.shell.bindingFor("video_step_forward") : ""
                onClicked: root.service.videoStep(1)
            }
            ToolButton {
                objectName: "videoDetach"; iconName: "maximize"; text: root.detached ? qsTr("Dock") : qsTr("Detach"); showLabel: true
                visible: root.present
                helpText: qsTr("Show the picture in its own window, which can be moved and resized, or bring it back into this tab.")
                onClicked: root.detached = !root.detached
            }
            ToolButton {
                objectName: "videoClear"; iconName: "x"; text: qsTr("Remove")
                visible: root.present
                helpText: qsTr("Take the reference picture out of the project. The file itself is untouched.")
                helpScope: qsTr("One Undo step.")
                onClicked: root.service.clearVideo()
            }
        }
    }

    // ── the detached window ──────────────────────────────────────────
    Window {
        id: detachedWindow
        objectName: "videoWindow"
        visible: root.detached && root.present
        title: qsTr("Video Reference") + (root.present ? " — " + root.info.name : "")
        width: 640; height: 400; color: "black"
        onClosing: root.detached = false
        VideoFrame {
            anchors.fill: parent
            service: root.service
            visible: root.ready && root.detached
        }
        Text {
            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: Theme.s2
            textFormat: Text.PlainText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading
            color: "white"; style: Text.Outline; styleColor: "black"
            text: root.service ? root.service.videoTimecode : ""
        }
    }
}
