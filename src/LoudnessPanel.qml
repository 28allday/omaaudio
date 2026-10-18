pragma ComponentBehavior: Bound
import QtQuick
import "ui"

// The rack's Loudness tab for the master bus: the live R128 meter on the mix
// (momentary, short-term, integrated, range, true peak) against the delivery
// target, and the analysed report of the whole programme underneath.
Item {
    id: root

    property var shell: null
    readonly property var service: shell ? shell.service : null
    readonly property var live: service ? service.loudness : ({})
    readonly property var report: service ? service.masterReport : ({})
    readonly property real target: shell ? shell.deliveryTargetLufs : -16
    readonly property real ceiling: shell ? shell.deliveryCeilingDbTp : -1
    readonly property bool loudnessMode: shell ? shell.deliveryNormalise === "loudness" : true

    function fmt(v, decimals) {
        return root.shell ? root.shell.formatLufs(v, decimals) : String(v)
    }

    // One horizontal loudness bar on the shared LUFS scale, with the target
    // marked, and the readout beside it.
    component LoudnessBar: Item {
        id: bar
        property string label: ""
        readonly property alias controlHelp: meterHelp
        Accessible.role: Accessible.Indicator
        Accessible.name: meterHelp.title
        Accessible.description: meterHelp.description
        ControlHelp {
            id: meterHelp
            title: qsTr("Live loudness: %1").arg(bar.label)
            purpose: qsTr("Read the loudness of the master mix during playback. LUFS measures perceived loudness; more negative is quieter.")
            scope: qsTr("M is momentary, S is short-term and I is integrated over the measurement so far. This meter does not change the audio.")
            topic: qsTr("Use the whole-programme analysis report when checking a delivery. A partial playback measurement does not describe the entire programme.")
        }
        property real value: -200
        property bool showTarget: true
        width: parent.width
        height: Theme.hRow

        Text {
            id: caption
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 22
            text: bar.label
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.textSecondary
        }
        Rectangle {
            id: well
            anchors.left: caption.right
            anchors.leftMargin: Theme.s1
            anchors.right: readout.left
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            height: Theme.hLoudnessMeter
            color: Theme.meterBg
            border.width: Theme.hairline
            border.color: Theme.border

            Rectangle {
                x: 1; y: 1
                height: parent.height - 2
                width: Math.round((parent.width - 2) * Theme.lufsFraction(bar.value))
                color: bar.value > root.target + 1 && root.loudnessMode ? Theme.loudnessOver
                     : Theme.loudnessShortTerm
                Behavior on width { NumberAnimation { duration: Theme.dFast } }
            }
            // The target: a hairline, cyan, where the delivery wants the level.
            Rectangle {
                visible: bar.showTarget && root.loudnessMode
                x: 1 + Math.round((parent.width - 2) * Theme.lufsFraction(root.target)) - 1
                y: -2; width: 2; height: parent.height + 4
                color: Theme.loudnessTarget
            }
        }
        Text {
            id: readout
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.wLoudnessReadout
            horizontalAlignment: Text.AlignRight
            text: root.fmt(bar.value, 1)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: Theme.textPrimary
        }
    }

    component Fact: Row {
        property string label: ""
        property string value: ""
        property color valueColor: Theme.textPrimary
        width: parent.width
        spacing: Theme.s2
        Text {
            width: parent.width - Theme.wLoudnessReadout - Theme.s2 * 2 - 24
            text: parent.label
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
        }
        Text {
            width: Theme.wLoudnessReadout + 24
            horizontalAlignment: Text.AlignRight
            text: parent.value
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            font.features: { "tnum": 1 }
            color: parent.valueColor
        }
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: Theme.s2
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: column
            width: parent.width
            spacing: Theme.s2

            // ── live ──────────────────────────────────────────────────
            Row {
                width: parent.width
                Text {
                    text: qsTr("LIVE · EBU R128")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    font.weight: Theme.wHeading; color: Theme.textMuted
                    anchors.verticalCenter: parent.verticalCenter
                }
                Item { width: parent.width - 180; height: 1 }
                ToolButton {
                    objectName: "loudnessReset"
                    iconName: "rotate-ccw"; text: qsTr("Reset"); showLabel: true
                    helpTitle: qsTr("Reset live loudness")
                    helpText: qsTr("Start the live measurement again from nothing.")
                    helpScope: qsTr("Clears the integrated value, loudness range and held true peak. The audio is unchanged.")
                    shortcut: root.shell ? root.shell.bindingFor("reset_loudness") : ""
                    onClicked: if (root.service) root.service.resetLoudness()
                }
            }
            LoudnessBar { objectName: "liveMomentary"; label: qsTr("M"); value: root.live.momentary !== undefined ? root.live.momentary : -200 }
            LoudnessBar { objectName: "liveShortTerm"; label: qsTr("S"); value: root.live.shortTerm !== undefined ? root.live.shortTerm : -200 }
            LoudnessBar { objectName: "liveIntegrated"; label: qsTr("I"); value: root.live.integrated !== undefined ? root.live.integrated : -200 }
            Fact { label: qsTr("Loudness range"); value: (root.live.range !== undefined ? root.live.range.toFixed(1) : "0.0") + " LU" }
            Fact {
                objectName: "liveTruePeak"
                label: qsTr("True peak, held")
                value: root.fmt(root.live.truePeak, 1) + " dBTP"
                valueColor: root.live.truePeak !== undefined && root.live.truePeak > root.ceiling ? Theme.loudnessOver : Theme.textPrimary
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
                text: qsTr("Measured on the mix after the bus, before the monitor level. Integrated keeps counting across stops and seeks until reset.")
            }
            Fact {
                objectName: "previewMatch"
                label: qsTr("Before the bus")
                value: !root.service ? ""
                     : root.service.previewBefore
                       ? (root.service.previewMatched ? qsTr("listening, matched %1 dB").arg(root.service.previewMatchDb.toFixed(1))
                                                      : qsTr("listening, not matched"))
                       : (root.service.previewMatched ? qsTr("bus adds %1 dB").arg(root.service.previewMatchDb.toFixed(1))
                                                      : qsTr("analyse to match"))
                valueColor: root.service && root.service.previewBefore ? Theme.warning : Theme.textPrimary
            }

            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

            // ── analysed ──────────────────────────────────────────────
            Row {
                width: parent.width
                Text {
                    text: qsTr("ANALYSED MIX")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    font.weight: Theme.wHeading; color: Theme.textMuted
                    anchors.verticalCenter: parent.verticalCenter
                }
                Item { width: parent.width - 200; height: 1 }
                ToolButton {
                    objectName: "analyseButton"
                    iconName: "refresh-cw"
                    text: root.service && root.service.analysing ? qsTr("Analysing…") : qsTr("Analyse")
                    helpTitle: qsTr("Analyse the mix")
                    helpText: qsTr("Measure the complete mix, including its effects, to check loudness and peak levels.")
                    helpScope: qsTr("Fills the report below and the loudness history. Nothing is written to disk and the project is unchanged.")
                    unavailableReason: root.service === null || root.service.trackCount === 0 ? qsTr("Open or import audio first.")
                                                                                              : qsTr("An analysis is already running.")
                    showLabel: true
                    shortcut: root.shell ? root.shell.bindingFor("analyse_master") : ""
                    enabled: root.service !== null && root.service.trackCount > 0 && !root.service.analysing
                    onClicked: root.shell.runAction("analyse_master")
                }
            }
            Text {
                objectName: "analysisNote"
                width: parent.width
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                color: root.service && root.service.analysisStale && root.service.analysed ? Theme.warning : Theme.textMuted
                text: !root.service || root.service.trackCount === 0 ? qsTr("Open a project to analyse its mix.")
                    : root.service.analysing ? qsTr("Measuring the complete mix and its effects…")
                    : !root.service.analysed ? qsTr("Not analysed yet. The report and the loudness history need one pass over the whole mix.")
                    : root.service.analysisStale ? qsTr("The mix has changed since this was measured.")
                    : qsTr("Measured on the whole programme, through the bus.")
            }
            Column {
                width: parent.width
                spacing: Theme.s1
                visible: root.service !== null && root.service.analysed
                opacity: root.service && root.service.analysisStale ? 0.6 : 1
                Fact { objectName: "reportIntegrated"; label: qsTr("Integrated"); value: root.fmt(root.report.integrated, 1) + " LUFS" }
                Fact { label: qsTr("Loudness range"); value: (root.report.range !== undefined ? root.report.range.toFixed(1) : "0.0") + " LU" }
                Fact {
                    objectName: "reportTruePeak"
                    label: qsTr("True peak"); value: root.fmt(root.report.truePeak, 1) + " dBTP"
                    valueColor: root.report.truePeak !== undefined && root.report.truePeak > root.ceiling ? Theme.loudnessOver : Theme.textPrimary
                }
                Fact { label: qsTr("Momentary, max"); value: root.fmt(root.report.momentaryMax, 1) + " LUFS" }
                Fact { label: qsTr("Short-term, max"); value: root.fmt(root.report.shortTermMax, 1) + " LUFS" }
                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Fact {
                    objectName: "reportGain"
                    visible: root.loudnessMode
                    label: qsTr("Gain to reach %1 LUFS").arg(root.target.toFixed(1))
                    value: {
                        if (root.report.integrated === undefined || root.report.integrated <= -100)
                            return "—"
                        const gain = root.target - root.report.integrated
                        return (gain >= 0 ? "+" : "") + gain.toFixed(1) + " dB"
                    }
                }
                Fact {
                    visible: root.loudnessMode
                    label: qsTr("Peak after that gain")
                    value: {
                        if (root.report.integrated === undefined || root.report.integrated <= -100
                            || root.report.truePeak === undefined || root.report.truePeak <= -100)
                            return "—"
                        return (root.report.truePeak + root.target - root.report.integrated).toFixed(1) + " dBTP"
                    }
                    valueColor: (root.report.integrated !== undefined && root.report.truePeak !== undefined
                                 && root.report.truePeak + root.target - root.report.integrated > root.ceiling)
                                ? Theme.loudnessOver : Theme.textPrimary
                }
                Text {
                    visible: root.loudnessMode && root.report.integrated !== undefined && root.report.truePeak !== undefined
                             && root.report.truePeak + root.target - root.report.integrated > root.ceiling
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    color: Theme.warning
                    text: qsTr("That would pass the %1 dBTP ceiling. The export will hold at the ceiling instead; a limiter on the bus is what closes the gap.").arg(root.ceiling.toFixed(1))
                }
            }
        }
    }
}
