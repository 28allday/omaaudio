pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"
import "ui/AutomationCurve.js" as Curve
import "ui/NumericDraft.js" as Numeric

C.Popup {
    id: root
    objectName: "generatedDuckingPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int requestedTrack: -1
    property int targetTrack: -1
    property var triggerTracks: []
    property string message: ""
    property string analysedSignature: ""
    property string analysedStamp: ""
    property string ownedToken: ""
    property bool analysisInFlight: false
    property bool hasResult: false
    property int appliedTrack: -1
    readonly property bool busy: service && service.generatingDucking
    readonly property var preview: service ? service.duckingPreview : ({})
    // Track data only while shown: a closed panel would otherwise re-evaluate its
    // per-track controls on every edit, which adds up in large sessions.
    property bool showing: false
    readonly property var tracks: showing && service ? service.trackSummaries : []
    readonly property var triggerChoices: tracks.filter(t => t.id !== targetTrack)
    readonly property bool targetExists: tracks.some(t => t.id === targetTrack)
    readonly property bool triggersExist: triggerTracks.length > 0 && triggerTracks.every(id => triggerChoices.some(t => t.id === id))
    readonly property bool valid: targetExists && triggersExist && startInput.valid && endInput.valid
        && endInput.value > startInput.value && threshold.valid && reduction.valid && attack.valid && hold.valid && release.valid
    readonly property string draftSignature: JSON.stringify({track: targetTrack, triggers: triggerTracks,
        start: startInput.value, end: endInput.value, thresholdDb: threshold.value, reductionDb: reduction.value,
        attackMs: attack.value, holdMs: hold.value, releaseMs: release.value, replaceExisting: replaceExisting.checked})
    readonly property bool settingsChanged: analysedSignature !== "" && analysedSignature !== draftSignature
    readonly property bool stale: hasResult && (preview.stale === true || !service || analysedStamp !== service.automationStamp)
    readonly property bool ownsPreview: ownedToken !== "" && preview.token === ownedToken
    readonly property bool canApply: valid && !busy && service && !service.recording && !service.loading && ownsPreview && preview.valid === true && !settingsChanged && !stale
    readonly property var points: hasResult ? preview.points || [] : []
    readonly property var previousPoints: hasResult ? preview.previousPoints || [] : []

    function openForTrack(track) { requestedTrack = track; open() }
    function useSelectedTime() {
        if (service && !service.selectionEmpty) { startInput.show(service.selectionStart); endInput.show(service.selectionEnd) }
    }
    function useWholeProject() {
        startInput.show(0)
        endInput.show(Math.max(0, tracks.reduce((end, track) => Math.max(end, track.frames || 0), 0)))
    }
    function toggleTrigger(track, checked) {
        triggerTracks = checked ? triggerTracks.concat([track]).sort((a, b) => a - b) : triggerTracks.filter(id => id !== track)
    }
    function syncAnalysis() {
        if (!analysisInFlight || busy) return
        analysisInFlight = false; hasResult = true
        ownedToken = preview.token || ""
        message = preview.message || (service ? service.lastError : "")
        plot.requestPaint()
    }
    function analyse() {
        if (!valid || busy || !service) return
        analysedSignature = draftSignature; analysedStamp = service.automationStamp
        ownedToken = ""; hasResult = false; appliedTrack = -1; message = ""; analysisInFlight = true
        const spec = JSON.parse(draftSignature); spec.stamp = analysedStamp
        if (!service.analyseDucking(spec)) {
            analysisInFlight = false; message = service.lastError; hasResult = false
        }
        Qt.callLater(syncAnalysis)
    }
    function cancelAnalysis() {
        analysisInFlight = false; ownedToken = ""; hasResult = false; analysedSignature = ""
        if (service) service.cancelDucking()
        message = qsTr("Analysis cancelled. No automation was changed.")
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(880, parent ? parent.width - 32 : 880)
    height: Math.min(680, parent ? parent.height - 40 : 680)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: {
        showing = true
        targetTrack = requestedTrack >= 0 ? requestedTrack : service ? service.focusedTrack : -1
        requestedTrack = -1; triggerTracks = []; message = ""; analysedSignature = ""; analysedStamp = ""
        ownedToken = ""; analysisInFlight = false; hasResult = false; appliedTrack = -1
        replaceExisting.checked = false
        threshold.text = "-30"; reduction.text = "12"; attack.text = "100"; hold.text = "250"; release.text = "500"
        if (service && !service.selectionEmpty) useSelectedTime(); else useWholeProject()
    }
    onAboutToHide: if (analysisInFlight && busy) cancelAnalysis()
    onClosed: { showing = false; if (shell) shell.restoreFocus() }
    Connections {
        target: root.service
        function onDuckingChanged() { Qt.callLater(root.syncAnalysis); plot.requestPaint() }
    }
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    component Setting: RowLayout {
        id: setting
        property string label: ""
        property string help: ""
        property real minimum: 0
        property real maximum: 10000
        property alias text: input.text
        readonly property bool valid: input.acceptableInput
        readonly property real value: Numeric.parse(input.text, "C")
        Layout.fillWidth: true
        Text { text: parent.label; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        TextEntry {
            id: input; objectName: setting.objectName + "Input"; Layout.fillWidth: true; Layout.minimumWidth: 62
            selectByMouse: true; enabled: !root.busy
            validator: DoubleValidator { bottom: setting.minimum; top: setting.maximum; decimals: 3; notation: DoubleValidator.StandardNotation; locale: "C" }
            helpTitle: setting.label; helpText: setting.help
            helpScope: qsTr("Changes require another analysis before Apply points.")
            unavailableReason: qsTr("Wait for analysis to finish, or cancel it.")
        }
    }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Generate ducking"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            ToolButton {
                text: qsTr("Close"); iconName: "x"; onClicked: root.close()
                helpText: qsTr("Close this panel. An analysis still running is cancelled.")
                helpScope: qsTr("Only Apply points changes automation. Applied edits remain and can be undone.")
            }
        }
        ScrollPane {
            id: scroll; objectName: "generatedDuckingScroll"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            contentWidth: availableWidth
            ColumnLayout {
                x: scroll.reachLeft; width: scroll.columnWidth; spacing: Theme.s3
                Text {
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
                    text: qsTr("Lower one track while the chosen triggers are audible. Analyse first, inspect the proposed volume points, then Apply. The points remain editable in Automation.")
                }
                ComboField {
                    objectName: "generatedDuckingTarget"; Layout.fillWidth: true; caption: qsTr("Track to lower")
                    model: root.tracks.map(t => t.name); currentIndex: root.tracks.findIndex(t => t.id === root.targetTrack)
                    enabled: !root.busy
                    helpText: qsTr("Choose the track that should become quieter, such as music underneath speech.")
                    helpScope: qsTr("The generated volume curve multiplies this track's fader. The fader setting stays unchanged.")
                    onActivated: index => {
                        if (index < 0 || index >= root.tracks.length) return
                        root.targetTrack = root.tracks[index].id
                        root.triggerTracks = root.triggerTracks.filter(id => id !== root.targetTrack)
                    }
                }
                Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Trigger tracks"); color: Theme.textSecondary }
                ScrollPane {
                    id: triggerScroll
                    // Inside the panel's own scrolling column: its bar stays inside the column.
                    Layout.rightMargin: 0
                    Layout.fillWidth: true; Layout.preferredHeight: Math.min(100, Math.max(32, triggerList.implicitHeight)); clip: true
                    contentWidth: availableWidth
                    GridLayout {
                        id: triggerList; x: triggerScroll.reachLeft; width: triggerScroll.columnWidth; columns: 2; columnSpacing: Theme.s4; rowSpacing: Theme.s1
                        Repeater {
                            // A count keeps the toggles alive while the track list refreshes
                            // (waveforms arriving, renames); a new array would rebuild them under the pointer.
                            model: root.triggerChoices.length
                            delegate: Toggle {
                                required property int index
                                readonly property var modelData: root.triggerChoices[index] || ({id: -1, name: ""})
                                objectName: "generatedDuckingTrigger_" + modelData.id
                                Layout.fillWidth: true; Layout.minimumWidth: 0
                                label: modelData.name; checked: root.triggerTracks.indexOf(modelData.id) >= 0; enabled: !root.busy
                                helpTitle: qsTr("Use %1 as a trigger").arg(Theme.shortName(modelData.name, 40))
                                helpText: qsTr("This track's output contributes to detecting where the target should become quieter. Select more than one for several speakers.")
                                helpScope: qsTr("Includes effects, fades, fader, automation and incoming tracks. Mute is respected; Solo is ignored during analysis.")
                                onClicked: root.toggleTrigger(modelData.id, checked)
                            }
                        }
                        Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; visible: root.triggerChoices.length === 0; Layout.columnSpan: 2; text: qsTr("Add another track to use as a trigger."); color: Theme.textMuted }
                    }
                }
                RowLayout {
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Start"); color: Theme.textSecondary }
                    TimeField { id: startInput; objectName: "generatedDuckingStart"; service: root.service; Layout.fillWidth: true; enabled: !root.busy
                        helpTitle: qsTr("Analysis start"); helpText: qsTr("The first sample where generated ducking may change the target's volume automation.") }
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("End"); color: Theme.textSecondary }
                    TimeField { id: endInput; objectName: "generatedDuckingEnd"; service: root.service; Layout.fillWidth: true; enabled: !root.busy
                        helpTitle: qsTr("Analysis end"); helpText: qsTr("The first sample after the generated range. Automation from this point onward is preserved.") }
                    ToolButton { objectName: "generatedDuckingSelectedTime"; text: qsTr("Selected time"); showLabel: true
                        enabled: !root.busy && root.service && !root.service.selectionEmpty; onClicked: root.useSelectedTime()
                        helpText: qsTr("Use the selected time range as the analysis range."); unavailableReason: qsTr("Select a time range first, or finish the current analysis.") }
                    ToolButton { objectName: "generatedDuckingWholeProject"; text: qsTr("Whole project"); showLabel: true
                        enabled: !root.busy; onClicked: root.useWholeProject(); helpText: qsTr("Use the timeline from its start to the end of the longest track.") }
                }
                GridLayout {
                    columns: 3; Layout.fillWidth: true; columnSpacing: Theme.s3; rowSpacing: Theme.s2
                    Setting { id: threshold; objectName: "generatedDuckingThreshold"; label: qsTr("Threshold dBFS"); minimum: -80; maximum: 0
                        help: qsTr("Trigger sound above this level causes ducking. Lower values also detect quieter speech and background sound. Range −80 to 0 dBFS.") }
                    Setting { id: reduction; objectName: "generatedDuckingReduction"; label: qsTr("Reduction dB"); minimum: 0; maximum: 60
                        help: qsTr("How far to lower the target while a trigger is active. 12 lowers it by 12 dB; 0 makes no reduction. Range 0 to 60 dB.") }
                    Setting { id: attack; objectName: "generatedDuckingAttack"; label: qsTr("Attack ms")
                        help: qsTr("How gradually the target reaches its reduced level at a detected passage. Range 0 to 10000 milliseconds.") }
                    Setting { id: hold; objectName: "generatedDuckingHold"; label: qsTr("Hold ms")
                        help: qsTr("Keep the target lowered across short pauses before it begins to return. Range 0 to 10000 milliseconds.") }
                    Setting { id: release; objectName: "generatedDuckingRelease"; label: qsTr("Release ms")
                        help: qsTr("How gradually the target returns after the trigger becomes quiet and Hold ends. Range 0 to 10000 milliseconds.") }
                }
                Toggle {
                    id: replaceExisting; objectName: "generatedDuckingReplace"; Layout.fillWidth: true; enabled: !root.busy
                    label: qsTr("Replace volume automation in this range")
                    helpText: qsTr("Allow generated points to replace the target's existing volume automation within the selected range. Automation outside it is preserved.")
                    helpScope: qsTr("Off refuses a conflicting curve. Stored disabled volume automation must be enabled or cleared first. Reanalyse after changing this choice. One Undo restores the previous curve.")
                }
                Toggle {
                    id: keepUpdated; objectName: "generatedDuckingKeepUpdated"; Layout.fillWidth: true; enabled: !root.busy
                    label: qsTr("Keep up to date when the triggers change")
                    helpText: qsTr("Generate these points again whenever the trigger tracks' sound changes, for example after moving, trimming or silencing a trigger clip.")
                    helpScope: qsTr("The update is part of the edit that caused it, so one Undo restores both. Editing the generated points by hand stops the updates and keeps your edit.")
                }
                Text {
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    objectName: "generatedDuckingFollowState"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    // automationStamp changes with every edit, so the state is re-read.
                    readonly property var state: root.service && root.targetExists && root.service.automationStamp !== undefined
                        ? root.service.generatedDuckingState(root.targetTrack) : ({})
                    visible: !!state.stored; color: state.following ? Theme.textSecondary : Theme.warning
                    text: state.stored ? (state.following ? qsTr("This track's ducking is kept up to date. ") : "") + state.reason : ""
                }
                Text {
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    objectName: "generatedDuckingReplacementNotice"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.warning
                    visible: root.hasResult && root.preview.replacementRequired === true
                    text: qsTr("This range already has volume automation. To replace it, turn on Replace volume automation in this range and analyse again.")
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                    text: qsTr("Detection follows the triggers' output, including effects, fades, faders, automation, incoming tracks and sidechains, with latency alignment. Mute is respected; Solo is ignored. Original media and effect routing are unchanged.")
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 150
                    color: Theme.controlBg; border.color: Theme.border; radius: Theme.rControl
                    Canvas {
                        id: plot; objectName: "generatedDuckingPlot"; anchors.fill: parent; anchors.margins: 12
                        readonly property color ink: Theme.accent
                        readonly property color oldInk: Theme.textMuted
                        readonly property color gridInk: Theme.border
                        readonly property var graph: [root.points, root.previousPoints, root.preview.start, root.preview.end, root.preview.previousHold, root.preview.previousEnabled]
                        onGraphChanged: requestPaint()
                        onInkChanged: requestPaint()
                        onOldInkChanged: requestPaint()
                        onGridInkChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        function sample(frame, proposed) {
                            const points = proposed ? root.points : root.preview.previousEnabled === false ? [] : root.previousPoints
                            return Curve.sample(points, frame, !proposed && root.preview.previousHold, 1)
                        }
                        onPaint: {
                            const ctx = getContext("2d"); ctx.reset()
                            ctx.strokeStyle = gridInk; ctx.lineWidth = 1
                            for (let i = 0; i <= 4; ++i) { ctx.beginPath(); ctx.moveTo(0, height * i / 4); ctx.lineTo(width, height * i / 4); ctx.stroke() }
                            if (!root.hasResult) return
                            const from = root.preview.start || 0, to = Math.max(from + 1, root.preview.end || 1)
                            const previous = root.preview.previousEnabled === false ? [] : root.previousPoints
                            const maximum = root.points.concat(previous).reduce((level, point) => Math.max(level, point.value), 1)
                            function px(frame) { return (frame - from) / (to - from) * width }
                            function py(value) { return height - value / maximum * (height - 4) }
                            for (let lane = 0; lane < 2; ++lane) {
                                const points = lane ? root.points : previous
                                ctx.strokeStyle = lane ? ink : oldInk; ctx.lineWidth = lane ? 2 : 1; ctx.beginPath()
                                Curve.trace(ctx, points, from, to, !lane && root.preview.previousHold, 1, px, py)
                                ctx.stroke()
                                if (lane && points.length <= 200) for (const point of points) if (point.frame >= from && point.frame <= to) {
                                    ctx.beginPath(); ctx.arc(px(point.frame), py(point.value), 2, 0, 2 * Math.PI); ctx.fillStyle = ink; ctx.fill()
                                }
                            }
                        }
                    }
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; anchors.centerIn: parent; visible: !root.hasResult
                        text: qsTr("Analyse to preview volume points"); color: Theme.textMuted }
                }
                RowLayout {
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Existing volume"); color: Theme.textMuted }
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: qsTr("Proposed volume"); color: Theme.accent }
                    Item { Layout.fillWidth: true }
                    Text { font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; text: root.hasResult ? qsTr("%1 points · %2 detected passages").arg(root.preview.pointCount || 0).arg(root.preview.activityCount || 0) : ""; color: Theme.textSecondary }
                }
                Text {
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    objectName: "generatedDuckingPreviewRange"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary
                    visible: root.hasResult && root.preview.start !== undefined
                    text: root.hasResult && root.shell ? qsTr("Preview: %1 — %2. Volume is relative to the target fader.").arg(root.shell.formatFrames(root.preview.start)).arg(root.shell.formatFrames(root.preview.end)) : ""
                }
            }
        }
        Text {
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
            objectName: "generatedDuckingMessage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            color: root.stale || root.settingsChanged ? Theme.warning : Theme.textPrimary
            text: root.busy ? qsTr("Analysing trigger output… %1%").arg(Math.round(Math.max(0, Math.min(1, root.service.duckingProgress)) * 100))
                : root.settingsChanged ? qsTr("Settings changed. Analyse again before applying points.")
                : root.stale ? qsTr("The session changed after analysis. Analyse again before applying points.") : root.message
            visible: text !== ""
        }
        RowLayout {
            ToolButton {
                objectName: "generatedDuckingEditPoints"; text: qsTr("Edit points…"); showLabel: true
                enabled: root.appliedTrack >= 0 && root.tracks.some(t => t.id === root.appliedTrack)
                helpText: qsTr("Open the target's volume automation to adjust the generated points.")
                unavailableReason: qsTr("Apply generated points first.")
                onClicked: { const track = root.appliedTrack; root.close(); root.shell.editGeneratedDucking(track) }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                objectName: "generatedDuckingCancel"; text: qsTr("Cancel analysis"); showLabel: true; visible: root.busy
                helpText: qsTr("Stop analysis and discard its proposal. No automation changes.")
                onClicked: root.cancelAnalysis()
            }
            ToolButton {
                objectName: "generatedDuckingAnalyse"; text: qsTr("Analyse"); showLabel: true
                enabled: root.valid && !root.busy && root.service && !root.service.recording && !root.service.loading
                helpText: qsTr("Measure the trigger tracks and prepare a volume curve for the chosen target and range.")
                helpScope: qsTr("Analysis makes no project changes. Inspect the preview, then use Apply points.")
                unavailableReason: root.busy ? qsTr("Wait for analysis to finish, or cancel it.")
                    : !root.targetExists ? qsTr("Choose the track to lower.") : !root.triggersExist ? qsTr("Choose at least one different trigger track.")
                    : !root.valid ? qsTr("Enter an increasing time range and settings within their limits.") : qsTr("Finish loading or recording first.")
                onClicked: root.analyse()
            }
            ToolButton {
                objectName: "generatedDuckingApply"; text: qsTr("Apply points"); showLabel: true
                enabled: root.canApply
                helpText: qsTr("Install this preview as the target's volume automation within the chosen range.")
                helpScope: qsTr("One Undo restores the previous curve. Outside automation, target fader and original media are preserved.")
                unavailableReason: root.busy ? qsTr("Wait for analysis to finish.") : root.settingsChanged || root.stale ? qsTr("Analyse again after changes.")
                    : root.preview.replacementRequired ? qsTr("Choose whether to replace existing automation, then analyse again.") : qsTr("Analyse and review a valid proposal first.")
                onClicked: {
                    if (!root.service.applyGeneratedDucking(root.ownedToken, keepUpdated.checked)) { root.message = root.service.lastError; return }
                    root.appliedTrack = root.targetTrack; root.ownedToken = ""; root.hasResult = false; root.analysedSignature = ""
                    root.message = qsTr("Ducking edit accepted. Undo restores the previous volume curve. Use Edit points to adjust it.")
                }
            }
        }
    }
}
