pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as C
import QtCore
import "ui"

// The Sound tab: the focused track's sound in plain terms. What kind of sound it
// is (the track role), a starting point, and for a voice a few fixes, each a
// switch, a line of plain words and one slider. Underneath they are real
// effects on the track (Backend::soundSettings / applySound), in a fixed order,
// so everything here is also in Track Effects with its full controls, and a
// change made there shows here. Every change is one Undo step.
Item {
    id: root
    objectName: "soundPanel"

    property var shell: null
    readonly property var service: shell ? shell.service : null
    property int trackId: -1

    // The descriptor, read again whenever the project, the track or the
    // catalogue changes.
    property var settings: ({})
    property string message: ""
    function reload() {
        settings = service && trackId >= 0 ? service.soundSettings(trackId) : ({})
    }
    onTrackIdChanged: { message = ""; matched = ""; reload(); restoreChoices() }

    // What this panel chose for the track that is not an effect (the loudness
    // target, the room echo amount, the duck settings) is saved with the
    // project, so the panel opens as it was left.
    function choice(key, fallback) {
        const saved = service && trackId >= 0 ? service.soundChoices(trackId) : ({})
        return saved[key] !== undefined ? saved[key] : fallback
    }
    function remember(key, value) { if (service && trackId >= 0) service.setSoundChoice(trackId, key, value) }
    readonly property var duckKinds: ["voice", "effects", "ambience"]
    function restoreChoices() {
        targetIndex = savedTarget()
        roomEcho = choice("roomEcho", 0.4)
        const mask = choice("duckUnder", 1)
        duckUnder = duckKinds.filter((k, i) => mask & (1 << i))
        duckReduction = choice("duckReduction", 12)
        duckRelease = choice("duckRelease", 600)
        followed = ""
        if (duck.stored && duck.following) followDuck()
    }
    function setTarget(i) { targetIndex = i; remember("target." + kind, i) }
    function shown() { if (service && !service.pluginsListed && !service.scanning) service.listPlugins(); reload(); restoreChoices() }
    onVisibleChanged: if (visible) shown()
    Component.onCompleted: if (visible) shown()
    // Hidden, it reads nothing; it reads afresh when it is shown.
    Connections {
        target: root.service
        function onAutomationStampChanged() { if (root.visible) root.reload() }
        function onPluginsChanged() { if (root.visible) root.reload() }
        function onTrackSummariesChanged() { if (root.visible) root.reload() }
        function onTakeChanged() { if (root.visible) { root.reload(); root.restoreChoices() } }
    }

    readonly property string kind: settings.kind || ""
    readonly property var fixes: settings.fixes || []
    function fix(id) {
        for (const f of fixes) if (f.id === id) return f
        return null
    }
    function change(changes) {
        if (!service) return false
        const ok = service.applySound(trackId, settings.stamp || "", changes)
        message = ok ? "" : service.lastError
        reload()
        return ok
    }

    // ── kinds of sound ────────────────────────────────────────────────────
    readonly property var kinds: [
        { key: "voice", label: qsTr("Voice") }, { key: "music", label: qsTr("Music") },
        { key: "effects", label: qsTr("Effects") }, { key: "ambience", label: qsTr("Ambience") }]
    function setKind(key) {
        if (!service || key === kind) return
        const role = key === "voice" ? "dialogue" : key === "effects" ? "sfx" : key
        service.setTrackRole(trackId, role)
        reload()
    }

    // ── starting points (voice) ───────────────────────────────────────────
    // Amounts are 0..1; a fix left out is turned off. Unavailable fixes are
    // skipped, so a preset still works without the Calf effects.
    readonly property var voicePresets: [
        { name: qsTr("Podcast voice"), target: 0,
          fixes: { rumble: 0.3, noise: 0.45, sibilance: 0.35, evenout: 0.5, presence: 0.3 } },
        { name: qsTr("Interview on location"), target: 0,
          fixes: { rumble: 0.6, noise: 0.7, sibilance: 0.3, evenout: 0.6, presence: 0.2 } },
        { name: qsTr("Phone or video call"), target: 0,
          fixes: { rumble: 0.8, noise: 0.6, sibilance: 0.2, evenout: 0.7, presence: 0.4 } },
        { name: qsTr("Light touch"), target: 0,
          fixes: { rumble: 0.2, noise: 0.25, evenout: 0.3 } },
        { name: qsTr("Nothing"), target: -1, fixes: {} }]
    // Built-in starting points leave Mains hum alone (it depends on where you
    // recorded); your own also keep its setting.
    function presetMatches(preset) {
        for (const f of fixes) {
            if (!f.available || (f.id === "hum" && !preset.own)) continue
            const wanted = preset.fixes[f.id]
            if (wanted === undefined) { if (f.on) return false; continue }
            if (!f.on) return false
            if (f.id === "hum" ? f.mains !== wanted : Math.abs(f.amount - wanted) > 0.005) return false
        }
        return true
    }
    readonly property int currentPreset: {
        for (let i = 0; i < allPresets.length; ++i) if (presetMatches(allPresets[i])) return i
        return -1
    }
    function applyPreset(index) {
        const preset = allPresets[index]
        const changes = {}
        for (const f of fixes) {
            if (!f.available || (f.id === "hum" && !preset.own)) continue
            const wanted = preset.fixes[f.id]
            if (wanted === undefined) { if (f.on) changes[f.id] = { on: false } }
            else changes[f.id] = f.id === "hum" ? { on: true, mains: wanted } : { on: true, amount: wanted }
        }
        if (Object.keys(changes).length) change(changes)
        if (preset.target >= 0 && preset.target < targets.length) setTarget(preset.target)
    }

    // ── loudness ──────────────────────────────────────────────────────────
    readonly property var targets: kind === "voice"
        ? [{ name: qsTr("Podcast"), lufs: -16, peak: -1 }, { name: qsTr("Online video"), lufs: -14, peak: -1 },
           { name: qsTr("Broadcast"), lufs: -23, peak: -1 }, { name: qsTr("Audiobook"), lufs: -19, peak: -3 }]
        : kind === "music"
        ? [{ name: qsTr("Under speech"), lufs: -24, peak: -1 }, { name: qsTr("Music on its own"), lufs: -16, peak: -1 },
           { name: qsTr("Broadcast"), lufs: -23, peak: -1 }]
        : [{ name: qsTr("In the mix"), lufs: -20, peak: -1 }, { name: qsTr("In the background"), lufs: -30, peak: -1 }]
    property int targetIndex: 0
    onKindChanged: { targetIndex = savedTarget(); matched = "" }
    function savedTarget() { return Math.max(0, Math.min((targets ? targets.length : 1) - 1, choice("target." + kind, 0))) }
    function targetLabel(t) { return qsTr("%1  (%2 LUFS, peaks %3 dB)").arg(t.name).arg(t.lufs).arg(t.peak) }
    property bool matching: false
    property string matched: ""        // what the last match did, for this track
    property real matchedGain: NaN     // the fader it set; the note goes when the fader moves
    readonly property var report: service ? service.trackLoudnessReport : ({})
    readonly property bool measured: report.valid === true && report.track === trackId && report.stale !== true
    function lufs(v) { return v <= -70 ? qsTr("silence") : qsTr("%1 LUFS").arg(Number(v).toFixed(1)) }
    function match() {
        if (!service) return
        message = ""; matched = ""
        if (!service.measureTrackLoudness(trackId)) { message = service.lastError; return }
        matching = true
    }
    Connections {
        target: root.service
        function onTrackLoudnessChanged() {
            if (!root.matching || root.service.measuringTrackLoudness) return
            root.matching = false
            const t = root.targets[root.targetIndex]
            const proposal = root.service.trackLoudnessProposal(root.trackId, t.lufs, t.peak)
            if (!proposal.valid) { root.message = proposal.reason; return }
            const before = root.service.trackLoudnessReport.integrated
            if (!root.service.applyTrackLoudness(root.trackId, proposal.stamp, t.lufs, t.peak)) { root.message = root.service.lastError; return }
            root.matchedGain = proposal.gainDb
            root.matched = qsTr("%1 → %2").arg(root.lufs(before)).arg(root.lufs(proposal.estimatedLufs))
                + (proposal.peakLimited ? "  " + qsTr("(held back by the peak limit)") : "")
            root.reload()
        }
    }

    // ── ducking (music) ───────────────────────────────────────────────────
    // Generated ducking, made here from a few plain settings. A new setting
    // rebuilds it from the volume curve it was first made from, so it never
    // stacks; points edited by hand are left alone.
    readonly property var duck: service && trackId >= 0 && service.tracksRevision >= 0 && settings.stamp !== undefined
                                ? service.generatedDuckingState(trackId) : ({ stored: false })
    readonly property bool duckOn: duck.stored === true && duck.following === true
    readonly property bool duckEditable: !duck.stored || duck.following === true
    property var duckUnder: ["voice"]
    property real duckReduction: 12
    property real duckRelease: 600
    property bool ducking: false
    readonly property var duckSources: {
        if (!service || service.tracksRevision < 0) return []
        const roles = { voice: ["dialogue", "host", "guest"], effects: ["sfx", "foley"], ambience: ["ambience"] }
        const wanted = [].concat(...duckUnder.map(k => roles[k] || []))
        return service.tracks.filter(t => t.id !== trackId && wanted.indexOf(t.role) >= 0).map(t => t.id)
    }
    onDuckChanged: if (duck.stored && duck.following) followDuck()
    property string followed: ""         // the duck last shown, so a re-read of the same one changes nothing
    // A duck that is on shows what it was made with, including which kinds of
    // track it ducks under.
    function followDuck() {
        const now = JSON.stringify(duck)
        if (now === followed) return
        followed = now
        duckReduction = duck.reductionDb; duckRelease = duck.releaseMs
        const kinds = { dialogue: "voice", host: "voice", guest: "voice", sfx: "effects", foley: "effects", ambience: "ambience" }
        const under = []
        for (const id of duck.triggers || []) {
            const t = service.tracks.find(x => x.id === id)
            const k = t ? kinds[t.role] : undefined
            if (k && under.indexOf(k) < 0) under.push(k)
        }
        if (under.length) duckUnder = duckKinds.filter(k => under.indexOf(k) >= 0)
    }
    // Read before writing: each write marks the project changed, which re-reads it.
    function rememberDuck() {
        const under = duckKinds.reduce((m, k, i) => duckUnder.indexOf(k) >= 0 ? m | (1 << i) : m, 0)
        const reduction = duckReduction, release = duckRelease
        remember("duckUnder", under)
        remember("duckReduction", reduction)
        remember("duckRelease", release)
    }
    function duckApply() {
        if (!service) return
        message = ""
        if (duckSources.length === 0) { message = qsTr("There is no track of the chosen kinds to duck under."); return }
        const end = service.tracks.reduce((e, t) => Math.max(e, t.frames || 0), 0)
        const release = Math.round(duckRelease)
        const spec = { track: trackId, triggers: duckSources, start: 0, end: end, thresholdDb: -36,
            reductionDb: Math.round(duckReduction * 10) / 10, attackMs: Math.round(release / 6), holdMs: 200,
            releaseMs: release, replaceExisting: false, rebuild: true, stamp: service.automationStamp }
        if (!service.analyseDucking(spec)) { message = service.lastError; return }
        ducking = true
    }
    function duckRemove() {
        if (!service) return
        message = service.removeGeneratedDucking(trackId) ? "" : service.lastError
        reload()
    }
    Connections {
        target: root.service
        function onDuckingChanged() {
            if (!root.ducking || root.service.generatingDucking) return
            root.ducking = false
            const preview = root.service.duckingPreview
            if (!preview.valid || preview.track !== root.trackId) { root.message = preview.message || root.service.lastError; return }
            if (preview.replacementRequired) {
                root.message = qsTr("This track already has volume points where the music would duck. Use More ducking settings… to replace them.")
                return
            }
            // Always kept up to date: that is what lets this panel change or remove it later.
            if (!root.service.applyGeneratedDucking(preview.token, true)) root.message = root.service.lastError
            root.reload()
        }
    }

    property real roomEcho: 0.4

    // ── length (music) ────────────────────────────────────────────────────
    property int fitMode: 0          // 0 re-cut, 1 stretch
    property bool fitting: false
    // The music clip at the playhead (or the selection start) on this track.
    readonly property var lengthClip: {
        if (!service || kind !== "music" || settings.stamp === undefined) return ({ valid: false })
        void service.selectionStart; void (service.playing ? 0 : service.playhead)
        const t = service.remixTarget()
        return t.valid && t.track === trackId ? t : ({ valid: false })
    }
    // A new clip (or a new length) fills in Fit to; reading the same clip again
    // keeps what was typed.
    property string lengthShown: ""
    onLengthClipChanged: {
        const key = lengthClip.valid ? lengthClip.track + ":" + lengthClip.clip + ":" + lengthClip.length : ""
        if (key === lengthShown) return
        lengthShown = key
        if (lengthClip.valid && !fitTo.activeFocus) fitTo.show(lengthClip.length)
    }
    function fit(frames) {
        message = ""
        if (fitMode === 0) {
            if (!service.remixClip(frames / service.sampleRate, lengthClip.stamp)) { message = service.lastError; return }
            fitting = service.remixing
        } else {
            const target = service.stretchTarget()
            if (!target.valid || target.track !== trackId) { message = qsTr("Click the music clip to stretch first."); return }
            service.stretchFocusedClip(frames, target.stamp, target.track, target.clip)
            if (service.lastError !== "") message = service.lastError
        }
    }
    Connections {
        target: root.service
        function onRemixChanged() {
            if (root.fitting && !root.service.remixing) {
                root.fitting = false
                if (root.service.lastError !== "") root.message = root.service.remixMessage
                root.reload()
            }
        }
    }

    // ── your own starting points ──────────────────────────────────────────
    Settings {
        category: "sound"
        property alias ownPresets: root.ownPresetsText
    }
    property string ownPresetsText: "[]"
    readonly property var ownPresets: {
        try { const list = JSON.parse(ownPresetsText); return Array.isArray(list) ? list : [] } catch (e) { return [] }
    }
    readonly property var allPresets: voicePresets.concat(ownPresets.map(p => Object.assign({ own: true }, p)))
    function savePreset(name) {
        name = String(name).trim()
        if (name === "") return
        const fixesNow = {}
        for (const f of fixes) {
            if (!f.on) continue
            fixesNow[f.id] = f.id === "hum" ? f.mains : Math.round(f.amount * 1000) / 1000
        }
        const list = ownPresets.filter(p => p.name !== name)
        list.push({ name: name, target: targetIndex, fixes: fixesNow })
        ownPresetsText = JSON.stringify(list)
    }
    function removePreset(name) {
        ownPresetsText = JSON.stringify(ownPresets.filter(p => p.name !== name))
    }

    // ── layout ────────────────────────────────────────────────────────────
    // Tidy pauses is a page of this tab, open until its ‹ Sound.
    readonly property bool pausesOpen: shell !== null && shell.tidyPausesOpen === true
    Loader {
        objectName: "pausesLoader"
        anchors.fill: parent
        active: root.pausesOpen
        visible: active
        sourceComponent: PausesPage { shell: root.shell }
    }

    EmptyState {
        anchors.fill: parent
        visible: !root.pausesOpen && (root.trackId < 0 || root.settings.available !== true)
        iconName: "sliders-horizontal"
        title: root.trackId < 0 ? qsTr("No active track") : qsTr("No Sound settings")
        description: root.trackId < 0 ? qsTr("Click a lane or a mixer strip to set up its sound.") : (root.settings.reason || "")
    }

    Flickable {
        id: flick
        anchors.fill: parent
        visible: !root.pausesOpen && root.trackId >= 0 && root.settings.available === true
        contentHeight: body.implicitHeight + Theme.s4 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: body
            x: Theme.s3; y: Theme.s3
            width: flick.width - Theme.s3 * 2
            spacing: Theme.s4

            // Which track, what kind of sound, where to start.
            Column {
                width: parent.width
                spacing: Theme.s2
                Text {
                    width: parent.width
                    text: root.settings.trackName || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading
                    color: Theme.textPrimary
                }
                SegmentedControl {
                    objectName: "soundKind"
                    labels: root.kinds.map(k => k.label)
                    currentIndex: root.kinds.findIndex(k => k.key === root.kind)
                    helpTitle: qsTr("Kind of sound")
                    helpText: qsTr("What this track holds. It sets the track's role and which settings appear here.")
                    onActivated: function (i) { root.setKind(root.kinds[i].key) }
                }
                Row {
                    visible: root.kind === "voice"
                    width: parent.width
                    spacing: Theme.s2
                    Text {
                        width: 72; height: Theme.hControl
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Start from")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                    }
                    ComboField {
                        objectName: "soundPreset"
                        width: parent.width - 72 - saveButton.width - Theme.s2 * 2
                        model: root.allPresets.map(p => p.name)
                        displayText: root.currentPreset >= 0 ? root.allPresets[root.currentPreset].name : qsTr("Your own settings")
                        currentIndex: root.currentPreset
                        helpTitle: qsTr("Start from")
                        helpText: qsTr("Sets every fix below at once. Adjust them afterwards; the change is one Undo step.")
                        onActivated: function (i) { root.applyPreset(i) }
                    }
                    ToolButton {
                        id: saveButton
                        objectName: "soundSavePreset"
                        text: root.currentPreset >= 0 && root.allPresets[root.currentPreset].own ? qsTr("Remove") : qsTr("Save…")
                        showLabel: true
                        helpText: root.currentPreset >= 0 && root.allPresets[root.currentPreset].own
                                  ? qsTr("Remove this starting point of your own. The track keeps its settings.")
                                  : qsTr("Keep these settings as a starting point of your own, for any voice track.")
                        onClicked: {
                            if (root.currentPreset >= 0 && root.allPresets[root.currentPreset].own) root.removePreset(root.allPresets[root.currentPreset].name)
                            else { presetName.text = ""; savePopup.open(); presetName.forceActiveFocus() }
                        }
                    }
                }
                C.Popup {
                    id: savePopup
                    objectName: "soundSavePopup"
                    width: parent.width
                    padding: Theme.s2
                    modal: false
                    focus: true
                    background: Rectangle { color: Theme.controlBg; border.color: Theme.border; border.width: Theme.hairline; radius: Theme.rMenu }
                    contentItem: Row {
                        spacing: Theme.s2
                        TextEntry {
                            id: presetName
                            objectName: "soundPresetName"
                            width: savePopup.availableWidth - keep.width - Theme.s2
                            placeholderText: qsTr("Name, for example My studio mic")
                            helpTitle: qsTr("Name")
                            helpText: qsTr("A name for this starting point. Using an existing name replaces it.")
                            onAccepted: keep.clicked()
                        }
                        ToolButton {
                            id: keep
                            objectName: "soundPresetKeep"
                            text: qsTr("Keep"); showLabel: true; prominent: true
                            enabled: presetName.text.trim() !== ""
                            helpText: qsTr("Save this starting point.")
                            onClicked: { root.savePreset(presetName.text); savePopup.close() }
                        }
                    }
                }
            }

            // Loudness: measure the track's output, then set its fader to the target.
            SoundSection {
                title: qsTr("Loudness")
                summary: root.matching ? qsTr("Measuring…")
                       : root.matched !== "" && Math.abs((root.settings.gainDb || 0) - root.matchedGain) < 0.01 ? root.matched
                       : root.measured ? qsTr("%1 now").arg(root.lufs(root.report.integrated)) : ""
                ComboField {
                    objectName: "soundLoudnessTarget"
                    width: parent.width
                    caption: qsTr("Target")
                    model: root.targets.map(t => root.targetLabel(t))
                    currentIndex: root.targetIndex
                    helpTitle: qsTr("Loudness target")
                    helpText: qsTr("How loud this track should be on its own. Match measures it and sets its level to reach the target without passing the peak limit.")
                    onActivated: function (i) { root.setTarget(i); root.matched = "" }
                }
                Row {
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "soundMatchLoudness"
                        text: qsTr("Match")
                        iconName: "gauge"; showLabel: true
                        enabled: !root.matching && root.service !== null && !root.service.measuringTrackLoudness
                        helpText: qsTr("Measure this track's output and set its level to the target. One Undo step.")
                        onClicked: root.match()
                    }
                    ToolButton {
                        visible: root.matching
                        text: qsTr("Cancel")
                        iconName: "x"; showLabel: true
                        helpText: qsTr("Stop measuring. Nothing changes.")
                        onClicked: { root.matching = false; if (root.service) root.service.cancelTrackLoudness() }
                    }
                }
            }

            // Voice: clean up, then polish.
            SoundSection {
                visible: root.kind === "voice"
                title: qsTr("Clean up")
                summary: {
                    const ids = ["noise", "rumble", "hum", "sibilance"]
                    const on = root.fixes.filter(f => ids.indexOf(f.id) >= 0 && f.on).length
                    return qsTr("%1 of %2 on").arg(on).arg(ids.length)
                }
                SoundFix { panel: root; fixId: "noise"; hint: qsTr("Hiss, air conditioning, room tone.") }
                SoundFix { panel: root; fixId: "rumble"; hint: qsTr("Traffic, handling noise, popped p and b.") }
                SoundFix { panel: root; fixId: "hum"; hint: qsTr("A steady buzz from the mains.") }
                SoundFix { panel: root; fixId: "sibilance"; hint: qsTr("Softens sharp s and sh sounds.") }
                // Room echo is written into the audio, not live: one Undo step.
                SoundAmount {
                    objectName: "soundRoomEcho"
                    title: qsTr("Room echo")
                    hint: qsTr("Reduces the sound of the room. Written into the selected audio, or the whole track when nothing is selected.")
                    lowText: qsTr("A little"); highText: qsTr("A lot")
                    from: 0; to: 100; suffix: "%"
                    value: Math.round(root.roomEcho * 100)
                    enabled: !root.service || !root.service.repairing
                    onCommitted: function (v) { root.roomEcho = v / 100; root.remember("roomEcho", root.roomEcho) }
                }
                Row {
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "soundReduceEcho"
                        text: root.service && root.service.repairing ? qsTr("Reducing…") : qsTr("Reduce room echo")
                        iconName: "waves"; showLabel: true
                        enabled: root.service !== null && !root.service.repairing
                        helpText: qsTr("Reduce the room's echo in the audio now. Listen, and use Undo to compare.")
                        onClicked: {
                            root.message = ""
                            if (!root.service.reduceRoomEcho(root.trackId, root.roomEcho)) root.message = root.service.lastError
                        }
                    }
                    ToolButton {
                        visible: root.service !== null && root.service.repairing
                        text: qsTr("Cancel")
                        iconName: "x"; showLabel: true
                        helpText: qsTr("Stop. The audio is not changed.")
                        onClicked: root.service.cancelRepair()
                    }
                }
            }
            SoundSection {
                visible: root.kind === "voice"
                title: qsTr("Polish")
                SoundFix { panel: root; fixId: "evenout"; hint: qsTr("Keeps quiet and loud words closer in level."); lowText: qsTr("Natural"); highText: qsTr("Tight") }
                SoundFix { panel: root; fixId: "presence"; hint: qsTr("Brings the voice forward."); lowText: qsTr("Subtle"); highText: qsTr("Forward") }
            }

            // Voice: the timing, on a page of its own.
            SoundSection {
                visible: root.kind === "voice"
                title: qsTr("Pauses")
                summary: qsTr("Changes the timing")
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Long gaps between words. Shorten, remove or mark them.")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                }
                ToolButton {
                    objectName: "soundTidyPauses"
                    text: qsTr("Tidy pauses…")
                    iconName: "scissors"; showLabel: true
                    helpText: qsTr("Find the gaps between words, then shorten, remove or mark them. Nothing changes until you choose.")
                    onClicked: if (root.shell) root.shell.runAction("show_tidy_pauses")
                }
            }

            // Effects and ambience: how wide they sit.
            SoundSection {
                visible: root.kind === "effects" || root.kind === "ambience"
                title: qsTr("Width")
                SoundFix { panel: root; fixId: "width"; hint: qsTr("Narrows or widens the sound: 0 % is mono, 100 % as recorded."); lowText: qsTr("Mono"); highText: qsTr("Wide") }
            }

            // Music: fit the clip at the playhead (or the selection) to a length.
            SoundSection {
                visible: root.kind === "music"
                title: qsTr("Length")
                summary: root.lengthClip.valid
                         ? qsTr("%1 → %2").arg(root.service.formatDuration(root.lengthClip.length)).arg(fitTo.valid ? root.service.formatDuration(fitTo.value) : "–")
                         : ""
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: root.lengthClip.valid ? qsTr("The clip at the playhead: %1.").arg(Theme.shortName(root.lengthClip.name, 48))
                                                : qsTr("Put the playhead over a music clip on this track.")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                }
                Row {
                    spacing: Theme.s2
                    Text {
                        width: 52; height: Theme.hControl; verticalAlignment: Text.AlignVCenter
                        text: qsTr("Fit to")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                    }
                    TimeField {
                        id: fitTo
                        objectName: "soundFitTo"
                        width: 110
                        duration: true
                        service: root.service
                        minimum: 1
                        enabled: root.lengthClip.valid && !root.fitting
                        helpTitle: qsTr("Fit to")
                        helpText: qsTr("How long the music should become.")
                    }
                    SegmentedControl {
                        objectName: "soundFitMode"
                        labels: [qsTr("Re-cut"), qsTr("Stretch")]
                        currentIndex: root.fitMode
                        enabled: root.lengthClip.valid && !root.fitting
                        helpTitle: qsTr("How to fit")
                        helpText: qsTr("Re-cut joins matching passages and keeps the speed; Stretch changes the speed and keeps the pitch.")
                        onActivated: function (i) { root.fitMode = i }
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: root.fitMode === 0 ? qsTr("Re-cut: at least 5 seconds; the result lands within about 2% of it.")
                                             : qsTr("Stretch: 25% to 400% of the clip's length.")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                }
                Row {
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "soundFitMusic"
                        text: root.fitting ? qsTr("Fitting…") : qsTr("Fit")
                        iconName: "timer"; showLabel: true
                        enabled: root.lengthClip.valid && fitTo.valid && !root.fitting
                        helpText: qsTr("Make the clip this length now. One Undo step.")
                        onClicked: root.fit(fitTo.value)
                    }
                    ToolButton {
                        text: qsTr("More…")
                        iconName: "settings-2"; showLabel: true
                        helpText: qsTr("Open Fit Music to Length with all its settings.")
                        onClicked: if (root.shell) root.shell.runAction(root.fitMode === 0 ? "show_remix" : "show_stretch")
                    }
                }
            }
            SoundSection {
                visible: root.kind === "music"
                title: qsTr("Duck under voices")
                summary: root.ducking ? qsTr("Working…")
                       : root.duck.stored ? (root.duck.following ? qsTr("−%1 dB").arg(Math.round(root.duck.reductionDb)) : qsTr("edited by hand")) : qsTr("off")
                Item {
                    width: parent.width
                    height: Theme.hRow
                    Toggle {
                        id: duckToggle
                        objectName: "soundDuckToggle"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.duckOn
                        enabled: !root.ducking && (root.duckOn || root.duckSources.length > 0)
                        helpTitle: qsTr("Duck under voices")
                        helpText: qsTr("Turns the music down while someone speaks, as volume points you can see and edit in Automation.")
                        unavailableReason: root.duckSources.length === 0 ? qsTr("There is no track of the chosen kinds to duck under.") : ""
                        onToggled: { if (checked) root.duckApply(); else root.duckRemove(); checked = Qt.binding(() => root.duckOn) }
                    }
                    Text {
                        anchors.left: duckToggle.right; anchors.leftMargin: Theme.s2
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        text: root.duck.stored && !root.duck.following
                              ? qsTr("Points edited by hand: change them in Automation.")
                              : qsTr("Follows later edits to the voices.")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                    }
                }
                Row {
                    spacing: Theme.s2
                    Text {
                        width: 52; height: Theme.hControl; verticalAlignment: Text.AlignVCenter
                        text: qsTr("Under")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary
                    }
                    Repeater {
                        model: [{ key: "voice", label: qsTr("Voice") }, { key: "effects", label: qsTr("Effects") }, { key: "ambience", label: qsTr("Ambience") }]
                        ToolButton {
                            required property var modelData
                            objectName: "soundDuckUnder_" + modelData.key
                            text: modelData.label
                            checkable: true
                            checked: root.duckUnder.indexOf(modelData.key) >= 0
                            enabled: !root.ducking && (root.duckEditable || !root.duck.stored)
                            helpText: qsTr("Duck while tracks of this kind play.")
                            onToggled: {
                                const under = root.duckUnder.filter(k => k !== modelData.key)
                                if (checked) under.push(modelData.key)
                                root.duckUnder = root.duckKinds.filter(k => under.indexOf(k) >= 0)
                                root.rememberDuck()
                                checked = Qt.binding(() => root.duckUnder.indexOf(modelData.key) >= 0)
                                if (root.duckOn) root.duckApply()
                            }
                        }
                    }
                }
                SoundAmount {
                    objectName: "soundDuckAmount"
                    title: qsTr("How much")
                    hint: qsTr("How far the music drops while someone speaks.")
                    lowText: qsTr("A little"); highText: qsTr("A lot")
                    from: 3; to: 24; suffix: "dB"
                    value: root.duck.stored ? root.duck.reductionDb : root.duckReduction
                    enabled: !root.ducking && root.duckEditable
                    onCommitted: function (v) { root.duckReduction = v; if (root.duckOn) root.duckApply(); root.rememberDuck() }
                }
                SoundAmount {
                    objectName: "soundDuckSpeed"
                    title: qsTr("How quickly")
                    hint: qsTr("How fast it drops and comes back.")
                    lowText: qsTr("Fast"); highText: qsTr("Gentle")
                    from: 150; to: 1500; suffix: "ms"
                    value: root.duck.stored ? root.duck.releaseMs : root.duckRelease
                    enabled: !root.ducking && root.duckEditable
                    onCommitted: function (v) { root.duckRelease = v; if (root.duckOn) root.duckApply(); root.rememberDuck() }
                }
                ToolButton {
                    objectName: "soundDuck"
                    text: qsTr("More ducking settings…")
                    iconName: "settings-2"; showLabel: true
                    helpText: qsTr("Choose the trigger tracks, time range, threshold and fade times yourself, and preview the result.")
                    onClicked: if (root.shell) root.shell.openGeneratedDucking(root.trackId)
                }
            }

            // Level: the track fader.
            SoundSection {
                title: qsTr("Level")
                summary: qsTr("track fader")
                SliderField {
                    objectName: "soundLevel"
                    width: parent.width
                    labelWidth: 0
                    from: -60; to: 12; decimals: 1; suffix: "dB"; origin: 0
                    value: root.settings.gainDb !== undefined ? root.settings.gainDb : 0
                    helpTitle: qsTr("Level")
                    helpText: qsTr("This track's level in decibels (dB), the same as its fader. 0 dB leaves the level unchanged.")
                    onEdited: function (v) { if (root.service) root.service.setTrackGainDb(root.trackId, v) }
                }
            }

            Text {
                width: parent.width
                visible: root.message !== ""
                wrapMode: Text.WordWrap
                text: root.message
                textFormat: Text.PlainText
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.danger
            }

            // What it is made of.
            Column {
                width: parent.width
                spacing: Theme.s1
                Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                    text: {
                        if (root.kind === "music")
                            return qsTr("Built from Fit Music to Length, generated ducking and the track fader. Each change is one Undo step.")
                        const used = root.fixes.filter(f => f.slot >= 0).map(f => f.plugin)
                        return used.length
                            ? qsTr("Built from %1 on this track. Each change is one Undo step.").arg(used.join(", "))
                            : qsTr("Each fix adds a real effect to this track. Each change is one Undo step.")
                    }
                }
                ToolButton {
                    objectName: "soundOpenEffects"
                    text: qsTr("Open in Track Effects")
                    iconName: "sliders-horizontal"; showLabel: true
                    helpText: qsTr("Show this track's effects with all their controls.")
                    onClicked: if (root.shell) root.shell.rackTabRequested(0)
                }
            }
        }
    }
}
