pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import "ui"

// The Mastering page's left dock: Source is the session mix -- what it is,
// what the bus does to it, the tracks that make it -- and Queue is the render
// queue with each job's state, progress and what can be done with it.
Rectangle {
    id: root
    objectName: "masteringSourceDock"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int currentTab: 0

    color: Theme.panelBg

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.hairline
        color: Theme.border
        z: 2
    }

    readonly property int queued: {
        if (!root.service)
            return 0
        var n = 0
        const q = root.service.renderQueue
        for (var i = 0; i < q.length; ++i) {
            if (q[i].state === "queued" || q[i].state === "rendering")
                ++n
        }
        return n
    }

    DockHeader {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        tabs: [qsTr("Source"), root.queued > 0 ? qsTr("Queue (%1)").arg(root.queued) : qsTr("Queue")]
        currentIndex: root.currentTab
        onActivated: function (i) { root.currentTab = i }
        trailing: [
            IconButton {
                iconName: "panel-left"; text: qsTr("Hide Source")
                helpText: qsTr("Hide this dock. Show it again from the View menu.")
                onClicked: if (root.shell) root.shell.leftDockVisible = false
            }
        ]
    }

    component Heading: Text {
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        font.weight: Theme.wHeading
        color: Theme.textMuted
    }
    component Fact: Row {
        property string label: ""
        property string value: ""
        width: parent.width
        spacing: Theme.s2
        Text {
            width: 96
            text: parent.label
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Text {
            width: parent.width - 96 - Theme.s2
            text: parent.value
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: Theme.textPrimary
        }
    }

    // ── Source ──────────────────────────────────────────────────────────
    Flickable {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.hairline
        anchors.bottom: parent.bottom
        visible: root.currentTab === 0
        contentHeight: sourceColumn.implicitHeight + Theme.s2 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: sourceColumn
            x: Theme.s2; y: Theme.s2
            width: parent.width - Theme.s2 * 2
            spacing: Theme.s2

            Heading { text: qsTr("FULL MIX") }

            Rectangle {
                objectName: "sessionMixCard"
                width: parent.width
                height: mixColumn.implicitHeight + Theme.s2 * 2
                radius: Theme.rControl
                color: Theme.panelRaised
                border.width: Theme.hairline
                border.color: Theme.border

                Column {
                    id: mixColumn
                    x: Theme.s2; y: Theme.s2
                    width: parent.width - Theme.s2 * 2
                    spacing: Theme.s1

                    Row {
                        width: parent.width
                        spacing: Theme.s2
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 3; height: 14; radius: 1
                            color: Theme.roleColor("master")
                        }
                        Text {
                            width: parent.width - 3 - Theme.s2
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: !root.service ? ""
                                : root.service.projectName !== "" ? root.service.projectName
                                : root.service.loaded ? root.service.takeName : qsTr("No project")
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsHeading
                            font.weight: Theme.wHeading
                            color: Theme.textPrimary
                        }
                    }
                    Fact {
                        label: qsTr("Tracks")
                        value: root.service ? String(root.service.trackCount) : "0"
                    }
                    Fact {
                        label: qsTr("Length")
                        value: root.service && root.shell ? root.shell.formatDuration(root.service.takeFrames) : ""
                    }
                    Fact {
                        label: qsTr("Rate")
                        value: root.service ? (root.service.sampleRate / 1000) + " kHz  ·  " + qsTr("stereo") : ""
                    }
                    Fact {
                        label: qsTr("Bus")
                        value: {
                            if (!root.service) return ""
                            const n = root.service.masterInserts.length
                            var line = n === 0 ? qsTr("no effects") : n === 1 ? qsTr("1 effect") : qsTr("%1 effects").arg(n)
                            if (root.service.masterTrimDb !== 0)
                                line += "  ·  " + qsTr("trim %1 dB").arg((root.service.masterTrimDb >= 0 ? "+" : "") + root.service.masterTrimDb.toFixed(1))
                            return line
                        }
                    }
                }
            }

            ToolButton {
                objectName: "sourceAnalyse"
                width: parent.width
                iconName: "refresh-cw"
                text: root.service && root.service.analysing ? qsTr("Analysing…")
                    : root.service && root.service.analysed && !root.service.analysisStale ? qsTr("Analysed — measure again")
                    : root.service && root.service.analysed ? qsTr("Mix changed — analyse again")
                    : qsTr("Analyse the mix")
                helpTitle: qsTr("Analyse the mix")
                helpText: qsTr("Measure the complete mix, including its effects, to check loudness and peak levels.")
                helpScope: qsTr("Needed before a level-matched Before/After preview. Nothing is written to disk.")
                unavailableReason: root.service === null || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                                                                                          : qsTr("An analysis is already running.")
                showLabel: true
                shortcut: root.shell ? root.shell.bindingFor("analyse_master") : ""
                enabled: root.service !== null && root.service.trackCount > 0 && !root.service.analysing
                onClicked: root.shell.runAction("analyse_master")
            }

            Column {
                width: parent.width
                spacing: Theme.s1
                visible: root.service !== null && root.service.analysed
                opacity: root.service && root.service.analysisStale ? 0.6 : 1
                Fact { label: qsTr("Integrated"); value: root.shell ? root.shell.formatLufs(root.service.masterReport.integrated, 1) + " LUFS" : "" }
                Fact { label: qsTr("Range"); value: root.service.masterReport.range !== undefined ? root.service.masterReport.range.toFixed(1) + " LU" : "" }
                Fact { label: qsTr("True peak"); value: root.shell ? root.shell.formatLufs(root.service.masterReport.truePeak, 1) + " dBTP" : "" }
            }

            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            Heading { text: qsTr("TRACKS IN THE MIX") }

            Repeater {
                model: root.service ? root.service.trackIdModel : null
                delegate: Rectangle {
                    id: row
                    objectName: "sourceTrack"
                    required property var modelData
                    property int revision: 0   // this track's data changed (see MultitrackWorkspace)
                    Connections {
                        target: root.service
                        function onTrackDataChanged(ids) { if (ids.indexOf(row.modelData) >= 0) ++row.revision }
                    }
                    readonly property var track: { row.revision; return root.service ? root.service.trackById(modelData) : null }
                    width: sourceColumn.width
                    height: Theme.hRow + Theme.s1
                    radius: Theme.rControl
                    // Selected here as on the Multitrack page: Export… → Selected tracks
                    // writes one stem for each.
                    readonly property bool selected: root.service !== null
                        && (root.service.selectedTracks.indexOf(modelData) >= 0 || root.service.focusedTrack === modelData)
                    color: row.selected ? (rowHover.hovered ? Theme.selectedHover : Theme.selectedFill)
                         : rowHover.hovered ? Theme.hoverOverlay : "transparent"
                    HoverHandler { id: rowHover }
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onClicked: (mouse) => { if (root.service) root.service.selectTrack(row.modelData,
                            (mouse.modifiers & Qt.ControlModifier) !== 0, (mouse.modifiers & Qt.ShiftModifier) !== 0) }
                    }
                    ControlHelp {
                        title: row.track ? row.track.name : qsTr("Track")
                        purpose: qsTr("A track feeding the mix. Click to select it; Ctrl-click or Shift-click to select several.")
                        scope: qsTr("Export… → Selected tracks writes one file for each selected track.")
                    }
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s2
                        anchors.rightMargin: Theme.s2
                        spacing: Theme.s2
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8; height: 8; radius: 4
                            color: row.track ? (row.track.color !== "" ? row.track.color : Theme.roleColor(row.track.role)) : Theme.textMuted
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 8 - 60 - Theme.s2 * 2
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: row.track ? row.track.name : ""
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                            color: row.track && row.track.silenced ? Theme.textMuted : Theme.textPrimary
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 60
                            horizontalAlignment: Text.AlignRight
                            text: !row.track ? "" : row.track.muted ? qsTr("muted") : row.track.soloed ? qsTr("solo")
                                : row.track.silenced ? qsTr("silent") : row.track.gainDb.toFixed(1) + " dB"
                            font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel
                            font.features: { "tnum": 1 }
                            color: row.track && (row.track.muted || row.track.silenced) ? Theme.warning : Theme.textMuted
                        }
                    }
                }
            }
            Text {
                visible: root.service === null || root.service.trackCount === 0
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Open a project on the Arrange or Take page. The mix of every track is what masters here.")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    // ── Queue ───────────────────────────────────────────────────────────
    Item {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.hairline
        anchors.bottom: parent.bottom
        visible: root.currentTab === 1

        ListView {
            id: queueList
            objectName: "renderQueueList"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: queueFooter.top
            anchors.margins: Theme.s2
            clip: true
            spacing: Theme.s1
            model: root.service ? root.service.renderQueue : []
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: job
                objectName: "renderJob"
                required property var modelData
                readonly property string jobState: modelData.state
                readonly property bool lossy: ["mp3", "ogg", "opus", "aac"].indexOf(modelData.format) >= 0
                width: queueList.width
                height: jobColumn.implicitHeight + Theme.s2 * 2
                radius: Theme.rControl
                color: Theme.panelRaised
                border.width: Theme.hairline
                border.color: job.jobState === "failed" ? Theme.danger : Theme.border

                Column {
                    id: jobColumn
                    x: Theme.s2; y: Theme.s2
                    width: parent.width - Theme.s2 * 2
                    spacing: Theme.s1

                    Row {
                        width: parent.width
                        spacing: Theme.s2
                        Text {
                            width: parent.width - stateChip.width - Theme.s2
                            elide: Text.ElideMiddle
                            textFormat: Text.PlainText
                            text: job.modelData.name
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                            color: Theme.textPrimary
                        }
                        Rectangle {
                            id: stateChip
                            width: stateText.implicitWidth + Theme.s2
                            height: 16; radius: 8
                            color: job.jobState === "done" ? Theme.withAlpha(Theme.success, 0.2)
                                 : job.jobState === "failed" ? Theme.withAlpha(Theme.danger, 0.2)
                                 : job.jobState === "rendering" ? Theme.withAlpha(Theme.accent, 0.2)
                                 : Theme.controlBg
                            Text {
                                id: stateText
                                objectName: "renderJobState"
                                anchors.centerIn: parent
                                text: job.jobState === "done" ? qsTr("Done")
                                    : job.jobState === "failed" ? qsTr("Failed")
                                    : job.jobState === "rendering" ? qsTr("Exporting")
                                    : job.jobState === "cancelled" ? qsTr("Cancelled")
                                    : qsTr("Queued")
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler
                                color: job.jobState === "done" ? Theme.success
                                     : job.jobState === "failed" ? Theme.danger
                                     : job.jobState === "rendering" ? Theme.accent : Theme.textSecondary
                            }
                        }
                    }
                    Text {
                        objectName: "renderJobFormat"
                        width: parent.width
                        elide: Text.ElideMiddle
                        textFormat: Text.PlainText
                        text: {
                            var bits = [job.modelData.format.toUpperCase()]
                            if (job.modelData.container && job.modelData.container !== job.modelData.format && job.jobState === "done")
                                bits.push(qsTr("written as %1").arg(String(job.modelData.container).toUpperCase()))
                            if (job.modelData.metadataWritten) bits.push(qsTr("metadata"))
                            if (job.modelData.markersWritten > 0) bits.push(qsTr("%1 markers").arg(job.modelData.markersWritten))
                            if (job.modelData.format === "aac")
                                bits.push(job.modelData.bitrateKbps > 0
                                          ? qsTr("%1 kb/s target").arg(job.modelData.bitrateKbps)
                                          : qsTr("Automatic bitrate"))
                            if (!job.lossy)
                                bits.push(job.modelData.bitDepth === 32 ? qsTr("float") : qsTr("%1-bit").arg(job.modelData.bitDepth))
                            bits.push((job.modelData.sampleRate / 1000) + " kHz")
                            bits.push(job.modelData.channels === 1 ? qsTr("mono") : qsTr("stereo"))
                            if (job.modelData.normalise === "loudness")
                                bits.push(job.modelData.targetLufs.toFixed(1) + " LUFS")
                            return bits.join(" · ")
                        }
                        font.family: Theme.monoFamily; font.pixelSize: Theme.fsRuler
                        font.features: { "tnum": 1 }
                        color: Theme.textMuted
                    }
                    Rectangle {
                        visible: job.jobState === "rendering"
                        width: parent.width; height: 4; radius: 2
                        color: Theme.controlBg
                        Rectangle {
                            width: parent.width * (root.service && root.service.rendering ? root.service.renderProgress : job.modelData.progress)
                            height: parent.height; radius: parent.radius
                            color: Theme.accent
                        }
                    }
                    Text {
                        visible: job.modelData.message !== "" || job.modelData.warning !== ""
                        width: parent.width
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: job.modelData.warning !== "" ? job.modelData.warning : job.modelData.message
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler
                        color: job.jobState === "failed" ? Theme.danger
                             : job.modelData.warning !== "" ? Theme.warning : Theme.textMuted
                    }
                    Text {
                        objectName: "renderJobReport"
                        visible: job.jobState === "done" && job.modelData.report.delivered !== undefined
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: {
                            const d = job.modelData.report.delivered
                            if (!d || !root.shell) return ""
                            return (job.lossy ? qsTr("Encoder input %1 LUFS · %2 dBTP · gain %3 dB")
                                              : qsTr("Delivered %1 LUFS · %2 dBTP · gain %3 dB"))
                                .arg(root.shell.formatLufs(d.integrated, 1))
                                .arg(root.shell.formatLufs(d.truePeak, 1))
                                .arg((job.modelData.report.appliedGainDb >= 0 ? "+" : "") + job.modelData.report.appliedGainDb.toFixed(1))
                        }
                        font.family: Theme.monoFamily; font.pixelSize: Theme.fsRuler
                        font.features: { "tnum": 1 }
                        color: Theme.textSecondary
                    }
                    Row {
                        spacing: Theme.s1
                        IconButton {
                            objectName: "renderCancel"
                            iconName: "x"; text: qsTr("Cancel")
                            helpTitle: qsTr("Cancel export")
                            helpText: qsTr("Drop this job if it is still queued, or stop it if it is exporting.")
                            helpScope: qsTr("Export again queues it once more with the same settings.")
                            visible: job.jobState === "queued" || job.jobState === "rendering"
                            onClicked: if (root.service) root.service.cancelRender(job.modelData.id)
                        }
                        IconButton {
                            objectName: "renderRetry"
                            iconName: "refresh-cw"; text: qsTr("Export again")
                            helpText: qsTr("Queue this job again with the same settings and output path.")
                            helpScope: qsTr("Uses the mix as it is now, so later edits are included.")
                            visible: job.jobState === "done" || job.jobState === "failed" || job.jobState === "cancelled"
                            onClicked: if (root.service) root.service.retryRender(job.modelData.id)
                        }
                        IconButton {
                            objectName: "renderReveal"
                            iconName: "folder-open"; text: qsTr("Show in folder")
                            helpText: qsTr("Open the folder that holds the exported file in your file manager.")
                            visible: job.jobState === "done"
                            onClicked: if (root.service) root.service.revealRender(job.modelData.id)
                        }
                        IconButton {
                            objectName: "renderRemove"
                            iconName: "trash-2"; text: qsTr("Remove from the queue")
                            helpText: qsTr("Drop this entry from the list.")
                            helpScope: qsTr("A exported file stays on disk. A running job must be cancelled first.")
                            visible: job.jobState !== "rendering"
                            iconColor: hovered ? Theme.danger : Theme.textSecondary
                            onClicked: if (root.service) root.service.removeRender(job.modelData.id)
                        }
                    }
                }
            }
        }

        Text {
            anchors.centerIn: queueList
            visible: queueList.count === 0
            width: queueList.width - Theme.s4
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("No exports yet. Choose your settings along the bottom and press Export.")
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }

        Rectangle {
            id: queueFooter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            // As tall as its note needs: the note wraps rather than losing its end.
            height: footerRow.implicitHeight + Theme.s2 * 2
            color: Theme.panelBg
            Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.border }
            RowLayout {
                id: footerRow
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                anchors.margins: Theme.s2
                spacing: Theme.s2
                ToolButton {
                    objectName: "queueClear"
                    iconName: "trash-2"; text: qsTr("Clear finished"); showLabel: true
                    helpText: qsTr("Remove every done, failed and cancelled job from the list.")
                    helpScope: qsTr("Exported files stay on disk. Queued and running jobs are kept.")
                    unavailableReason: qsTr("The queue is empty.")
                    enabled: queueList.count > 0
                    onClicked: if (root.service) root.service.clearFinishedRenders()
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Cancel a running export to stop it; retry to export it again.")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsRuler
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
