pragma Singleton
import QtQuick

// App-local coordination. Only the explicit help panel takes focus.
Item {
    id: root
    property var entries: new Set()
    property var current: null
    property var pending: null
    property var hovered: null
    property bool blocked: false
    property bool hoverAllowed: true
    property bool pointerPressed: false
    property real textScale: 1
    // Insertion order keeps the help list predictable; Set removes controls in
    // constant time while a large workspace is being destroyed. No live model
    // observes this registry: the help panel takes its own snapshot on opening.
    function registerControl(control) { entries.add(control) }
    function unregisterControl(control) {
        if (hovered === control) hovered = null
        if (current === control) current = null
        if (pending === control) dismiss()
        entries.delete(control)
    }
    function forItem(item) {
        for (let p = item; p; p = p.parent) {
            if (p.controlHelp && p.controlHelp.title !== "") return p.controlHelp
        }
        return null
    }
    function request(control, fromHover = true) {
        if (!control || !control.available) return
        if (fromHover) hovered = control
        if (blocked || pointerPressed || (fromHover && !hoverAllowed)
                || control.suppressed) return
        // Hover and keyboard focus can name the same control. Keep both a
        // pending delay and an already visible card steady in that case.
        if (current === control || pending === control) return
        current = null
        pending = control
        delay.restart()
    }
    function leave(control) {
        if (hovered === control) hovered = null
        if (pending === control) { delay.stop(); pending = null }
        if (current === control) current = null
    }
    function dismiss() { delay.stop(); pending = null; current = null }
    function interact() { hoverAllowed = false; dismiss() }
    onBlockedChanged: if (blocked) dismiss()
    onPointerPressedChanged: if (pointerPressed) interact()
    Timer {
        id: delay
        interval: Theme.tooltipDelay
        onTriggered: {
            if (!root.blocked && root.pending && root.pending.available && !root.pending.suppressed)
                root.current = root.pending
            root.pending = null
        }
    }

    // Short local walkthroughs. Each names the actual menu entries and controls;
    // they work offline and never claim a capability the application lacks.
    function guide(id) {
        for (const g of guides) if (g.id === id) return g
        return null
    }
    readonly property var guides: [
        {
            id: "start",
            title: qsTr("Getting started"),
            summary: qsTr("Open or record audio, make your edits, then export a file to share. Your original files stay unchanged."),
            steps: [
                qsTr("File › Open Audio… starts a new project. Import Audio as Track… adds audio to your current project. The red Record… button opens recording settings."),
                qsTr("Press Space to play or stop. Click a track to make it active, then drag across the part you want to edit. Ctrl+Z undoes an edit."),
                qsTr("Choose a workspace: Take edits one track, Arrange lays out tracks, Spectral repairs unwanted sounds, and Mastering checks and exports the mix."),
                qsTr("File › Save keeps an editable project. File › Export Audio… creates a finished audio file. Save and Export do different jobs."),
                qsTr("Hold the pointer over a control for help. Press F1 for more detail, or open Help › User Manual for step-by-step instructions and Terms explained.")
            ]
        },
        {
            id: "import",
            title: qsTr("Importing audio"),
            summary: qsTr("Open starts a new project. Import adds audio to the project you already have."),
            steps: [
                qsTr("Choose File › Open Audio… to begin with a file. You are asked to save any unsaved work first."),
                qsTr("Choose File › Import Audio as Track… to add another file without replacing the existing tracks. You can also drag audio files onto the window."),
                qsTr("Common audio formats include WAV, AIFF, FLAC, MP3, Ogg and Opus. AAC and M4A need the optional FFmpeg package. A missing requirement is explained when you try to open a file."),
                qsTr("Audio at a different sample rate is converted to the project rate without changing its speed or pitch. The original file stays unchanged."),
                qsTr("File › Recent Documents… lists recent files and projects. Removing an entry from this list does not delete the file.")
            ]
        },
        {
            id: "record",
            title: qsTr("Recording"),
            summary: qsTr("Record a microphone, instrument or another audio input onto a track."),
            steps: [
                qsTr("Open File › Record… or press the red Record button. Choose the input and mono or stereo."),
                qsTr("To record over an existing track, press its small R button first. Otherwise the take goes to a new track. Press Start recording when you are ready."),
                qsTr("Monitor primary input lets you hear the incoming sound without effects. Use headphones to avoid recording the speakers. Monitoring starts off."),
                qsTr("Optional settings add count-in clicks, play a lead-in before recording, repeat a selected range for several takes, or record extra inputs onto separate tracks."),
                qsTr("Press Space or Stop to finish, then listen and Save. Undo removes the completed take. Unfinished recordings can be offered for recovery after an interruption.")
            ]
        },
        {
            id: "edit",
            title: qsTr("Editing"),
            summary: qsTr("Click a track to make it active. Most edits affect the highlighted selection on that track; the status bar names it."),
            steps: [
                qsTr("Drag across audio to select a time range. On the Arrange page, Ctrl-click track headers to choose several tracks, or Shift-click for a range. Cut, Copy and Delete affect those tracks; other edits use the focused track."),
                qsTr("Ctrl-click waveforms or clip names to choose whole clips. Move drags selected clips together; Escape cancels. Slip always edits one clip. Processing uses the focused clip's interval."),
                qsTr("Whole-clip Cut and Delete leave other clips in place. Ripple closes gaps for time-range edits and makes room for Paste. Whole-clip Paste keeps existing audio between the copied clips."),
                qsTr("Fade In and Fade Out change level across the selection. The Take page also offers Reverse, Normalize, Paste and Mix and Insert Silence. Choose All channels or an individual channel before editing."),
                qsTr("Ctrl+Z undoes an edit and Ctrl+Shift+Z redoes it. File › Save keeps the project for later.")
            ]
        },
        {
            id: "effects",
            title: qsTr("Adding effects"),
            summary: qsTr("Effects change the sound of a track or the whole mix. OmaAudio uses separately installed LV2 and CLAP plugins."),
            steps: [
                qsTr("Click a track and choose an effect from the Effects menu: the everyday ones are grouped at the top, every installed effect is under All Effects. Add Effect… searches them all."),
                qsTr("Adjust its settings while listening. Bypass turns the effect off without removing it. Parameter presets save settings you want to reuse."),
                qsTr("Quick Fixes panels offer simpler controls for EQ, Dynamics, De-Esser, Noise Gate, Brightness and Ducking. Each names the required plugin. Choose settings, then press Apply to hear them."),
                qsTr("Voice Cleanup is built in and reduces background noise in speech. It is not intended for music. Undo lets you compare the result."),
                qsTr("Master effects change the complete mix. Routing sends audio between tracks; Automation changes a setting over time. Their help explains the controls and any timing limits.")
            ]
        },
        {
            id: "spectral",
            title: qsTr("Spectral editing"),
            summary: qsTr("The Spectral picture shows time from left to right and low-to-high tones from bottom to top. Select an unwanted sound, then reduce or repair it."),
            steps: [
                qsTr("Use a rectangle for a small time-and-frequency area, or draw a shape with Lasso or Brush. Feather softens the edges of the selected area."),
                qsTr("Change level makes the area quieter or louder. Start with a small negative Gain to reduce noise. Repair estimates a replacement from the surrounding audio; keep the selection short."),
                qsTr("For steady noise, first select a passage containing only that noise and press Learn noise. Then select the audio to clean and press Reduce noise in the selection."),
                qsTr("Find clicks lists possible clicks. Inspect and repair them, or use Find and Repair. Restore clipped peaks and Reduce late reverb can help with distortion and lingering room sound."),
                qsTr("Hum Comb selects hum and its related tones. Follow it with Change level to reduce them. Listen after each repair; Ctrl+Z restores the previous audio.")
            ]
        },
        {
            id: "save",
            title: qsTr("Saving and recovery"),
            summary: qsTr("Save keeps an editable project in a folder ending in .omaaudio. Export makes a finished audio file."),
            steps: [
                qsTr("Choose File › Save to keep the project, or Save As… to work in a new project folder. Move the whole folder when taking it to another computer."),
                qsTr("Autosave writes a recovery copy after a short pause when there are unsaved changes. Set the interval in Preferences or choose File › Autosave Now."),
                qsTr("After an interruption, the next start offers work that can be recovered. Save it as a project once you have checked it."),
                qsTr("File › Storage and Cleanup… shows the stored audio. Cleanup removes only audio no longer needed by clips or Undo history, when you ask. Imported original files are kept.")
            ]
        },
        {
            id: "aaf",
            title: qsTr("Importing from another editor"),
            summary: qsTr("AAF transfers tracks and clip positions from another editor. It may use separate audio files, so keep the export folder together."),
            steps: [
                qsTr("In Resolve, use Pro Tools AAF export and keep its WAV files with the AAF."),
                qsTr("Choose File › Import Session (AAF)… in OmaAudio. Choose the AAF, select Audio folder… if its audio has moved, then press Import tracks."),
                qsTr("The import keeps supported tracks, positions, source start points, gaps, fixed clip levels and straight fades. Read the result for anything that cannot transfer."),
                qsTr("Turn speed changes, time effects and changing volume into finished audio in Resolve before export. Plugins, markers and picture are not transferred."),
                qsTr("Listen and check timing after import. One Undo removes the imported tracks. OMF import is not available.")
            ]
        },
        {
            id: "video",
            title: qsTr("Working to picture"),
            summary: qsTr("Show a reference video while editing its sound. The picture follows the audio playhead."),
            steps: [
                qsTr("Choose File › Import Video Reference…. The picture appears in the Video tab. FFmpeg is required. Import audio separately if you need the video's sound."),
                qsTr("Type Starts at or choose At playhead to place the beginning of the video. Place by timecode matches the video's recorded time with the project reference."),
                qsTr("Use Alt+, and Alt+. to step back and forward one frame. Detach puts the picture in its own window."),
                qsTr("Drop-frame changes timecode numbering at 29.97 or 59.94 fps to stay close to clock time. It does not remove any picture frames."),
                qsTr("If the file moves, choose Relink…. If the video cannot keep up, it may skip pictures so the sound can continue. This does not control an external recorder.")
            ]
        },
        {
            id: "export",
            title: qsTr("Exporting"),
            summary: qsTr("Export writes the complete mix and its effects to an audio file. It does not replace your editable project."),
            steps: [
                qsTr("Open Mastering. Analyse mix checks overall loudness and peaks. LUFS is perceived loudness; more negative values are quieter."),
                qsTr("Choose a Target preset, file format, sample rate, bit depth and channels along the bottom. Presets fill in a starting set of export settings."),
                qsTr("Choose Keep current level, Set peak level or Set loudness. A peak limit can make the result quieter than the requested loudness."),
                qsTr("Leave Dither on for a final 16-bit or 24-bit export. Compressed formats make smaller files; AAC/M4A needs FFmpeg. Incompatible settings explain what to change."),
                qsTr("Press Export… and choose the file name and folder. The Queue tab shows progress. When it finishes, use Show in folder. File › Export Audio… works from any workspace.")
            ]
        }
    ]
}
