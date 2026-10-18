pragma Singleton

import QtQuick
import OmaAudio.Desktop

// The single source of every colour, dimension, radius, duration and type
// size in OmaAudio. Nothing outside this file may contain a colour literal
// or a magic dimension. Palette is the shared Oma design system (the same
// family as OmaEdit and OmaPhoto) and is FIXED: an editing surface stays
// colour-stable whatever the desktop theme does, because a surround that
// changes hue changes how the work inside it reads, and here the meters,
// the loudness scale and the spectrogram ramp are measurements a person
// judges by eye.
//
// The one exception is the ACCENT, which follows the Omarchy theme
// (BRIEF.md amendment A93, amending A15). Buttons, selection and focus are
// chrome and belong to the desktop; the lanes, the meters, the audio family
// and the spectral palettes are exactly the colours they were.
QtObject {
    id: theme

    // ── Identity ────────────────────────────────────────────────────────────
    readonly property string appName: "OmaAudio"

    // A file the dialogs or a drop hand over as file:///…, shown and kept as the
    // ordinary path a person would type. Anything else is returned unchanged.
    function localPath(value) {
        const text = String(value || "")
        if (!text.startsWith("file://")) return text
        let path = text.slice(7)
        if (path.startsWith("localhost/")) path = path.slice(9)
        try { return decodeURIComponent(path) } catch (e) { return path }
    }

    // A name inside a sentence or a menu entry, cut in the middle so the start and
    // the end (a file's extension, a take number) both stay: a long file or track
    // name never stretches the line, the menu or the panel around it. A Text that
    // shows only a name elides itself instead, and its help card shows it whole.
    function shortName(name, max) {
        const text = String(name === undefined || name === null ? "" : name)
        const limit = Math.max(8, max || 40)
        if (text.length <= limit) return text
        const head = Math.ceil((limit - 1) * 0.6)
        return text.slice(0, head).replace(/\s+$/, "") + "…" + text.slice(text.length - (limit - 1 - head)).replace(/^\s+/, "")
    }

    // ── Colour ──────────────────────────────────────────────────────────────
    readonly property color windowBg: "#0B0E13"
    readonly property color panelBg: "#111722"
    readonly property color panelRaised: "#171E2A"
    readonly property color controlBg: "#1B2431"
    readonly property color controlHover: Qt.tint(controlBg, Qt.rgba(0.906, 0.929, 0.969, 0.10))   // = hovered(controlBg)
    readonly property color border: "#293241"
    readonly property color borderStrong: "#3A4658"

    readonly property color textPrimary: "#E7EDF7"
    readonly property color textSecondary: "#A7B1C1"
    readonly property color textMuted: "#727E90"

    // The desktop's accent when Omarchy is there, the Oma accent when it is not
    // (plain Arch, a container, CI) — and the app has to look finished either
    // way, so the fallback is the colour this always used.
    readonly property color omaAccent: "#27C2FF"
    readonly property color accent: OmarchyTheme.accentFollowed ? OmarchyTheme.accent : omaAccent
    // ⚠️ Derived, not a literal: the old #139BCF was a hand-picked shade of the
    // Oma cyan and means nothing against an arbitrary accent.
    readonly property color accentPressed: Qt.darker(accent, 1.35)
    readonly property color aiAccent: "#8B5CF6"
    readonly property color warning: "#FBBF24"
    readonly property color danger: "#EF4444"
    readonly property color success: "#34D399"

    // The audio family: what a waveform, a music bed, an effects clip and a
    // bus are drawn in. Never used for chrome.
    readonly property color audioPrimary: "#34D399"
    readonly property color audioSecondary: "#21B6B7"
    readonly property color music: "#2F9ED9"
    readonly property color effects: "#A66BE8"
    readonly property color bus: "#C05C9C"
    // Red is for the playhead and the record state only — never for selection.
    readonly property color playhead: "#FF3B45"
    readonly property color record: "#FF3B45"

    // Text that stays legible on top of a filled accent control.
    // ⚠️ Derived, not a literal: an accent can be pale gold or deep violet and
    // the label on top has to read on both, so the accent's Rec.709 luminance
    // decides whether that text is near-black or near-white.
    readonly property color accentText:
        (0.2126 * accent.r + 0.7152 * accent.g + 0.0722 * accent.b) > 0.55 ? "#04121A" : "#F2F7FF"
    readonly property color aiAccentText: "#F5F3FF"

    // Derived interaction states. Every control derives from these rather
    // than inventing its own hover or pressed colour.
    // A dark surface lifts toward the text colour on hover (10 %) and less when
    // pressed (4 %), so a press reads darker than the hover but still not the
    // surface itself; a light fill (the accent, a marker colour) brightens 22 %
    // on hover and darkens when pressed. (Qt.lighter by 6 % was invisible on
    // these near-black panels.)
    readonly property real hoverStrength: 0.10
    readonly property real pressStrength: 0.04
    function lightness(c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b }
    function hovered(base) {
        return lightness(base) > 0.45 ? Qt.tint(base, Qt.rgba(1, 1, 1, 0.22))
                                      : Qt.tint(base, Qt.rgba(textPrimary.r, textPrimary.g, textPrimary.b, hoverStrength))
    }
    function pressedOn(base) {
        return lightness(base) > 0.45 ? Qt.darker(base, 1.18)
                                      : Qt.tint(base, Qt.rgba(textPrimary.r, textPrimary.g, textPrimary.b, pressStrength))
    }
    // Over any surface (a list row, a menu item, a flat button on whatever is
    // behind it): a see-through lift that matches hovered().
    readonly property color hoverOverlay: Qt.rgba(textPrimary.r, textPrimary.g, textPrimary.b, hoverStrength)
    readonly property color pressOverlay: Qt.rgba(textPrimary.r, textPrimary.g, textPrimary.b, pressStrength)
    // The selected or checked row or tool: an accent tint, so it never reads
    // as a hover; hovering it deepens the tint.
    readonly property color selectedFill: Qt.rgba(accent.r, accent.g, accent.b, 0.18)
    readonly property color selectedHover: Qt.rgba(accent.r, accent.g, accent.b, 0.26)
    readonly property real disabledOpacity: 0.38
    readonly property color dropValid: accent
    readonly property color dropInvalid: danger

    // ── Editing surfaces ────────────────────────────────────────────────────
    // A lane's waveform well is the darkest value so the audio reads true.
    readonly property color laneBg: windowBg
    readonly property color laneFocusedBg: "#0E1420"
    // Under a spectrogram: the palette's floor, so an unbuilt lane and silence
    // read the same.
    readonly property color spectralBg: "#070B1E"
    readonly property color rulerBg: panelBg
    readonly property color rulerTick: textMuted
    readonly property color rulerText: textSecondary
    // Selected audio range: the accent as an outline, the same accent as a
    // translucent fill. ⚠️ The fill used to spell the Oma cyan out in floats,
    // which would leave it cyan under an orange outline once the accent moved.
    readonly property color selectionFill: withAlpha(accent, 0.16)
    readonly property color selectionOutline: accent
    // The spectral mask: what a repair will touch, and a cut out of it.
    readonly property color maskFill: Qt.rgba(0.98, 0.75, 0.14, 0.28)
    readonly property color maskOutline: "#FBBF24"
    readonly property color maskSubtractFill: Qt.rgba(0.95, 0.25, 0.25, 0.22)
    readonly property color maskSubtractOutline: "#F26363"
    // And a keep: what the mask is narrowed to.
    readonly property color maskKeepFill: Qt.rgba(0.2, 0.85, 0.72, 0.2)
    readonly property color maskKeepOutline: "#34D3B4"
    // Translucent scrims laid over the window.
    readonly property color scrim: Qt.rgba(0.043, 0.055, 0.075, 0.86)

    // Meters: unlit ground, green through amber to red, thin peak-hold.
    readonly property color meterBg: "#0A0D12"
    readonly property color meterLow: audioPrimary
    readonly property color meterMid: warning
    readonly property color meterHigh: danger
    readonly property color meterHold: textPrimary

    // Loudness: the short-term curve, the momentary trace under it, the target
    // line, and what is over the target or the ceiling.
    readonly property color loudnessShortTerm: audioSecondary
    readonly property color loudnessMomentary: withAlpha(audioPrimary, 0.45)
    readonly property color loudnessTarget: accent
    readonly property color loudnessOver: danger
    readonly property color loudnessBand: withAlpha(accent, 0.10)

    // Track roles, as a palette a header can pick from. The three voice roles
    // share the dialogue green, as the concept draws them; the family colours
    // are for what is not a voice.
    function roleColor(role) {
        switch (role) {
        case "music": return music
        case "sfx": case "foley": case "effects": return effects
        case "bus": case "master": case "aux": return bus
        case "ambience": return audioSecondary
        default: return audioPrimary
        }
    }
    // What a role is called in the UI. The model spells them lower-case.
    function roleLabel(role) {
        switch (role) {
        case "dialogue": return qsTr("Dialogue")
        case "host": return qsTr("Host")
        case "guest": return qsTr("Guest")
        case "music": return qsTr("Music")
        case "ambience": return qsTr("Ambience")
        case "sfx": return qsTr("SFX")
        case "foley": return qsTr("Foley")
        case "aux": return qsTr("Aux")
        case "bus": return qsTr("Bus")
        case "master": return qsTr("Master")
        default: return role
        }
    }
    // The track colour override palette: the audio family, then the two
    // chrome colours that still read on a dark lane.
    readonly property var swatches: [audioPrimary, audioSecondary, music, effects, bus,
                                     warning, accent, textSecondary]
    function withAlpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    // Spectral display palettes: five stops, darkest first, for the shader to
    // blend between. The default is the brief's deep navy through cyan to amber;
    // the others are for eyes that do not read that ramp (section 8: accessible
    // alternatives).
    function spectralPalette(name) {
        switch (name) {
        case "gray": return ["#05070B", "#2B3442", "#6B7787", "#B4BDCA", "#FFFFFF"]
        case "heat": return ["#05070B", "#5A0F1A", "#D63A1F", "#FBBF24", "#FFF6D5"]
        default: return ["#070B1E", "#12306B", "#1C86C9", "#27C2FF", "#FBBF24"]
        }
    }
    function spectralPaletteLabel(name) {
        switch (name) {
        case "gray": return qsTr("Greyscale")
        case "heat": return qsTr("Heat")
        default: return qsTr("Navy to amber")
        }
    }
    readonly property var spectralPalettes: ["navy", "gray", "heat"]
    // 1007.8 → "1.0 kHz", 12000 → "12 kHz", 250 → "250 Hz".
    function formatHz(hz) {
        if (hz >= 10000) return Math.round(hz / 1000) + " kHz"
        if (hz >= 1000) return (hz / 1000).toFixed(1) + " kHz"
        return Math.round(hz) + " Hz"
    }

    // Markers, by type. Warning is amber AND carries an icon (brief: never
    // colour alone).
    function markerColor(type) {
        switch (type) {
        case "range": return music
        case "chapter": return success
        case "warning": return warning
        default: return accent
        }
    }
    function markerIcon(type) {
        switch (type) {
        case "range": return "flag"
        case "chapter": return "tag"
        case "warning": return "triangle-alert"
        default: return "bookmark"
        }
    }
    function markerLabel(type) {
        switch (type) {
        case "range": return qsTr("Range")
        case "chapter": return qsTr("Chapter")
        case "warning": return qsTr("Warning")
        default: return qsTr("Cue")
        }
    }

    // ── Spacing ─────────────────────────────────────────────────────────────
    // The brief's scale: 4, 8, 12, 16, 24. Named by step so call sites read
    // as intent rather than arithmetic.
    readonly property int s1: Math.round(4 * densityScale)
    readonly property int s2: Math.round(8 * densityScale)
    readonly property int s3: Math.round(12 * densityScale)
    readonly property int s4: Math.round(16 * densityScale)
    readonly property int s5: Math.round(24 * densityScale)

    // ── Radius ──────────────────────────────────────────────────────────────
    // Controls 3, menus and dialogs 4, major docks square.
    readonly property int rControl: 3
    readonly property int rMenu: 4
    readonly property int rPanel: 0

    // ── Lines ───────────────────────────────────────────────────────────────
    readonly property int hairline: 1
    readonly property int focusRing: 1
    readonly property int selectionRing: 2
    readonly property int activeUnderline: 2
    readonly property int playheadWidth: 1

    // ── Density ─────────────────────────────────────────────────────────────
    // compact (default) | normal | comfortable. Scales spacing and control
    // heights only — never type size.
    property string density: "compact"
    readonly property real densityScale: density === "comfortable" ? 1.25
                                       : density === "normal" ? 1.12
                                       : 1.0

    // ── Type ────────────────────────────────────────────────────────────────
    // Inter when installed, Noto Sans otherwise.
    readonly property string fontFamily: {
        const families = Qt.fontFamilies()
        if (families.indexOf("Inter") >= 0) return "Inter"
        if (families.indexOf("Noto Sans") >= 0) return "Noto Sans"
        return "sans-serif"
    }
    // Numeric readouts want tabular digits so values do not jitter. Inter's
    // digits are tabular under the tnum feature, which every readout sets.
    readonly property string monoFamily: {
        const families = Qt.fontFamilies()
        if (families.indexOf("Inter") >= 0) return "Inter"
        if (families.indexOf("Noto Sans Mono") >= 0) return "Noto Sans Mono"
        return "monospace"
    }

    readonly property int fsBase: 12
    readonly property int fsLabel: 11
    readonly property int fsControl: 12
    readonly property int fsHeading: 12
    readonly property int fsTitle: 15
    readonly property int fsRuler: 9
    readonly property int fsTimecode: 24
    readonly property int fsTimecodeSmall: 14
    readonly property int wHeading: Font.DemiBold
    readonly property int wNormal: Font.Normal

    // ── Control metrics ─────────────────────────────────────────────────────
    readonly property int hTitleBar: 44
    readonly property int hTransport: 64
    readonly property int hToolbar: 36
    readonly property int hStatusBar: 30
    readonly property int hRuler: 22
    readonly property int hControl: Math.round(24 * densityScale)
    readonly property int hRow: Math.round(22 * densityScale)
    readonly property int hTab: Math.round(32 * densityScale)
    readonly property int hDockHeader: Math.round(32 * densityScale)
    readonly property int hEffectRow: Math.round(28 * densityScale)
    readonly property int hLaneMin: 72
    // The four lane heights the brief names. "normal" also stretches to share
    // the column when there is room; the others are exactly this.
    readonly property int hLaneMinimized: 28
    readonly property int hLaneCompact: 52
    readonly property int hLaneNormal: 96
    readonly property int hLaneExpanded: 176
    function laneHeight(name) {
        switch (name) {
        case "minimized": return hLaneMinimized
        case "compact": return hLaneCompact
        case "expanded": return hLaneExpanded
        default: return hLaneNormal
        }
    }
    function heightLabel(name) {
        switch (name) {
        case "minimized": return qsTr("Minimized")
        case "compact": return qsTr("Compact")
        case "expanded": return qsTr("Expanded")
        default: return qsTr("Normal")
        }
    }
    readonly property int hMarkerLane: 20
    // The Mastering page: the loudness history under the mix, the delivery bar
    // along the bottom, the meter column in the Loudness panel.
    readonly property int hLoudnessHistory: 110
    readonly property int hDeliveryBar: 132
    readonly property int hLoudnessMeter: 10
    readonly property int wLoudnessReadout: 64
    // The LUFS scale every loudness drawing shares: -42 at the bottom, -6 at the top.
    readonly property real lufsFloor: -42
    readonly property real lufsCeiling: -6
    function lufsFraction(v) {
        if (v === undefined || v === null || v <= -100)
            return 0
        return Math.max(0, Math.min(1, (v - lufsFloor) / (lufsCeiling - lufsFloor)))
    }
    // The Waveform page: the amplitude ruler column, the overview strip, the
    // take tabs.
    readonly property int wAmpRuler: 44
    readonly property int hOverview: 56
    readonly property int hDocTabs: 30
    readonly property int wDocTabMax: 220
    readonly property int hClipLabel: 16
    readonly property int wClipLabelMin: 48
    readonly property int snapPixels: 8
    readonly property int szIcon: 16
    readonly property int szIconHit: Math.round(24 * densityScale)
    readonly property int szTransportHit: Math.round(32 * densityScale)
    readonly property int wLeftDock: 280
    readonly property int wRightDock: 340
    readonly property int wMixerDock: 300
    readonly property int wStrip: 84
    readonly property int hFader: 150
    readonly property int wFader: 10
    readonly property int wMiniMeter: 5
    readonly property int wTrackHeader: 176
    readonly property int wSplitter: 4
    readonly property int wScrollBar: 10
    readonly property int wMeter: 8
    readonly property int dragThreshold: 4

    // ── Motion ──────────────────────────────────────────────────────────────
    readonly property int dFast: 100
    readonly property int dNormal: 130
    readonly property int dSlow: 160
    readonly property int easing: Easing.OutCubic
    readonly property int tooltipDelay: 450
}
