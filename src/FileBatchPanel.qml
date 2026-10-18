pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtQuick.Dialogs
import "ui"

C.Popup {
    id: root
    objectName: "fileBatchPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property var inputs: []
    property int revision: 0
    property string message: ""
    property real targetLufs: -16
    property real ceilingDbTp: -1
    property real peakTargetDb: -0.3
    property alias outputDirectory: directory.text
    property alias filenameSuffix: suffix.text
    readonly property var formats: service ? service.exportFormats.filter(f => f.available) : []
    readonly property var currentFormat: formats.length ? formats[Math.max(0, format.currentIndex)] : null
    readonly property var rates: [0, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 192000]
    readonly property var bitrates: [0, 64, 96, 128, 160, 192, 256, 320]
    readonly property var depths: currentFormat && currentFormat.bitDepths.length ? currentFormat.bitDepths : [24]
    readonly property var settings: {
        root.revision
        const chosen = { directory: directory.text, suffix: suffix.text,
            format: currentFormat ? currentFormat.id : "wav", bitDepth: depths[Math.max(0, depth.currentIndex)],
            sampleRate: rates[Math.max(0, rate.currentIndex)], channels: channels.currentIndex,
            dither: dither.checked, bitrateKbps: bitrates[Math.max(0, bitrate.currentIndex)], normalise: ["none", "peak", "loudness"][level.currentIndex],
            targetLufs: root.targetLufs, ceilingDbTp: root.ceilingDbTp, peakTargetDb: root.peakTargetDb,
            includeMetadata: metadata.checked, chain: ["none", "track", "master", "recipe"][effects.currentIndex],
            track: service ? service.focusedTrack : -1, stamp: service ? service.automationStamp : "" }
        // Which recipe goes only with the Recipe choice.
        if (effects.currentIndex === 3) chosen.recipe = root.chosenRecipe ? root.chosenRecipe.id : ""
        return chosen
    }
    // Effects › Recipe: one of the Recipes tab's recipes, run on each file.
    readonly property var recipes: service ? service.recipes : []
    property string recipeId: ""
    readonly property int recipeIndex: {
        const i = recipes.findIndex(r => r.id === recipeId)
        return i >= 0 ? i : (recipes.length ? 0 : -1)
    }
    readonly property var chosenRecipe: recipeIndex >= 0 ? recipes[recipeIndex] : null
    // From the Recipes tab: Recipe chosen as the effects, with this recipe
    // ("" keeps the one already chosen).
    function chooseRecipe(id) {
        effects.currentIndex = 3
        if (id) recipeId = id
        revision++
    }
    readonly property var review: {
        root.revision
        return service && inputs.length ? service.previewFileBatch(inputs, settings) : ({ok: false, error: "", files: []})
    }
    readonly property var jobs: service ? service.fileBatchJobs : []
    readonly property var queueStatus: service ? service.fileBatchStatus : ({available: false, error: ""})
    readonly property bool queueAvailable: queueStatus.available === true
    readonly property int pausedCount: jobs.filter(j => j.state === "paused").length
    ListModel { id: resultModel; dynamicRoles: true }
    // Keep delegate identity while progress and other jobs change. Replacing a
    // QVariantList model would destroy a pressed Cancel or Retry control.
    function syncResults() {
        if (!resultModel) return
        for (let i = 0; i < jobs.length; ++i) {
            while (resultModel.count > i && resultModel.get(i).identity !== jobs[i].id) resultModel.remove(i)
            if (resultModel.count === i) resultModel.append({identity: jobs[i].id, entry: jobs[i]})
            else resultModel.setProperty(i, "entry", jobs[i])
        }
        if (resultModel.count > jobs.length) resultModel.remove(jobs.length, resultModel.count - jobs.length)
    }
    onJobsChanged: syncResults()
    Component.onCompleted: syncResults()
    function addFiles(files) {
        let next = inputs.slice()
        for (let i = 0; i < files.length; ++i) {
            const path = String(files[i])
            if (next.indexOf(path) < 0) next.push(path)
        }
        inputs = next; message = ""; revision++
    }
    function removeInput(index) { let next = inputs.slice(); next.splice(index, 1); inputs = next }
    function pending(state) { return state === "preparing" || state === "queued" || state === "reading" || state === "processing" }
    function unfinished(state) { return pending(state) || state === "paused" }
    function jobSummary(job) {
        const s = job.settings
        const layout = [qsTr("original channels"), qsTr("mono"), qsTr("stereo")][s.channels]
        const rate = s.sampleRate ? qsTr("%1 kHz").arg(s.sampleRate / 1000) : qsTr("original rate")
        const level = s.normalise === "peak" ? qsTr("peak %1 dBTP").arg(s.peakTargetDb)
            : s.normalise === "loudness" ? qsTr("%1 LUFS, ceiling %2 dBTP").arg(s.targetLufs).arg(s.ceilingDbTp) : qsTr("level unchanged")
        const lossless = ["wav", "bwf", "rf64", "aiff", "flac"].indexOf(s.format) >= 0
        const format = s.format.toUpperCase() + (lossless ? qsTr(" %1-bit").arg(s.bitDepth)
            : s.format === "aac" ? (s.bitrateKbps ? qsTr(" %1 kb/s").arg(s.bitrateKbps) : qsTr(" automatic bitrate")) : "")
        let summary = qsTr("%1 · %2 · %3 · %4").arg(format).arg(rate).arg(layout).arg(level)
        summary += " · " + (s.includeMetadata ? qsTr("keep metadata") : qsTr("omit metadata"))
        const report = job.report
        if (report && report.frames) {
            const measured = report.delivered
            const loudness = measured.integrated > -199 ? qsTr("%1 LUFS").arg(measured.integrated.toFixed(1)) : qsTr("below loudness gate")
            summary += qsTr("\nWritten: %1 kHz · %2 channels · %3 frames · %4 · %5 dBTP")
                .arg(report.sampleRate / 1000).arg(report.channels).arg(report.frames).arg(loudness).arg(measured.truePeak.toFixed(1))
        }
        return summary
    }
    function stateLabel(state) {
        switch (state) {
        case "preparing": return qsTr("Preparing effects")
        case "queued": return qsTr("Queued")
        case "paused": return qsTr("Paused")
        case "reading": return qsTr("Reading")
        case "processing": return qsTr("Processing")
        case "done": return qsTr("Done")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        }
        return state
    }
    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(1000, parent ? parent.width - 32 : 1000)
    height: Math.min(800, parent ? parent.height - 32 : 800)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onOpened: { revision++; message = "" }
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.borderStrong }
    Connections {
        target: root.service
        function onTimelineChanged() { root.revision++ }
        function onTracksChanged() { root.revision++ }
        function onMasterChanged() { root.revision++ }
        function onFileBatchChanged() { if (!root.service.fileBatchBusy) root.revision++ }
    }
    FileDialog {
        id: inputDialog; objectName: "batchInputDialog"
        title: qsTr("Choose files to process"); fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Audio (*.wav *.wave *.aiff *.aif *.flac *.ogg *.opus *.mp3 *.m4a *.aac *.rf64 *.w64)"), qsTr("All files (*)")]
        onAccepted: root.addFiles(selectedFiles)
    }
    FolderDialog {
        id: folderDialog; title: qsTr("Choose the output folder")
        onAccepted: directory.text = Theme.localPath(selectedFolder)
    }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        RowLayout {
            Text { text: qsTr("Process Many Files"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; color: Theme.textPrimary; Layout.fillWidth: true }
            ToolButton { text: qsTr("Close"); showLabel: true; helpText: qsTr("Close this panel. Queued files keep processing while OmaAudio stays open."); onClicked: root.close() }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("Process full files independently. Originals and existing outputs are never replaced. Unfinished jobs return paused after reopening; resume them when you are ready.")
        }
        Text {
            objectName: "batchQueueWarning"; Layout.fillWidth: true; wrapMode: Text.WordWrap
            textFormat: Text.PlainText; color: Theme.warning; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            visible: text !== ""
            text: root.queueStatus.error || (!root.queueAvailable ? qsTr("The saved batch queue is unavailable. Processing is disabled.") : "")
        }
        RowLayout {
            visible: !root.queueAvailable
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: qsTr("If another OmaAudio is open, close it and reopen this panel. If the saved queue is damaged, start a new empty queue: the damaged file is kept beside it and finished outputs are not touched.")
            }
            ToolButton {
                objectName: "batchResetQueue"; text: qsTr("Start a new queue"); showLabel: true
                helpText: qsTr("Set the damaged saved queue aside and start an empty one. Files already written are kept.")
                helpScope: qsTr("Paused and unfinished rows in the damaged queue are not resumed; queue those files again.")
                onClicked: if (!root.service.resetFileBatchQueue()) root.message = root.service.lastError
            }
        }
        RowLayout {
            ToolButton { objectName: "batchAddFiles"; text: qsTr("Add files…"); showLabel: true; iconName: "plus"; helpText: qsTr("Choose several audio files. Each receives the same settings and its own output and result."); onClicked: inputDialog.open() }
            ToolButton { text: qsTr("Clear chosen files"); showLabel: true; enabled: root.inputs.length > 0; helpText: qsTr("Empty the input list. Already queued jobs and output files stay intact."); onClicked: root.inputs = [] }
            Text { text: qsTr("%1 chosen").arg(root.inputs.length); color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
            Item { Layout.fillWidth: true }
            ToolButton { text: qsTr("Review paths"); showLabel: true; helpText: qsTr("Check input and output paths again, including files created since the last review."); onClicked: root.revision++ }
        }
        RowLayout {
            TextEntry { id: directory; objectName: "batchDirectory"; Layout.fillWidth: true; placeholderText: qsTr("Output folder"); helpTitle: qsTr("Output folder"); helpText: qsTr("An existing local folder for the processed files. You can type its path or choose it with Browse.") }
            ToolButton { text: qsTr("Browse…"); showLabel: true; helpText: qsTr("Choose an existing folder for the outputs."); onClicked: folderDialog.open() }
            TextEntry { id: suffix; objectName: "batchSuffix"; text: "-processed"; Layout.preferredWidth: 155; helpTitle: qsTr("Filename suffix"); helpText: qsTr("Append this to each input name before the new extension, such as -processed. Duplicate or existing names fail without replacing files.") }
        }
        RowLayout {
            ComboField { id: format; objectName: "batchFormat"; Layout.fillWidth: true; caption: qsTr("Format"); model: root.formats.map(f => f.name); helpText: qsTr("The container written for every file. Only available encoders are listed."); onActivated: depth.currentIndex = Math.max(0, root.depths.indexOf(24)) }
            ComboField { id: depth; objectName: "batchDepth"; Layout.preferredWidth: 125; caption: qsTr("Depth"); currentIndex: Math.max(0, root.depths.indexOf(24)); model: root.depths.map(d => d === 32 ? qsTr("32 float") : qsTr("%1-bit").arg(d)); visible: !root.currentFormat || !root.currentFormat.lossy; helpText: qsTr("Sample depth for lossless outputs. 32-bit float is available only in compatible containers.") }
            ComboField { id: bitrate; objectName: "batchBitrate"; Layout.preferredWidth: 170; caption: qsTr("Bitrate"); model: root.bitrates.map(b => b ? qsTr("%1 kb/s").arg(b) : qsTr("Automatic")); visible: root.currentFormat && root.currentFormat.id === "aac"; helpText: qsTr("AAC encoding bitrate for each output. Automatic chooses a suitable value for its channel count.") }
            ComboField { id: rate; objectName: "batchRate"; Layout.fillWidth: true; caption: qsTr("Rate"); model: root.rates.map(r => r === 0 ? qsTr("Original") : qsTr("%1 kHz").arg(r / 1000)); helpText: qsTr("Preserve each input rate or convert before applying the effects. Unsupported format and rate combinations fail that file.") }
            ComboField { id: channels; objectName: "batchChannels"; Layout.fillWidth: true; caption: qsTr("Channels"); model: [qsTr("Original"), qsTr("Mono"), qsTr("Stereo")]; helpText: qsTr("Preserve the layout, average to mono, or duplicate mono into stereo. Stereo also accepts stereo inputs; larger layouts need Original or Mono.") }
        }
        RowLayout {
            ComboField { id: effects; objectName: "batchEffects"; Layout.fillWidth: true; caption: qsTr("Effects"); model: [qsTr("None"), qsTr("Focused track"), qsTr("Master inserts"), qsTr("Recipe")]; helpText: qsTr("Copy the current effect settings, or a recipe's steps, when you queue the files. Bypassed effects are omitted. Later session edits cannot change accepted jobs."); helpScope: qsTr("Clip edits, faders, automation, trim and routing are excluded. A missing or incompatible effect fails the file.") }
            ComboField {
                objectName: "batchRecipe"; Layout.fillWidth: true; visible: effects.currentIndex === 3
                caption: qsTr("Recipe")
                model: root.recipes.map(r => (r.steps || []).length === 1 ? qsTr("%1  (1 step)").arg(Theme.shortName(r.name, 40)) : qsTr("%1  (%2 steps)").arg(Theme.shortName(r.name, 40)).arg((r.steps || []).length))
                currentIndex: root.recipeIndex
                helpText: qsTr("The recipe whose steps run on every file, in order, before the level and format settings.")
                helpScope: qsTr("Make and change recipes in the Recipes tab of the right dock.")
                onActivated: function (i) { root.recipeId = root.recipes[i].id; root.revision++ }
            }
            ComboField { id: level; objectName: "batchLevel"; Layout.fillWidth: true; caption: qsTr("Level"); model: [qsTr("Unchanged"), qsTr("Peak"), qsTr("Loudness")]; helpText: qsTr("Keep the processed level, normalize its true peak, or target integrated loudness with a true-peak ceiling.") }
            ValueField { objectName: "batchPeak"; visible: level.currentIndex === 1; Layout.preferredWidth: 130; label: qsTr("Peak"); value: root.peakTargetDb; from: -90; to: 0; suffix: "dBTP"; helpText: qsTr("Target true-peak level after the effects, in dBTP."); onEdited: value => root.peakTargetDb = value }
            ValueField { objectName: "batchLufs"; visible: level.currentIndex === 2; Layout.preferredWidth: 115; label: qsTr("Loudness"); value: root.targetLufs; from: -70; to: 0; suffix: "LUFS"; helpText: qsTr("Target integrated loudness after effects. The ceiling can reduce the achieved loudness."); onEdited: value => root.targetLufs = value }
            ValueField { objectName: "batchCeiling"; visible: level.currentIndex === 2; Layout.preferredWidth: 115; label: qsTr("Ceiling"); value: root.ceilingDbTp; from: -24; to: 0; suffix: "dBTP"; helpText: qsTr("Maximum true peak for loudness normalization. This backs off gain; it does not add a limiter."); onEdited: value => root.ceilingDbTp = value }
        }
        RowLayout {
            Toggle { id: dither; objectName: "batchDither"; label: qsTr("Dither"); checked: true; helpText: qsTr("Add TPDF dither when reducing to integer PCM. It is not applied to float or lossy output.") }
            Toggle { id: metadata; objectName: "batchMetadata"; label: qsTr("Keep file metadata"); checked: true; helpText: qsTr("Keep each input's broadcast metadata and markers in WAV-family output. Timing is rescaled and loudness is measured again; metadata never comes from the open project.") }
            Item { Layout.fillWidth: true }
            Text { text: root.review.effects || ""; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.maximumWidth: 320 }
        }
        ScrollPane {
            id: fileListScroll
            Layout.fillWidth: true; Layout.preferredHeight: 116; clip: true; contentWidth: availableWidth
            Column {
                x: fileListScroll.reachLeft; width: fileListScroll.columnWidth; spacing: Theme.s1
                Repeater {
                    model: root.review.files || []
                    delegate: RowLayout {
                        id: chosenFile
                        required property var modelData
                        required property int index
                        width: parent.width
                        Text { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: chosenFile.modelData.valid ? Theme.textSecondary : Theme.warning;
                            text: chosenFile.modelData.input + "\n→ " + chosenFile.modelData.output + (chosenFile.modelData.message ? "\n" + chosenFile.modelData.message : "") }
                        ToolButton { iconName: "x"; helpTitle: qsTr("Remove chosen file"); helpText: qsTr("Remove this file from the input list. Queued jobs are unaffected."); onClicked: root.removeInput(chosenFile.index) }
                    }
                }
            }
        }
        RowLayout {
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary; text: root.review.error || "" }
            ToolButton {
                objectName: "batchQueue"; text: qsTr("Process files"); showLabel: true
                enabled: root.service && root.queueAvailable && root.review.ok && root.review.files.some(f => f.valid)
                helpText: qsTr("Queue these files with a fixed copy of the chosen settings. Processing changes no project or Undo history.")
                unavailableReason: root.queueStatus.error || root.review.error || qsTr("Add files and choose an output folder with at least one available output name.")
                onClicked: {
                    const id = root.service.queueFileBatch(root.inputs, root.settings)
                    root.message = id > 0 ? qsTr("Batch %1 queued. Each file has its own result below.").arg(id) : root.service.lastError
                    root.revision++
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: Theme.hairline; color: Theme.border }
        RowLayout {
            Text { text: qsTr("Results · %1 files").arg(root.jobs.length); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; Layout.fillWidth: true }
            ToolButton {
                objectName: "batchResumeAll"; text: qsTr("Resume paused"); showLabel: true
                visible: root.pausedCount > 0; enabled: root.queueAvailable && root.pausedCount > 0
                helpText: qsTr("Resume all paused files using their saved paths and captured settings. The current project and the settings above do not change them.")
                helpScope: qsTr("Every paused job is checked first. If any input or output cannot be used, none of the paused jobs resumes. Existing outputs are never replaced.")
                unavailableReason: root.queueStatus.error || qsTr("There are no paused files to resume.")
                onClicked: root.message = root.service.resumeFileBatch() ? qsTr("Paused files accepted for processing.") : root.service.lastError
            }
            ToolButton { objectName: "batchCancelAll"; text: qsTr("Cancel unfinished"); showLabel: true; enabled: root.service && root.jobs.some(j => root.unfinished(j.state)); helpText: qsTr("Cancel running, queued and paused files. Completed outputs remain; partial files are removed."); onClicked: root.service.cancelFileBatch() }
            ToolButton { objectName: "batchClearFinished"; text: qsTr("Clear finished rows"); showLabel: true; enabled: root.queueAvailable && root.jobs.some(j => !root.unfinished(j.state)); helpText: qsTr("Remove completed, failed and cancelled rows from this list. Paused jobs and output files are kept."); unavailableReason: root.queueStatus.error || qsTr("There are no finished rows to clear."); onClicked: root.service.clearFinishedBatchFiles() }
        }
        Text {
            objectName: "batchPausedNotice"; Layout.fillWidth: true; visible: root.pausedCount > 0
            wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: qsTr("%1 saved files are paused. Review their paths and settings below, then Resume one file or Resume paused to process them all.").arg(root.pausedCount)
        }
        Text {
            objectName: "batchActionMessage"; Layout.fillWidth: true; visible: text !== ""
            text: root.message; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        }
        ListView {
            id: resultList
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 100; clip: true
            spacing: Theme.s2; model: resultModel
            C.ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: resultRow
                required property var entry
                readonly property var modelData: entry
                objectName: "batchResultRow"
                width: resultList.width; height: result.implicitHeight + Theme.s2 * 2
                color: Theme.controlBg; radius: Theme.rControl
                ColumnLayout {
                    id: result; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: Theme.s2
                    RowLayout {
                        Text { text: root.stateLabel(resultRow.modelData.state) + (root.pending(resultRow.modelData.state) ? " · " + Math.round(resultRow.modelData.progress * 100) + "%" : ""); color: resultRow.modelData.state === "failed" ? Theme.warning : Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
                        Text { text: resultRow.modelData.effects; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                        ToolButton { objectName: "batchCancelFile"; visible: root.unfinished(resultRow.modelData.state); text: qsTr("Cancel"); showLabel: true; helpText: qsTr("Cancel this file only. Other files keep their current state."); onClicked: root.service.cancelBatchFile(resultRow.modelData.id) }
                        ToolButton {
                            objectName: "batchResumeFile"; visible: resultRow.modelData.state === "paused"; enabled: root.queueAvailable
                            text: qsTr("Resume"); showLabel: true
                            helpText: qsTr("Resume this saved file using its original paths and captured settings. The current project and the settings above do not change it. Existing outputs are never replaced.")
                            unavailableReason: root.queueStatus.error
                            onClicked: root.message = root.service.resumeBatchFile(resultRow.modelData.id) ? qsTr("File accepted for processing.") : root.service.lastError
                        }
                        ToolButton { objectName: "batchRetryFile"; visible: !root.unfinished(resultRow.modelData.state) && resultRow.modelData.state !== "done"; enabled: root.queueAvailable; text: qsTr("Retry"); showLabel: true; helpText: qsTr("Try this input and output again using its originally captured settings. Restore missing files first; existing outputs are never replaced."); unavailableReason: root.queueStatus.error; onClicked: if (!root.service.retryBatchFile(resultRow.modelData.id)) root.message = root.service.lastError }
                    }
                    Text { Layout.fillWidth: true; text: resultRow.modelData.input + "\n→ " + resultRow.modelData.output; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                    Text { Layout.fillWidth: true; text: root.jobSummary(resultRow.modelData); textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                    Text { Layout.fillWidth: true; text: resultRow.modelData.message + (resultRow.modelData.warning ? "\n" + resultRow.modelData.warning : ""); textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary }
                }
            }
        }
    }
}
