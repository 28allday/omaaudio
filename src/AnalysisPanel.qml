pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import "ui"

// Level statistics, a frequency spectrum and stereo phase for the selection
// (or the focused track), measured from what its clips play before track effects.
// Clip loudness matching sits here too: it measures the same way.
C.Popup {
    id: root
    objectName: "analysisPanel"
    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int view: 0           // 0 statistics, 1 frequency, 2 phase
    property var result: ({})
    property string measuredStamp: ""
    property string matchMessage: ""
    // Live: Frequency and Phase follow what is playing, about ten times a second.
    property bool live: false
    readonly property bool stale: !live && !!result.ok && (!service || measuredStamp !== service.automationStamp)
    readonly property var kinds: ["statistics", "spectrum", "phase"]
    readonly property var fftSizes: [1024, 2048, 4096, 8192, 16384, 32768]
    readonly property var windows: ["hann", "blackman", "rectangular"]

    function db(v, decimals) {
        return v === undefined || v <= -199 ? "−∞" : Number(v).toFixed(decimals === undefined ? 1 : decimals).replace("-", "−")
    }
    function openView(index) { view = index; open() }
    function refreshLive() {
        if (!service) return
        const reading = service.liveAnalysis(Math.min(8192, fftSizes[fftSize.currentIndex]))
        if (reading.ok && !reading.fresh && result.live) return
        result = reading
        spectrum.requestPaint()
    }
    function measure() {
        if (!service) return
        if (live && view !== 0) { refreshLive(); return }
        result = service.analyseAudio(kinds[view], fftSizes[fftSize.currentIndex], windows[windowShape.currentIndex])
        measuredStamp = service.automationStamp
        spectrum.requestPaint()
    }
    function matchLoudness() {
        if (!service || !matchTarget.acceptableInput) return
        const ok = service.matchClipLoudness(Number(matchTarget.text), service.automationStamp)
        matchMessage = ok ? qsTr("Clip gains set. Undo restores them.") : service.lastError
        if (ok) measure()
    }

    parent: C.Overlay.overlay; anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 32 : 760)
    height: Math.min(620, parent ? parent.height - 40 : 620)
    padding: Theme.s4; modal: true; focus: true
    closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
    onAboutToShow: { matchMessage = ""; measure() }
    onViewChanged: if (opened) measure()
    onLiveChanged: if (opened) measure()
    Timer {
        interval: 100; repeat: true
        running: root.opened && root.live && root.view !== 0 && !!root.service && root.service.playing
        onTriggered: root.refreshLive()
    }
    onClosed: if (shell) shell.restoreFocus()
    background: Rectangle { color: Theme.panelBg; radius: Theme.rPanel; border.color: Theme.border }

    component Reading: ColumnLayout {
        property string label: ""
        property string value: ""
        spacing: 2
        Text { text: parent.label; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        Text { text: parent.value; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.features: {"tnum": 1} }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Text { text: qsTr("Audio analysis"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; Layout.fillWidth: true }
            SegmentedControl {
                objectName: "analysisView"
                labels: [qsTr("Statistics"), qsTr("Frequency"), qsTr("Phase")]
                names: labels; currentIndex: root.view
                onActivated: index => root.view = index
                helpTitle: qsTr("Analysis")
                helpText: qsTr("Choose what to measure.")
            }
            Toggle {
                objectName: "analysisLive"; label: qsTr("Live"); visible: root.view !== 0
                checked: root.live; onToggled: root.live = checked
                helpText: qsTr("Follow what is playing: the whole mix before the monitor level, updated about ten times a second.")
                helpScope: qsTr("Turn off to measure the selection or the focused track instead.")
            }
            ToolButton {
                objectName: "analysisMeasure"; visible: !root.live || root.view === 0
                text: root.stale ? qsTr("Measure again") : qsTr("Measure"); showLabel: true
                onClicked: root.measure()
                helpText: qsTr("Measure the selection, or the focused track's whole length when nothing is selected.")
                helpScope: qsTr("Reads what the clips play, including clip effects, gain, fades and envelopes, before track effects and the master bus.")
            }
            ToolButton {
                text: qsTr("Close"); iconName: "x"; onClicked: root.close()
                helpText: qsTr("Close this panel. Measuring changes nothing.")
            }
        }
        Text {
            objectName: "analysisScope"
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.result.ok && root.result.live
                ? (root.service && root.service.playing ? qsTr("Live output · the mix as it plays, before the monitor level")
                                                         : qsTr("Live output · paused; play to update"))
                : root.result.ok
                ? qsTr("%1 · %2 to %3%4").arg(Theme.shortName(root.result.trackName, 40))
                    .arg(root.shell ? root.shell.formatFrames(root.result.start) : root.result.start)
                    .arg(root.shell ? root.shell.formatFrames(root.result.end) : root.result.end)
                    .arg(root.stale ? qsTr(" · the project changed since this measurement") : "")
                : (root.result.reason || "")
        }

        // ── statistics ──
        GridLayout {
            objectName: "analysisStatistics"
            visible: root.view === 0 && !!root.result.ok
            columns: 4; columnSpacing: Theme.s4; rowSpacing: Theme.s3; Layout.fillWidth: true
            Reading { label: qsTr("Peak"); value: root.db(root.result.peakDb, 2) + " dBFS" }
            Reading { label: qsTr("True peak"); value: root.db(root.result.truePeakDb, 2) + " dBTP" }
            Reading { label: qsTr("RMS"); value: root.db(root.result.rmsDb, 2) + " dBFS" }
            Reading { label: qsTr("Integrated loudness"); value: root.db(root.result.integratedLufs) + " LUFS" }
            Reading { label: qsTr("Loudest 50 ms"); value: root.db(root.result.loudestRmsDb) + " dBFS" }
            Reading { label: qsTr("Quietest 50 ms"); value: root.db(root.result.quietestRmsDb) + " dBFS" }
            Reading { label: qsTr("Dynamic range"); value: Number(root.result.dynamicRangeDb || 0).toFixed(1) + " dB" }
            Item { Layout.fillWidth: true }
        }
        Repeater {
            model: root.view === 0 && root.result.ok ? root.result.perChannel : []
            delegate: Text {
                required property var modelData
                required property int index
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: qsTr("Channel %1: peak %2 dBFS, RMS %3 dBFS, DC offset %4%, %5 clipped samples in %6 runs")
                    .arg(index + 1).arg(root.db(modelData.peakDb, 2)).arg(root.db(modelData.rmsDb, 2))
                    .arg(Number(modelData.dcOffsetPercent).toFixed(3)).arg(modelData.clipped).arg(modelData.clippedRuns)
            }
        }

        // ── frequency ──
        RowLayout {
            visible: root.view === 1
            ComboField {
                id: fftSize; objectName: "analysisFftSize"; caption: qsTr("Points"); currentIndex: 2
                Layout.preferredWidth: 150; model: root.fftSizes.map(n => String(n))
                onActivated: root.measure()
                helpText: qsTr("More points separate close frequencies; fewer follow short events better.")
            }
            ComboField {
                id: windowShape; objectName: "analysisWindow"; caption: qsTr("Window"); currentIndex: 0
                Layout.preferredWidth: 190
                model: [qsTr("Hann"), qsTr("Blackman"), qsTr("Rectangular")]
                onActivated: root.measure()
                helpText: qsTr("Blackman shows quiet tones beside loud ones; Rectangular is for exact cycles only.")
            }
            Item { Layout.fillWidth: true }
        }
        Canvas {
            id: spectrum; objectName: "analysisSpectrum"
            visible: root.view === 1 && !!root.result.ok
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 200
            onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
            onPaint: {
                const g = getContext("2d"); g.reset()
                g.fillStyle = Theme.laneBg; g.fillRect(0, 0, width, height)
                const curves = root.result.curves || []
                if (!curves.length) return
                const top = 0, bottom = -120, low = Math.log10(20), high = Math.log10(curves[0][curves[0].length - 1].frequency)
                const x = f => (Math.log10(f) - low) / (high - low) * width
                const y = d => (top - Math.max(bottom, Math.min(top, d))) / (top - bottom) * height
                g.strokeStyle = Theme.border; g.fillStyle = Theme.rulerText; g.font = "10px sans-serif"; g.lineWidth = 1
                for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) {
                    if (Math.log10(f) > high) continue
                    g.beginPath(); g.moveTo(x(f), 0); g.lineTo(x(f), height); g.stroke()
                    g.fillText(f >= 1000 ? (f / 1000) + "k" : String(f), x(f) + 2, height - 4)
                }
                for (let d = -20; d > bottom; d -= 20) {
                    g.beginPath(); g.moveTo(0, y(d)); g.lineTo(width, y(d)); g.stroke()
                    g.fillText(String(d).replace("-", "−") + " dB", 2, y(d) - 2)
                }
                const colours = [Theme.audioPrimary, Theme.audioSecondary, Theme.music, Theme.effects]
                curves.forEach((curve, c) => {
                    g.strokeStyle = colours[c % colours.length]; g.lineWidth = 1.5; g.beginPath()
                    curve.forEach((p, i) => i ? g.lineTo(x(p.frequency), y(p.db)) : g.moveTo(x(p.frequency), y(p.db)))
                    g.stroke()
                })
            }
        }

        // ── phase ──
        ColumnLayout {
            objectName: "analysisPhase"
            visible: root.view === 2 && !!root.result.ok
            Layout.fillWidth: true; spacing: Theme.s3
            RowLayout {
                spacing: Theme.s4
                Reading { label: qsTr("Correlation"); value: Number(root.result.correlation || 0).toFixed(2).replace("-", "−") }
                Reading { label: qsTr("Balance (left − right)"); value: Number(root.result.balanceDb || 0).toFixed(1).replace("-", "−") + " dB" }
                Reading { label: qsTr("Width (side ÷ mid)"); value: root.db(root.result.sideToMidDb) + " dB" }
            }
            Rectangle {
                Layout.fillWidth: true; height: 14; radius: 2; color: Theme.meterBg
                Rectangle {
                    readonly property real c: Math.max(-1, Math.min(1, root.result.correlation || 0))
                    x: c >= 0 ? parent.width / 2 : parent.width / 2 * (1 + c); width: Math.abs(c) * parent.width / 2
                    height: parent.height; radius: 2; color: c < 0 ? Theme.danger : Theme.success
                }
                Rectangle { x: parent.width / 2; width: 1; height: parent.height; color: Theme.borderStrong }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: qsTr("+1 is mono; around 0 is wide stereo; below 0 the channels cancel when summed to mono.")
            }
        }
        Item { visible: root.view !== 1; Layout.fillHeight: true }

        // ── clip loudness ──
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
        RowLayout {
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: qsTr("Match the selected clips (or the clip at the playhead) to one loudness by setting each clip's gain.")
            }
            TextEntry {
                id: matchTarget; objectName: "analysisMatchTarget"; Layout.preferredWidth: 70; text: "-23"; selectByMouse: true
                validator: DoubleValidator { bottom: -60; top: -5; decimals: 1; notation: DoubleValidator.StandardNotation; locale: "C" }
                helpTitle: qsTr("Clip loudness target")
                helpText: qsTr("Integrated loudness in LUFS each clip is set to, measured as the clip plays at unity gain.")
                helpScope: qsTr("Range: −60 to −5 LUFS. Changes only clip gain; one Undo step.")
            }
            Text { text: "LUFS"; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
            ToolButton {
                objectName: "analysisMatch"; text: qsTr("Set clip loudness"); showLabel: true
                enabled: matchTarget.acceptableInput; onClicked: root.matchLoudness()
                helpText: qsTr("Set each selected clip's gain so it reaches the target loudness.")
                unavailableReason: qsTr("Type a target from −60 to −5 LUFS.")
            }
        }
        Text {
            objectName: "analysisMatchMessage"; visible: text !== ""
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            text: root.matchMessage
        }
    }
}
