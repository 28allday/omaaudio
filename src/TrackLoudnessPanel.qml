pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "trackLoudnessPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int targetId: -1
    property string targetName: ""
    property string message: ""
    readonly property bool busy: service && service.measuringTrackLoudness
    readonly property var report: service ? service.trackLoudnessReport : ({})
    readonly property bool hasReport: report.valid === true && report.track === targetId
    readonly property bool validInputs: target.acceptableInput && ceiling.acceptableInput
    readonly property var proposal: {
        const measurement = root.report
        const stamp = root.service ? root.service.automationStamp : ""
        return root.service && root.validInputs
            ? root.service.trackLoudnessProposal(root.targetId, Numeric.parse(target.text, "C"), Numeric.parse(ceiling.text, "C"))
            : {valid: false, reason: qsTr("Enter a valid loudness target and peak ceiling.")}
    }
    function reading(value, unit) { return Number.isFinite(value) && value > -100 ? value.toFixed(1) + " " + unit : "−∞ " + unit }
    function measure() {
        if (!service) return
        message = service.measureTrackLoudness(targetId) ? "" : service.lastError
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(640, parent ? parent.width - 32 : 640)
    height: Math.min(580, parent ? parent.height - 40 : 580)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: {
        targetId = service ? service.focusedTrack : -1
        targetName = service ? service.trackById(targetId).name || "" : ""
        target.text = String(shell ? shell.deliveryTargetLufs : -14)
        ceiling.text = String(shell ? shell.deliveryCeilingDbTp : -1)
        message = hasReport ? "" : qsTr("Press Measure to analyse this track.")
    }
    onClosed: { if (busy) service.cancelTrackLoudness(); if (shell) shell.restoreFocus() }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    component Note: Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
        color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
    }
    component Fact: RowLayout {
        property string label: ""
        property string value: ""
        Layout.fillWidth: true
        Text { text: parent.label; Layout.preferredWidth: 190; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
        Text { text: parent.value; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
    }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Set Track Loudness"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton { iconName: "x"; text: qsTr("Close"); helpText: qsTr("Close this panel. A measurement still running is cancelled."); onClicked: root.close() }
        }
        ScrollPane {
            id: scroll; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
            ColumnLayout {
                x: scroll.reachLeft; width: scroll.columnWidth; spacing: Theme.s3
                Note { objectName: "trackLoudnessName"; text: root.targetName || qsTr("Click a track before opening Loudness."); color: Theme.textPrimary }
                Note { text: qsTr("Measures this track after its effects, fader and automation, including incoming audio and sidechains. Solo is ignored; muted feeds remain silent.") }
                Note { text: qsTr("The peak ceiling applies here, before downstream buses and master effects. Apply changes this track’s fader in one Undo step.") }
                RowLayout {
                    Text { text: qsTr("Target (LUFS)"); Layout.preferredWidth: 190; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
                    TextEntry {
                        id: target; objectName: "trackLoudnessTarget"; Layout.fillWidth: true; selectByMouse: true
                        validator: DoubleValidator { bottom: -40; top: -5; decimals: 1; notation: DoubleValidator.StandardNotation; locale: "C" }
                        onTextEdited: root.message = ""
                        Accessible.name: targetHelp.title
                        Accessible.description: targetHelp.description
                        ControlHelp {
                            id: targetHelp
                            title: qsTr("Target loudness")
                            purpose: qsTr("The loudness you want this track to reach. LUFS measures perceived loudness; a more negative value is quieter.")
                            scope: qsTr("Range −40 to −5 LUFS. Starts from the delivery target. Apply changes the fader to reach it.")
                        }
                    }
                }
                RowLayout {
                    Text { text: qsTr("Peak ceiling (dBTP)"); Layout.preferredWidth: 190; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
                    TextEntry {
                        id: ceiling; objectName: "trackLoudnessCeiling"; Layout.fillWidth: true; selectByMouse: true
                        validator: DoubleValidator { bottom: -12; top: 0; decimals: 1; notation: DoubleValidator.StandardNotation; locale: "C" }
                        onTextEdited: root.message = ""
                        Accessible.name: ceilingHelp.title
                        Accessible.description: ceilingHelp.description
                        ControlHelp {
                            id: ceilingHelp
                            title: qsTr("Peak ceiling")
                            purpose: qsTr("The highest true peak the match may reach. dBTP is true-peak level, measured between samples, relative to full scale.")
                            scope: qsTr("Range −12 to 0 dBTP. If reaching the target would exceed it, the match stops at the ceiling.")
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Theme.hairline; color: Theme.border }
                ColumnLayout {
                    visible: root.hasReport; Layout.fillWidth: true; spacing: Theme.s2
                    Fact { objectName: "trackLoudnessMeasured"; label: qsTr("Measured loudness"); value: root.reading(root.report.integrated, "LUFS") }
                    Fact { label: qsTr("Measured true peak"); value: root.reading(root.report.truePeak, "dBTP") }
                    Note { visible: root.report.stale === true; text: qsTr("The mix has changed. Measure again before applying."); color: Theme.warning }
                }
                ColumnLayout {
                    visible: root.proposal.valid === true; Layout.fillWidth: true; spacing: Theme.s2
                    Fact { objectName: "trackLoudnessAdjustment"; label: qsTr("Fader adjustment"); value: Number(root.proposal.changeDb || 0).toFixed(2) + " dB" }
                    Fact { label: qsTr("New fader level"); value: Number(root.proposal.gainDb || 0).toFixed(2) + " dB" }
                    Fact { label: qsTr("Estimated result"); value: root.reading(root.proposal.estimatedLufs, "LUFS") + " · " + root.reading(root.proposal.estimatedPeak, "dBTP") }
                    Note { visible: root.proposal.peakLimited === true; color: Theme.warning; text: qsTr("The peak ceiling limits the match. A gain change alone cannot reach this loudness target.") }
                    Note { visible: root.proposal.gainLimited === true; color: Theme.warning; text: qsTr("The +12 dB fader limit prevents reaching this target.") }
                    Note { text: qsTr("Measure again after applying to check the result.") }
                }
                Note { visible: !root.proposal.valid && !root.busy; text: root.proposal.reason || "" }
            }
        }
        Rectangle {
            visible: root.busy; Layout.fillWidth: true; Layout.preferredHeight: 4; color: Theme.controlBg
            Rectangle { width: parent.width * (root.service ? root.service.trackLoudnessProgress : 0); height: parent.height; color: Theme.accent }
        }
        Note { objectName: "trackLoudnessMessage"; text: root.message || root.report.message || ""; visible: text !== "" }
        RowLayout {
            ToolButton {
                objectName: "trackLoudnessMeasure"; text: root.hasReport ? qsTr("Measure again") : qsTr("Measure"); showLabel: true
                helpText: qsTr("Measure this track's loudness and true peak after its effects, fader and automation.")
                helpScope: qsTr("Runs in the background and plays nothing aloud. Nothing changes until you Apply.")
                unavailableReason: root.targetId < 0 ? qsTr("Click a track to make it active first.") : qsTr("A measurement is already running.")
                enabled: root.targetId >= 0 && !root.busy; onClicked: root.measure()
            }
            ToolButton {
                objectName: "trackLoudnessCancel"; text: qsTr("Cancel measurement"); showLabel: true; visible: root.busy
                helpText: qsTr("Stop the running measurement. Nothing is changed.")
                onClicked: root.service.cancelTrackLoudness()
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "trackLoudnessApply"; text: qsTr("Apply gain"); showLabel: true
                helpText: qsTr("Move this track's fader so its loudness matches the target, within the peak ceiling.")
                helpScope: qsTr("One Undo step. The source audio and effects are unchanged; only the fader moves.")
                unavailableReason: root.busy ? qsTr("Wait for the measurement to finish.")
                                 : !root.hasReport ? qsTr("Measure the track first.")
                                 : (root.proposal.reason || qsTr("Enter a valid loudness target and peak ceiling."))
                enabled: root.proposal.valid === true && !root.busy
                onClicked: {
                    const stamp = root.proposal.stamp
                    if (root.service.applyTrackLoudness(root.targetId, stamp, Numeric.parse(target.text, "C"), Numeric.parse(ceiling.text, "C")))
                        root.message = stamp === root.service.automationStamp ? qsTr("The level already matches.") : qsTr("Gain applied. Undo restores the previous level.")
                    else root.message = root.service.lastError
                }
            }
        }
    }
}
