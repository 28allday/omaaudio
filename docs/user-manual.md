# OmaAudio user guide

OmaAudio helps you record, edit, mix and clean up sound. You can work on one recording or combine several tracks, then export an audio file to share.

Start with [your first edit](#start-here), look up a task in the contents, or use [Terms explained](#terms-explained) when a word is unfamiliar. This guide works without an internet connection.

Your original audio files stay unchanged. **Save** keeps an editable project. **Export Audio** creates the finished audio file you can play in other apps. These are different steps.

## Start here

New to the app? Follow [Your first edit](#your-first-edit), then read [Find your way around](#find-your-way-around) for the four workspaces and the active track.

### Your first edit

1. Choose **File › Open Audio…** and pick a recording. This starts a new project. To add audio to a project you already have, choose **File › Import Audio as Track…** instead.
2. Press **Space** to play or stop. Click the sound display to move the playhead—the line showing where playback starts.
3. Drag across the part you want to edit. This makes a time selection. Click the track first if you have more than one.
4. Choose an edit from the **Edit** or **Clip** menu. For example, **Trim to Selection** keeps the selected part and removes the rest of that track's audio.
5. Press **Ctrl+Z** to undo an edit. Press **Ctrl+Shift+Z** to redo it. The current session keeps up to 256 undoable edits; reopening a saved project starts a fresh history.
6. Choose **File › Save** to keep your work as an OmaAudio project folder.
7. To make a finished audio file, open **Mastering**, choose the export settings along the bottom, and press **Export…**.

Hold the pointer over a button or setting for an explanation. A greyed-out control explains what you need to do before using it. **F1** opens help for the control under the pointer or selected with the keyboard. Tooltips also follow **Tab** and **Shift+Tab** navigation after a short pause. Pressing a key, clicking, scrolling or dragging dismisses them, including any tooltip still waiting to appear. Closing a panel does not request a tooltip for the control underneath; move away and hover again to see its help.

### Find your way around

The four tabs at the top are different views of the same project:

| Workspace | Use it to |
| --- | --- |
| Take | Edit one track and choose which audio channels to change. |
| Arrange | Arrange clips, combine tracks and adjust their levels. |
| Spectral | Find and reduce unwanted sound by its time and frequency. |
| Mastering | Check the complete mix and export a finished audio file. |

Use **Alt+1**, **Alt+2**, **Alt+3** and **Alt+4** to switch between them.

The playback controls stay at the top in every workspace. The message at the bottom shows progress, problems and which track an edit will affect.

The **active track** is the track you are working on. Click a track or its tab to make it active. Its header is highlighted, its colour bar is wider, and the message at the bottom names it. A **selection** is the part of that track you have highlighted. Most edits affect that selection on the active track.

The side panels hold files, markers, editing history, the mixer and effects. Drag the divider between panels to resize them. Use **View** to show a hidden panel, or **Window › Reset Layout** to restore the layout.

The panel on the right has the tabs **Effects**, **Sound**, **Recipes**, **Inspector** and **Video** for the active track. The Spectral tools open from **View › Spectral**, the **Spectral tools** button on the toolbar, the Inspector's **Repair** module or **Effects › Restoration › Spectral Editing Tools**. To set a track's loudness, use **Effects › Set Track Loudness…**, **Match** in the Sound tab or the Inspector's **Loudness** module.

### Enter a value

You can type into number fields or drag the number sideways. Hold **Shift** while dragging for finer adjustments; you can press or release it during the drag. Press **Enter** or **Tab**, or click the next action, to use the typed value. **Escape** restores the previous value. **Up** and **Down** change it in small steps.

In a settings panel, **Apply** uses the changes shown. **Reload** discards changes you have not applied and reads the current settings again. Closing a panel before pressing Apply usually discards those changes; the Close button's help says if an operation continues.

## Bring in your audio

[Open or add files](#open-or-add-files) to start with recordings. To move an arrangement from another editor, follow [Import an AAF session](#import-an-aaf-session).

### Open or add files

**Open Audio…** starts a new project from an audio file. You are asked about unsaved work first. **Import Audio as Track…** adds files as tracks in the current project: select several at once (Ctrl- or Shift-click, or Ctrl+A in the file chooser) and each becomes its own track, for example the stems of a multitrack recording.

You can also drag files onto the window. In an empty project, the first file starts the project and the others become extra tracks. In an existing project, each file becomes an extra track.

OmaAudio reads WAV, AIFF, FLAC, Ogg Vorbis, Opus, MP3, W64 and CAF when the required support is installed. AAC, M4A and MXF are read with **FFmpeg**, which is installed with OmaAudio. If a file cannot be opened, the message explains why.

A file with a different sample rate is converted to the project's rate on import. This keeps its speed and pitch the same. The original file stays unchanged. You can cancel a long import from the bottom status bar.

Use **File › Recent Documents…** to reopen a recent project or add a recent audio file; double-click an entry to open it straight away. **Forget** and **Clear list** remove entries from that list, not the files themselves.

### Import an AAF or OMF session

AAF is a file used to move an arrangement between audio or video editors. It describes the tracks, clips and their positions; the audio may be stored beside it or inside it.

1. Export an AAF from your picture editor — for example **Pro Tools AAF** in DaVinci Resolve, or **File › Output › Export to File** with an AAF setting in Avid Media Composer. Keep the exported audio (WAV, AIFF or MXF) with the AAF file, or embed it.
2. In OmaAudio, choose **File › Import Session (AAF/OMF)…**.
3. Choose the AAF file. If the audio has moved, choose **Audio folder…** and select its folder.
4. Press **Import tracks** and read the result shown below the settings.

The import keeps supported mono/stereo tracks, clip names, timeline positions, the part of each source file used, gaps, clip levels, straight fades and dissolves. Clip volume keyframes (for example Avid's clip gain automation) become the clip's volume envelope, which you can edit in **Clip Envelopes…**. Audio inside MXF and QuickTime media — Avid's own media files and camera clips — is read directly. Existing tracks stay in place. One Undo removes the import.

If the AAF holds several sequences, the first one with audio is imported and the result names the others; export them separately to import them. An AAF that holds a single clip rather than a sequence is placed at the start of new tracks. Auxiliary timecode tracks are ignored in favour of the sequence's main timecode.

Turn speed changes and time effects into finished audio before exporting. Pan and other audio effects are not transferred (the result lists them). **Video:** the AAF's picture comes in as the project's reference video and plays in the **Video** tab, placed where the timeline shows it; the same Undo removes it. When the picture is one clip of a file kept beside the AAF, that file is used as it is. An edited picture — cuts between shots or several files, as Resolve exports a timeline — is built into one video from its shots (960×540, a keyframe every half second, so it scrubs quickly), with black between shots and for any shot whose file is missing; dissolves become cuts and video effects show their source shot, and the result says so. Building takes a few seconds per minute of picture. The built video is kept with your recordings until you save, and is then saved inside the project. A project that already has a reference video keeps it. Markers do not transfer. Unsupported, missing or damaged items are explained in the result.

**OMF files** (OMF Interchange 2.0, from picture editors such as Avid Media Composer or Premiere) import the same way. Audio embedded in the OMF is copied into the project; audio kept beside it is found as for an AAF. Clip positions, source offsets, gaps, straight fades, dissolves between clips and fixed clip gain carry over. Other effects, gain changes over time and stereo-sound tracks must be rendered or split into mono tracks before exporting the OMF; the result explains anything that was refused. OMF 1.0 files are not supported.

An empty project can adopt an AAF's starting timecode. An existing project keeps its own time reference and reports a difference. Check the imported timing and listen before continuing your edit.

## Arrange and edit

Start in [Arrange](#arrange) to put clips in order, or [Take](#take) to work on one track. The sections below explain each edit and what happens to the timing.

### Arrange

Each track holds clips on a shared timeline. Tracks play together, so a voice recording can sit above a music track without moving either one. A project can hold up to 256 tracks; how many play smoothly depends on your computer and effects (see **Audio load** and **Check this computer**).

- Click a track to make it active; double-click its name to rename it.
- Use **Mute** to silence it and **Solo** to hear it with only the tracks needed for its audio routes. A muted track stays silent even when soloed.
- Use the level slider to make it louder or quieter. Double-click to return to **0 dB**, meaning no level change.
- Use **Pan** to place mono sound left or right. On stereo tracks it adjusts the balance between the two sides. Double-click to centre it.
- Change the track colour, role or height from the track's menu or Inspector. A role, such as Music or Dialogue, is a label and colour; it does not change the sound or connect tracks.
- Right-click a track's lane, its header or its mixer strip for the track's menu, including **Duplicate Track** and **Delete Track**. Edits that need a time range or clips, such as **Delete**, are greyed out until you select one.

**Ctrl+Shift+N** adds an empty track. **Ctrl+Shift+D** duplicates the active track with its clips, effects and settings.

### Move, split and trim clips

A **clip** is a piece of audio placed on a track. Its position controls when you hear it.

| Tool or action | What happens |
| --- | --- |
| Selection tool, V | Drag across audio to select a time range. |
| Move tool, G | Drag a selected clip to move the whole selection to another position or tracks. |
| Slip tool, Y | Keep the clip in place but change which part of its source audio plays. |
| Split at Playhead | Divide the clip at the playback line so the two parts can be edited separately. |
| Trim to Selection | Keep the selected part and remove the rest of the active track's audio. |
| Clip Properties… | See the source and timing, or enter exact level and fade settings. |

The Move tool shows a preview for every selected clip. Red means the group cannot move there, for example because it would overlap occupied audio or leave the available tracks. Press **Escape** to cancel a move; losing the pointer grab also cancels it. **Snap**, the magnet button or **S**, lines nearby edges up with clips, markers or the playhead.

**Ctrl+mouse wheel** zooms in or out. **Shift+mouse wheel** moves along the timeline. Small wheel and trackpad movements make proportionally small changes. Fit the whole project or just the selected range with the zoom buttons.

### Cut, copy, paste and gaps

On the Arrange page, click a track header to select one lane, **Ctrl-click** to toggle
several, or **Shift-click** to select a range of lanes. Drag across a selected
lane to choose a common time range. Cut, Copy and Delete act on every highlighted
track; the rack, inspector, clip tools and other processing use the focused
track. Leaving the Arrange page returns to one track. Track selection is view state
and does not add an Undo step.

To remove tracks, select their headers with no time range and press **Delete**, or
choose **Delete Track** from the right-click menu (**Ctrl+Shift+W**). Every selected
track goes in one Undo step.

Copies retain the complete time range, including silence, clip gains/fades,
channel layouts and spacing between selected tracks. Paste starts at the focused
track; skipped lanes remain untouched. **Paste to New Track** creates the
required tracks and gaps at time zero. Each paste or multi-track deletion is one
Undo step. Track effects, automation, routing and faders are not copied.
These edits move clips only; track automation and markers keep their
existing timeline positions.

To select **whole clips**, Ctrl-click their waveforms or names. Click a clip name
without Ctrl to replace the selection with that clip and any linked group members.
With Move, drag any selected clip to move the whole selection, keeping its timing
and track spacing. Borders and the status bar show the selection. Each move is one Undo
step. Slip always edits one clip. Timeline time fields and time-range processing
use the focused clip's interval. Clip Properties offers the selected-clip scope
described below. Drag a time
range, use Select All or select track headers to return to range editing.

Whole-clip Copy retains the selected clips and their spacing. With Ripple off,
Paste replaces only the copied clips' intervals, preserving existing audio in
between them. With Ripple on, Paste opens the full copied span on each affected
track. Whole-clip Cut and Delete remove exactly those clips and leave other
clips in place even when Ripple is on. Choose All channels for a group paste;
a single whole clip can also be pasted into an individual channel.

**Clip › Clip Groups…** manages named groups in the local development build.
Select at least two whole clips, choose **New group from selected clips**, enter a
name and click **Create group**. An optional `#RRGGBB` colour applies to clip labels
and outlines; leaving it empty retains each track's colour. Choose an existing
group in the same panel to rename or recolour it. Each change is one Undo step.

With **Clip › Link Group Selection** on, selecting a member selects its group.
Ctrl-click toggles that whole group while preserving unrelated selected clips.
Turn linking off to select individual members, then use **Detach selected clips**
to remove their membership. **Dissolve this group** removes all its links and the
saved group without removing audio. Empty groups remain available to dissolve.
New and Open turn linking back on. The switch does not change project history.

Membership survives Save/recovery and Undo. Split fragments retain their group;
pasted copies and duplicated tracks receive independent groups. Channel processing,
spectral repair, voice cleanup, stretch and pitch and duration edits retain membership.
Crossfades keep each group's links and remain adjustable after moving or copying
the whole pair and saving. Later regrouping or dissolving a group remains effective
when adjusting the crossfade.

Time-range processing retains the groups already present in that range, leaving
gaps ungrouped. Pasting independent audio into all channels does not give it the
destination's group; pasting into one channel or using Paste and Mix retains the
destination membership.

With several whole clips selected, **Clip › Clip Properties…** starts with
**Apply to all selected clips** on. Turn it off to edit only the reference clip
named in the panel. **Keep relative levels** adds the same dB change to each clip,
preserving their balance; 0 leaves their levels unchanged. Turn it off to set all
selected clips to a common gain. An edit that would put any clip above +48 dB is
refused without changing the others.

**Replace selected clips' fades** enables the fade lengths and shape controls. With **Scale
fades to each clip's length** on, enter lengths for the reference clip; each member
receives the same fraction of its own duration, rounded to a sample. Turn it off
to use identical lengths, which must fit the shortest selected clip. Leaving
Replace fades off preserves each clip's existing curves. **Reset level and fades**
sets all selected clips to 0 dB, removes their fades and resets the shapes to
Linear with Curve 0 when applied. Each change
is one Undo step, and original audio is retained. Reload if the selection changes
while the panel is open. Use **Pitch and Duration…** to scale a whole-clip
selection; time-range processing retains its existing scope.

For a **time range**, with **Ripple Edits off**, Delete and Cut leave a gap. Paste replaces audio at the playhead for the length of what you copied. **Paste to New Track** keeps the existing tracks untouched.

For a time range with **Ripple Edits on**, Delete and Cut close the gap, and Paste moves later audio along to make room. This changes timing on the edited track. Use the **R** shortcut to switch Ripple Edits on or off; the small **R button on a track** has a different job—it prepares that track for recording.

**Delete, Close the Gap** requires an explicit time range and closes it on the selected tracks. **Delete Time Across All Tracks** also requires a time range and removes the same interval from every track, keeping their later audio aligned with each other. Markers keep their timeline positions, so check them afterwards.

**Silence** replaces the selection with silence without moving later audio. **Insert Silence…** adds a chosen length of empty space and moves later clips along. Choose whether it affects the active track or all tracks. Automation and markers keep their timeline positions.

**Paste and Mix…** blends a single-track copy with the existing sound. Set the level of each before applying. Undo restores the previous audio. Use ordinary Paste for a multitrack copy.

### Fades and crossfades

Select the beginning of a clip and choose **Fade In** to bring it up from silence. Select its end and choose **Fade Out** to reduce it to silence.

On a track lane, each clip also has two small square fade handles at its top corners, just below its name. They appear when the pointer is over the clip, when it is selected, or when it already has a fade. Drag the left handle right to fade the clip in, or the right handle left to fade it out; drag a handle back to the clip's edge to remove that fade. The shading follows the pointer, the fade's shape is kept, and each drag is one Undo step.

To trim a clip, point at the lower half of its left or right edge: the pointer becomes a two-way arrow and the edge lights. (Higher up, a drag from the edge selects as usual.) Drag in to hide the start or end of the clip, or out to bring back audio that was trimmed away. The audio stays where it is in time, and the file itself is never changed. An edge stops at the ends of the recording, at the next clip on the track, and where a clip effect or a clip envelope was prepared; a clip is always at least 10 ms long. Snap applies while you drag, the new length shows beside the edge, and each trim is one Undo step. A fade stays on its edge and is shortened if the clip becomes shorter than it. Crossfades are changed with their own edge handles, not trimmed.

To change a clip's level, point at the clip: a line across it shows its gain, with a small handle in the middle. Drag the handle up to make the clip louder or down to make it quieter, from +12 dB at the top to −40 dB near the bottom; hold **Shift** for fine steps. The line holds at 0 dB as you pass it, the level shows beside the handle while you drag, and double-clicking the handle returns the clip to 0 dB. Each change is one Undo step, and the waveform redraws at the new level. **Clip › Properties** takes an exact value.

For a shaped fade, open **Clip › Clip Properties…** and turn on **Replace fades**.
Set each fade's length, then choose **Linear** or **Smooth**. Smooth eases into and
out of the change. **Curve %** bends either shape: positive values raise its middle
level, negative values lower it, and 0 keeps the chosen shape. The previews show
the result before **Apply**; the timeline shows the applied curve. When several
clips are selected, the scope switch chooses all selected clips or the reference
clip. All chosen clips receive the shown shapes when Replace fades is on.

Split fragments retain the relevant part of their existing curves. Leave Replace
fades off to preserve them while changing level. Stretching scales fade timing
while preserving shape, and Undo restores the previous settings. The menu's range
Fade In/Out commands use straight fades. Saving nondefault shapes or curve amounts
requires project format 21, including settings retained for later fade-length edits.

A **crossfade** blends the end of one clip with the beginning of the next:

1. Select a short range that crosses one join between two clips.
2. Choose a crossfade from the **Clip** menu. **Linear** uses straight level changes; **Equal Power** uses curves that can help keep a blend sounding even.
3. Use **Adjust Crossfade…**, or drag its handles, to adjust the start, end and shape.
4. Listen across the join. Undo or adjust it if the blend changes the level too much.

Both clips need enough unused source audio around the join. If they do not, the app explains why the crossfade cannot be made.

### Clip envelopes

Use **Clip › Clip envelopes…**, or the button in Clip Properties, to change one
clip's volume or stereo balance over time. The panel names the clip being edited,
even when it belongs to a linked group. Its point times are measured from the
clip's start.

Choose an envelope, add points with the graph or exact time/value fields, and
choose Linear or Hold for the curve default. **After selected point** can use
Linear, Hold or **Smooth** for that segment. Smooth eases between the point values
without exceeding them. Volume is a
percentage of the clip's existing gain and fades: 100% leaves that level intact.
Stereo balance attenuates the opposite side as you move left or right; centre
leaves both channels unchanged. It applies before track effects and track pan.
For a mono source, use the track's pan control instead.

Disable an envelope to keep its points without playing them. Clear removes all
its points, including inherited points outside a trimmed clip. Each actual edit
has one Undo step. If another edit makes the panel stale, reload the pinned clip
or choose **Use current clip** to deliberately switch targets.

Points move and copy with the clip. Splits and trims retain the original curve,
including the outside points needed to define its edges. Slip moves the source
audio under the curve. Stretch and project-rate changes scale the point times;
if rounding would merge two points, separate those points before trying again.
Most rendered processing bakes the audible envelope into a new source; duration
processing retains a scaled editable curve. Original imported media is retained.
Saving clip envelopes requires project format 22, or format 23 when Smooth is
used. Clip-effect controls have their own curves; see Clip effects.

### Effect windows

Many effects come with a window of their own, designed by their maker. Select
**Open effect window** (the square-arrows button) on an effect's row in the
Effects dock to open it beside OmaAudio. Turning knobs there changes the effect
exactly as its controls in the rack do: the changes play, export and save with the
project. Opening it again brings the same window forward. The window closes by
itself when the effect is removed, replaced or bypassed, or when another project
opens.

Effect windows use X11, which a Wayland desktop provides through XWayland; if it
is not available, OmaAudio says so and the rack controls remain. VST3, CLAP and
LV2 effects with an X11 window open this way; an LV2 window and the rack controls
always show the same values. Clip effects use their controls in the Clip Effects
panel.

### Clip effects

Use **Clip › Clip Effects…**, or **Effects…** in Clip Properties, to give one clip
its own effects. They process that clip before its gain, fades and envelopes, and
before the track's effects. Choose **Add effect** to pick an installed effect; it is
added after the existing ones. Turn an effect off, move it earlier or later, or
remove it from its row. Open a row to set its controls. Each change is one Undo step.

Clip effects travel with the clip. When you split it, the pieces share the same
effects and play as one continuous sound, so a delay or reverb started in the first
piece carries on into the next. Changing the effects through any piece changes all
of them; the panel says how many pieces share them. A moved or copied clip keeps
its effects.

The effected sound is prepared in the background, and the panel shows **Ready**
when it is done. Until then the clip is silent rather than playing without its
effects, so what you hear is always the finished sound. Exports, loudness checks
and other analysis wait for it and always include the effects. If an effect is
missing or fails, the panel says so and the clip stays silent until you fix or
remove it. With every effect turned off, the clip plays its own audio and keeps
the settings.

Select **Automate** beside a control to draw how it changes across the clip.
Points are measured from the clip's start, move with it and continue across its
split pieces.

**Bake into audio** replaces one piece's audio with its effected sound and removes
that piece's effects; other pieces keep theirs. Repairs and other edits that change
samples need baked audio, and say so when a clip still has effects. A mono clip
through an effect that only works in stereo becomes stereo. Projects with clip
effects use project format 24.

### Take

The Take workspace shows one active track in detail. On stereo audio, the two lanes are its left and right channels.

Choose **All channels** or an individual channel before editing. Channel-only edits leave the other channels unchanged; channel-only Cut or Delete keeps time in place.

Use the **Clip** menu to reverse a selection, change its level, add fades, insert silence or blend copied audio. **Normalize to −1 dBFS** raises or lowers the selection so its loudest sample is 1 dB below the maximum file level. It does not even out every quiet and loud moment.

**Map Track Channels…** changes how input channels feed output channels. Use the presets to swap left and right or make mono. In the grid, 1 keeps an input at its current level, 0 leaves it out, and negative values reverse its polarity. Changes apply to the whole active track. Listen afterwards, since combining channels can change the level or cancel some sound.

Drag the framed view in the overview strip to move along the project. Click outside the frame to jump to another part.

### Generate tones and noise

**Clip › Generate Tone or Noise…** creates new audio as a clip on the active track. With time selected on that track it fills the selection; otherwise it starts at the playhead and lasts for the **Duration** you type. Either way it replaces what was there for its length, and one Undo removes it.

- **Tone** makes a sine, square, triangle or sawtooth wave at a frequency, set by its peak level. Turn on **Sweep** to glide from the start frequency to an end frequency, evenly by octave. Square, triangle and sawtooth tones are band-limited, so high tones do not fold back into unwanted lower ones.
- **Noise** makes white, pink or brown noise, set by its average (RMS) level. Pink noise has equal energy in each octave and is useful for setting speaker levels; brown falls away further towards the top.

### Measure audio

**Effects › Analysis** opens three measurements of the selection, or of the active track's whole length when nothing is selected. They measure the audio as its clips play, including clip effects, clip gain, fades and envelopes, before track effects and the master bus. Up to 20 minutes can be measured at once. **Measure again** updates the readings after an edit.

- **Level Statistics…** shows peak, true peak, RMS and integrated loudness; the loudest and quietest 50 ms and the range between them; and, per channel, DC offset and how many samples reach full scale (clipped), counted as samples and as separate runs.
- **Spectrum…** draws the average spectrum on a logarithmic frequency scale. More **Points** separate close frequencies; fewer follow short sounds better. The **Blackman** window shows quiet tones next to loud ones.
- **Stereo Phase…** shows correlation (+1 is mono, near 0 is wide stereo, below 0 the channels cancel when summed to mono), the level difference between left and right, and width as side level relative to mid.

In Frequency and Phase, turn on **Live** to follow what is playing instead: the whole mix, before the monitor level and the metronome, updated about ten times a second. Turn it off to measure a selection again.

### Match clip loudness

At the bottom of the measurement panel, type a loudness and press **Set clip loudness**. Each selected clip, or the clip at the playhead, gets the clip gain that brings it to that integrated loudness as it plays at unity gain. Clips shorter than 0.4 seconds cannot be measured. One Undo restores the previous gains. To set a whole track instead, use **Effects › Match Track Loudness…**.

### Fit music to length

**Clip › Fit Music to Length…** makes a piece of music shorter or longer, for example to fit a 30-second advert. It finds passages where the music matches itself — the same chords, sound and level around both points — and skips or repeats the music between them, joined with short equal-power crossfades. The first and last four seconds are kept as they are.

1. Put the playhead over the music clip on the active track.
2. Type the **New duration**. It can be from 5 seconds up to four times the clip's length; the result lands within about 2% of it (at least half a second).
3. Press **Fit**. The clip is replaced by the fitted music at the same position. Listen to each join; one Undo restores the original clip.

Fitting works best on music with repeating sections. If no matching passages give the requested duration, it says so and changes nothing; try a duration closer to the original. A longer result needs room before the next clip on the track. The clip's gain, fades and effects are included in the fitted music.

### Align speech

**Arrange › Match Speech Timing…** re-times a second performance of the same words, such as a line re-recorded in a quiet room, so it follows the timing of a reference, such as the original location recording. Pauses and syllables move to match; the voice and pitch stay those of the new performance.

1. Put the two recordings on separate tracks, and place the playhead over both clips (a track with only one clip needs no playhead).
2. Choose the **Reference** track and the **Track to align**.
3. Set **Tightness**: higher follows the reference closely moment to moment; lower keeps more of the performance's own rhythm over short spans.
4. Press **Align**. The aligned copy appears on a new track at the reference clip's position and length. Mute or remove the original take when you are happy with it.

Both clips are compared as they play, including clip effects, gain and fades; each can be up to 10 minutes long. Alignment works best when both recordings contain the same words in the same order. One Undo removes the new track, and Cancel stops a running alignment without changing anything.

### Pitch and duration

Open **Clip › Pitch and Duration…** to change a clip's pitch or length. With **Realtime pitch and duration** on, you can adjust the setting again later.

- **Duration 100%** keeps the original length; 200% doubles it; 50% halves it.
- **Pitch 0** keeps the original pitch. Twelve semitones is one octave; positive numbers raise it and negative numbers lower it.
- Live processing allows 50–200% length and −12 to +12 semitones.
- With live processing off, OmaAudio creates a processed copy. This allows 25–400% length and keeps the pitch. It needs FFmpeg.

Press Apply to use the settings. **Reset pitch / duration** removes live pitch and length changes. Later clips and markers stay in place, so leave enough room for a longer clip. Extreme changes can make short sounds less sharp or introduce unwanted sound.

With several whole clips selected, **Apply to all selected clips** starts on.
**Duration 100%** now means the current span from the earliest selected start to
the latest selected end, including gaps, in either mode. The earliest start stays
fixed; clip boundaries, spacing and fades scale together. Unselected clips,
automation and markers stay in place. Turn the scope switch off to work on the
reference clip alone.

In realtime group mode, **Pitch change 0** preserves each member's current pitch;
entering a change adds it to every member. Each final pitch must stay within −12
to +12 semitones, and each duration within 50–200% of its original source interval.
Rendered group mode preserves each clip's current audible pitch and allows
25–400% of its current duration. Every input and output must be at least 50 ms,
and every source mono or stereo. A member outside its limits or overlapping an
unselected clip refuses the entire edit. Group percentages can be entered from
25–400%; Apply checks each resulting member's mode-specific limits.

Undo restores the group's previous timing and pitch. There is no group Reset to
an original arrangement. Reload after changing the selection. Rendered work can
be cancelled; failure or cancellation leaves all selected clips unchanged.

### Play faster or backwards

Press **L** to play. Press it again while playing to go faster: 2×, 4×, then 8×. Press **J** to play backwards, and again for 2×, 4× or 8× backwards. **J** while playing forwards, or **L** while playing backwards, turns round at normal speed. **K** stops. Stopping always returns to normal speed, and **Space** or Play always plays at normal speed.

While the speed is not normal, a small tag on the transport's time shows it, for example **4×** or **◀ 2×**. Faster playback sounds like tape run fast: the pitch goes up. Effects, automation and loops work as they do at normal speed. Playing backwards ignores the loop and stops at the start. The loudness meter does not measure while you play faster or backwards.

While recording, or while automation is being written (any mode other than Read), **L** just plays and **J** moves the playhead back two seconds. On the Spectral page **L** is the lasso tool.

### Markers and loops

Markers name places you want to find again. Add one at the playhead, or select a range to make a range marker. Use the **Markers** panel to rename markers and jump to them.

Select a range and press **Ctrl+L** to play it repeatedly. Press Ctrl+L again to turn looping off. Making a different selection does not move the loop you already set. The playback loop is not saved with the project.

**Edit › Metronome** (Alt+K) clicks on each beat while you play or record, with a higher click on the first beat of each bar. It follows the tempo and meter in **Project Properties**, even when the ruler shows clock time. The click is only heard: it is never recorded, measured or exported, and it is not saved with the project. Agents can set `metronomeLevelDb` (−40 to 0 dB, default −12).

### Automation

Automation changes volume, pan or an effect setting over time. For example, it can lower music during speech and bring it back afterwards.

1. Open **Automation…** and choose the track and setting.
2. Double-click the graph to add points. Drag a point to change its time or value; right-click it to remove it.
3. Enable the curve and play the section to hear the result. Undo restores each graph edit.

Select a point and choose **After selected point** to shape the transition to the
next point. **Linear** changes steadily, **Hold** keeps the value until the next
point, and **Smooth** eases into and out of the change. **Curve default** follows
the curve's Linear/Hold setting. Smooth stays between the two values. Recording
over part of it keeps the untouched portions; the editor shows their retained
shape. Smooth curves require the newer project format 23 when saved.

| Mode | What it does during playback |
| --- | --- |
| Read | Plays the changes already stored in the curve. |
| Write | Records new control movements while it is writing; use with care over existing changes. |
| Touch | Records while you move the control, then returns to the existing curve when you release it. |
| Latch | Starts recording when you touch a control and keeps its last value until playback stops. |

**Touch return** sets how long the released control takes to reach the existing curve:
0–5 seconds, with a 1-second default. Zero returns immediately. Change it in the
Automation panel or **Preferences › Editing & recovery**. It is saved for all
projects. Changing it affects future returns; a return already in progress keeps
its original timing. The return is recorded as part of the same automation pass
and Undo step.

An effect setting controlled by automation may be read-only in a simpler settings panel. Edit its curve or turn that curve off in the Track Effects panel first.

The timing note explains how smoothly a control follows the curve. Volume and pan update for each audio sample. CLAP effects support exact points and very small steps between them; many simultaneous changes can make the steps larger. LV2 effects keep exact points and use 64-sample steps between them. An effect may also smooth changes itself. Movements recorded live are timed to the audio processing blocks.

### Generate ducking

Generate ducking creates editable volume changes that lower one track while other
tracks are audible, such as music under speech.

1. Open **Automation…**, then **Generate ducking…**. Choose the track to lower and
   tick the trigger tracks that should make it quieter.
2. Use **Selected time** or **Whole project**. Set the detection threshold,
   reduction, attack, hold and release. Attack starts the drop before detected
   sound; hold bridges pauses; release brings the level back up.
3. Choose **Analyse** and compare **Existing volume** with **Proposed volume**.
   Analysis leaves the session unchanged. Adjust settings and analyse again as
   needed, or cancel to discard the proposal.
4. Choose **Apply points**. This is one Undo step. **Edit points…** opens the
   target's normal volume automation so you can refine the result.

Existing volume automation in the range requires **Replace volume automation in
this range** and another analysis. Automation outside the range is preserved.
If the track has disabled stored volume points, enable or clear them in Automation
first. Silent triggers and zero reduction leave the curve unchanged.

Detection follows the trigger output, including effects, fader changes, automation
and incoming audio. Mute is respected; Solo is ignored. The target fader and source
audio stay unchanged.

Turn on **Keep up to date when the triggers change** before **Apply points** to have
the ducking follow later edits. Whenever a trigger track's sound changes — a clip
moved, trimmed or silenced, an effect or fader changed — the points are generated
again with the same settings. The update belongs to the edit that caused it, so one
Undo restores both. If you edit the generated points by hand, updates stop and your
edit is kept; the panel says so. Without this option the points stay where they
were generated; analyse again after editing the triggers.
The Quick Fixes **Ducking** continues to provide realtime sidechain compression.

### Routing and sends

Routing chooses where a track's sound goes. Open **Arrange › Routing…** or use the Inspector.

**Main output** normally sends the track to **Master**, which combines the final mix. Choose another track to use it as a **bus**: a track that receives several other tracks so you can adjust or process them together. Giving a track the Bus role alone does not connect it.

A **send** makes an extra copy of a track at its own level. For example, several voice tracks can send copies to one reverb track. You can add up to eight sends per track.

- **Post-fader** follows the track's volume and pan.
- **Pre-fader** includes its effects but ignores its volume and pan.
- Mute silences both types. Choosing **None** for Main output leaves sends available.

**Trigger track** is the sidechain source: sound that controls an effect on this track. For example, a voice track can trigger a compressor that turns down the music. The effect must support a sidechain and have its external-input mode enabled. The trigger follows its source track's effects, volume, pan and mute settings.

Press **Apply routing** to use the routes. Undo restores the previous routing. The app refuses connections that would send sound back into itself.

## Shape and clean the sound

Use [Sound](#sound) for plain controls that set up a voice or a music track. Use [Effects](#effects) to change the sound during playback with full controls. Use [Recipes](#recipes) to run a saved list of steps in one click. Use [Spectral](#spectral) to select and repair unwanted sound, or [Voice Cleanup](#voice-cleanup) for noisy speech.

### Sound

The **Sound** tab, beside Track Effects, sets up the active track in plain terms.

1. Choose what the track holds: **Voice**, **Music**, **Effects** or **Ambience**. This sets the track's role.
2. For a voice, pick a starting point in **Start from** (for example **Podcast voice** or **Phone or video call**). It sets every fix at once. **Save…** keeps the current settings as a starting point of your own, for any voice track; choose it and press **Remove** to delete it.
3. Turn fixes on or off and move their sliders while listening. A slider takes effect when you let go.
4. Under **Loudness**, choose a target and press **Match**. OmaAudio measures the track and sets its level to reach the target without passing the peak limit.

| Fix | What it helps with |
| --- | --- |
| Background noise | Hiss, air conditioning and room tone, reduced live. Needs a 48 kHz project; at other rates, select the audio and use **Voice Cleanup…** instead. |
| Rumble and pops | Traffic, handling noise and the thump of popped p and b sounds. |
| Mains hum | A steady buzz from the mains. Choose **50 Hz** (Europe, Africa, Asia, Australia) or **60 Hz** (the Americas). |
| Harsh "s" | Sharp s and sh sounds. |
| Room echo | The sound of the room. Unlike the others it is written into the audio: set the amount and press **Reduce room echo**. It changes the selected audio, or the whole track when nothing is selected, as one Undo step. Listen and use Undo to compare. |
| Even out | Keeps quiet and loud words closer in level. |
| Presence | Brings the voice forward. |

Every fix is a real effect on the track, added in a fixed order. You can see them, and all their controls, in **Track Effects**. A change made there shows in the Sound tab too. Turning a fix off bypasses its effect and keeps its settings. Each change in the Sound tab is one Undo step. Harsh "s", Even out and Presence use Calf effects, which come with OmaAudio; if they are missing, those rows say what to install.

For music, the Sound tab has **Loudness**, **Length**, **Duck under voices** and **Level**. Turn ducking on to lower the music while tracks of the chosen kinds play (**Voice** by default; add **Effects** or **Ambience**). **How much** sets how far it drops and **How quickly** how fast it drops and comes back. The result is volume points you can see in Automation, and they follow later edits to the voices. Changing a setting makes the ducking again from the original level, so it never adds up; turning it off puts the original level back. If you edit the points by hand, the Sound tab leaves them alone. **More ducking settings…** opens [Generate ducking](#generate-ducking) for full control.

**Length** fits the music clip at the playhead to a duration: type it in **Fit to**, choose **Re-cut** (joins matching passages and keeps the speed, as in [Fit music to length](#fit-music-to-length)) or **Stretch** (changes the speed, keeping the pitch) and press **Fit**. Each is one Undo step.

For effects and ambience, the Sound tab has **Loudness**, **Width** and **Level**. Width narrows or widens a stereo sound: 0 % is mono, 100 % as recorded and 200 % twice as wide.

The Sound tab remembers each track's other choices: the **Loudness** target (separately for each kind of sound), the **Room echo** amount, and the ducking choices (which kinds, **How much** and **How quickly**). They are saved with the project and shown again when you come back to the track, reopen the tab or reopen the project. Remembering them adds nothing to History, but it marks the project as having unsaved changes. A new project starts with the defaults.

#### Tidy pauses

**Tidy pauses** finds the long gaps between words and shortens, removes or marks them. Open it from the **Pauses** section of the Sound tab for a voice track (**Tidy pauses…**), or choose **Edit › Tidy Pauses…** (`Shift+Alt+T`); with a time range selected it looks in the selection. It opens as a page of the Sound tab; **‹ Sound** goes back. Nothing changes until you press the button at the bottom.

1. Under **Look in**, choose **Selection** (the selected stretch of the active track), **This track** (the whole active track) or **Every voice track** (every track whose kind is Voice, over the whole project). With several voice tracks, a pause is where all of them are quiet at once, so a guest's answer in a host's gap is never cut.
2. Set what counts as a pause. **Quieter than** is the level below which the audio counts as quiet; with **Auto** on, OmaAudio measures the background noise and sets it a little above that. **For at least** is how long a quiet gap must last. **Leave each side** is the air kept next to the words. With **Keep breaths** on, a breath in a gap stays and the pause is shortened around it.
3. Press **Find pauses**. The pauses are shaded on the waveform with their lengths: amber where they will change, faint where they are already short enough. Click a shaded pause to keep it as it is (it turns teal and dashed); click it again to let it change. **Play the joins** plays 2 seconds either side of each pause in turn. **Clear** takes the shading away.
4. Under **Then**, choose what to do:
   - **Shorten** makes each pause **Shorten each to** long, keeping the rhythm of the talk. Pauses already that short are left.
   - **Remove** leaves only the air either side.
   - **Mark** cuts nothing. Choose **A marker at each pause**, **A range over each pause** or **A range over each stretch of speech**. The markers are named "Pause 1", "Pause 2"… (or "Speech 1"…) in the Markers list.
   - **Split** makes each stretch of speech its own clip and leaves the pauses empty. The timing does not change.
5. For Shorten and Remove, choose whether the **Other tracks** move too. **Move the other tracks too** moves effects, ambience and the other tracks that are not music with the voices, so they stay in step. **Only the tracks looked in** leaves every other track where it is. Music tracks never move, and markers move with the voices. **Smooth each join** sets a short fade either side of each join so it does not click; 0 ms is a plain cut.
6. Press the button at the bottom, for example **Shorten 35 pauses**.

The change is one Undo step, such as "shorten 35 pauses". A card shows the length before and after, how many pauses changed and how many were kept. **Play the joins** on the card plays each new join; before the change it plays the pauses as they were, so to compare the two, press **Undo** on the card: it puts the audio back and shows the same pauses again. **Done** closes the card.

If the project changes after you find the pauses, the shading dims and the page says **The project changed. Find pauses again.** Finding again forgets which pauses you kept.

### Effects

Effects change the sound during playback and export. OmaAudio supports separately installed **LV2**, **CLAP** and **VST3** effects. These are plugin formats—ways of adding effects from other makers. VST3 effects are found when **Find VST3 effects** is on in **Edit › Preferences… › More** (it is on by default).

1. Click the track you want to change.
2. Open the **Track Effects** panel and choose **Add Effect…**.
3. Search for an effect, select it and add it. Its panel opens in the middle of the window: adjust its controls while listening.
4. Use **Bypass** to hear the track without that effect, while keeping the settings.

**Track Effects** lists a track's effects by name, in the order the sound passes through them. Click an effect's name, or choose **Open Panel** from its **⋯** menu (or right-click it), to open its controls in the middle of the window. The panel changes the effect live; there is nothing to apply. It does not block the rest of the window, so you can keep playing, moving the playhead and using shortcuts. Drag its title to move it and its bottom-right corner to resize it; OmaAudio keeps its place and size until you quit. Only one effect panel is open at a time: clicking another name moves it to that effect. In the panel:

- The switch beside the name turns the effect on and off, like **Before** and **After**.
- **Play** plays the selection on that track over and over, or from the playhead when nothing is selected there. If nothing was playing, it starts by itself when the panel opens. Closing the panel stops only playback it started.
- **Effect window** opens the effect's own window, as its maker designed it. The ⤢ button on an effect in Track Effects does the same.
- **Reset all**, **Preset** and **Save…** work as they do in the Apply panel below.
- **Close** or `Escape` closes it.

With one effect on a track, the mixer strip's **FX** button opens its panel too. The master bus effects on the Mastering page open the same panel. On the Arrange page, the mixer's **Master** strip has an **FX** button too: it shows the master bus's effects in the Effects panel (with one effect, its panel opens as well), until you focus another track or press a track's **FX** button. Right-click the Master strip for **Master Effects**, **Add Effect to the Master Bus…** and **Go to the Mastering Page**. A clip's effects (**Clip › Clip Effects**) list by name in the same way: click a name, or its **Open Panel** button, to open the panel. There, each change is written when you let go of the control, as one Undo step, and the clip's sound is prepared again. The **Automate** button beside a control draws its curve across the clip; a control that follows a curve is read-only in the panel.

An equaliser or filter shows its **frequency curve** above its controls: what it does to each frequency, from 20 Hz to 20 kHz, at its current settings. OmaAudio measures the curve by playing a short click through a separate copy of the effect, so it is right for any equaliser or filter, whoever made it. Each band the effect has is a numbered handle on the curve (**L** and **H** for low and high shelves, **HP** and **LP** for high-pass and low-pass). Drag a handle sideways to change its frequency and up or down to change its gain. Use the mouse wheel over a handle, or drag with `Shift` held, to make the band narrower or wider. A band that is switched off shows as a hollow handle. Moving a handle changes the same controls as the sliders below it, so, like a slider, it is not an Undo step. A bypassed effect draws its curve dashed: that is what it will do when it is on. When the effect's controls cannot be matched to bands, you see the curve without handles.

Effects run in order. To affect the whole mix, add an effect to the **master bus** on the Mastering page. This is separate from effects on individual tracks.

To compare before and after for the whole project, choose **Effects › Hear Without Effects** (`Alt+B`). Every effect on tracks, clips and the master bus is off while it is on, and the status bar says **Without effects**. Press `Alt+B` again, or click that notice, to hear the effects. It only changes what you hear: the project and exports keep their effects.

To change only part of a track, select a time range on the Take page and choose the effect from the **Effects** menu. The selection starts playing through the effect straight away and keeps looping, so you hear each change as you make it. **Pause** (or `Space`) stops it, **Loop** turns the repeat off, and **Before** and **After** switch the effect off and on without stopping. The small picture shows where you are in the selection; click it to jump. **Reset all** puts every control back to its default, and **Preset** and **Save…** use the effect's saved settings. When you like it, **Apply** writes it into the selected audio as one Undo step, even when you left every setting as it was (an effect can change the sound at its defaults). If the effect leaves the audio exactly as it was, the status line says so and there is nothing to undo. **Apply <effect> Again** in the Effects menu (`Ctrl+Shift+Alt+R`) writes the same effect, with the same settings, into whatever you select next, even in another file.

Two level effects come with OmaAudio, so they need nothing extra. **OmaAudio Limit Peaks** keeps every peak at or under its **Ceiling**: it looks 5 ms ahead, so the level eases down before a peak instead of clipping it, and **Release** sets how quickly it comes back. **OmaAudio Change Level** makes the sound louder or quieter, from −48 to +24 dB. Add them from the effect browser like any other effect; recipes use them for **Limit peaks** and **Change level**.

The **Plug-in Manager** lets you search and filter effects, star effects, and allow or prevent new uses of an effect. Disabling one there does not stop copies already in your project. Restart OmaAudio after installing new plugins.

#### Reading and setting an effect's controls

Controls are grouped in titled boxes, as the effect groups them (or one **Controls** box). Each slider has a numbered scale above it; frequency and time controls are usually spaced so each step of ten gets the same room. The value and its unit are on the right:

- Click the value to type a new one. Enter sets it, Escape leaves it as it was. You can type `1.2k` for 1200 Hz.
- A small mark under the slider shows the effect's default. Once you move a control, a dot appears beside its value: click the dot, or double-click the slider, to put the default back.
- On/off controls are switches. Two or three named values are buttons; more are a menu.
- The effect's own readings, such as how much a limiter is turning the sound down, show as meters while the project plays. They sit in their own box under the controls, so a long list of controls never scrolls them out of view.

- A level the effect gives as a plain factor, such as a gain of 0.0156 to 64 or 0 to 10 "G", reads in decibels; type it in dB too (`-inf` for silence where the control goes down to nothing). A ratio reads as `4.0 : 1`.
- Input and output levels read in dB, from −60 to 0. A left and right pair is one meter with two bars, and a red light at its end shows when the level reached 0 dB. Gain reduction fills amber, with the bar full at 24 dB. Level bars turn amber above −6 dB and red above −1 dB. When the meters rest, a line above them says why: **Play to see the meters.**, the effect is bypassed (its power is off or **Before** is chosen, so it is not running), or **Hear Without Effects** is on. A muted track's effects still run, so its meters move. Readings an effect only uses for its own drawing are left out.
- Controls that only set up the effect's own window (its graph's zoom, an FFT display, which tab it shows) are gathered at the end in a **Display** group, folded away at first.
- A long effect that does not group its controls, but numbers its parts (Delay 1, Delay 2…), is grouped by those parts, and each name leaves out the part: "High-cut frequency" under **Delay 3**.

Compressors, limiters, gates and expanders with up to 16 controls show them as knobs: drag a knob up or down (hold Shift for fine steps), roll the wheel over it, or click its value to type. A multiband one, with dozens of controls, uses rows. Click a group's title to fold it away to one line of its values; OmaAudio remembers that for the effect. An effect's own on/off switch, when it has one and does not group its controls, comes first.

CLAP controls show the effect's own value labels and units when available, including
while you drag a slider. Range and default help use those labels too. If an effect
cannot supply usable text, the rack shows a number. A label-only change does not
change the sound or add an Undo step.

**Parameter presets** save an effect's settings for reuse: press **Save…** in its panel. Choose a name and Save, or select a saved preset and Apply. Direct changes in an effect's panel are not individual Undo steps; save a preset if you want to return to a sound. Removing effects and applying a preset are undoable.

#### Starting points

The **Preset** menu in an effect's panel (and in the Apply panel, and for an effect on a clip) lists, under their own headings:

- **Starting points**: settings OmaAudio ships for common jobs, such as a podcast voice, a vocal plate reverb or a −1 dB peak ceiling for podcast platforms. They are deliberately moderate: choose one, listen, then adjust to the recording.
- **From the effect**: presets the effect came with, when it has any.
- **Your presets**: the ones you saved with **Save…**.

Choosing one is one Undo step. A starting point sets the controls it is about and puts every other control back to its default, so it always sounds the same however the effect was set before. Starting points cannot be changed; change the controls and press **Save…** to keep your own version. **Default** puts every control back to the effect's default.

Starting points are included for these effects when they are installed:

| Effect | Starting points |
| --- | --- |
| Calf Compressor | Podcast voice · Interview levelling · Broadcast voice · Gentle vocal · Mix bus glue · Drum bus · Parallel drums · Bass control · Acoustic guitar |
| Calf Mono Compressor | Podcast voice · Interview levelling · Broadcast voice · Gentle vocal · Parallel drums · Bass control · Acoustic guitar |
| Calf Gate | Dialogue noise gate · Gentle expander · Drum gate |
| Calf Deesser | Lower voice · Higher voice · Gentle de-ess |
| Calf Limiter | Podcast, peaks at −1 dB · Streaming music, −1 dB · Broadcast, peaks at −2 dB · Safety catch |
| Calf Equalizer 5 Band | Voice clarity · Rumble cut · De-mud · Presence lift · Air lift · Telephone |
| Calf Equalizer 8 and 12 Band | Voice clarity · Rumble cut · De-mud · Presence lift · Air lift · Telephone · Acoustic guitar · Bass weight |
| Calf Filter | Rumble cut · Voice low cut · Hiss cut · Telephone band |
| Calf Reverb | Small room · Dialogue ambience · Vocal plate · Medium room · Concert hall |
| Calf Vintage Delay | Slap-back · Quarter-note echo · Ping-pong eighths · Doubler |
| Calf Exciter | Voice presence · Air · Gentle sparkle |
| Calf Stereo Tools | Mono (both channels) · Left channel to both · Right channel to both · Swap left and right · A little wider · Narrower |
| x42-comp (compressor) | Podcast voice · Gentle vocal · Mix bus glue · Drum bus |
| x42-dpl (peak limiter) | Podcast, true peak −1 dB · Streaming music, −1 dB · Broadcast, true peak −2 dB · Safety catch |
| x42-eq (parametric equaliser) | Voice clarity · Rumble cut · De-mud · Presence lift · Air lift · Telephone · Acoustic guitar |
| LSP Compressor | Podcast voice · Gentle vocal · Mix bus glue · Drum bus · Bass control |
| LSP Limiter | Podcast, true peak −1 dB · Streaming music, −1 dB · Broadcast, true peak −2 dB · Safety catch |
| LSP Gate | Dialogue noise gate · Drum gate |
| LSP Deesser | Lower voice · Higher voice |
| LSP Parametric Equalizer x8 | Voice clarity · Rumble cut · De-mud · Air lift · Telephone |
| OmaAudio Low Cut | Spoken voice · Deep voice · Close microphone · Phone recording |
| OmaAudio Hum Removal | 50 Hz mains · 60 Hz mains · Strong buzz, 50 Hz · Strong buzz, 60 Hz |
| OmaAudio Voice Cleanup | Light · Medium · Strong |
| OmaAudio Width | Mono · Narrower · Wider |
| OmaAudio Limit Peaks | Podcast, peaks at −1 dB · Streaming music, −1 dB · Broadcast, peaks at −2 dB · Safety catch |
| OmaAudio Change Level | 6 dB quieter · 3 dB quieter · 3 dB louder · 6 dB louder |

The delay starting points that follow the beat assume 120 BPM; set **BPM** to the song. A peak ceiling is not a loudness target: set the loudness first (**Effects › Set Track Loudness…** or **Match** in the Sound tab), then add a limiter starting point.

### Recipes

A recipe is a named list of steps that runs in order, in one click. Each step is written into the audio, as **Effects › Apply** does, and the whole run is one Undo step, shown in History as "run" and the recipe's name. Open the **Recipes** tab in the right panel, or choose **Effects › Recipes › Show Recipes…**.

1. Under **Run on**, choose where the recipe goes: **Selection** (the selected time range, across both channels), **This track** (the whole active track) or **Files…** (opens [Process Many Files](#process-several-independent-files) with the recipe chosen). The line underneath says what a run would cover, or what to do first.
2. Press **Run** beside a recipe (**Run…** when Files… is chosen).
3. The tab shows each step as it runs. **Stop** puts the audio back as it was.
4. When it finishes, **Undo** takes back the whole run; **Done** closes the report and keeps the result. Edit › Undo also takes back the run in one step.

A step that cannot run, for example because the effect it needs is not installed, is skipped and shown with the reason. The other steps still run.

Three recipes are built in:

| Recipe | Steps |
| --- | --- |
| Podcast voice | Low cut below 80 Hz, Mains hum at 50 Hz, Voice Cleanup 60 %, Harsh "s" 35 %, Even out 50 %, Room echo 40 % (off), Loudness to target −16 LUFS, Limit peaks at −1 dB |
| Clean up a phone recording | Low cut below 120 Hz, Voice Cleanup 70 %, Find and Repair Clicks at sensitivity 50 %, Even out 60 %, Presence 40 %, Loudness to target −16 LUFS, Limit peaks at −1 dB |
| Music bed under speech | Loudness to target −24 LUFS, Limit peaks at −1 dB, Fade in over 2 s, Fade out over 3 s |

The **⋯** button beside a recipe has **Edit…** and **Duplicate**, and for your own recipes **Rename…** and **Delete…**. A built-in recipe cannot be changed: **Edit…** opens a copy for you to change and save as your own, and the built-in one stays as it was. Your own recipes are listed under **Your recipes**. Recipes are kept on this computer and work in every project. They are not saved in the project file.

To make or change a recipe:

1. Press **New recipe**, or choose **Edit…** in a recipe's **⋯** menu.
2. Type a **Name**.
3. Press **Add step…**, type part of a step's name under **Find a step**, and choose it. It is added as the last step, at its usual settings. A step that needs an optional plugin you have not installed is greyed out; hold the pointer over it to see which.
4. Press **Change…** on a step to set it. **Done** closes its settings.
5. Steps run from top to bottom. Drag a step by its dots to move it, or use **Move Up** and **Move Down** in its **⋯** menu, which also has **Remove Step**. The switch on a step turns it off, so the run skips it, without removing it.
6. Press **Save** to keep the recipe, or **Cancel** to leave without keeping the changes.

**Record steps** builds a recipe from what you do. While it is on, work as usual: each effect you apply with **Effects › Apply** or **Apply Again**, Voice Cleanup, Room echo, Find and Repair Clicks, each fade and each Shorten or Remove in Tidy pauses becomes a step, with its settings. The audio changes as normal. A strip along the bottom of the window counts the steps; **Stop and keep** adds them to the recipe, and **Discard** forgets them.

| Step | Settings |
| --- | --- |
| Low cut | Removes sound below a frequency, 20–300 Hz. |
| Mains hum | 50 Hz or 60 Hz. |
| Voice Cleanup | How much, 0–100 %. |
| Room echo | How much, 0–100 %. |
| Find and Repair Clicks | Sensitivity, 0–100 %. |
| Loudness to target | A target from −30 to −10 LUFS. Measures the range and changes its level to reach the target. |
| Limit peaks | No higher than −12 to 0 dB. |
| Even out | How much, from Natural to Tight. Uses the Calf Compressor. |
| Change level | By −24 to +12 dB. |
| Fade in, Fade out | Over 0.1–10 seconds: the first or last seconds of the range. |
| Harsh "s" | How much, from Light to Strong. Uses the Calf Deesser. |
| Presence | How much, from Soft to Bright. Uses the Calf Exciter. |
| Width | 0 % is mono, 100 % as recorded, 200 % twice as wide. |
| Shorten pauses | Pauses longer than 0.2–5 seconds become 0–3 seconds long (0 leaves only a little air either side). **Other tracks** **Stay**, or **Move too** so they stay in step; then a pause must be quiet on every voice track, so no one's words are cut. Music never moves. Later steps work on the shorter range. Works like [Tidy pauses](#tidy-pauses) with Auto level and Keep breaths on. |
| Installed effects | Any installed effect. Its own controls are its settings; it starts at its defaults. |

**Effects › Recipes** lists every recipe. Choose one to run it on the selection, or on the whole active track when nothing is selected. **Run Last Recipe Again** (`Shift+Alt+R`) runs the recipe you last ran the same way; once you have run one, it reads **Run** and the recipe's name **Again**.

### Quick Fixes

These panels provide simpler controls for compatible installed effects. Start with a preset, press **Apply**, listen, then adjust. A preset is a starting point; it has not analysed your recording.

| Effect | What it helps with |
| --- | --- |
| Dynamics | Reduces loud passages to make the level more even. |
| Noise Gate | Turns down quiet background sound between words or notes. It cannot remove noise underneath speech. |
| De-Esser | Softens sharp s and sh sounds. |
| EQ | Adjusts bass, middle and high tones. |
| Brightness | Adds brightness and upper tones. It does not remove noise. |
| Ducking | Turns this track down while a chosen trigger track plays, such as music under speech. |
| Pitch Correction | Moves a sung note towards the nearest note of a chosen scale (or any semitone with Chromatic). **Natural** keeps slides and vibrato; **Strong** snaps quickly for an obvious effect. Works on one voice at a time and uses the installed x42-Autotune. It has no switch of its own: turning it off bypasses the effect in the rack. |

If an effect is missing, the panel names the plugin to install. Changes in these panels wait until you press Apply. **Reload** discards unapplied changes. Undo restores the previous applied settings.

### Understand effect controls

Hold the pointer over an individual control for help that matches the chosen effect.

| Control | Meaning |
| --- | --- |
| Threshold | The level where the effect starts responding. A compressor turns down sound above it; a gate reduces sound below it. |
| Ratio | How strongly the effect reduces level changes. Higher compressor ratios reduce loud passages more. |
| Attack | How quickly the effect starts responding, in milliseconds. On a gate, it controls how quickly the sound opens up. |
| Release | How quickly the effect lets go after the sound passes back across the threshold. |
| Hold | How long the effect keeps acting before it starts to release. |
| Makeup | Level added after reducing louder sound. Use it to balance the before and after levels. |
| Mix or effect amount | How much processed sound is blended with the original. |
| Frequency | Which pitch range is affected. Lower values affect bass; higher values affect higher tones. |
| Gain | How much the level changes. Positive dB values are louder; negative values are quieter. |
| Q | For a bell-shaped EQ band, how narrow the affected range is. A higher Q makes it narrower. Shelf controls use Q to shape the transition. |
| Bypass | Hear the sound without this effect, while keeping its settings. |

### Voice Cleanup

**Effects › Quick Fixes › Voice Cleanup…** reduces background noise in speech using the built-in RNNoise tool. It works on your computer without sending audio elsewhere.

Select a short speech passage, open the panel, choose a **Cleanup amount**, and apply. At 100% you hear the fully cleaned result. Lower amounts blend more original sound back in.

Listen for changes to the voice as well as the noise. Undo restores the previous audio. This tool is designed for speech; it may remove or change wanted parts of music.

### Spectral

The Spectral workspace shows sound as a picture. Time runs from left to right. Low frequencies are near the bottom; high frequencies are near the top. Colours show how loud each part is. This helps you find a click, hum or whistle without selecting all sound at the same time.

Click the track you want to repair and open **Spectral**. The display may take a moment to appear for a long recording.

| Selection tool | Use it for |
| --- | --- |
| Time | All frequencies during a chosen time range. |
| Rectangle | A box covering both time and frequency. |
| Band | A chosen range of frequencies. |
| Lasso | Drawing around an irregular sound. |
| Brush | Painting the exact area to repair. |
| Harmonics | A tone and its related higher tones, such as hum. |

A **mask** is the area you have painted or drawn. Add to it, subtract from it or use **Feather** to soften its edges. Selecting an area alone does not change the sound.

### Reduce or heal a selected sound

**Change level** turns the selected area up or down. A negative Gain reduces it; 0 dB leaves it unchanged. Start with a small reduction and listen before using a stronger one. Older notes may call this **Attenuate**.

**Heal the selection** estimates a replacement from the surrounding audio. Keep the selection close around a short unwanted sound. It can help with a click or short interruption, but cannot reliably recreate a long missing performance. Undo lets you compare the repair.

**Repair brush** selects and repairs a small area in one action. Choose its size first. If the result affects wanted sound, undo it and try a smaller area.

### Reduce steady noise

1. Find a short part containing only the unwanted background noise—no voice or music you want to keep.
2. Select it and press **Learn noise**. This teaches the tool which sound to reduce.
3. Select the passage you want to clean.
4. Set a modest reduction amount and press **Reduce noise in the selection**.
5. Listen and compare using Undo. Too much reduction can leave a watery or metallic sound.

A noise sample is a short stretch of the unwanted sound, not a general recording profile. Learn a new one if the background noise changes.

### Hum, clicks, clipped peaks and reverb

**Hum Comb 50 Hz** and **Hum Comb 60 Hz** select mains hum and related higher tones. Use the frequency that matches your noise, then use **Change level** to reduce the selection. A selection by itself does not remove the hum.

**Find clicks** lists suspected clicks. Inspect and repair them individually, or use **Find and Repair** for the detected list. The detector may also find wanted drum hits or sharp notes, so compare the result.

**Restore clipped peaks** tries to repair the flattened tops of recordings made too loud. It cannot recover every distorted sound. Uncertain peaks and long flattened sections may be left alone.

**Reduce late reverb** lowers lingering room sound after words or notes. It cannot turn a very echoey recording into a completely dry one. Start gently, listen, and undo if wanted sound changes too much.

### Change the spectral display

The **Display** tab lets you choose colours, brightness, stereo layout and a linear or logarithmic frequency scale. These display choices do not change the sound.

The analysis settings also affect the detail available to repairs:

- **FFT size** trades timing detail for frequency detail. Larger sizes separate nearby tones more clearly; smaller sizes show short sounds more clearly.
- **Window** shapes the audio used for each analysis step. Hann is a useful starting point.
- **Overlap** controls how much neighbouring analysis steps overlap. More overlap gives smoother detail but takes more processing.

If you are unsure, start with the existing settings. The display updates in the background when you change the analysis settings.

## Record and connect equipment

Follow [Recording](#recording) to capture a new take. [Audio Setup](#audio-setup) helps you check inputs, outputs and connections.

### Recording

1. Open **File › Record…** or press the red Record button.
2. Choose the input device and mono or stereo.
3. To record over an existing track, first press its small **R** button. Otherwise the recording is added as a new track just under the track you are working on, and the view scrolls to show it.
4. Press **Start recording**. Use **Space**, Stop or the bottom **Stop recording** button to finish.
5. Play the take back, then Save the project. Undo removes the completed take.

**Monitor primary input** lets you hear the incoming sound without effects. It starts off; use headphones to avoid the microphone picking up the speakers. Only the main input is monitored.

**Additional input** records another device to its own track. Up to eight inputs can be recorded together, within the project's track limit.

### Punch-in, count-in and loop takes

**Punch into selected range** replaces only a selected interval on a track prepared for recording. Select the interval and press the track's R button before starting.

**Count-in beats** gives you clicks before recording. **BPM** is their speed in beats per minute. **Pre-roll** plays a short lead-in before the point where the take starts.

**Loop passes** records several takes of the selected range. Zero turns it off; 1–16 chooses the number of passes. Earlier takes are kept on muted tracks, in order, just under the track you are working on. The last take goes to the prepared track, or a new track if none is prepared. Undo removes the set together.

**Align overdubs to device timing** uses the device's timing reports to line up a new recording with existing tracks. If you measure a remaining delay, enter a correction in milliseconds. A positive correction moves the recorded audio earlier. Check the result with your own device when timing matters.

### Audio Setup

Open **Window › Audio Setup…** to check inputs, outputs and connections. **Refresh** reads them again. Choose the system's default input and output in your desktop sound settings.

PipeWire is the Linux sound service OmaAudio uses. The **device rate** is the rate used by the sound system; it may differ from the project's rate. The **audio buffer** is the small block of samples processed at once.

JACK audio applications can appear here when the optional **pipewire-jack** package is installed. **Send playback here** connects OmaAudio's sound to the chosen app. **Disconnect playback** removes those connections. These controls connect audio; they do not link the apps' play and stop buttons.

## Finish your project

Check the whole mix in [Mastering](#mastering), then [export an audio file](#export-an-audio-file). Save the project as well if you want to edit it again.

### Collect a portable project

Choose **File › Collect Project…** (`Ctrl+Shift+Alt+J`) and give the copy a new
`.omaaudio` name inside an existing folder. Press **Collect project**. This copies
the current session, all retained source audio and the reference video. It keeps
the current project path, edits, Undo history and original files unchanged. Later
edits do not change a collection already in progress.

Move the **whole project folder** to the other machine. `requirements.toml` lists
the LV2/CLAP/VST3 insert identifiers, track/master/clip slots and bypass states at collection
time. Install compatible plugins separately; their binaries, versions and
availability are not included or verified. Collect again after changing effects
to refresh this inventory. Audio original paths are retained as provenance and
are not needed to reopen the copy.

The panel shows progress and **Cancel collection**. Closing the panel keeps the
copy running; closing OmaAudio cancels unfinished collection. Existing files or
folders are never replaced. A missing reference video must be relinked or removed
before collection can succeed. A late disk-sync failure reports that the copy
exists and identifies its path.

Collected pictures stay inside the bundle through Save, Save As and recovery.
Relinking deliberately uses the new external picture until you collect again.
Bundles with collected video use project format 15; older OmaAudio builds refuse
them rather than opening the wrong reference.

### Mastering

Mastering lets you check and adjust the complete mix before export. Track effects and the master effects are included in the mix analysis.

Use **Analyse mix** to check overall loudness and the highest peaks. **LUFS** describes how loud the whole mix feels; a more negative value is quieter. **True peak** estimates the highest playback level, including peaks between audio samples.

Compare with and without the master effects using the before/after controls. Matching the listening level helps you judge the sound without simply preferring the louder version.

The **monitor level** changes what you hear through your speakers or headphones. It does not change the exported file. Track levels and the master-bus level do affect it.

For one track, **Loudness…** measures that track with its processing and routing, and can adjust its level towards a target. A peak limit can prevent reaching the requested loudness exactly.

### Export an audio file

1. Open **Mastering** and choose a **Target** preset if one suits the intended use.
2. Choose the file format, sample rate, bit depth and channels along the bottom. Changing a preset's fields makes it Custom.
3. Choose **Keep current level**, **Set peak level** or **Set loudness**.
4. Press **Export…**. Its menu asks how much to export, **Whole project** or **Time selection only**, then what: **Full mix** or **Selected tracks**. Choose a name and folder, and confirm.
5. Watch the **Queue** tab or the bottom progress message. When it finishes, use **Show in folder** to find the file.

**File › Export Audio…** uses the same settings from any workspace. Export creates a new audio file; Save keeps the editable project.

| Level setting | Result |
| --- | --- |
| Keep current level | Uses the mix as it is, including track and master effects. |
| Set peak level | Raises or lowers the whole mix so its highest peak reaches the target. |
| Set loudness | Adjusts overall loudness towards a LUFS target while staying below the true-peak limit. |

**Selected tracks** exports one file per selected track instead of the mix: select them by Ctrl-clicking or Shift-clicking track headers on the Arrange page, or tracks in **Tracks in the Mix** on the Mastering page, before choosing **Export…**. Each file is named after the export and the track, for example `Mix - Dialogue.wav`. A stem includes the track's clip effects, track effects, fader, pan and any tracks routed into it; tracks routed into another track are heard in that track's stem. The master bus effects and level are left out. Each stem appears in the Queue as its own export.

Loudness adjustment uses one overall level change. If the peak limit prevents the target loudness, the file will be quieter than requested. It does not automatically even out individual passages.

**Time selection only** exports just the time range you selected before choosing **Export…**; it is picked for you when there is one, and the menu shows the range. It works for the full mix and for selected tracks. The file holds exactly the selected stretch, sample for sample what the whole export has there: a reverb or echo carried in from before the range is heard, and sound still ringing at the end of the range is cut off there, as the whole export ends where the last clip ends. Markers inside the range are written at their place in the shorter file; markers outside it are left out.

An export can be cancelled. **Export again** retries it with the same settings and path. Removing an entry from the queue does not delete a finished file.

### Process several independent files

1. Choose **File › Process Many Files…** (`Ctrl+Shift+Alt+B`) and use **Add files…** to select the inputs.
2. Choose an existing output folder and a filename suffix. The default suffix is
   `-processed`; review the resulting paths before starting.
3. Choose the format, depth, rate, channels and level settings. **Original** keeps
   each file's own rate or layout. **Mono** averages its channels; **Stereo** keeps
   stereo or duplicates mono. Larger layouts require Original or Mono.
4. Choose **None**, **Focused track**, **Master inserts** or **Recipe** under Effects,
   and for Recipe choose the recipe. When you press **Process files**, OmaAudio
   captures the chosen effects' current settings, or the recipe's steps.
   **Preparing effects** appears while that copy is being prepared.
5. Read the result for each file, including its output, format, loudness and any warning.
   A failed file does not stop the remaining files.

With a recipe, its effects become the chain, run in order on each file before the
level and format settings. **Loudness to target** sets the delivered loudness
instead, done last, and **Limit peaks** also sets the peak ceiling. Room echo,
Find and Repair Clicks, fades and Shorten pauses are not done on files; the summary names any step
that is left out. Voice Cleanup on files needs a 48 kHz output rate.

This processes each complete input file. Track and master faders, clip edits,
automation, trim and routing are excluded. Bypassed effects are omitted. A chain
using another track as a sidechain is refused; a missing effect or incompatible
channel layout fails that file. Rate conversion happens before the effects.
Later session edits, or opening a different project, cannot change accepted jobs.
The batch makes no project edits or Undo steps.

Originals and existing output files are never replaced, including duplicate names
and files created while processing. Choose another folder or suffix to make a
new version. **Keep file metadata** preserves supported broadcast fields and markers
from each input in WAV-family output, rescales their timing and measures loudness
again. Other containers report any metadata limitations. Lossy-format loudness
reports describe the signal supplied to the encoder.

Use **Cancel** on one result, or **Cancel unfinished** for every pending file.
Completed outputs remain; partial files are removed. **Retry** keeps that row's
captured settings and paths; restore a missing input first. If preparation failed
or was cancelled before effect settings were captured, queue a new batch from
the session you want. Stop recording before preparing a batch with live plug-in
state. **Clear finished rows** removes completed, failed and cancelled results
without deleting files; paused rows stay in the queue. Up to 512 rows can be kept at once.
Closing the panel keeps processing; the status bar reopens it.

The queue and captured effect settings survive app restarts. Unfinished files
reopen **Paused** and wait for **Resume** on a row or **Resume paused** for the
queue. A resumed file starts again from its beginning. Review the paths first:
inputs and effects must still be available, and any existing output blocks that
file. After an unexpected exit, an output may have finished before its result was
saved; inspect it before deciding to queue another version. Completed results
remain in the list. A job closed during **Preparing effects** needs a new batch
from the intended session because its settings were not fully captured.

One OmaAudio instance owns the saved queue at a time. If another instance owns it,
or the saved queue cannot be read or updated, the panel explains why processing
is unavailable. Close the queue-owning instance before reopening OmaAudio to use
that queue. If the saved queue itself is damaged, **Start a new queue** sets it
aside — the damaged file stays beside the new one — and starts an empty queue.
Files already written are not touched; queue any unfinished files again. Leftover
temporary files from an unexpected exit are tidied away the next time the queue
opens. Queue recovery is independent of the open project and Undo history.

### Choose a format

| Format or setting | When it helps |
| --- | --- |
| WAV or AIFF | An uncompressed audio file for further editing or delivery. |
| FLAC | Keeps the audio without losing detail, usually in a smaller file. |
| MP3, AAC/M4A, Ogg Vorbis or Opus | Smaller files for sharing or playback. Some audio detail is removed to save space. Availability depends on installed support. |
| BWF | A WAV file with extra broadcast details, such as a description and starting time. |
| RF64 | A WAV-style file that can hold audio too large for ordinary WAV. |
| Bit depth | The precision used to store samples. 16-bit and 24-bit are common; 32-bit float allows a wider range for further processing. |
| Bitrate | For compressed formats, how much data is used each second. Higher values usually give a larger file and better quality. |
| Dither | Very quiet noise added when reducing to an integer bit depth, to avoid distortion in quiet parts. Usually leave it on for a final 16-bit or 24-bit export. |

OmaAudio offers supported settings and explains incompatible choices. AAC/M4A needs FFmpeg. If WAV would exceed its size limit, it is written as RF64 and the app tells you.

### File details and markers

**Metadata…** means the descriptive details stored with an export. You can enter a description, creator information, date and a time reference, and choose whether to include project markers.

The preview shows what the next export will contain. BWF includes broadcast details; WAV and RF64 can include them when enabled. A **time reference** says where the audio starts against a clock or another timeline. Leave it empty if your delivery does not need one.

## Work with picture

Add a [Video reference](#video-reference) to see picture while you edit its sound. Video and audio use the same playback position.

### Video reference

Choose **File › Import Video Reference…** to show picture while editing its sound. The video appears in the right-hand **Video** tab and follows the audio playhead. FFmpeg is required.

Only one reference video is used per project. Its sound is not imported automatically; add an audio file separately if needed.

- Type **Starts at** to place the beginning of the video on the timeline.
- **At playhead** places its start at the current playback line.
- **Place by timecode** uses the video's starting timecode and the project's time reference.
- **Alt+,** and **Alt+.** move back or forward by one video frame.
- **Detach** shows picture in a separate window. Use **Replace…** for a different video or **Relink…** if its file has moved.

Timecode displays hours, minutes, seconds and video-frame numbers. **Drop-frame** is a numbering method for 29.97 and 59.94 fps that stays close to clock time. It skips two or four numbers at each minute except every tenth minute; it never removes picture frames.

If video playback cannot keep up, picture frames may be skipped so the sound can continue. This is an editing preview, not a connection that controls an external recorder or another app's playback.

## Keep your work

[Saving](#saving) keeps your editable project. [Recovery and storage](#recovery-and-storage) explains autosave, unfinished recordings and cleanup.

### Audio load and what your computer can play

The status bar shows **Audio load**: how much of each audio block OmaAudio needs to prepare the sound while playing. The dot is green normally, amber above 70% and red above 90% or after a dropout. Above 100% a block is late and you hear a dropout. If playback drops out or stays near the limit, a message says so.

Click it, or choose **Window › Audio Load…** (Ctrl+Alt+Z), for the recent peak, the number of dropouts, the buffer size and the effects using the most time. To lower the load, choose a larger buffer in **Audio Setup** (smoother playback with a little more delay), bake clip effects you have finished with, bypass or remove heavy effects, or mute tracks you are not using.

**Check this computer** (in that panel and in **Preferences › Performance**) mixes generated sessions for a few seconds and says how many tracks fit in half of each audio block at your sample rate and buffer size: plain tracks, tracks with dense automation, and tracks routed through a bus. Effects use the other half, so heavy plug-ins lower these numbers; the live Audio load shows the real figure for your project. A project can hold up to 256 tracks.

### Saving

**File › Save** stores the project in a folder ending in **.omaaudio**. Projects saved by earlier versions end in **.omawave**; they open the same way and keep their name when saved. It includes the audio copies, clips, effects and settings needed to continue editing. Keep the whole folder when moving the project to another computer.

Use [Collect Project](#collect-a-portable-project) to make a separate copy that also includes reference video and lists required plugins.

**Save As…** makes a project in a new folder and continues working there. Use it to keep another version. A compatible effect still needs to be installed on the other computer to hear its processing.

Wait for an operation to finish before saving if you want its result included. A queued save keeps the session as it stands when the save begins; changes made afterward need another save.

Save and recovery include extra state supplied by compatible effects. New saves retain those settings when reopened, including independent mono instances. Later changes in the Track Effects panel or an applied parameter preset still take precedence. Capturing that state can interrupt playback. During recording, these saves wait until the take finishes. If an effect cannot supply its state, the previous saved files remain intact and the session stays unsaved.

Changes reported by CLAP effects mark the project unsaved and start the recovery timer, including hidden settings and changes made while an effect is bypassed or playback is stopped. If a change arrives after a save starts writing, the project stays unsaved until the next save. Completed gestures reported by supported CLAP effects appear in History as **change plug-in settings**. The generic rack follows their captured values without interrupting an active slider drag. Changes from effects that do not report gesture boundaries may be grouped together.

Supported effects can also report that a preset changed their parameter values. OmaAudio refreshes the rack and captures those settings for Save and History without recording extra automation points. Older queued knob movements cannot replace a newer reported preset; knob movements made afterward still take effect. A failed capture keeps the update pending and retries.

If an effect's change queue overflows, OmaAudio tries to recover its current settings. For supported CLAP effects with earlier settings available, History shows **recover plug-in settings**: Undo returns to the last known settings and Redo restores the recovered settings. Individual changes lost from the queue cannot be separated into their original steps. A failed capture reports an error and retries; empty gestures do not add an edit.

Before changing a track's effects, a track, preset, sample rate or history position, OmaAudio captures supported live effect settings. A failed capture leaves the requested change unapplied and reports the problem. If the history changes while an Undo, Redo or history jump is waiting, that request is refused; review History and try it again. During recording, finish the take before requesting these effect/history changes.

### Recovery and storage

Autosave writes a recovery copy after a short pause when there are unsaved changes. The interval is in **Preferences**. **File › Autosave Now** requests a recovery copy. Save the project yourself as well.

After an interruption, OmaAudio offers recoverable work when it starts. Unfinished recordings can also be recovered from the Recording panel. The final moments of an interrupted recording may be missing.

**File › Storage and Cleanup…** shows the audio kept by the project. Audio still needed by a clip or Undo history is kept. Cleanup removes only audio no longer needed, when you ask. It does not delete the original files you imported. Undo history is available while editing but is not saved as a history list inside the project. It retains up to 256 commands and 512 MiB of distinct opaque effect-state data. Older steps disappear when either limit is reached; this limit is separate from audio and the project's other memory use.

### Project settings and preferences

**Project Properties…** sets musical timing, the project's sample rate and default export data type. **Enable project tempo and meter**, enter the quarter-note tempo (1–480 BPM, up to three decimal places), beats per bar (1–32) and beat note (1, 2, 4, 8, 16 or 32), then use **Apply musical timing**. This is one Undo step and saves with the project. It sets the musical display and optional snap grid without stretching audio, moving existing clips, or changing the recording count-in. Effects that follow the host tempo — CLAP, VST3 and LV2 plug-ins with tempo-synced delays, LFOs or gates — receive this tempo, meter and the playback position, during playback and in exports. **Reload timing** discards the draft and reads the current project settings. The sample settings have their own Apply button. Rates from 8 to 192 kHz are supported. Changing the rate stops playback while the audio is converted; timing in seconds stays the same, and Undo restores the previous rate. The clipboard is cleared.

**Preferences…** holds your editing, recovery, spectral-display and export defaults. If a setting is invalid, the message names the page and field to correct.

**Edit › Snap Grid** adds an optional musical grid to the magnet button's Snapping setting. Choose a bar, beat, half, quarter, eighth or triplet division of a beat. Enable musical timing in Project Properties first. Beat divisions follow the meter: in 7/8, half a beat is a sixteenth note. The ruler shows accent marks for the grid in any time display; at wide zooms it shows fewer marks for readability.

With a grid chosen, selection edges and moved clips use the nearest grid point unless a clip edge, marker or the playhead is within the usual snapping distance. A clip can align either endpoint; moving selected clips keeps their spacing. Selections stay inside the available audio, and Trim uses that selected range. Typing exact positions, seeking and slipping audio inside a clip keep their existing behavior. Switch the magnet off for free editing, or choose **No musical grid** to keep the usual nearby audio targets and selection zero crossings. The choice is also in **Preferences › Editing & recovery** and is remembered across launches.

**Time display** (also under **View › Time Display**) chooses how every time is shown and typed: clock time (`00:00:01.500`), Samples, Decimal seconds (`1.500`), CD frames (`00:01:37`), Bars and Beats (`1:1:000`), or Timecode at a chosen picture rate or the reference video's rate. The video option shows plain time until a reference is available. Bars and Beats shows plain time until musical timing is enabled in Project Properties. The clock, rulers, read-outs and panel fields follow the setting. Changing the display preserves the exact sample positions in your project.

In any display you can type decimal seconds (`1.5`), a decimal clock (`0:01.5` or `1:02:03.125`), or a number with its unit: `25ms`, `1.5s`, `1200smp` (samples). A bare whole number means seconds, except in Samples where it means samples. Whole-number colon forms follow the display:

- **Clock time, Decimal seconds and Samples:** `1:12` is one minute and twelve seconds.
- **Timecode:** `1:12` is one second and twelve picture frames.
- **Bars and Beats:** enter all three fields, `bar:beat:tick` for a position or `bars:beats:ticks` for a duration. Positions start at `1:1:000`; zero duration is `0:0:000`. Each quarter note has 960 ticks, so an eighth-note beat has 480. At 120 BPM in 4/4, position `2:1:000` is two seconds from the start and duration `1:0:000` is two seconds long. In 7/8 a whole bar is 1.75 seconds at that tempo. The ruler follows ticks, beats and bars as you zoom. Timing is constant throughout the project; there is no tempo map.
- **CD frames:** enter all three fields, `minutes:seconds:frames`. There are 75 CD frames per second, numbered 00–74. `00:01:37` is one second and 37 CD frames; `62:03:01` is 62 minutes, three seconds and one frame. CD timing works at any project sample rate and does not change the export format.

Press Up or Down in a time field to move one millisecond, sample, musical tick, CD frame or picture frame; hold Shift for ten steps. CD and musical entry and steps land on the first sample of the frame or tick, even at rates where a frame is not a whole number of samples. Use `…smp` for an exact sample position. Drag sideways to adjust the value; hold Shift for finer movement. Escape discards the current text edit or drag. Changing the display preserves an unfinished edit's exact value; Return or leaving the field accepts it.

The **Performance** page sets how much memory and disk OmaAudio may use to keep things ready:

- **Automatic memory budget** uses a quarter of your computer's memory, up to 8 GiB. Turn it off to type your own **Memory budget**. Half of the budget keeps audio ready for playback, so a larger budget means long or busy sessions seek, loop and play without gaps. It is a target, not a hard limit on the whole application.
- **Spectral cache limit** is the most disk space kept spectral displays may use. A display that took a moment to compute is kept, so reopening a project or undoing back to an earlier edit shows it at once. The oldest displays are removed first.
- **Clear spectral cache** deletes the kept displays now. Your projects, audio and edits are not touched; displays are computed again when needed.

### The manual

Open **Help › User Manual** to read this guide. Search narrows the contents to sections containing your words. Clear the search to see everything again.

Use **Larger text** for the manual and help cards. Press Tab to reach the contents, then use the arrow keys, Home or End to choose a section. Links to other parts of this guide stay inside the app.

**Help › Getting Started** opens short task guides. **Help › Control Help** or **F1** explains a particular control. Keyboard shortcuts are listed below and beside menu actions.

## Terms explained

| Term | Plain meaning |
| --- | --- |
| Active track | The track you clicked and are currently editing. Older notes may call it the focused track. |
| Project or session | Your tracks, clips, effects and settings together. Save it as an .omaaudio folder to continue editing later. |
| Track | A row holding audio clips. Several tracks can play together. |
| Clip | A piece of audio placed at a time on a track. |
| Source | The recording a clip uses. Several clips can use different parts of one source. |
| Source offset | How far into the source file a clip begins reading. |
| Playhead | The line showing the current playback position. |
| Selection | A highlighted time range or area you want to change. |
| Channel | One part of an audio signal: mono has one, stereo has left and right. |
| Gain or level | How much louder or quieter sound becomes. 0 dB gain means no change. |
| Pan | Where mono sound sits between left and right. Stereo tracks use balance. |
| Fader | A slider controlling a track's volume. |
| Bus | A track or output that combines other tracks so they can be processed together. |
| Master | The combined output of the mix. |
| Send | An extra copy of a track sent to another track at its own level. |
| Sidechain or external key | A separate sound used to control an effect, such as speech turning down music. Called Trigger track in routing. |
| Plugin or effect | An added sound processor, such as EQ, compression or reverb. |
| Insert | An effect added to a track's effects list. |
| Preset | A saved group of settings you can reuse. |
| Recipe | A saved list of steps, such as a low cut, cleanup and loudness, run in order in one click. |
| Apply | Use the settings currently shown in a panel. |
| Staged settings | Changes waiting for you to press Apply. |
| Bypass | Turn an effect off without deleting it or its settings. |
| Automation | Stored changes to a setting over time. |
| Ripple edit | An edit that moves later audio to close a gap or make space. |
| Crossfade | A blend from one clip into the next. |
| Attenuate | Make sound quieter. Use a negative amount in Change level. |
| Spectrogram | A picture showing which frequencies are present over time. |
| Frequency, Hz and kHz | How low or high a tone is. Hz means cycles per second; 1 kHz is 1,000 Hz. |
| Mask and feather | The area marked for spectral repair, and how soft its edges are. |
| Harmonics | Higher tones related to a main tone. |
| Noise sample | A short sample containing the background noise you want to reduce. |
| Dry and wet | The original sound and the sound after effects. A Mix control blends them. |
| Latency | A delay introduced by a device or effect. |
| Audio sample | One moment of audio, with a value for each channel. OmaAudio counts positions in samples and shows them as time unless you choose the Samples display. A video frame is a different thing: one picture. |
| Sample rate | How many audio samples are stored each second. 48 kHz is 48,000 per second. |
| dB | A unit used to describe sound level or a change in level. |
| dBFS | Sample level relative to a digital file's maximum; 0 dBFS is the maximum. |
| True peak or dBTP | An estimate of peak playback level, including peaks between stored samples. |
| LUFS | A measure of perceived loudness across audio. More negative is quieter. |
| Clipping | Distortion caused when a signal is too loud for its available range. |
| Export or render | Make a finished audio file from the project. |
| Metadata | Descriptive details stored with a file. |
| Timecode | A position written as hours, minutes, seconds and video-frame numbers. |
| Mono and stereo | One audio channel, or a left-and-right pair. |
| LV2 and CLAP | Plugin formats supported by OmaAudio. Effects using them are installed separately. |
| FFmpeg | Installed with OmaAudio; used for AAC/M4A, MXF audio, processed stretching and the reference video. |

## Keyboard

The menu shows the current shortcut beside each action. These are the built-in shortcuts. If you use a custom keymap, the menu and tooltips show your choices.

| Key | Action |
| --- | --- |
| `Space` | Play / stop |
| `Ctrl+Space` | Stop |
| `Home` | Go to start |
| `End` | Go to end |
| `Left` / `Right` | Back / forward one second |
| `J` / `K` / `L` | Play backwards / stop / play; press `J` or `L` again for 2×, 4× or 8× (on the Spectral page `L` is the lasso) |
| `Alt+1` … `Alt+4` | Take / Arrange / Spectral / Mastering |
| `Ctrl+S` | Save |
| `Ctrl+Shift+Alt+B` | Process Many Files |
| `Ctrl+Shift+Alt+J` | Collect Project |
| `Ctrl+Shift+Alt+S` | Save as |
| `Ctrl+Alt+A` | Autosave on / off |
| `Ctrl+Alt+S` | Write the autosave now |
| `Ctrl+Shift+E` | Export the finished audio |
| `Ctrl+Shift+A` | Analyse the mix |
| `Ctrl+Alt+P` | Add an effect to the master bus |
| `Ctrl+Alt+B` | Hear the mix before the bus, level-matched |
| `T` / `B` / `F` | Spectral: time / rectangle / band selection tool |
| `Ctrl+Shift+B` | Spectral: clear the frequency band |
| `Ctrl+Shift+U` | Spectral: change the selection level |
| `Ctrl+Shift+H` | Spectral: heal the selection |
| `Ctrl+Alt+R` | Spectral: rebuild the display |
| `Ctrl+Alt+F` | Spectral: linear / logarithmic frequency |
| `Ctrl+Alt+N` | Spectral: learn the noise from the selection |
| `Ctrl+Alt+D` | Spectral: reduce noise in the selection |
| `Ctrl+Alt+K` | Spectral: find clicks and pops in the selection, or the take |
| `Ctrl+Alt+J` | Spectral: heal the clicks found |
| `Ctrl+Shift+Alt+K` | Spectral: Find and Repair Clicks — find the clicks and repair them |
| `L` / `P` / `H` | Spectral: lasso / brush / repair brush tool |
| `O` | Spectral: harmonic tool — click a tone to select its related higher tones, or drag a span |
| `Ctrl+Alt+5` / `Ctrl+Alt+6` | Spectral: a hum comb at 50 / 60 Hz over the selection, or the take |
| `F1` | Explain the control under the pointer, or with keyboard focus |
| `Shift+F1` | Open this manual |
| `X` | Spectral: new shapes cut from the mask instead of adding |
| `Ctrl+Alt+X` | Spectral: new shapes keep only what the mask already had inside them |
| `Ctrl+[` / `Ctrl+]` | Spectral: smaller / larger brush |
| `Ctrl+Backspace` | Spectral: undo the last mask shape |
| `Ctrl+Shift+X` | Spectral: clear the mask |
| `Ctrl+Alt+L` | Reset the loudness meter |
| `Ctrl+Shift+P` | Open a project |
| `Ctrl+N` | New, empty project |
| `Ctrl+O` | Open an audio file, starting a new project |
| `Ctrl+Shift+O` | Import an audio file as a new track |
| `Ctrl+Shift+Alt+O` | Import an AAF session as new tracks |
| `Escape` | Close the effect browser |
| `Ctrl`+scroll | Zoom, centred on the pointer |
| `Shift`+scroll | Scroll sideways |
| `Ctrl+=` / `Ctrl+-` | Zoom in / out |
| `Ctrl+0` | Zoom to fit the whole take |
| `Ctrl+E` | Zoom to the selection |
| `` ` `` | Fill the window with the waveform (again puts the panels back) |
| `Ctrl+A` | Select everything |
| `[` / `]` or `I` / `O` | Set the selection start / end to the playhead (on the Spectral page `O` is the harmonic tool) |
| `Ctrl+Shift+Alt+R` | Apply the last effect again, with its settings, to the selection |
| `Shift+Alt+R` | Run the last recipe again |
| `Q` / `W` | Move the playhead to the selection's start / end |
| `Ctrl+D` | Clear the selection |
| `Ctrl+X` / `Ctrl+C` / `Ctrl+V` | Cut / copy / paste |
| `Delete` | Delete the selection, leaving the gap |
| `Shift+Delete` | Delete the selection and close the gap |
| `Ctrl+Alt+Delete` | Delete the selected time across all tracks and close the gap |
| `Ctrl+T` | Trim to the selection |
| `Ctrl+K` | Split at the playhead |
| `Ctrl+I` / `Ctrl+Shift+I` | Fade in / out across the selection |
| `Ctrl+Up` / `Ctrl+Down` | Selection gain, 1 dB at a time |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |
| `Up` / `Down` | Make the track above / below active |
| `Ctrl+Shift+N` | Add an empty track |
| `Ctrl+Shift+W` | Remove the selected tracks (or the active track) |
| `G` / `V` | Clip Move / Selection tool |
| `Escape` / `Ctrl+Escape` | Cancel a clip drag |
| `Ctrl+L` | Toggle playback of the selected loop range |
| `Ctrl+Shift+R` | Recording panel / stop active recording |
| `Ctrl+M` | Mute the active track |
| `Ctrl+Shift+S` | Solo the active track |
| `Ctrl+Alt+Up` / `Ctrl+Alt+Down` | Move the active track up / down |
| `Ctrl+Shift+D` | Duplicate the active track |
| `Alt+=` / `Alt+-` | Make the active lane taller / shorter |
| `Ctrl+Delete` | Silence the selection (leaves the gap, whatever ripple says) |
| `Ctrl+Shift+L` | Normalize the selection to −1 dBFS |
| `Alt+R` | Reverse selection |
| `Ctrl+Alt+1` / `Ctrl+Alt+2` | Convert active track to mono / stereo |
| `Ctrl+Alt+Escape` | Cancel sample editing |
| `Ctrl+Shift+V` | Paste to a new track |
| `M` | Add a marker at the playhead, or a range over the selection |
| `Ctrl+.` / `Ctrl+,` | Next / previous marker |
| `S` | Snapping on / off |
| `R` | Ripple on / off |
| `Ctrl+Shift+M` | Mixer |
| `Ctrl+P` | Effect browser |
| `Ctrl+R` | Track Effects panel |
| `Ctrl+B` | Bypass the active track's chain |
| `Alt+B` | Hear the whole project without effects, to compare (again to hear them) |
| `Ctrl+Alt+O` | Recent documents |
| `Y` | Slip tool: move audio inside a clip |
| `Ctrl+Alt+Left` / `Ctrl+Alt+Right` | Slip audio earlier / later |
| `Ctrl+Shift+F` / `Ctrl+Shift+Alt+F` | Linear / equal-power crossfade |
| `Alt+T` | Pitch and Duration |
| `Alt+F` | Adjust crossfade |
| `A` | Automation |
| `Alt+U` | Routing |
| `Alt+C` | Map track channels |
| `Alt+I` | Insert silence |
| `Alt+P` | Paste and Mix |
| `Alt+S` | Project properties |
| `Ctrl+Alt+I` | Clip properties |
| `Ctrl+Alt+E` | Clip envelopes |
| `Alt+K` | Metronome on or off |
| `Ctrl+Alt+V` | Clip effects |
| `Ctrl+Alt+M` | Level statistics |
| `Ctrl+Alt+Q` | Frequency analysis |
| `Ctrl+Alt+H` | Phase analysis |
| `Ctrl+Alt+T` | Generate tone or noise |
| `Ctrl+Alt+W` | Align speech |
| `Ctrl+Alt+Y` | Fit music to length |
| `Ctrl+Alt+Z` | Audio load |
| `Ctrl+Alt+G` | Clip groups |
| `Ctrl+Shift+G` | Link or unlink grouped clips |
| `Ctrl+Alt+U` | Generate ducking |
| `Ctrl+Alt+,` | Preferences |
| `Ctrl+Shift+Alt+L` | Track loudness |
| `Alt+D` | Storage and cleanup |
| `Ctrl+Shift+Alt+P` | Plug-in Manager |
| `Shift+Alt+P` | Starred effects |
| `Alt+,` / `Alt+.` | Previous / next video frame |
| `Ctrl+Shift+Alt+D` | Quick Fixes: Dynamics |
| `Ctrl+Shift+Alt+G` | Quick Fixes: Noise Gate |
| `Ctrl+Shift+Alt+E` | Quick Fixes: De-Esser |
| `Ctrl+Shift+Alt+Q` | Quick Fixes: EQ |
| `Ctrl+Shift+Alt+C` | Quick Fixes: Brightness |
| `Ctrl+Shift+Alt+U` | Quick Fixes: Ducking |
| `Ctrl+Shift+Alt+T` | Quick Fixes: Pitch Correction |
| `Ctrl+Shift+Alt+V` | Voice Cleanup |

### Custom shortcuts

Preferences shows the location of your keymap file, normally `~/.config/omaaudio/keymap.toml`. The installed example is `/usr/share/omaaudio/keymap/default.toml`. Copy it to your own keymap location, then edit only the shortcuts you want to change. The app picks up changes without a restart. Use different shortcuts for actions you need at the same time.

## Troubleshooting

Choose the problem below, or search this guide using words from the message shown in the app. For a greyed-out control, holding the pointer over it usually explains the next step.

### I cannot hear anything

Check your speakers or headphones, the desktop output choice and **Window › Audio Setup…**. Make sure playback is running, the track and master are not muted, and the monitor level is turned up. Check Solo buttons and Routing if only some tracks are silent.

### A button is greyed out

Hold the pointer over it to read why. You may need an active track, a time selection, an installed effect, or to wait for the current operation to finish. Do not repeatedly click Apply while an operation is running.

### An edit changed the wrong audio

Press Ctrl+Z. Click the intended track and check its name in the bottom status bar. Check the selected range and whether All channels or one channel is selected before trying again.

### Later audio moved after a cut

Check **Ripple Edits**. Turn it off to leave a gap after Cut or Delete. Undo restores the previous positions. Use Delete Time Across All Tracks when you want the same interval removed from every track.

### A file or effect is unavailable

Read the message for the missing file or requirement. AAC/M4A and video need FFmpeg. LV2 and CLAP effects are installed separately. Restart after installing a plugin. For an AAF import, keep the audio export folder and use Audio folder… to locate it.

### My recording is delayed or silent

Check the input choice and its desktop input level. Confirm the device appears in Audio Setup. For timing, check Align overdubs to device timing and measure any remaining delay with your own device. A positive correction moves the recorded sound earlier.

### The repair sounds unnatural

Undo and try a smaller selected area or a gentler amount. For noise reduction, capture a passage containing only the unwanted noise. A voice or note in the noise sample can cause wanted sound to be reduced.

### The export is quieter than expected

Check the chosen level mode and target. A true-peak limit can stop loudness adjustment from reaching its target. The monitor volume affects only listening, so turning it up does not make the exported file louder.

### A project has missing effects or video

Install the same compatible effects on the new computer. Use Relink… in the Video tab if the picture file moved. Keep the whole .omaaudio project folder together when copying it.

### The window or playback becomes slow

Wait for a long import or analysis to finish. Close unused applications, try a smaller spectral analysis size, or bypass demanding effects. Check the status message for audio-device or disk problems. Save your work before restarting.

## Agent command line

OmaAudio can be operated without a window using `omaaudio cli`. Run
`omaaudio cli --help` for usage and `omaaudio cli schema` for the complete typed
command catalogue. The CLI supports editing, mixing, processors, automation,
analysis, saving/exporting, recipes and persistent JSON sessions; add `--audio`
for playback and recording. See [the agent CLI guide](agent-cli.md) for the
protocol, examples, recovery and validation.

An agent skill comes with the package. Run `omaaudio skill --link` once as your
own user and start a new agent session. `omaaudio skill` shows which agents can find it.

## Licence

OmaAudio is released under the MIT licence. Its bundled components keep their own licences; their texts are installed in `/usr/share/licenses/omaaudio/`. Separately installed plugins keep their own licences.
