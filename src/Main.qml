// Delegates below reach the window's own ids. Without this pragma that is an
// unqualified access into an outer component, which qmllint flags and which Qt is
// free to resolve later than you expect.
pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic as C
import QtQuick.Dialogs
import "ui"

// OmaAudio — the application shell.
//
//     omaaudio                       opens empty
//     omaaudio /takes/voice.wav      opens on a take
//
// Title bar with the menus and the four workspaces, the global transport, the
// active workspace, the status bar. Layout state persists in Settings. `service`
// is the C++ Backend; the ported editor QML still says so.
C.ApplicationWindow {
    id: win

    readonly property var service: backend
    property bool graphicsRestarted: false
    property bool timelineMoveTool: false
    property bool timelineSlipTool: false

    visible: true
    width: 1600
    height: 960
    minimumWidth: 1280
    minimumHeight: 720
    // Frameless: AppTitleBar carries the drag handle and window controls.
    flags: Qt.Window | Qt.FramelessWindowHint
    color: Theme.windowBg

    title: {
        const name = !service ? ""
                   : service.projectName !== "" ? service.projectName
                   : service.loaded ? service.takeName : ""
        if (name === "")
            return Theme.appName
        // A leading dot for unsaved changes: visible in a window list without
        // reading the whole title.
        return Theme.appName + " — " + (service.dirty ? "• " : "") + name
    }

    // ── layout state ────────────────────────────────────────────────────
    property string workspace: "Multitrack"
    readonly property var workspaceLoader: workspace === "Waveform" ? waveformLoader
        : workspace === "Spectral" ? spectralLoader
        : workspace === "Mastering" ? masteringLoader : multitrackLoader
    readonly property bool workspaceLoading: workspaceLoader.status === Loader.Loading
    property int pendingEffectSlot: -1
    property int pendingEffectTrack: -1
    onWorkspaceChanged: if (workspace !== "Multitrack") {
        pendingEffectSlot = -1
        if (service) service.clearGroupSelection()
    }
    property bool leftDockVisible: true
    property bool effectsDockVisible: true
    // The Arrange page's Effects panel shows the master bus after the master
    // strip asks for it, until another track is focused or a track's own
    // effects are asked for.
    property bool effectsDockMaster: false
    property int effectsDockMasterFocus: -1
    readonly property bool effectsDockShowsMaster: effectsDockMaster && service !== null
        && service.focusedTrack === effectsDockMasterFocus
    function showMasterEffects() {
        if (!service) return
        win.effectsDockMaster = true
        win.effectsDockMasterFocus = service.focusedTrack
        win.effectsDockVisible = true
    }
    function showTrackEffects() { win.effectsDockMaster = false }
    // Tidy pauses: a page of the Sound tab, open across tab changes (the tab is
    // made again each time it is shown); where Edit › Tidy Pauses… looks first;
    // and what the page will do, for the shading on the lanes.
    property bool tidyPausesOpen: false
    property string tidyPausesScope: ""
    property var tidyPausesAction: ({ then: "shorten", changed: [], lengths: [] })
    property bool mixerVisible: true
    // What fill_window hid, to put back; null when nothing is filled.
    property var filledFrom: null
    property int leftDockWidth: Theme.wLeftDock
    property int rightDockWidth: Theme.wRightDock
    property int mixerWidth: Theme.wMixerDock

    // The two edit modes. The shell owns them so they persist; the Backend
    // reads them through the bindings below.
    property bool snapping: true
    property string snapGrid: "none"
    onSnapGridChanged: if (service && service.snapGrids.indexOf(snapGrid) < 0) snapGrid = "none"
    property bool ripple: false
    // An in point set with I (or [) while nothing was selected, waiting for its out point.
    property real markIn: -1

    Settings {
        id: shellSettings
        category: "shell"
        property alias windowWidth: win.width
        property alias windowHeight: win.height
        property alias workspace: win.workspace
        property alias leftDockVisible: win.leftDockVisible
        property alias effectsDockVisible: win.effectsDockVisible
        property alias mixerVisible: win.mixerVisible
        property alias leftDockWidth: win.leftDockWidth
        property alias rightDockWidth: win.rightDockWidth
        property alias mixerWidth: win.mixerWidth
        property alias snapping: win.snapping
        property alias snapGrid: win.snapGrid
        property alias ripple: win.ripple
        property alias autosaveSeconds: win.autosaveSeconds
        property alias automationReturnSeconds: win.automationReturnSeconds
        property alias memoryMiB: win.memoryMiB
        property alias spectralStoreGiB: win.spectralStoreGiB
        property alias timeDisplay: win.timeDisplay
        property alias gettingStartedDismissed: win.gettingStartedDismissed
        property alias vst3Discovery: win.vst3Discovery
    }
    // Read installed VST3 effects as well as LV2 and CLAP ones.
    property bool vst3Discovery: true
    Binding { target: win.service; property: "vst3Discovery"; value: win.vst3Discovery }
    // The welcome card on an empty session. Dismissed once; Help › Getting Started brings it back.
    property bool gettingStartedDismissed: false

    // Autosave (A22): how long a dirty session waits before its recovery layer is
    // written. 0 is off. The engine holds the timer; this is the setting.
    property int autosaveSeconds: 30
    onAutosaveSecondsChanged: if (service) service.autosaveSeconds = win.autosaveSeconds

    property real automationReturnSeconds: 1
    onAutomationReturnSecondsChanged: {
        if (!Number.isFinite(automationReturnSeconds) || automationReturnSeconds < 0 || automationReturnSeconds > 5) {
            automationReturnSeconds = 1
            return
        }
        if (service) service.automationReturnSeconds = automationReturnSeconds
    }

    // The memory budget in MiB, zero for automatic. The engine holds the caches
    // and what the machine allows; this is the setting.
    property int memoryMiB: 0
    onMemoryMiBChanged: if (service) service.memoryMiB = win.memoryMiB
    // How positions and lengths are written and typed everywhere: "time",
    // "samples" or a timecode rate. The engine formats and parses; this is the setting.
    property string timeDisplay: "time"
    onTimeDisplayChanged: if (service) service.timeDisplay = win.timeDisplay
    // The most disk the kept spectral displays may use, in GiB.
    property int spectralStoreGiB: 4
    onSpectralStoreGiBChanged: if (service) service.spectralStoreGiB = win.spectralStoreGiB

    // ── delivery (Phase 3) ──────────────────────────────────────────────
    // What a render is: the Mastering page's delivery bar edits these, the
    // File menu's Export uses them, and they persist like the layout does.
    property string deliveryPreset: "streaming"
    property string deliveryFormat: "wav"
    property int deliveryBitDepth: 24
    property int deliveryBitrateKbps: 0
    property int deliverySampleRate: 0
    property int deliveryChannels: 0
    property bool deliveryDither: true
    // What Export… writes, chosen from its menu each time and never remembered:
    // "mix" (one file, as heard) or "selected" (one stem per selected track).
    property string exportScope: "mix"
    // What was selected when Export… was chosen, kept for its menu and the render:
    // going to the Mastering page lets go of the Arrange page's track selection.
    property var exportTracks: []
    property real exportRangeStart: -1
    property real exportRangeEnd: -1
    readonly property bool exportHasRange: exportRangeEnd > exportRangeStart && exportRangeStart >= 0
    // "project" or "selection": the selection when Export… was chosen with one.
    property string exportRange: "project"
    property bool exportContextFresh: false
    function captureExportContext() {
        if (!service) return
        win.exportTracks = service.selectedTracks.slice()
        const has = service.selectionLength > 0
        win.exportRangeStart = has ? service.selectionStart : -1
        win.exportRangeEnd = has ? service.selectionEnd : -1
        win.exportRange = has ? "selection" : "project"
        win.exportContextFresh = true
    }
    // Export… lives in the Mastering page's delivery bar; the export menu opens
    // from it, so a shortcut used on another page waits for the page to load.
    property Item exportButton: null
    property bool exportMenuPending: false
    onExportButtonChanged: if (exportButton && exportMenuPending) Qt.callLater(win.openExportMenu)
    property string deliveryNormalise: "loudness"
    property real deliveryTargetLufs: -14
    property real deliveryCeilingDbTp: -1
    property real deliveryPeakTargetDb: -0.3
    // The user's own presets, as JSON in the settings: a name over the same
    // fields a built-in has. Saved from whatever the bar shows.
    property string deliveryUserPresetsJson: "[]"
    // W05: what the next render writes besides samples, and where an import lands.
    property bool deliveryIncludeMetadata: false
    property string deliveryMarkers: "none"      // none | project | source
    property string importPlacement: "start"     // start | playhead | timestamp

    Settings {
        id: deliverySettings
        category: "delivery"
        property alias preset: win.deliveryPreset
        property alias format: win.deliveryFormat
        property alias bitDepth: win.deliveryBitDepth
        property alias bitrateKbps: win.deliveryBitrateKbps
        property alias sampleRate: win.deliverySampleRate
        property alias channels: win.deliveryChannels
        property alias dither: win.deliveryDither
        property alias normalise: win.deliveryNormalise
        property alias targetLufs: win.deliveryTargetLufs
        property alias ceilingDbTp: win.deliveryCeilingDbTp
        property alias peakTargetDb: win.deliveryPeakTargetDb
        property alias userPresets: win.deliveryUserPresetsJson
        property alias includeMetadata: win.deliveryIncludeMetadata
        property alias markers: win.deliveryMarkers
        property alias importPlacement: win.importPlacement
    }
    Binding { target: win.service; property: "importPlacement"; value: win.importPlacement }

    // Any hand edit turns the preset into "custom": a preset is a starting point.
    function setDelivery(name, value) {
        if (win[name] === value)
            return
        win[name] = value
        // Metadata and markers describe the render, not the delivery preset.
        if (name !== "deliveryIncludeMetadata" && name !== "deliveryMarkers")
            win.deliveryPreset = "custom"
    }
    readonly property var deliveryUserPresets: {
        try {
            const list = JSON.parse(win.deliveryUserPresetsJson)
            return Array.isArray(list) ? list : []
        } catch (e) {
            return []
        }
    }
    // Built-ins first, then the user's, in one list the bar can index.
    readonly property var deliveryAllPresets: {
        var out = []
        const builtIn = service ? service.deliveryPresets : []
        for (var i = 0; i < builtIn.length; ++i)
            out.push(builtIn[i])
        for (var j = 0; j < win.deliveryUserPresets.length; ++j)
            out.push(win.deliveryUserPresets[j])
        return out
    }
    function presetById(id) {
        for (var i = 0; i < win.deliveryUserPresets.length; ++i) {
            if (win.deliveryUserPresets[i].id === id)
                return win.deliveryUserPresets[i]
        }
        return service ? service.presetById(id) : null
    }
    readonly property bool deliveryPresetIsUser: {
        for (var i = 0; i < win.deliveryUserPresets.length; ++i) {
            if (win.deliveryUserPresets[i].id === win.deliveryPreset)
                return true
        }
        return false
    }
    // Saves the bar's current delivery under `name`; a user preset of that name
    // is replaced. The saved one becomes the current preset.
    function saveUserPreset(name) {
        const trimmed = String(name).trim()
        if (trimmed === "")
            return false
        var list = win.deliveryUserPresets.slice()
        var id = ""
        for (var i = 0; i < list.length; ++i) {
            if (list[i].name === trimmed)
                id = list[i].id
        }
        if (id === "")
            id = "user-" + Date.now()
        const preset = { id: id, name: trimmed, user: true, description: qsTr("Your preset"),
                         format: win.deliveryFormat, bitDepth: win.deliveryBitDepth, bitrateKbps: win.deliveryBitrateKbps,
                         sampleRate: win.deliverySampleRate, channels: win.deliveryChannels,
                         dither: win.deliveryDither, normalise: win.deliveryNormalise,
                         targetLufs: win.deliveryTargetLufs, ceilingDbTp: win.deliveryCeilingDbTp,
                         peakTargetDb: win.deliveryPeakTargetDb }
        list = list.filter(function (p) { return p.id !== id })
        list.push(preset)
        win.deliveryUserPresetsJson = JSON.stringify(list)
        win.deliveryPreset = id
        return true
    }
    function deleteUserPreset(id) {
        const list = win.deliveryUserPresets.filter(function (p) { return p.id !== id })
        if (list.length === win.deliveryUserPresets.length)
            return
        win.deliveryUserPresetsJson = JSON.stringify(list)
        if (win.deliveryPreset === id)
            win.deliveryPreset = "custom"
    }
    function applyPreset(id) {
        if (!service)
            return
        const p = win.presetById(id)
        if (!p || p.id === undefined)
            return
        win.deliveryFormat = p.format
        win.deliveryBitDepth = p.bitDepth
        win.deliveryBitrateKbps = p.bitrateKbps === undefined ? 0 : p.bitrateKbps
        win.deliverySampleRate = p.sampleRate
        win.deliveryChannels = p.channels
        win.deliveryDither = p.dither
        win.deliveryNormalise = p.normalise
        win.deliveryTargetLufs = p.targetLufs
        win.deliveryCeilingDbTp = p.ceilingDbTp
        win.deliveryPeakTargetDb = p.peakTargetDb
        // A format this libsndfile cannot write falls back to WAV, and says so.
        var ok = false
        const formats = service.exportFormats
        for (var i = 0; i < formats.length; ++i) {
            if (formats[i].id === p.format && formats[i].available)
                ok = true
        }
        if (!ok) {
            win.deliveryFormat = "wav"
            win.deliveryBitDepth = p.bitDepth === 32 ? 32 : 24
            win.deliveryBitrateKbps = 0
            statusMessage = qsTr("%1 cannot be written here; the preset falls back to WAV.").arg(p.format.toUpperCase())
        }
        win.deliveryPreset = id
    }
    // ── spectral (Phase 4) ──────────────────────────────────────────────
    // How the Spectral page analyses (the engine's STFT; a change rebuilds the
    // cache) and draws (the shader's; a change costs nothing), plus the tool
    // in hand and the attenuation amount. All persisted.
    property string spectralTool: "rect"
    property string spectralScale: "log"
    property int spectralRange: 60
    property real spectralGain: 0
    property string spectralPalette: "navy"
    property real spectralAttenuateDb: -24
    property real spectralDenoiseDb: 12
    property real spectralDenoiseStrength: 1.0
    property real spectralClickSensitivity: 0.5
    property int spectralFftSize: 2048
    property string spectralWindow: "hann"
    property int spectralOverlap: 4
    // The drawing tools: brush radius in pixels, the mask's feather in cells
    // (bins and columns), and whether a new shape adds or cuts (not saved: a
    // session starts adding).
    property real spectralBrushSize: 24
    property int spectralFeather: 2
    // What a new shape does to the mask: "add", "cut" (X) or "keep" (Ctrl+Alt+X,
    // A25 -- only what the mask already had inside the shape stays).
    property string spectralMaskMode: "add"
    readonly property bool spectralMaskSubtract: spectralMaskMode === "cut"
    readonly property bool spectralMaskIntersect: spectralMaskMode === "keep"
    onSpectralFeatherChanged: if (service) service.maskFeather = win.spectralFeather
    // The harmonic tool (A24): how many partials a pick becomes, how wide each
    // band is (cents, or Hz), and whether the fundamental is in the series.
    property int spectralHarmonicPartials: 16
    property real spectralHarmonicWidth: 30
    property bool spectralHarmonicWidthInHz: false
    property bool spectralHarmonicFundamental: true
    property bool spectralFollowPitch: false
    // A pick on the display, or a click with the tool: the series from the
    // settings above. `fromSeconds >= toSeconds` leaves the span to the engine
    // (the time selection, or the whole take).
    function addHarmonics(fromSeconds, toSeconds, hz, subtract, intersect, seedSeconds) {
        if (!service)
            return
        service.addMaskHarmonics(fromSeconds, toSeconds, hz, win.spectralHarmonicPartials,
                                 win.spectralHarmonicWidth, win.spectralHarmonicWidthInHz,
                                 win.spectralHarmonicFundamental, subtract === true, true,
                                 intersect === true, win.spectralFollowPitch,
                                 seedSeconds === undefined ? -1 : seedSeconds)
    }
    // Mains hum as a comb over the selection (or the take): forty partials, three
    // hertz either side, the fundamental found from the spectrum near `hz`.
    function addHumComb(hz) {
        if (service)
            service.addMaskHarmonics(0, 0, hz, 40, 3, true, true, false, true)
    }
    Settings {
        id: spectralSettings
        category: "spectral"
        property alias tool: win.spectralTool
        property alias scale: win.spectralScale
        property alias range: win.spectralRange
        property alias gain: win.spectralGain
        property alias palette: win.spectralPalette
        property alias attenuateDb: win.spectralAttenuateDb
        property alias denoiseDb: win.spectralDenoiseDb
        property alias denoiseStrength: win.spectralDenoiseStrength
        property alias clickSensitivity: win.spectralClickSensitivity
        property alias fftSize: win.spectralFftSize
        property alias window: win.spectralWindow
        property alias overlap: win.spectralOverlap
        property alias brushSize: win.spectralBrushSize
        property alias feather: win.spectralFeather
        property alias harmonicPartials: win.spectralHarmonicPartials
        property alias harmonicWidth: win.spectralHarmonicWidth
        property alias harmonicWidthInHz: win.spectralHarmonicWidthInHz
        property alias harmonicFundamental: win.spectralHarmonicFundamental
        property alias followPitch: win.spectralFollowPitch
    }
    function setSpectralAnalysis(fftSize, window, overlap) {
        win.spectralFftSize = fftSize
        win.spectralWindow = window
        win.spectralOverlap = overlap
        if (service)
            service.setSpectralConfig(fftSize, window, overlap)
    }
    function syncPreferences() {
        shellSettings.sync()
        spectralSettings.sync()
        deliverySettings.sync()
    }

    // The extension the current delivery wants, for the file dialog.
    function deliveryExtension() {
        if (!service)
            return "wav"
        const formats = service.exportFormats
        for (var i = 0; i < formats.length; ++i) {
            if (formats[i].id === win.deliveryFormat)
                return formats[i].extension
        }
        return "wav"
    }
    function deliverySpec(path) {
        var spec = {
            path: path,
            format: win.deliveryFormat,
            bitDepth: win.deliveryBitDepth,
            bitrateKbps: win.deliveryBitrateKbps,
            sampleRate: win.deliverySampleRate,
            channels: win.deliveryChannels,
            dither: win.deliveryDither,
            normalise: win.deliveryNormalise,
            targetLufs: win.deliveryTargetLufs,
            ceilingDbTp: win.deliveryCeilingDbTp,
            peakTargetDb: win.deliveryPeakTargetDb,
            includeMetadata: win.deliveryIncludeMetadata,
            markers: win.deliveryMarkers,
            stems: win.exportScope === "selected",
            name: ""
        }
        if (win.exportScope === "selected")
            spec.tracks = win.exportTracks
        if (win.exportRange === "selection" && win.exportHasRange) {
            spec.start = Math.round(win.exportRangeStart)
            spec.end = Math.round(win.exportRangeEnd)
        }
        return spec
    }
    // A loudness or true-peak value for a label: one decimal, or -inf.
    function formatLufs(v, decimals) {
        if (v === undefined || v === null || v <= -100)
            return "−∞"
        return Number(v).toFixed(decimals === undefined ? 1 : decimals)
    }

    Binding { target: win.service; property: "snapping"; value: win.snapping }
    Binding { target: win.service; property: "snapGrid"; value: win.snapGrid }
    Binding { target: win.service; property: "ripple"; value: win.ripple }
    // The snap tolerance is a few pixels at whatever the zoom is.
    Binding {
        target: win.service; property: "snapToleranceFrames"
        value: Math.round(sharedAxis.framesPerPixel * Theme.snapPixels)
    }

    function openTrackEffect(track, slot) {
        if (!service) return
        service.focusTrack(track)
        effectsDockVisible = true
        workspace = "Multitrack"
        pendingEffectSlot = slot
        pendingEffectTrack = track
        showPendingEffect()
    }
    function showPendingEffect() {
        const page = multitrackLoader.item as MultitrackWorkspace
        if (!page || multitrackLoader.status !== Loader.Ready || pendingEffectSlot < 0) return
        const slot = pendingEffectSlot
        pendingEffectSlot = -1
        if (service && service.focusedTrack === pendingEffectTrack) page.showEffect(slot)
    }

    function resetLayout() {
        leftDockVisible = true
        effectsDockVisible = true
        mixerVisible = true
        leftDockWidth = Theme.wLeftDock
        rightDockWidth = Theme.wRightDock
        mixerWidth = Theme.wMixerDock
    }

    function openPluginPresets(track, slot) { pluginPresetsPanel.showFor(track, slot) }
    // One effect's controls in the middle of the window (EffectPanel): an effect
    // on a track or the bus (`chain` is the track id or service.masterTarget), or
    // on a clip. One at a time; opening another moves the panel to it.
    readonly property alias effectPanel: effectPanel
    // An effect just added from the browser opens its panel when it lands.
    property int addedEffectChain: -3
    property string addedEffectUri: ""
    function openAddedEffect() {
        if (win.addedEffectUri === "" || !win.service) return
        const chain = win.addedEffectChain
        const list = chain === win.service.masterTarget ? (win.service.masterInserts || [])
            : (win.service.trackById(chain).inserts || [])
        for (let i = list.length - 1; i >= 0; --i)
            if (list[i].uri === win.addedEffectUri) { win.addedEffectUri = ""; effectPanel.openFor(chain, i); return }
    }
    Timer {
        id: addedEffectWatch
        property int tries: 0
        interval: 50; repeat: true
        running: win.addedEffectUri !== ""
        onTriggered: { if (++tries > 60) win.addedEffectUri = ""; else win.openAddedEffect() }
    }
    function openEffectPanel(chain, slot) { effectPanel.openFor(chain, slot) }
    function openClipEffectPanel(track, clip, slot) { effectPanel.openForClip(track, clip, slot) }
    // A key a non-modal panel does not use itself, run through the keymap as if
    // the window had it, so shortcuts keep working while the panel has focus.
    function handleKey(event) {
        const action = keymap.actionFor(event, win.workspace)
        if (action === "") return false
        win.runAction(action)
        return true
    }

    function openRecentDocument(document, importing) {
        if (!service || !document || !document.path) return
        if (importing) {
            if (!document.project) service.importFile(document.path)
        } else {
            win.confirmDiscard(qsTr("open another document"), function() {
                if (document.project) service.loadProject(document.path)
                else service.open(document.path)
            })
        }
    }

    // The focused track's entry, for menus that show its state. Depends on
    // `tracks` so it re-reads when anything about it changes.
    readonly property var focusedTrackData:
        (service && service.tracksRevision >= 0 && service.focusedTrack >= 0)
        ? service.trackById(service.focusedTrack) : null

    function setFocusedRole(role) {
        if (service && service.focusedTrack >= 0)
            service.setTrackRole(service.focusedTrack, role)
    }
    function setFocusedHeight(height) {
        if (service && service.focusedTrack >= 0)
            service.setTrackHeight(service.focusedTrack, height)
    }
    function setFocusedColor(color) {
        if (service && service.focusedTrack >= 0)
            service.setTrackColor(service.focusedTrack, color)
    }

    // A position in project samples, as the chosen Time Display
    // writes it, for every list, read-out and field. Reading the setting and the
    // rate here makes any binding that calls this follow a change of either.
    function formatFrames(frames) {
        if (!service) return ""
        void service.timeDisplay; void service.sampleRate; void service.musicalTime
        return service.formatTime(Math.round(Number(frames) || 0))
    }
    function formatDuration(frames) {
        if (!service) return ""
        void service.timeDisplay; void service.sampleRate; void service.musicalTime
        return service.formatDuration(Math.round(Number(frames) || 0))
    }
    // A short length ("25.0 ms", or samples in the Samples display).
    function formatLength(frames) {
        if (!service) return ""
        void service.timeDisplay; void service.sampleRate; void service.musicalTime
        return service.formatShortLength(Math.round(Number(frames) || 0))
    }

    function about() {
        if (service)
            service.clearError()
        statusMessage = Theme.appName + " — " + qsTr("audio editing, mixing and mastering for Omarchy. Engine: PipeWire, libsndfile, lilv, RNNoise.")
    }

    // Closing the window asks the same question New Project does: a session with
    // unsaved changes is never dropped silently. (The recovery layer is written on
    // a dirty exit either way; this is about the person's own copy.)
    property bool closeConfirmed: false
    onClosing: function (close) {
        if (win.closeConfirmed || !service)
            return
        close.accepted = false
        win.confirmDiscard(qsTr("close OmaAudio"), function () {
            win.closeConfirmed = true
            Qt.callLater(function() { win.close() })
        })
    }

    function showControlHelp() { controlHelpSurface.showHelp() }
    function showGettingStarted() {
        gettingStartedDismissed = false
        controlHelpSurface.showGuides("start")
    }
    function showGuide(id) { controlHelpSurface.showGuides(id) }

    // Where the latest finished render landed, for the status bar's Show in folder.
    property string lastRenderPath: ""
    function say(text) {
        if (service) service.clearError()
        statusMessage = text
    }
    function showInFolder(path) {
        const slash = path.lastIndexOf("/")
        Qt.openUrlExternally("file://" + (slash > 0 ? path.substring(0, slash) : path))
    }

    HelpSurface {
        id: controlHelpSurface
        objectName: "controlHelpSurface"
        shell: win
        z: 1000
    }

    function showManual(section) { manualPanel.showSection(section || "") }
    function showKeymapHelp() {
        // The manual holds the whole table; the status line says where a
        // keymap of your own would go.
        manualPanel.showSection("Keyboard")
        say(qsTr("Shortcuts come from the keymap: %1 overrides the defaults compiled in. Every menu shows its binding.")
                .arg(service ? service.keymapFilePath() : "~/.config/omaaudio/keymap.toml"))
    }

    // A shell message that is not an engine error; the status bar shows whichever
    // is newer.
    property string statusMessage: ""

    // ── shared state the workspaces reach for ───────────────────────────
    readonly property alias keymap: keymap
    readonly property alias timeAxis: sharedAxis
    readonly property alias laneContextMenu: laneContextMenu
    readonly property alias masterContextMenu: masterContextMenu
    readonly property alias markerContextMenu: markerContextMenu
    // The waveform shader is compiled into the binary along with the QML.
    readonly property string shaderUrl: "qrc:/wave.frag.qsb"
    readonly property string spectroShaderUrl: "qrc:/spectro.frag.qsb"

    function bindingFor(action) {
        const b = keymap.bindings
        return (b && b[action]) ? b[action] : ""
    }

    // The net under every focus fix below: anything that takes focus and then
    // simply drops it lands on null, and null means no key handler and a keymap
    // that has gone silent. Catch the fall and give the keys back to the editor.
    // Deliberately only the null case: while something real holds focus (a menu,
    // the browser, a text field being typed in), it is entitled to it.
    //
    // Two landings count as "nobody": null, and one of the workspace Loaders. A
    // Loader is a focus scope, so when the lane-rename field inside it hides, focus
    // falls back to the Loader rather than to null -- and a Loader has no key
    // handler either. Found by the shell suite: rename a track, press Ctrl+A, nothing.
    onActiveFocusItemChanged: {
        // A window that is not active has no focus item at all; that is not the
        // stranded focus this net is for. Acting on it moved focus out of an
        // open dialog every time the window was left and returned to, after
        // which Escape no longer closed it and keys reached the project behind.
        if (!win.active)
            return
        if (activeFocusItem === null || (activeFocusItem instanceof Loader))
            content.forceActiveFocus()
    }
    function restoreFocus() { content.forceActiveFocus() }

    // The shared view. One axis for every lane is what makes a stack of tracks
    // read as one timeline (see TimeAxis.qml). Named `sharedAxis` rather than
    // `axis` deliberately: TrackLane declares a property called `axis`.
    TimeAxis {
        id: sharedAxis
        objectName: "timeAxis"
        totalFrames: win.service ? win.service.takeFrames : 0
    }

    // ── actions ─────────────────────────────────────────────────────────
    // Everything the user can do arrives here as a named action, from the keymap
    // or from a button. Nothing else tests for a key (BRIEF.md section 6).

    function runAction(name) {
        if (name === "show_control_help") { showControlHelp(); return }
        if (name === "show_getting_started") { showGettingStarted(); return }
        if (!service)
            return

        switch (name) {
        case "timeline_select":
            service.cancelClipMove()
            timelineMoveTool = false
            timelineSlipTool = false
            break
        case "timeline_move":
            service.cancelClipMove()
            timelineSlipTool = false
            timelineMoveTool = true
            break
        case "timeline_slip":
            service.cancelClipMove()
            timelineMoveTool = false
            timelineSlipTool = true
            break
        case "slip_earlier": service.slipFocusedClip(-Math.max(1, Math.round(service.sampleRate / 100))); break
        case "slip_later": service.slipFocusedClip(Math.max(1, Math.round(service.sampleRate / 100))); break
        case "crossfade_linear": service.crossfadeSelection(false); break
        case "crossfade_power": service.crossfadeSelection(true); break
        case "cancel_clip_move":
            service.cancelClipMove()
            break
        case "record":
            if (service.recording) service.stopRecording()
            else recordingPanel.open()
            break
        case "show_stretch": stretchPanel.open(); break
        case "show_crossfade": crossfadePanel.open(); break
        case "show_session_properties": sessionPanel.open(); break
        case "show_clip_properties": clipPropertiesPanel.open(); break
        case "show_clip_envelopes": clipEnvelopePanel.open(); break
        case "show_clip_effects": clipEffectsPanel.open(); break
        case "show_analysis": analysisPanel.openView(0); break
        case "show_frequency_analysis": analysisPanel.openView(1); break
        case "show_phase_analysis": analysisPanel.openView(2); break
        case "show_generate": generatePanel.open(); break
        case "show_speech_align": speechAlignPanel.open(); break
        case "show_audio_load": audioLoadPanel.open(); break
        case "show_remix": remixPanel.open(); break
        case "show_clip_groups": clipGroupsPanel.open(); break
        case "toggle_clip_group_link": service.setClipGroupsLinked(!service.clipGroupsLinked); break
        case "show_preferences": preferencesPanel.open(); break
        case "show_recent_documents": recentDocumentsPanel.open(); break
        case "show_track_loudness": trackLoudnessPanel.open(); break
        case "show_voice_cleanup": voiceCleanupPanel.open(); break
        case "show_essential_dynamics": essentialProcessorPanel.module = "dynamics"; essentialProcessorPanel.open(); break
        case "show_essential_gate": essentialProcessorPanel.module = "gate"; essentialProcessorPanel.open(); break
        case "show_essential_eq": essentialProcessorPanel.module = "eq"; essentialProcessorPanel.open(); break
        case "show_essential_clarity": essentialProcessorPanel.module = "clarity"; essentialProcessorPanel.open(); break
        case "show_essential_ducking": essentialProcessorPanel.module = "ducking"; essentialProcessorPanel.open(); break
        case "show_essential_deesser": essentialProcessorPanel.module = "deesser"; essentialProcessorPanel.open(); break
        case "show_essential_pitch": essentialProcessorPanel.module = "pitch"; essentialProcessorPanel.open(); break
        case "show_storage": storagePanel.open(); break
        case "collect_project": collectProjectPanel.open(); break
        case "show_file_batch": fileBatchPanel.open(); break
        case "show_channel_map": channelMapPanel.open(); break
        case "show_insert_silence": audioEditPanel.operation = "silence"; audioEditPanel.open(); break
        case "show_mix_paste": audioEditPanel.operation = "mix"; audioEditPanel.open(); break
        case "show_automation": automationPanel.open(); break
        case "show_generated_ducking": generatedDuckingPanel.open(); break
        case "show_routing": routingPanel.open(); break
        case "show_recording": recordingPanel.open(); break
        case "show_audio_setup": audioSetupPanel.open(); break
        case "show_metadata": metadataPanel.open(); break
        case "show_manual": manualPanel.open(); break
        case "show_video": win.effectsDockVisible = true; win.rackTabRequested(4); break
        case "show_recipes":
            // The Recipes tab is in the track dock, which the Spectral and
            // Mastering pages do not have.
            if (win.workspace === "Spectral") win.workspace = "Waveform"
            else if (win.workspace === "Mastering") win.workspace = "Multitrack"
            win.showRecipesTab()
            break
        case "show_tidy_pauses":
            // A page of the Sound tab, in the track dock, which the Spectral and
            // Mastering pages do not have. It looks in the selection when there is one.
            if (win.workspace === "Spectral") win.workspace = "Waveform"
            else if (win.workspace === "Mastering") win.workspace = "Multitrack"
            win.tidyPausesScope = ""
            win.tidyPausesScope = !service.selectionEmpty ? "selection" : "voices"
            win.tidyPausesOpen = true
            win.effectsDockVisible = true
            win.rackTabRequested(1)
            break
        case "import_video": videoDialog.open(); break
        case "video_step_forward": service.videoStep(1); break
        case "video_step_back": service.videoStep(-1); break
        case "start_recording": service.startRecording(); break
        case "refresh_recording_inputs": service.refreshRecordingInputs(); break
        case "record_new_track": service.setRecordArmedTrack(-1); break
        case "toggle_record_punch": service.setRecordPunch(!service.recordPunch); break
        case "toggle_record_monitor": service.setRecordMonitor(!service.recordMonitor); break
        case "toggle_loop":
            service.toggleLoop()
            break
        case "toggle_metronome": service.metronome = !service.metronome; break
        case "play_stop":
            service.togglePlay()
            break
        case "stop":
            service.stop()
            break
        case "go_start":
            service.seekFrames(0)
            break
        case "go_end":
            service.seekFrames(service.takeFrames)
            break
        // J, K and L: backwards, stop and forwards. Pressed again in the same
        // direction, J and L double the speed, up to eight times. Where shuttle
        // is not allowed (writing automation), L plays and J steps back two seconds.
        case "shuttle_back":
            if (!service.shuttle(!service.playing || service.playSpeed > 0 ? -1 : Math.max(-8, service.playSpeed * 2)))
                service.seekFrames(Math.max(0, service.playhead - 2 * service.sampleRate))
            break
        case "shuttle_stop":
            if (service.playing) service.stop()
            break
        case "shuttle_play":
            if (!service.shuttle(!service.playing || service.playSpeed < 0 ? 1 : Math.min(8, service.playSpeed * 2)) && !service.playing)
                service.play()
            break
        case "nudge_back":
            service.seekFrames(service.playhead - service.sampleRate)
            break
        case "nudge_forward":
            service.seekFrames(service.playhead + service.sampleRate)
            break
        case "open_file":
            // Replaces the whole session, so it is one of the things that can lose work.
            win.confirmDiscard(qsTr("open another file"), function () {
                fileDialog.importing = false
                fileDialog.open()
            })
            break
        case "import_file":
            fileDialog.importing = true
            fileDialog.open()
            break
        case "import_session":
            sessionImportPanel.showFile("")
            break
        case "close_window":
            if (recordingPanel.visible) { recordingPanel.close(); break }
            if (service.clipMoving) {
                service.cancelClipMove()
                break
            }
            // Escape closes the innermost thing that is open, and only that.
            if (pluginBrowser.open) {
                pluginBrowser.open = false
                break
            }
            // Escape does not close an application window. With nothing open it
            // does nothing, which is the right amount.
            break

        // --- projects (Phase 4B) ---
        case "save":
            service.save()
            break
        case "save_as":
            saveDialog.open()
            break
        case "open_project":
            win.confirmDiscard(qsTr("open another project"), function () {
                projectDialog.open()
            })
            break
        case "new_project":
            win.confirmDiscard(qsTr("start a new project"), function () {
                service.newProject()
            })
            break
        case "toggle_autosave":
            win.autosaveSeconds = win.autosaveSeconds > 0 ? 0 : 30
            break
        case "autosave_now":
            service.autosaveNow()
            break

        // --- plugins (Phase 5) ---
        case "show_plugins":
            pluginBrowser.managerMode = false
            pluginBrowser.collection = "all"
            pluginBrowser.category = ""; pluginBrowser.author = ""
            pluginBrowser.target = -1
            pluginBrowser.open = !pluginBrowser.open
            break
        case "show_master_plugins":
            pluginBrowser.managerMode = false
            pluginBrowser.collection = "all"
            pluginBrowser.category = ""; pluginBrowser.author = ""
            pluginBrowser.target = service.masterTarget
            pluginBrowser.open = !pluginBrowser.open
            break
        case "show_plugin_manager":
            pluginBrowser.managerMode = true
            pluginBrowser.collection = "all"
            pluginBrowser.category = ""; pluginBrowser.author = ""
            pluginBrowser.open = true
            break
        case "show_plugin_favorites":
            pluginBrowser.managerMode = false
            pluginBrowser.collection = "favorites"
            pluginBrowser.category = ""; pluginBrowser.author = ""
            pluginBrowser.target = -1
            pluginBrowser.open = true
            break

        // --- mastering (Phase 3) ---
        case "analyse_master":
            service.analyseMaster()
            break
        case "preview_before":
            service.previewBefore = !service.previewBefore
            break
        case "hear_without_effects":
            service.hearWithoutEffects = !service.hearWithoutEffects
            break
        case "reset_loudness":
            service.resetLoudness()
            break
        case "export":
            // The delivery is whatever the Mastering page shows; the page comes
            // up so the render lands somewhere visible, and Export… there asks
            // what to export.
            if (!service.loaded && service.trackCount === 0)
                break
            win.captureExportContext()
            win.workspace = "Mastering"
            if (win.exportButton)
                win.openExportMenu()
            else
                win.exportMenuPending = true
            break
        case "export_mix":
        case "export_selected_tracks":
            if (!service.loaded && service.trackCount === 0)
                break
            if (!win.exportContextFresh)
                win.captureExportContext()
            if (name === "export_selected_tracks" && win.exportTracks.length === 0) {
                win.say(qsTr("Select the tracks to export first"))
                break
            }
            win.exportScope = name === "export_mix" ? "mix" : "selected"
            win.workspace = "Mastering"
            exportDialog.currentFile = win._suggestedExportFile()
            exportDialog.open()
            break
        case "show_rack":
            win.effectsDockVisible = !win.effectsDockVisible
            break
        case "repeat_effect":
            if (!service.repeatLastEffect()) win.say(service.lastError)
            break
        case "repeat_recipe":
            if (service.runLastRecipeAgain()) win.showRecipesTab()
            else win.say(service.lastError)
            break
        case "bypass_track_plugins": {
            // One key for the whole chain: A/B-ing a track against itself is the
            // thing you do constantly. Any slot still live means "bypass
            // everything"; only when they are all bypassed does it switch them
            // all back on.
            const track = service.focusedTrack >= 0 ? service.trackById(service.focusedTrack) : null
            const inserts = track && track.inserts ? track.inserts : []
            if (inserts.length === 0)
                break
            var anyLive = false
            for (var i = 0; i < inserts.length; ++i) {
                if (!inserts[i].bypass)
                    anyLive = true
            }
            for (var j = 0; j < inserts.length; ++j)
                service.setPluginBypass(j, anyLive)
            break
        }

        // --- zoom (Phase 2) ---
        case "zoom_in":
            sharedAxis.zoomAt(sharedAxis.viewWidth / 2, 1 / 1.4)
            break
        case "zoom_out":
            sharedAxis.zoomAt(sharedAxis.viewWidth / 2, 1.4)
            break
        case "zoom_fit":
            sharedAxis.zoomToFit()
            break
        case "zoom_selection":
            if (!service.selectionEmpty) {
                sharedAxis.zoomToRange(service.selectionStart,
                                       service.selectionEnd - service.selectionStart)
            }
            break

        // --- selection (Phase 2) ---
        case "select_all":
            service.selectAll()
            break
        case "clear_selection":
            service.clearSelection()
            break
        // Q and W: the playhead to the selection's start or end (to the in point
        // kept by I with nothing selected yet).
        case "go_selection_start":
            if (!service.selectionEmpty) service.seekFrames(service.selectionStart)
            else if (win.markIn >= 0) service.seekFrames(win.markIn)
            break
        case "go_selection_end":
            if (!service.selectionEmpty) service.seekFrames(service.selectionEnd)
            break
        // `: the waveform fills the window (side panels and mixer hidden); again
        // puts back what was showing.
        case "fill_window":
            if (win.filledFrom) {
                win.leftDockVisible = win.filledFrom.left
                win.effectsDockVisible = win.filledFrom.right
                win.mixerVisible = win.filledFrom.mixer
                win.filledFrom = null
            } else {
                win.filledFrom = { left: win.leftDockVisible, right: win.effectsDockVisible, mixer: win.mixerVisible }
                win.leftDockVisible = false
                win.effectsDockVisible = false
                win.mixerVisible = false
            }
            break
        // In and out points. With nothing selected yet the in point is kept
        // until the out point arrives, so I then O (or [ then ]) selects the range.
        case "mark_in":
        case "select_start":
            win.markIn = service.playhead
            if (!service.selectionEmpty && service.playhead !== service.selectionEnd)
                service.select(Math.min(service.playhead, service.selectionEnd), Math.max(service.playhead, service.selectionEnd))
            break
        case "mark_out":
        case "select_end": {
            const from = !service.selectionEmpty ? service.selectionStart : win.markIn >= 0 ? win.markIn : service.playhead
            if (service.playhead !== from)
                service.select(Math.min(from, service.playhead), Math.max(from, service.playhead))
            win.markIn = -1
            break
        }

        // --- editing (Phase 3) ---
        // Each of these is a no-op in the engine when its preconditions are not
        // met, and reports why through the error path, so the UI does not
        // duplicate the rules about what is editable. Clipboard and delete actions
        // use the selected tracks; processing and clip controls use the focus.
        case "cut":
            service.cut()
            break
        case "copy":
            service.copy()
            break
        case "paste":
            service.paste(service.playhead)
            break
        case "delete_selection":
            service.deleteSelection()
            break
        case "delete_close_gap":
            service.deleteClosingGap()
            break
        case "delete_time_all_tracks":
            service.deleteTimeAllTracks()
            break
        case "trim":
            service.trim()
            break
        case "split":
            service.split(service.playhead)
            break
        case "fade_in":
            service.fadeIn()
            break
        case "fade_out":
            service.fadeOut()
            break
        case "silence":
            service.silenceSelection()
            break
        case "reverse": service.reverseSelection(); break
        case "convert_mono": service.convertTrackChannels(1); break
        case "convert_stereo": service.convertTrackChannels(2); break
        case "cancel_sample_edit": service.cancelSampleEdit(); break
        case "normalize":
            service.normalizeSelection(-1.0)
            break
        case "paste_to_new":
            service.pasteToNewTrack()
            break
        case "gain_up":
            service.gainSelection(1.0)
            break
        case "gain_down":
            service.gainSelection(-1.0)
            break
        case "undo":
            service.undo()
            break
        case "redo":
            service.redo()
            break

        // --- tracks (Phase 4) ---
        case "add_track":
            service.addTrack()
            break
        case "remove_track":
            if (service.focusedTrack >= 0)
                service.removeSelectedTracks()
            break
        case "focus_next_track":
            service.focusNextTrack(1)
            break
        case "focus_prev_track":
            service.focusNextTrack(-1)
            break
        case "toggle_mute":
            if (service.focusedTrack >= 0)
                service.toggleMute(service.focusedTrack)
            break
        case "toggle_solo":
            if (service.focusedTrack >= 0)
                service.toggleSolo(service.focusedTrack)
            break
        case "move_track_up":
            win._moveFocused(-1)
            break
        case "move_track_down":
            win._moveFocused(1)
            break

        // --- Phase 1: markers, modes, heights ---
        case "add_marker":
            // With a selection, a range over it; otherwise a cue at the playhead.
            if (!service.loaded)
                break
            if (!service.selectionEmpty)
                service.addRangeMarker(service.selectionStart, service.selectionEnd)
            else
                service.addMarker(service.playhead)
            break
        case "next_marker":
            service.jumpToMarker(1)
            break
        case "prev_marker":
            service.jumpToMarker(-1)
            break
        case "toggle_snap":
            win.snapping = !win.snapping
            break
        case "toggle_ripple":
            win.ripple = !win.ripple
            break
        case "show_mixer":
            win.mixerVisible = !win.mixerVisible
            break
        case "track_taller":
            if (service.focusedTrack >= 0)
                service.stepTrackHeight(service.focusedTrack, 1)
            break
        case "track_shorter":
            if (service.focusedTrack >= 0)
                service.stepTrackHeight(service.focusedTrack, -1)
            break
        case "duplicate_track":
            if (service.focusedTrack >= 0)
                service.duplicateTrack(service.focusedTrack)
            break

        // --- spectral (Phase 4) ---
        case "spectral_tool_time":
            win.spectralTool = "time"
            break
        case "spectral_tool_rect":
            win.spectralTool = "rect"
            break
        case "spectral_tool_band":
            win.spectralTool = "band"
            break
        case "spectral_clear_band":
            service.clearBand()
            break
        case "spectral_attenuate":
            service.attenuateSpectral(win.spectralAttenuateDb)
            break
        case "spectral_heal":
            service.healSpectral()
            break
        case "spectral_rebuild":
            service.rebuildSpectral()
            break
        case "spectral_capture_print":
            service.captureNoisePrint()
            break
        case "spectral_denoise":
            service.denoiseSpectral(win.spectralDenoiseDb, win.spectralDenoiseStrength)
            break
        case "spectral_find_clicks":
            service.detectClicks(win.spectralClickSensitivity)
            break
        case "spectral_heal_clicks":
            service.healClicks()
            break
        case "spectral_auto_heal":
            service.autoHealClicks(win.spectralClickSensitivity)
            break
        case "spectral_scale":
            win.spectralScale = win.spectralScale === "log" ? "linear" : "log"
            break
        case "spectral_tool_lasso":
            win.spectralTool = "lasso"
            break
        case "spectral_tool_brush":
            win.spectralTool = "brush"
            break
        case "spectral_tool_spot":
            win.spectralTool = "spot"
            break
        case "spectral_tool_harmonic":
            win.spectralTool = "harmonic"
            break
        case "spectral_hum_50":
            win.addHumComb(50)
            break
        case "spectral_hum_60":
            win.addHumComb(60)
            break
        case "spectral_mask_subtract":
            win.spectralMaskMode = win.spectralMaskMode === "cut" ? "add" : "cut"
            break
        case "spectral_mask_intersect":
            win.spectralMaskMode = win.spectralMaskMode === "keep" ? "add" : "keep"
            break
        case "spectral_mask_undo":
            service.undoMaskShape()
            break
        case "spectral_clear_mask":
            service.clearMask()
            break
        case "spectral_brush_larger":
            win.spectralBrushSize = Math.min(200, Math.round(win.spectralBrushSize * 1.25 + 1))
            break
        case "spectral_brush_smaller":
            win.spectralBrushSize = Math.max(2, Math.round(win.spectralBrushSize / 1.25 - 1))
            break

        // --- workspaces ---
        case "workspace_waveform":
            win.workspace = "Waveform"
            break
        case "workspace_multitrack":
            win.workspace = "Multitrack"
            break
        case "workspace_spectral":
            win.workspace = "Spectral"
            break
        case "workspace_mastering":
            win.workspace = "Mastering"
            break
        }
    }

    // Runs `action` immediately when there is nothing to lose, and otherwise
    // asks. The prompt never appears for a session with no unsaved changes,
    // which is what keeps it from becoming something people learn to dismiss
    // without reading.
    property var pendingDiscardAction: null
    property var pendingDocumentCheck: null
    property double documentCheckRequest: 0

    property var afterRecordingAction: null
    function confirmDiscard(what, action) {
        if (service && service.recording) {
            afterRecordingAction = function () { win.confirmDiscard(what, action) }
            service.stopRecording()
            return
        }
        if (!service) {
            action()
            return
        }
        // A changing readout is not a confirmed edit. Check its saved state at
        // this boundary before deciding whether the document can be replaced.
        pendingDocumentCheck = { what: what, action: action }
        documentCheckRequest += 1
        service.prepareDocumentChange(documentCheckRequest)
    }

    function _confirmDiscardReady(what, action, changes) {
        if (!changes) {
            action()
            return
        }
        discardPrompt.what = what
        win.pendingDiscardAction = action
        discardPrompt.open = true
    }

    function _resolveDiscard(choice) {
        const action = win.pendingDiscardAction
        win.pendingDiscardAction = null
        discardPrompt.open = false

        if (choice === "cancel" || !action)
            return
        if (choice === "save") {
            // Saving is asynchronous, so the action waits for the engine to
            // confirm rather than racing it.
            win.afterSaveAction = action
            service.save()
            return
        }
        action()
    }

    property var afterSaveAction: null

    function _moveFocused(delta) {
        if (!service || service.focusedTrack < 0)
            return
        const index = service.trackIndexById(service.focusedTrack)
        if (index < 0)
            return
        const to = index + delta
        if (to < 0 || to >= service.trackCount)
            return
        service.moveTrack(service.focusedTrack, to)
    }

    Keymap {
        id: keymap
        objectName: "keymap"
    }

    Connections {
        target: win.service

        function onProjectSampleSettingsChanged() {
            win.deliveryBitDepth = win.service.defaultExportBits
            win.deliverySampleRate = 0
            win.deliveryPreset = "custom"
        }

        function onRecordingChanged() {
            if (win.afterRecordingAction && win.service && !win.service.recording) {
                const action = win.afterRecordingAction
                win.afterRecordingAction = null
                Qt.callLater(action)
            }
        }

        // Ctrl+S on a session that has never been saved: the engine says it
        // needs a path rather than guessing one.
        function onPathNeeded() { saveDialog.open() }

        function onTimelineChanged() {
            if (win.afterSaveAction && win.service && !win.service.saving && !win.service.dirty) {
                const action = win.afterSaveAction
                win.afterSaveAction = null
                win.confirmDiscard(qsTr("continue after saving"), action)
            }
        }

        function onDocumentChangePrepared(request, succeeded, changes) {
            if (request !== win.documentCheckRequest || !win.pendingDocumentCheck)
                return
            const pending = win.pendingDocumentCheck
            win.pendingDocumentCheck = null
            if (succeeded)
                win._confirmDiscardReady(pending.what, pending.action, changes)
        }

        // A failed save abandons the action it was queued for. A new error also
        // outranks whatever the shell last said: the status bar shows the newer one.
        function onLastErrorChanged() {
            if (win.service && win.service.lastError !== "") {
                win.afterSaveAction = null
                win.statusMessage = ""
            }
        }

        // A file that read with a reservation (truncated, damaged, no gapless data).
        function onImportWarning(text) { win.say(text) }
        // Not an error: an edit that left the audio as it was, for one.
        function onNotice(text) { win.say(text) }

        // The result of a render, in words, with where it went.
        function onRenderFinished(id, state, path, message) {
            const name = path.substring(path.lastIndexOf("/") + 1)
            if (state === "done") {
                win.lastRenderPath = path
                win.say(qsTr("Exported %1 to %2").arg(name).arg(path.substring(0, path.lastIndexOf("/"))))
            } else if (state === "cancelled") {
                win.say(qsTr("Export of %1 cancelled; nothing was written").arg(name))
            } else {
                win.statusMessage = ""   // the engine's error carries the reason
            }
        }
    }

    function _suggestedExportFile() {
        return service ? service.suggestedExportUrl(win.deliveryFormat) : ""
    }

    // ── dialogs ─────────────────────────────────────────────────────────
    // Routed through the XDG portal like every other app on the machine.
    FileDialog {
        id: exportDialog
        objectName: "exportDialog"
        title: win.exportScope === "selected" ? qsTr("Export the selected tracks named after") : qsTr("Export the mix to")
        fileMode: FileDialog.SaveFile
        defaultSuffix: win.deliveryExtension()
        nameFilters: [qsTr("%1 files (*.%2)").arg(win.deliveryFormat.toUpperCase()).arg(win.deliveryExtension()),
                      qsTr("All files (*)")]
        onAccepted: {
            if (!win.service)
                return
            const id = win.service.queueRender(win.deliverySpec(selectedFile))
            if (id > 0)
                win.say(win.exportScope === "selected" ? qsTr("Stems queued; the Queue tab on the Mastering page shows each track's progress")
                                          : qsTr("Export %1 queued; the Queue tab on the Mastering page shows its progress").arg(id))
        }
        // It opens from the export menu, whose focus is already gone by the time
        // the dialog closes; without this the keys land on nothing afterwards.
        onVisibleChanged: if (!visible) { win.exportContextFresh = false; win.restoreFocus() }
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Save project as")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "omaaudio"
        nameFilters: [qsTr("OmaAudio projects (*.omaaudio *.omawave)"), qsTr("All files (*)")]
        onAccepted: {
            if (win.service)
                win.service.saveAs(selectedFile)
        }
        onRejected: win.afterSaveAction = null
    }

    FolderDialog {
        id: projectDialog
        title: qsTr("Open an OmaAudio project")
        onAccepted: {
            if (win.service)
                win.service.loadProject(selectedFolder)
        }
    }

    // The right dock's tab, asked for by an action (the docks are per workspace):
    // 0 Effects, 1 Sound, 2 Recipes, 3 Inspector, 4 Video.
    signal rackTabRequested(int tab)

    // A recipe picked from Effects › Recipes runs on the selection when there
    // is one, else on the whole active track; the Recipes tab shows the run.
    function runRecipe(id) {
        if (!service) return
        const target = service.recipeTarget("selection").valid ? "selection" : "track"
        if (service.runRecipe(id, target)) win.showRecipesTab()
        else win.say(service.lastError)
    }
    // The track dock on its Recipes tab, where a run shows its steps; on a page
    // without that dock the run still goes ahead.
    function showRecipesTab() {
        win.effectsDockVisible = true
        win.rackTabRequested(2)
    }
    // Process Many Files with Recipe chosen as its effects ("" keeps the
    // recipe it last had, or the first).
    function processFilesWithRecipe(id) {
        fileBatchPanel.chooseRecipe(id)
        fileBatchPanel.open()
    }
    // The effect browser, to choose an effect rather than add it (a recipe
    // step). `chosen` is called with the effect's URI.
    property var effectPicked: null
    function pickEffect(chosen) {
        win.effectPicked = chosen
        pluginBrowser.managerMode = false
        pluginBrowser.collection = "all"
        pluginBrowser.category = ""; pluginBrowser.author = ""
        pluginBrowser.picking = true
        pluginBrowser.open = true
    }

    FileDialog {
        id: videoDialog
        // Relinking keeps the picture's place; importing places it afresh.
        property bool relinking: false
        title: relinking ? qsTr("Relink the reference picture") : qsTr("Import a reference picture")
        nameFilters: [
            qsTr("Video files (*.mp4 *.mov *.mkv *.webm *.avi *.mxf *.m4v *.mts *.m2ts *.ts *.mpg *.mpeg *.wmv *.nut)"),
            qsTr("All files (*)")
        ]
        onAccepted: {
            if (!win.service)
                return
            if (relinking) win.service.relinkVideo(selectedFile)
            else win.service.importVideo(selectedFile)
            win.effectsDockVisible = true
            win.rackTabRequested(4)
        }
    }
    function importVideo() { videoDialog.relinking = false; videoDialog.open() }
    function relinkVideo() { videoDialog.relinking = true; videoDialog.open() }

    SessionImportPanel { id: sessionImportPanel; shell: win }
    function showSessionImport(path) { sessionImportPanel.showFile(path) }

    FileDialog {
        id: fileDialog
        objectName: "openFileDialog"
        // Set by whichever action opened it: replace the project, or add a track.
        property bool importing: false

        title: importing ? qsTr("Import audio or an AAF/OMF session") : qsTr("Open audio")
        // Importing takes several files at once (stems): each becomes its own track,
        // queued like files dropped on the window.
        fileMode: importing ? FileDialog.OpenFiles : FileDialog.OpenFile
        nameFilters: [
            importing ? qsTr("Audio and sessions (*.wav *.flac *.aiff *.aif *.ogg *.opus *.mp3 *.m4a *.aac *.mp4 *.m4b *.w64 *.caf *.mxf *.mov *.aaf *.AAF *.omf *.OMF)")
                      : qsTr("Audio files (*.wav *.flac *.aiff *.aif *.ogg *.opus *.mp3 *.m4a *.aac *.mp4 *.m4b *.w64 *.caf)"),
            qsTr("All files (*)")
        ]
        onAccepted: win.acceptChosenFiles(importing, selectedFiles)
    }

    // ── chrome ──────────────────────────────────────────────────────────
    AppTitleBar {
        id: titleBar
        objectName: "titleBar"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        target: win
        shell: win
        workspace: win.workspace
        onWorkspaceActivated: function (name) { win.workspace = name }
    }

    GlobalTransport {
        id: transport
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        shell: win
    }

    // The workspace host. `content` is the one item with the key handler; every
    // menu and popup hands focus back here on close (see AppMenu).
    // Audio files dropped from a file manager: the first opens an empty session,
    // the rest (and any drop onto a session) come in as tracks.
    readonly property var audioExtensions: ["wav", "flac", "aiff", "aif", "ogg", "opus", "mp3", "m4a", "aac", "mp4", "m4b", "w64", "caf", "mxf", "mov"]
    // The Open/Import dialog's choice: one file opens a project; importing adds
    // every chosen audio file as a track, or one AAF/OMF session.
    function acceptChosenFiles(importing, files) {
        if (!service || !files || files.length === 0) return 0
        if (!importing) { service.openFile(files[0]); return 1 }
        if (files.length === 1 && /\.(aaf|omf)$/.test(String(files[0]).toLowerCase())) { showSessionImport(files[0]); return 1 }
        const accepted = dropFiles(files)
        if (accepted === 0) say(qsTr("Choose audio files to add as tracks, or a single AAF or OMF session."))
        return accepted
    }
    function dropFiles(urls) {
        if (!service) return 0
        for (var index = 0; index < urls.length; ++index) {
            if (String(urls[index]).startsWith("file:") && /\.(aaf|omf)$/.test(String(urls[index]).toLowerCase())) {
                if (urls.length !== 1) { win.say(qsTr("Drop one AAF or OMF session at a time; import audio files separately.")); return 0 }
                win.showSessionImport(urls[index])
                return 1
            }
        }
        var accepted = 0
        for (var i = 0; i < urls.length; ++i) {
            const url = String(urls[i])
            if (!url.startsWith("file:")) continue
            const dot = url.lastIndexOf(".")
            const ext = dot >= 0 ? url.substring(dot + 1).toLowerCase() : ""
            if (audioExtensions.indexOf(ext) < 0) continue
            pendingDrops.push(url)
            ++accepted
        }
        drainDrops()
        return accepted
    }
    // Imports queue behind the file being read: the engine takes one at a time.
    property var pendingDrops: []
    function drainDrops() {
        if (!service || service.loading || pendingDrops.length === 0) return
        if (service.trackCount >= service.maxTracks) {
            // Full: say once how many files were left out, rather than refusing each.
            win.say(qsTr("%1 file(s) were not imported: OmaAudio plays up to %2 tracks.").arg(pendingDrops.length).arg(service.maxTracks))
            pendingDrops = []
            return
        }
        if (service.tryImportFile(pendingDrops[0], true)) pendingDrops.shift()
        if (pendingDrops.length > 0) dropRetry.restart()
    }
    Timer {
        id: dropRetry
        interval: 50
        onTriggered: win.drainDrops()
    }
    Connections {
        target: win.service
        function onLoadingChanged() { if (!win.service.loading) Qt.callLater(win.drainDrops) }
    }

    Item {
        id: content
        objectName: "content"
        anchors.top: transport.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: recipeStrip.top
        focus: true

        DropArea {
            anchors.fill: parent
            objectName: "audioDropArea"
            keys: ["text/uri-list"]
            onDropped: function (drop) {
                if (drop.hasUrls && win.dropFiles(drop.urls) > 0) drop.acceptProposedAction()
            }
        }

        // The only key handler in the shell, and it resolves through the keymap
        // rather than testing for any particular key.
        Keys.onPressed: function (event) {
            const action = keymap.actionFor(event, win.workspace)
            if (action !== "") {
                win.runAction(action)
                event.accepted = true
            }
        }

        // Create controls across frames, keeping transport and keyboard input live.
        // Inactive pages are still released instead of retaining four full UIs.
        Loader {
            anchors.fill: parent
            objectName: "loader_Waveform"
            id: waveformLoader
            asynchronous: true
            active: win.workspace === "Waveform"
            sourceComponent: WaveformWorkspace { shell: win }
        }
        Loader {
            anchors.fill: parent
            id: multitrackLoader
            objectName: "loader_Multitrack"
            asynchronous: true
            active: win.workspace === "Multitrack"
            sourceComponent: MultitrackWorkspace { shell: win }
            onLoaded: win.showPendingEffect()
        }
        Loader {
            anchors.fill: parent
            objectName: "loader_Spectral"
            id: spectralLoader
            asynchronous: true
            active: win.workspace === "Spectral"
            sourceComponent: SpectralWorkspace { shell: win }
        }
        Loader {
            anchors.fill: parent
            objectName: "loader_Mastering"
            id: masteringLoader
            asynchronous: true
            active: win.workspace === "Mastering"
            sourceComponent: MasteringWorkspace { shell: win }
        }
        Text {
            objectName: "workspaceLoadingMessage"
            anchors.centerIn: parent
            visible: win.workspaceLoading
            text: qsTr("Opening %1…").arg(win.workspace)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: Theme.textMuted
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    StretchPanel { id: stretchPanel; shell: win }
    CrossfadePanel { id: crossfadePanel; shell: win }
    SessionPanel { id: sessionPanel; shell: win }
    ClipPropertiesPanel { id: clipPropertiesPanel; shell: win }
    function openClipEnvelope(track, clip) { clipEnvelopePanel.openForClip(track, clip) }
    ClipEnvelopePanel { id: clipEnvelopePanel; shell: win }
    function openClipEffects(track, clip) { clipEffectsPanel.openForClip(track, clip) }
    // The effect browser adds to one clip, then returns to that clip's effects.
    function openClipEffectBrowser(track, clip, stamp) {
        pluginBrowser.target = -1
        pluginBrowser.clipTrack = track; pluginBrowser.clipIndex = clip; pluginBrowser.clipStamp = stamp
        pluginBrowser.open = true
    }
    function openClipEffectAutomation(track, clip, slot, port) {
        clipEffectsPanel.close(); clipEffectAutomationPanel.openFor(track, clip, slot, port)
    }
    ClipEffectsPanel { id: clipEffectsPanel; shell: win }
    AnalysisPanel { id: analysisPanel; shell: win }
    GeneratePanel { id: generatePanel; shell: win }
    SpeechAlignPanel { id: speechAlignPanel; shell: win }
    AudioLoadPanel { id: audioLoadPanel; shell: win }
    RemixPanel { id: remixPanel; shell: win }
    ClipEffectAutomationPanel { id: clipEffectAutomationPanel; shell: win }
    ClipGroupsPanel { id: clipGroupsPanel; shell: win }
    PreferencesPanel { id: preferencesPanel; shell: win }
    TrackLoudnessPanel { id: trackLoudnessPanel; shell: win }
    VoiceCleanupPanel { id: voiceCleanupPanel; shell: win }
    EffectApplyPanel { id: effectApplyPanel; shell: win }
    EffectPanel { id: effectPanel; shell: win }
    EssentialProcessorPanel { id: essentialProcessorPanel; shell: win }
    PluginPresetsPanel { id: pluginPresetsPanel; shell: win }
    RecentDocumentsPanel { id: recentDocumentsPanel; shell: win }
    StoragePanel { id: storagePanel; shell: win }
    ChannelMapPanel { id: channelMapPanel; shell: win }
    AudioEditPanel { id: audioEditPanel; shell: win }
    CollectProjectPanel { id: collectProjectPanel; shell: win }
    FileBatchPanel { id: fileBatchPanel; shell: win }
    function openGeneratedDucking(track) { generatedDuckingPanel.openForTrack(track) }
    function editGeneratedDucking(track) { automationPanel.openForTrack(track, "volume") }
    GeneratedDuckingPanel { id: generatedDuckingPanel; shell: win }
    AutomationPanel { id: automationPanel; shell: win }
    RoutingPanel { id: routingPanel; shell: win }
    RecordingPanel { id: recordingPanel; shell: win }
    AudioSetupPanel { id: audioSetupPanel; shell: win }
    MetadataPanel { id: metadataPanel; shell: win }
    ManualPanel { id: manualPanel; shell: win }

    // Record steps (Recipes tab): along the bottom while it is on.
    RecipeRecordingStrip {
        id: recipeStrip
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: statusBar.top
        shell: win
    }

    AudioStatusBar {
        id: statusBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        shell: win
        axis: sharedAxis
        workspace: win.workspace
    }

    // An effect picked by name from the Effects menu's groups. On the Waveform
    // page with a time selection it is applied to that audio, after a preview
    // (EffectApplyPanel); anywhere else it goes on the active track's chain, as
    // from the browser, and the rack opens on it. Meters change nothing, so they
    // always go on the track, where they can watch it.
    readonly property bool effectAppliesToSelection: win.workspace === "Waveform"
                                                     && service !== null && !service.selectionEmpty
    function effectApplies(entry) {
        return win.effectAppliesToSelection && !(entry && entry.category === "meters")
    }
    function chooseEffect(uri, entry) {
        if (!service)
            return
        if (win.effectApplies(entry)) {
            if (service.beginEffectPreview(uri))
                effectApplyPanel.open()
            else
                win.say(service.lastError)
            return
        }
        if (service.addPlugin(uri, -1))
            win.effectsDockVisible = true
    }
    function effectChoiceHelp(plugin, name) {
        const label = name || plugin.name
        const maker = plugin.author ? qsTr("%1, %2").arg(plugin.author).arg(plugin.format) : plugin.format
        return win.effectApplies(plugin)
            ? qsTr("Apply %1 to the selection: hear it and adjust it first (%2).").arg(label).arg(maker)
            : qsTr("Add %1 to the end of the active track's chain (%2).").arg(label).arg(maker)
    }

    // Export… asks what to export, every time: the mix as heard (Mute and Solo
    // count), or one stem per selected track (chosen, so played even if muted).
    function openExportMenu() {
        win.exportMenuPending = false
        const button = win.exportButton
        if (!button) return
        const height = exportMenu.implicitHeight > 0 ? exportMenu.implicitHeight : exportMenu.height
        exportMenu.popup(button, button.width - exportMenu.implicitWidth, -height - Theme.s1)
    }
    AppMenu {
        id: exportMenu
        objectName: "exportMenu"
        shell: win
        // How much of the project: all of it, or the time selection made before
        // Export… was chosen (picked for you when there was one). Choosing keeps
        // the menu open for the next choice.
        AppMenuItem {
            objectName: "exportRangeProject"
            text: qsTr("Whole project")
            detail: qsTr("From the start to the end of the last clip")
            checkable: true
            checked: win.exportRange === "project"
            helpText: qsTr("Export the whole project, from its start to the end of the last clip.")
            onTriggered: {
                win.exportRange = "project"
                checked = Qt.binding(function () { return win.exportRange === "project" })
                Qt.callLater(win.openExportMenu)
            }
        }
        AppMenuItem {
            objectName: "exportRangeSelection"
            text: qsTr("Time selection only")
            detail: win.exportHasRange ? qsTr("%1 to %2 (%3)").arg(win.formatFrames(win.exportRangeStart))
                                             .arg(win.formatFrames(win.exportRangeEnd)).arg(win.formatDuration(win.exportRangeEnd - win.exportRangeStart))
                                       : qsTr("No time selection")
            checkable: true
            checked: win.exportRange === "selection" && win.exportHasRange
            enabled: win.exportHasRange
            reason: qsTr("Select a time range first: drag across a lane or the waveform, then choose Export….")
            helpText: qsTr("Export only the selected time range, exactly from its start to its end.")
            helpScope: qsTr("Effects sound as they do at that point in the whole export, reverb carried in from before included; sound ringing past the end is cut off there.")
            onTriggered: {
                win.exportRange = "selection"
                checked = Qt.binding(function () { return win.exportRange === "selection" && win.exportHasRange })
                Qt.callLater(win.openExportMenu)
            }
        }
        MenuSep {}
        AppMenuItem {
            objectName: "exportMixItem"
            text: qsTr("Full mix"); actionName: "export_mix"
            detail: win.exportRange === "selection" && win.exportHasRange ? qsTr("All tracks as one file, the time selection only")
                                                                       : qsTr("All tracks as one file")
            helpText: qsTr("Write the mix as you hear it, through the master bus, to one file.")
            helpScope: qsTr("Muted tracks are left out; when tracks are soloed, only they are heard.")
        }
        AppMenuItem {
            objectName: "exportSelectedItem"
            readonly property int selectedCount: win.exportTracks.length
            text: qsTr("Selected tracks (%1)").arg(selectedCount); actionName: "export_selected_tracks"
            detail: win.exportRange === "selection" && win.exportHasRange ? qsTr("One file per track, the time selection only")
                                                                       : qsTr("One file per track")
            enabled: selectedCount > 0
            reason: qsTr("Select tracks first: Ctrl-click or Shift-click them in Tracks in the Mix or on the Arrange page.")
            helpText: qsTr("Write each selected track to its own file, named after the export and the track.")
            helpScope: qsTr("Each keeps its effects, fader and pan and the tracks routed into it, and plays even if muted; the master bus is left out.")
        }
    }

    // Right-click on a lane. The same named actions as the Edit menu -- nothing
    // here exists only here -- but reachable without leaving the waveform, and it
    // is where "delete, close the gap" was asked to live (A13).
    AppMenu {
        id: laneContextMenu
        objectName: "laneContextMenu"
        shell: win
        // Edits that need something picked first say so instead of doing nothing;
        // the track itself goes with Delete Track below.
        readonly property bool hasRange: win.service !== null && win.service.selectionEmpty === false
        readonly property bool hasPicks: hasRange || (win.service !== null && ((win.service.selectedClips || []).length > 0 || win.service.tracksPicked === true))
        readonly property string pickFirst: qsTr("Select a time range or clips first. To delete the whole track, choose Delete Track.")
        readonly property string rangeFirst: qsTr("Select a time range first: drag across the lane. To delete the whole track, choose Delete Track.")
        AppMenuItem { text: qsTr("Cut"); enabled: laneContextMenu.hasPicks; reason: laneContextMenu.pickFirst; actionName: "cut"; helpText: qsTr("Copy and remove the selected audio. Whole clips leave gaps; a time range closes its gap when Ripple Edits is on."); helpScope: qsTr("Uses the selected whole clips or the time range across selected tracks. Ctrl-click clips or track headers to choose several.") }
        AppMenuItem { text: qsTr("Copy"); enabled: laneContextMenu.hasPicks; reason: laneContextMenu.pickFirst; actionName: "copy"; helpText: qsTr("Copy the selected audio to the clipboard, leaving the track as it is."); helpScope: qsTr("Uses the selected whole clips or the time range across selected tracks. Ctrl-click clips or track headers to choose several.") }
        AppMenuItem { text: qsTr("Paste"); actionName: "paste"; helpText: qsTr("Paste at the playhead, starting on the focused track. Multiple copied tracks keep their spacing; Ripple Edits opens time on each destination."); helpScope: qsTr("Whole-clip paste preserves audio between copied clips; range paste replaces the full interval. One Undo step.") }
        MenuSep {}
        AppMenuItem { text: qsTr("Delete"); enabled: laneContextMenu.hasPicks; reason: laneContextMenu.pickFirst; actionName: "delete_selection"; helpText: qsTr("Remove selected whole clips, leaving gaps, or the selected time range. With track headers picked and no time range, removes those tracks. Ripple closes time-range gaps only."); helpScope: qsTr("Uses the selected whole clips or the time range across selected tracks. One Undo step.") }
        AppMenuItem { text: qsTr("Delete, Close the Gap"); enabled: laneContextMenu.hasRange; reason: laneContextMenu.rangeFirst; actionName: "delete_close_gap"; helpText: qsTr("Remove an explicit time range and pull later audio back to close the gap."); helpScope: qsTr("Acts on the selected tracks; other tracks keep their timing. One Undo step.") }
        AppMenuItem { text: qsTr("Silence"); enabled: laneContextMenu.hasRange; reason: laneContextMenu.rangeFirst; actionName: "silence"; helpText: qsTr("Replace the selected audio with silence, keeping its length."); helpScope: qsTr("Acts on the selected time range of the active track. One Undo step.") }
        MenuSep {}
        AppMenuItem { text: qsTr("Trim to Selection"); enabled: laneContextMenu.hasRange; reason: laneContextMenu.rangeFirst; actionName: "trim"; helpText: qsTr("Keep only the selected part of the track's audio."); helpScope: qsTr("One Undo step.") }
        AppMenuItem { text: qsTr("Normalize to −1 dBFS"); actionName: "normalize"; helpText: qsTr("Raise or lower the selection so its loudest sample reaches −1 dBFS, the level 1 dB below the maximum a file can hold."); helpScope: qsTr("One Undo step.") }
        AppMenuItem { text: qsTr("Split at Playhead"); actionName: "split"; helpText: qsTr("Cut the clip under the playhead into two clips at that point."); helpScope: qsTr("Nothing is removed; the two halves can then be moved apart. One Undo step.") }
        AppMenuItem { text: qsTr("Add Marker"); actionName: "add_marker"; helpText: qsTr("Put a marker at the playhead. Double-click a marker to name it."); helpScope: qsTr("Markers are project-wide and can be exported as cue points.") }
        MenuSep {}
        AppMenuItem { text: qsTr("Add Effect…"); actionName: "show_plugins"; helpText: qsTr("Open the effect browser to add an installed plugin to the active track."); helpScope: qsTr("The new insert goes at the end of the track's chain.") }
        MenuSep {}
        AppMenu {
            title: qsTr("Track Role"); shell: win
            Repeater {
                model: win.service ? win.service.trackRoles : []
                AppMenuItem {
                    required property string modelData
                    text: Theme.roleLabel(modelData)
                    checkable: true
                    checked: win.focusedTrackData ? win.focusedTrackData.role === modelData : false
                    helpText: qsTr("Give this track this role. A role sets the track's colour and says what it carries; the sound is not changed.")
                    onTriggered: win.setFocusedRole(modelData)
                }
            }
        }
        AppMenu {
            title: qsTr("Track Colour"); shell: win
            AppMenuItem {
                text: qsTr("Role's Colour"); checkable: true
                helpText: qsTr("Colour this track by its role, so it changes when the role does.")
                helpScope: qsTr("Display only: the lane, mixer strip and clips use this colour.")
                checked: win.focusedTrackData ? win.focusedTrackData.color === "" : true
                onTriggered: win.setFocusedColor("")
            }
            Repeater {
                model: Theme.swatches
                AppMenuItem {
                    required property color modelData
                    readonly property string hex: String(modelData).toUpperCase()
                    text: hex
                    checkable: true
                    checked: win.focusedTrackData ? win.focusedTrackData.color === hex : false
                    helpText: qsTr("Give this track a fixed colour instead of its role's colour.")
                    helpScope: qsTr("Display only: the lane, mixer strip and clips use it.")
                    onTriggered: win.setFocusedColor(hex)
                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.s5 + Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12; height: 12; radius: 6
                        color: parent.modelData
                    }
                }
            }
        }
        AppMenu {
            objectName: "laneMenuHeight"
            title: qsTr("Track Height"); shell: win
            Repeater {
                model: win.service ? win.service.trackHeights : []
                AppMenuItem {
                    required property string modelData
                    text: Theme.heightLabel(modelData)
                    checkable: true
                    checked: win.focusedTrackData ? win.focusedTrackData.height === modelData : false
                    helpText: qsTr("Show this track at this height. Display only, saved with the project.")
                    onTriggered: win.setFocusedHeight(modelData)
                }
            }
        }
        MenuSep {}
        AppMenuItem { text: qsTr("Duplicate Track"); actionName: "duplicate_track"; helpText: qsTr("Make a copy of the active track, clips, effects and automation included, below it."); helpScope: qsTr("One Undo step.") }
        AppMenuItem { text: qsTr("Delete Track"); actionName: "remove_track"; helpText: qsTr("Delete the selected tracks, or the active track, and their clips from the project."); helpScope: qsTr("Undo restores it.") }
    }

    // Right-click on the mixer's master strip: the master bus's effects, the
    // same chain the Mastering page shows.
    AppMenu {
        id: masterContextMenu
        objectName: "masterContextMenu"
        shell: win
        AppMenuItem {
            objectName: "masterMenuEffects"
            readonly property int count: win.service ? win.service.masterInserts.length : 0
            text: qsTr("Master Effects")
            detail: count === 0 ? qsTr("None yet") : count === 1 ? qsTr("1 effect") : qsTr("%1 effects").arg(count)
            helpText: qsTr("Show the master bus's effects in the Effects panel. Click an effect's name there to open its controls.")
            helpScope: qsTr("The master bus processes the whole mix in playback and exports.")
            onTriggered: win.showMasterEffects()
        }
        AppMenuItem {
            objectName: "masterMenuAdd"
            text: qsTr("Add Effect to the Master Bus…"); actionName: "show_master_plugins"
            helpText: qsTr("Open the effect browser to add an effect to the end of the master bus chain.")
            helpScope: qsTr("The master bus processes the whole mix in playback and exports.")
            onTriggered: win.showMasterEffects()
        }
        MenuSep {}
        AppMenuItem {
            objectName: "masterMenuMastering"
            text: qsTr("Go to the Mastering Page"); actionName: "workspace_mastering"
            helpText: qsTr("Open the Mastering page: the master bus with its loudness, delivery settings and export.")
        }
    }

    // Right-click on a marker flag. `popupFor` remembers which one.
    AppMenu {
        id: markerContextMenu
        objectName: "markerContextMenu"
        shell: win
        property int markerId: -1
        readonly property var marker: (win.service && win.service.markers && markerId >= 0)
                                      ? win.service.markerById(markerId) : null
        function popupFor(id) {
            markerId = id
            popup()
        }
        AppMenuItem {
            objectName: "markerMenuGoto"
            text: qsTr("Go to Marker")
            helpText: qsTr("Move the playhead to this marker.")
            onTriggered: if (markerContextMenu.marker && win.service)
                             win.service.seekFrames(markerContextMenu.marker.position)
        }
        AppMenuItem {
            objectName: "markerMenuSelectRange"
            text: qsTr("Select Range")
            helpText: qsTr("Select the time range this marker spans.")
            helpScope: qsTr("Only a range marker has a range.")
            enabled: markerContextMenu.marker !== null && markerContextMenu.marker.length > 0
            reason: qsTr("Only a range marker has a range")
            onTriggered: if (markerContextMenu.marker && win.service)
                             win.service.select(markerContextMenu.marker.position,
                                                markerContextMenu.marker.end, false)
        }
        MenuSep {}
        AppMenu {
            objectName: "markerTypeMenu"
            title: qsTr("Type"); shell: win
            Repeater {
                model: win.service ? win.service.markerTypes : []
                AppMenuItem {
                    required property string modelData
                    text: Theme.markerLabel(modelData)
                    checkable: true
                    checked: markerContextMenu.marker ? markerContextMenu.marker.type === modelData : false
                    helpText: qsTr("Make this a marker of this type. The type sets its colour and how it exports.")
                    onTriggered: if (win.service) win.service.setMarkerType(markerContextMenu.markerId, modelData)
                }
            }
        }
        MenuSep {}
        AppMenuItem {
            objectName: "markerMenuDelete"
            text: qsTr("Delete Marker")
            helpText: qsTr("Remove this marker from the project.")
            helpScope: qsTr("One Undo step.")
            onTriggered: if (win.service) win.service.removeMarker(markerContextMenu.markerId)
        }
    }

    // ── overlays ────────────────────────────────────────────────────────
    PluginBrowser {
        id: pluginBrowser
        objectName: "pluginBrowser"
        service: win.service

        // Adding from the browser opens the rack, because the next thing anybody
        // wants is the plugin's controls. A pick for the bus opens the page the
        // bus lives on.
        onClipEffectAdded: (track, clip) => win.openClipEffects(track, clip)
        onPicked: function (uri) {
            const chosen = win.effectPicked
            win.effectPicked = null
            if (chosen) chosen(uri)
        }
        onAdded: function (uri) {
            win.effectsDockVisible = true
            if (win.service && pluginBrowser.target === win.service.masterTarget)
                win.workspace = "Mastering"
            // ...and its controls, in the middle, once the effect is in the chain.
            if (win.service) {
                win.addedEffectChain = pluginBrowser.target === win.service.masterTarget ? win.service.masterTarget
                    : pluginBrowser.target >= 0 ? pluginBrowser.target : win.service.focusedTrack
                win.addedEffectUri = uri
                addedEffectWatch.tries = 0
                win.openAddedEffect()
            }
        }

        // The browser takes focus while open -- it filters as you type -- and
        // must give it back when it closes, or Escape out of it leaves the
        // keymap deaf.
        onOpenChanged: if (!open) content.forceActiveFocus()
    }

    // The unsaved-changes prompt. Modal by covering the window rather than by
    // opening a second one. Cancel is the default and the Escape key, because
    // the destructive choices are the ones that should need aiming at.
    Rectangle {
        id: discardPrompt
        objectName: "discardPrompt"
        property bool open: false
        property string what: ""

        anchors.fill: parent
        visible: open
        color: Theme.scrim

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: {}
        }

        Keys.onEscapePressed: win._resolveDiscard("cancel")
        focus: discardPrompt.open
        onOpenChanged: if (open) discardPrompt.forceActiveFocus()
                       else content.forceActiveFocus()

        Rectangle {
            anchors.centerIn: parent
            width: promptColumn.width + Theme.s5 * 2
            height: promptColumn.height + Theme.s5 * 2
            radius: Theme.rMenu
            color: Theme.panelBg
            border.width: Theme.hairline
            border.color: Theme.border
        }

        Column {
            id: promptColumn
            anchors.centerIn: parent
            spacing: Theme.s3
            width: Math.min(420, parent.width - Theme.s5 * 4)

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsTitle
                font.weight: Theme.wHeading
                text: {
                    const name = (win.service && win.service.projectName !== "")
                               ? win.service.projectName : qsTr("This project")
                    return qsTr("%1 has unsaved changes.").arg(name)
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl
                text: qsTr("Save before you %1?").arg(discardPrompt.what)
            }

            Row {
                anchors.right: parent.right
                spacing: Theme.s2

                component PromptButton: Rectangle {
                    property string label: ""
                    property string choice: ""
                    property bool loud: false
                    property bool danger: false
                    width: Math.max(88, promptLabel.implicitWidth + Theme.s4 * 2)
                    height: Theme.hControl + Theme.s1
                    radius: Theme.rControl
                    color: loud ? (hover.hovered ? Theme.hovered(Theme.accent) : Theme.accent)
                         : (hover.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg)
                    border.width: Theme.hairline
                    border.color: loud ? Theme.accent : danger ? Theme.danger : Theme.border
                    Text {
                        id: promptLabel
                        anchors.centerIn: parent
                        text: parent.label
                        color: parent.loud ? Theme.accentText : parent.danger ? Theme.danger : Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        font.weight: parent.loud ? Theme.wHeading : Theme.wNormal
                    }
                    HoverHandler { id: hover }
                    TapHandler { onTapped: win._resolveDiscard(parent.choice) }
                    Accessible.role: Accessible.Button
                    Accessible.name: label
                    ControlHelp {
                        title: parent.label
                        purpose: parent.choice === "save" ? qsTr("Save the project first, then carry on with what you asked for.")
                               : parent.choice === "discard" ? qsTr("Carry on without saving. The unsaved changes are lost.")
                               : qsTr("Stay in the project as it is.")
                    }
                }

                PromptButton { objectName: "promptCancel"; label: qsTr("Cancel"); choice: "cancel" }
                PromptButton { objectName: "promptDiscard"; label: qsTr("Discard"); choice: "discard"; danger: true }
                PromptButton { objectName: "promptSave"; label: qsTr("Save"); choice: "save"; loud: true }
            }
        }
    }

    // The recovery prompt (A22): once, at launch, when an earlier run left a dirty
    // session behind. Recover is the default and Enter; Discard needs aiming at.
    function _resolveRecovery(choice) {
        recoveryPrompt.open = false
        if (!service)
            return
        if (choice === "recover")
            service.recoverSession()
        else
            service.discardRecovery()
    }
    Rectangle {
        id: recoveryPrompt
        objectName: "recoveryPrompt"
        property bool open: false

        anchors.fill: parent
        visible: open
        color: Theme.scrim
        z: 60

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: {}
        }
        Keys.onEscapePressed: {}
        Keys.onReturnPressed: win._resolveRecovery("recover")
        Keys.onEnterPressed: win._resolveRecovery("recover")
        focus: recoveryPrompt.open
        onOpenChanged: if (open) recoveryPrompt.forceActiveFocus()
                       else content.forceActiveFocus()

        Rectangle {
            anchors.centerIn: parent
            width: recoveryColumn.width + Theme.s5 * 2
            height: recoveryColumn.height + Theme.s5 * 2
            radius: Theme.rMenu
            color: Theme.panelBg
            border.width: Theme.hairline
            border.color: Theme.border
        }
        Column {
            id: recoveryColumn
            anchors.centerIn: parent
            spacing: Theme.s3
            width: Math.min(440, parent.width - Theme.s5 * 4)

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsTitle
                font.weight: Theme.wHeading
                text: win.graphicsRestarted ? qsTr("Your project was preserved.") : qsTr("Unsaved work was found.")
            }
            Text {
                objectName: "recoveryPromptText"
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl
                text: {
                    if (!win.service)
                        return ""
                    const name = win.service.recoveryName !== "" ? win.service.recoveryName
                                                                 : qsTr("An unsaved project")
                    if (win.graphicsRestarted)
                        return qsTr("OmaAudio restarted in software mode after a graphics failure. Recover %1 to continue from the preserved project. Undo history is not restored.").arg(name)
                    return qsTr("%1 was autosaved at %2 and never saved. Recover it, or let it go?")
                        .arg(name).arg(win.service.recoveryWritten)
                }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.s2
                ToolButton {
                    objectName: "recoveryDiscard"
                    text: qsTr("Discard"); showLabel: true
                    helpText: qsTr("Let the autosaved project go and start with an empty one.")
                    helpScope: qsTr("The recovery copy is removed. A project that was saved is not affected.")
                    onClicked: win._resolveRecovery("discard")
                }
                ToolButton {
                    objectName: "recoveryRecover"
                    text: qsTr("Recover"); showLabel: true
                    helpText: qsTr("Open the autosaved project as it was when it was last written.")
                    helpScope: qsTr("Save it to keep it; it is only a recovery copy until then.")
                    onClicked: win._resolveRecovery("recover")
                }
            }
        }
    }

    // A name, asked for in place: the save-preset flow. `askName(title, initial,
    // done)` calls done(name) on Enter or the button, nothing on Escape.
    function askName(title, initial, done) {
        namePrompt.title = title
        namePrompt.done = done
        nameField.text = initial
        namePrompt.open = true
        nameField.selectAll()
    }
    Rectangle {
        id: namePrompt
        objectName: "namePrompt"
        property bool open: false
        property string title: ""
        property var done: null

        anchors.fill: parent
        visible: open
        color: Theme.scrim
        z: 50

        function finish(accept) {
            const cb = namePrompt.done
            const text = nameField.text
            namePrompt.open = false
            namePrompt.done = null
            if (accept && cb)
                cb(text)
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: {}
        }
        onOpenChanged: if (open) nameField.forceActiveFocus()
                       else content.forceActiveFocus()

        Rectangle {
            anchors.centerIn: parent
            width: nameColumn.width + Theme.s5 * 2
            height: nameColumn.height + Theme.s5 * 2
            radius: Theme.rMenu
            color: Theme.panelBg
            border.width: Theme.hairline
            border.color: Theme.border
        }
        Column {
            id: nameColumn
            anchors.centerIn: parent
            spacing: Theme.s3
            width: 320
            Text {
                width: parent.width
                text: namePrompt.title
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsTitle
                font.weight: Theme.wHeading
            }
            Rectangle {
                width: parent.width
                height: Theme.hControl + Theme.s1
                color: Theme.controlBg
                radius: Theme.rControl
                border.width: nameField.activeFocus ? Theme.focusRing : Theme.hairline
                border.color: nameField.activeFocus ? Theme.accent : Theme.border
                TextInput {
                    id: nameField
                    objectName: "namePromptField"
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s2
                    anchors.rightMargin: Theme.s2
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.textPrimary
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentText
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    clip: true
                    Keys.onReturnPressed: namePrompt.finish(true)
                    Keys.onEnterPressed: namePrompt.finish(true)
                    Keys.onEscapePressed: namePrompt.finish(false)
                }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.s2
                ToolButton {
                    objectName: "namePromptCancel"
                    text: qsTr("Cancel"); showLabel: true
                    helpText: qsTr("Close without saving anything.")
                    onClicked: namePrompt.finish(false)
                }
                ToolButton {
                    objectName: "namePromptSave"
                    text: qsTr("Save"); showLabel: true
                    helpText: qsTr("Save under the name typed above.")
                    unavailableReason: qsTr("Type a name first.")
                    enabled: nameField.text.trim() !== ""
                    onClicked: namePrompt.finish(true)
                }
            }
        }
    }

    Component.onCompleted: {
        // A settings file can name a page that does not exist (edited by hand, or
        // written by another version). No Loader would be active and the window
        // would open blank until a tab was clicked.
        if (["Waveform", "Multitrack", "Spectral", "Mastering"].indexOf(win.workspace) < 0)
            win.workspace = "Multitrack"
        content.forceActiveFocus()
        // The engine starts with its own defaults; the saved analysis settings win.
        if (service) {
            service.setSpectralConfig(win.spectralFftSize, win.spectralWindow, win.spectralOverlap)
            service.maskFeather = win.spectralFeather
            service.autosaveSeconds = win.autosaveSeconds
            service.automationReturnSeconds = win.automationReturnSeconds
            service.memoryMiB = win.memoryMiB
            service.spectralStoreGiB = win.spectralStoreGiB
            service.timeDisplay = win.timeDisplay
            win.timeDisplay = service.timeDisplay       // an unknown stored name becomes the default
            service.refreshSpectralStore()
            if (service.recoveryAvailable)
                recoveryPrompt.open = true
        }
    }
}
