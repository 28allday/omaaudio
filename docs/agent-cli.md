# OmaAudio agent command line

The current application has a windowless command line backed by the **same Backend,
project model, undo history, processors and renderer as the desktop application**.
It opens an independent session; it does not attach to a running desktop window.

Use `omaaudio cli --help`. `omaaudio --cli` is equivalent. Ordinary `omaaudio path.wav` and
`omaaudio path.omaaudio` still launch the desktop. To open an audio file literally
named `cli` in the desktop, use `./cli` or `-- cli`.

No window, display server or audio server is needed for editing, analysis, project
storage, processing and export. Qt's offscreen platform supplies font/image
services; no QML interface is loaded. Playback, capture and monitoring require
`--audio` and a working PipeWire server. Hardware remains entirely untouched by
normal offline commands. Plugin discovery loads installed plugin code just as the
application does.

## Install the agent skill

The [agent skill](../skills/omaaudio/SKILL.md) ships in the package. Run
`omaaudio skill --link` once as your own user, then start a new agent session. `omaaudio skill` reports status.
Links use `~/.agents/skills`, `~/.claude/skills`, and the installed Codex, Pi and
Hermes folders (including Hermes profiles); Codex honours `CODEX_HOME`.
App upgrades update the linked skill automatically. Existing entries are kept;
`omaaudio skill --link --force` moves conflicts to hidden backups and reports their
paths. `omaaudio skill --unlink` removes this copy's links, leaving custom skills
and backups intact. Run it before removing the package if you want to remove
personal discovery links too; package removal itself does not change user homes.

The packaged skill is `/usr/share/omaaudio/skills/omaaudio/SKILL.md`.
`omaaudio skill` uses no display, audio server or application settings. Like
`cli`, `skill` is a reserved first argument; open a file named skill with
`omaaudio ./skill` or `omaaudio -- skill`.

## Discover the complete interface

```sh
omaaudio cli schema
omaaudio cli --project '/path/My session.omaaudio' inspect
omaaudio cli --project '/path/My session.omaaudio' get '{"property":"tracks"}'
```

`schema` reports protocol version 1, every public Backend operation and its typed
arguments, every readable/writable property, and the convenient dotted aliases.
Each listed signature can be called, including shorter signatures that use trailing default arguments.

Operations include audio and AAF import; tracks, clips, selection, cut/copy/paste,
fades, slip/move, stretch/pitch, channel mapping and undo/redo; routing, mixer and
master controls; LV2/CLAP discovery, controls and presets; automation; markers and
metadata; spectral masks, repair, noise samples and voice cleanup; tidy pauses; saved recipes
(the Recipes tab); loudness analysis;
video reference alignment; recording setup/capture/recovery; project storage,
cleanup/recovery; and render/export queues.

The only excluded Backend action is `revealRender`: opening a desktop file manager
has no windowless equivalent. `renderById` supplies the exact output path instead.
`manualHtml` accepts a Qt font description string and a color string; ordinary
`manual` returns the readable documentation without presentation arguments.

`inspect` returns the complete property snapshot plus project metadata. Track and
clip descriptors, markers, plugin parameters, routing, automation curves, storage
previews and edit stamps are also available through the corresponding schema
operations. An empty result from a lookup means no matching item; do not invent IDs.
Read available formats and presets with `get` on `exportFormats` and
`deliveryPresets`. The existing application manual describes the audio operations.

`plugin.list` prepares the installed catalogue in the background. Read
`pluginScanWarnings` after it completes to inspect any discovery diagnostics;
`cancel` stops pending discovery. A cancelled attempt can be retried. Ordinary
completed discovery is cached for the session. Default discovery remains LV2/CLAP;
staged VST3 application integration is still gated by its remaining acceptance.

`pluginHasEditor` (`{"slot":n,"track":id}`) reports whether a live track or master
effect has its own window (VST3/CLAP editor or LV2 X11 interface).
`plugin.window` (`openPluginEditor`) opens it on the session's X display
(XWayland on Wayland) and fails with a reason when there is none;
`plugin.windowClose` (`closePluginEditor`) closes it. A headless CLI session normally
has no display: use the rack controls (`plugin.ports`, `plugin.control`) instead.

`plugin.kind` (`pluginKind`, `{"slot":n,"target":id}`) says what sort of effect a
slot holds, from its class: `eq` (equalisers and filters), `dynamics`
(compressors, limiters, gates, expanders) or `other`. `plugin.response`
(`pluginResponse`, same arguments) is the slot's frequency response at its current
settings, measured by playing an impulse through a separate offline copy of the
effect: `{valid, pending, reason, kind, bypassed, frequencies, db, bands}`, with 256
log-spaced `frequencies` from 20 Hz to 20 kHz and the gain at each in `db`. The
measuring runs in the background: a call for settings not yet measured returns
`pending: true` (with the last curve of that effect, if any); call again until
`pending` is false. Results are kept per effect and settings, so returning to a
setting is immediate. `bands` are the handles the curve draws, matched from the
control names and units: `{label, key, freqPort, gainPort, qPort, freq, freqMin,
freqMax, gain, gainMin, gainMax, q, qMin, qMax, qLog, gainLinear, enablePort,
enabled}`; a port is -1 when the band has none, and `gainLinear` means the gain
control is a factor (1 is 0 dB). Set a band with `plugin.control` on its ports.

`preset.list` (`pluginPresetContext`, `{"track":id,"slot":n}`; `track` is the master
target for the master bus) is what an effect's Preset menu offers: `{available,
reason, name, uri, stamp, fields, builtIn, pluginPresets, presets}`. `builtIn` are
OmaAudio's starting points for that effect (`{id, name, group, note, skipped}`;
`skipped` names controls a different version of the effect lacks), `pluginPresets`
the presets the effect ships with (`{id, name}`) and `presets` the ones saved with
**Save…**. `preset.apply` (`applyPluginPreset`, `{"track":id,"slot":n,"stamp":...,
"id":...}`) applies any of them as one Undo step. A starting point or an effect's own
preset sets the controls it names and puts every other control back to its default.
For an effect on a clip, `preset.clipList` (`clipEffectPresetContext`,
`{"track":id,"clip":n,"slot":n}`) and `preset.clipApply` (`applyClipEffectPreset`,
`{"track":id,"clip":n,"slot":n,"id":...,"stamp":...}`) do the same; a preset that
keeps an effect's own complete settings is refused on a clip.

### Touch return duration

The writable `automationReturnSeconds` property controls Touch return (Automatch):
0–5 seconds, default 1. Zero returns immediately. Non-finite and out-of-range values
are rejected. Changing it affects future returns; a return already in progress
keeps its timing. It does not change project history or dirty the project.

```json
{"op":"set","args":{"property":"automationReturnSeconds","value":0.75}}
```

The GUI saves this application preference across launches. A headless CLI session
starts with the default; set it explicitly when a script needs another duration.
The recorded curve, including the return, is saved in the project as usual.

### Shuttle playback

`transport.shuttle` (`shuttle`) takes `{"speed":n}`, a whole number from −8 to 8:
positive plays forwards, negative backwards, and 0 is treated as 1; values beyond
are clamped. It starts playback when stopped and, like `transport.play`, needs
`--audio`. It is refused with an error while recording, and any speed but 1 is
refused while automation is being written (an automation mode other than Read).
The readable `playSpeed` property is 1 at normal speed and negative backwards; it
is 1 whenever playback is stopped, and `transport.play` always plays at normal
speed.

```json
{"op":"transport.shuttle","args":{"speed":-2}}
```

Faster playback raises the pitch like tape run fast; effects, latency
compensation, automation and loops behave as at normal speed. Backwards playback
ignores the loop and stops at the start. The loudness meter does not measure while
`playSpeed` is not 1.

`{"op":"set","args":{"property":"hearWithoutEffects","value":true}}` plays the whole
project with every track, clip and master effect off, for comparing; `false` puts
them back. It changes only what is heard: renders, exports and the project keep
their effects, and a new or opened project starts with it off.

### Trim, gain and fades of one clip

`clip.trim` / `trimClip(track, clip, start, end)` moves a clip's edges to the
timeline span `[start, end)` over its recording. The audio keeps its place in time:
trimming the left edge in advances the clip's source offset by the same amount.
Nothing in the file changes. `clip.trimLimits` / `clipTrimLimits(track, clip)`
returns `{valid, earliestStart, latestEnd, minimumLength, start, end}`; an edge can
go out to the recording's ends, the next clip on the track, a clip effect's prepared
region and a stored clip envelope's start, and the clip stays at least 10 ms long.
A crossfade clip is not trimmed (`valid:false`); use its crossfade edges. Whole fades
keep to their edges and are shortened to fit. Each trim is one Undo step; a refused
trim changes nothing and says why.

`clip.gain` / `setClipGainDb(track, clip, gainDb)` sets one clip's gain (−80 to
+48 dB) and `clip.fades` / `setClipFades(track, clip, fadeIn, fadeOut)` its fade
lengths in frames, each as one Undo step; shapes are kept.

```json
{"op":"clip.trimLimits","args":{"track":0,"clip":1}}
{"op":"clip.trim","args":{"track":0,"clip":1,"start":30000,"end":48000}}
{"op":"clip.gain","args":{"track":0,"clip":1,"gainDb":-6}}
```

### Clip effects

A clip can carry its own effects, processed for that clip before its gain, fades and
envelopes. Split pieces of a clip share one set of effects and one continuous
processing region; edits through any piece change all of them. Track and master
effects are separate.

`clip.effects` (`clipEffectsTarget`) returns the current reference clip, or a pinned
clip with `{"track":id,"clip":index}`. The descriptor has `valid`, `reason`,
`track`, `clip`, `name`, `trackName`, `start`, `length`, `channels`, `views` (how
many pieces share the effects), `stamp`, `status`, `problem` and `effects`. Each
effect has `slot`, `uri`, `name`, `bypass`, `available`, `automationTiming` and
`controls` (`index`, `name`, `min`, `max`, `defaultValue`, `value`, `toggled`,
`integer`, `automated`, `automationEnabled`, `points`).

`status` is `none`, `bypassed` (every effect off: the clip plays its own audio),
`rendering`, `ready` or `failed` (see `problem`). The effected sound is prepared in
the background. While it is rendering the clip is silent in playback; it is never
played without its effects. Exports, analysis and loudness checks wait for the
rendering, so they always contain the effects. The property `clipEffectsRendering`
counts renders in progress.

Every edit needs the current nonempty `stamp` and is one Undo step:

| Operation | Arguments |
| --- | --- |
| `clip.effect.add` (`addClipEffect`) | `track`, `clip`, `uri`, `stamp` — appended after existing effects |
| `clips.effect.add` (`addEffectToSelectedClips`) | `uri`, `stamp` — each selected clip keeps its own effects |
| `clip.effect.remove` (`removeClipEffect`) | `track`, `clip`, `slot`, `stamp` |
| `clip.effect.move` (`moveClipEffect`) | `track`, `clip`, `from`, `to`, `stamp` |
| `clip.effect.bypass` (`setClipEffectBypass`) | `track`, `clip`, `slot`, `bypass`, `stamp` |
| `clip.effect.control` (`setClipEffectControl`) | `track`, `clip`, `slot`, `port`, `value`, `stamp` |
| `clip.effect.automation` (`editClipEffectAutomation`) | `track`, `clip`, `slot`, `port`, `operation`, `frame`, `to`, `value`, `stamp` |
| `clip.effects.clear` (`clearClipEffects`) | `track`, `clip`, `stamp` — removes the effects from every piece |
| `clip.effects.bake` (`bakeClipEffects`) | `track`, `clip`, `stamp` — replaces this piece's audio with the effected sound |

Effect automation operations are `point` (`frame:-1` adds at `to`; an existing
`frame` moves to `to`), `delete`, `enabled` (0 or 1) and `clear`. Positions are
clip-local frames, 0..length, and belong to the shared region, so pieces continue
one curve. Bake requires `status` `ready`; sample edits and repairs on a clip with
effects are refused until its effects are baked or removed. A mono clip through an
effect that only works in stereo becomes a stereo sound.

### Effects by group, and applying an effect to a selection

`get {"property":"effectMenu"}` returns the desktop's Effects menu after
`plugin.list`: `core` (the short everyday set) and `all` (every effect that can be
added), each a list of groups `{key, entries, sidechain}`. Group keys are
`dynamics`, `eq`, `reverb`, `delay`, `modulation`, `distortion`, `space`,
`restoration`, `pitch`, `meters`, `utility` and `other`. An entry is one effect
family (`name`, `family`, `uri`, `author`, `format`, `category`, `variants`): `uri`
is its stereo version, and `variants` lists `{variant, uri}` for `stereo`,
`leftright` and `midside` when the plugin ships those. `sidechain` holds copies
keyed from another track. Use an entry's `uri` with `plugin.add` or `effect.preview`.

Applying an effect to a time range writes it into the audio, as one Undo step:

| Operation | Arguments |
| --- | --- |
| `effect.preview` (`beginEffectPreview`) | `uri` — needs a selection on the focused track (`selection.set` with its `track`); the effect goes first in that track's chain |
| `effect.apply` (`applyEffectPreview`) | — writes the effect, with its settings at that moment, into the selected audio only |
| `effect.cancel` (`cancelEffectPreview`) | — takes the preview off; history, audio and the saved state are as before |
| `effect.repeat` (`repeatLastEffect`) | — writes the effect last applied this way, with the same settings, into the selection on the focused track now; one Undo step. `get {"property":"lastEffectName"}` names it ("" before the first apply). Kept across New and Open |

While a preview is open, `get {"property":"effectPreview"}` gives `active`,
`applying`, `track`, `slot`, `uri`, `name`, `start` and `end`; set its controls with
`plugin.control` on that `slot` and `track`. `effect.apply` waits until the audio is
written. The edges blend over 5 ms so there is no click; nothing is written past the
selection (no tail), and a tempo-synced effect sees the selection start as time 0.
The clip's own clip effects are not part of the audio fed to the effect.

### Sound settings for a voice

The Sound panel's voice fixes are real effects on the track, each driven by one
amount from 0 to 1. `sound.get` (`soundSettings`) takes a `track` (-1 for the focused
one) and returns `available`, `reason`, `track`, `trackName`, `role`, `kind`
(`voice`, `music`, `effects` or `ambience`, from the track role), `gainDb`,
`sampleRate`, `stamp` and `fixes`. `voice` has the fixes below; `effects` and
`ambience` have `width`; `music` has none (use loudness, `remix` and ducking). Each fix has `id`,
`title`, `plugin`, `available`, `reason`, `on`, `amount`, `slot` (-1 when the effect
is not on the track yet), `automated`, `live` and `scale`; Mains hum also has
`mains` (50 or 60) and no amount.

| Fix `id` | What it does | Effect |
| --- | --- | --- |
| `rumble` | Rumble and pops: a low cut from 40 Hz (amount 0) to 160 Hz (1) | OmaAudio Low Cut |
| `hum` | Mains hum: notches at 50 or 60 Hz and three harmonics | OmaAudio Hum Removal |
| `noise` | Background noise, live; 48 kHz projects only (`live` is false otherwise: apply Voice Cleanup to a selection instead) | OmaAudio Voice Cleanup |
| `sibilance` | Harsh "s" | Calf Deesser |
| `evenout` | Even out: gentle to firm compression | Calf Compressor |
| `presence` | Presence: added upper harmonics | Calf Exciter |
| `width` | Effects and ambience only: stereo width; `amount` 0.5 leaves it alone, 0 is mono, 1 twice as wide (`scale` 200 means the slider shows amount × 200 %) | OmaAudio Width |

`sound.set` (`applySound`) takes `track`, the `stamp` from `sound.get` and `changes`,
an object from fix `id` to any of `on` (true or false), `amount` and `mains`, for
example `{"noise":{"on":true,"amount":0.45},"hum":{"on":true,"mains":50}}`. The whole
call is one Undo step. A fix drives the first effect of its kind on the track, the
same one Track Effects and Quick Fixes show, so `amount` reads back whatever was last
set there. A missing effect is added in the fixed order low cut, hum, noise, "s",
even out, presence, next to the fixes already on the track (first in the chain when
there are none). `on: false` bypasses the effect and keeps its settings. Loudness,
music length and ducking use `trackLoudnessProposal`/`applyTrackLoudness`,
`remix.target`/`remix` and `ducking.analyse`/`ducking.apply`.

Room echo is applied, not live: `sound.roomEcho` (`reduceRoomEcho`) takes `track`
(the focused track) and `amount` from 0 to 1, and reduces late reverb in the
selection on that track, or the whole track when nothing is selected there, on
every channel and frequency whatever the Spectral page's mask, band and channel
are. It runs in the background like the spectral repairs (watch `repairing`) and
lands as one Undo step, "reduce room echo".

The panel's choices that are not effects are kept per track with the project.
`sound.choices` (`soundChoices`) takes `{"track":id}` and returns a map such as
`{"roomEcho":0.4,"target.voice":2,"duckUnder":1,"duckReduction":12,"duckRelease":600}`;
`sound.choice` (`setSoundChoice`) stores one, for example
`{"track":id,"key":"roomEcho","value":0.6}`. The panel uses `target.<kind>` (an
index into that kind's loudness target list), `roomEcho` (0 to 1), `duckUnder` (a
bit mask: 1 voice, 2 effects, 4 ambience), `duckReduction` (dB) and `duckRelease`
(ms). Storing a choice changes no audio and is not an Undo step, but it marks the
project unsaved. The choices are written to `session.toml` under
`[sound."<track id>"]` as additive keys, with no project format change; older
builds ignore them. A new project starts with none.

### Tidy pauses

The Sound tab's Tidy pauses page. `pauses.find` (`findPauses`) takes `scope`
(`selection`: the selection on the focused track; `track`: the whole focused track;
`voices`: every track whose role is a voice — dialogue, host or guest — over the
whole project), `auto` (default true: the level is the background noise plus
10 dB, kept between −70 and −20 dB), `thresholdDb` (used when `auto` is false),
`minSeconds` (default 0.5), `paddingSeconds` (air kept each side, default 0.1) and
`keepBreaths` (default true). A pause is where every track looked at is quieter
than the level for at least `minSeconds`; levels are read from the clips without
clip effects, before the faders. Nothing changes. It returns, and `get pauses`
then reads, `{valid, stale, scope, tracks, trackNames, start, end, thresholdDb,
noiseFloorDb, auto, minSeconds, paddingSeconds, keepBreaths, count, seconds, kept,
pauses: [{index, start, end, seconds, kept}]}` with `start`/`end` in frames.
`stale` turns true once the project changes; find again.

| Command | Arguments and result |
| --- | --- |
| `pauses.keep` (`setPauseKept`) | `index`, `kept` — keep one pause as it is, or let it change again |
| `pauses.plan` (`pausePlan`) | an action (below) — `{valid, reason, changes, changed: [indices], lengths: [seconds after, same order], lengthBefore, lengthAfter, removedSeconds, joins}`; changes nothing |
| `pauses.apply` (`applyPauses`) | an action — does it as one Undo step; the result is `get pauseResult`: `{then, changed, kept, total, lengthBefore, lengthAfter, removedSeconds, smoothMs, moved, joins, undoLabel}` |
| `pauses.clear` (`clearPauses`) | — forgets the pauses found |

An action is `{then, shortenTo, moveOthers, smoothMs, markAs}`. `then` is
`shorten` (each pause becomes `shortenTo` seconds, default 0.4, but never less
than twice the padding; shorter pauses are left), `remove` (only the padding each
side is left), `mark` or `split`. `moveOthers` (default true) moves the other
tracks that hold audio and are not music along with the tracks looked at; music
tracks never move, and markers move with the audio. `smoothMs` (default 10) is
split into a fade out and a fade in at each join. `markAs` is `marker` (default:
a cue in the middle of each pause), `pause` (a range over each pause) or `speech`
(a range over each stretch of speech between them); markers are named "Pause N"
or "Speech N". `split` clears each pause inside the padding on the tracks looked
at, so each stretch of speech is its own clip; the timing does not change. Kept
pauses are left alone by every action. The Undo label is "shorten N pauses",
"remove N pauses", "mark N pauses" or "split at N pauses". `pauses.apply` fails
with `lastError` when the pauses are stale or none would change.

### Saved recipes (the Recipes tab)

These are the desktop's saved recipes: a named list of steps written into the
audio in order, the whole run one Undo step labelled `run <name>`. They are not
the JSON request files that `omaaudio cli run` takes (see
[Recipes and result references](#recipes-and-result-references)). Own recipes are
kept in the application settings on this computer, for every project, not in the
project file; built-in recipes (`builtIn: true`) cannot be changed, and saving one
stores a new own recipe.

| Operation | Arguments |
| --- | --- |
| `recipe.get` (`recipe`) | `id` — `{id, name, builtIn, steps, summary}`, or `{}` |
| `recipe.kinds` (`recipeStepKinds`) | — the step kinds in picker groups: `{kind, group, groupTitle, title, settings, available, reason}`; each setting is `{key, label, from, to, step, unit, default, choices?}`. `available` is false, with a `reason`, when the effect a kind needs is not installed |
| `recipe.effectStep` (`effectStep`) | `uri` — a step for that installed effect at its default settings, or `{}` with `lastError` |
| `recipe.save` (`saveRecipe`) | `recipe`: `{id?, name, steps}` — returns the id; a missing or built-in `id` makes a new recipe, an own `id` replaces it |
| `recipe.duplicate` (`duplicateRecipe`) | `id` — a copy named "<name> (copy)"; returns its id |
| `recipe.rename` (`renameRecipe`) | `id`, `name` — own recipes only |
| `recipe.remove` (`removeRecipe`) | `id` — own recipes only |
| `recipe.target` (`recipeTarget`) | `target`: `"selection"` or `"track"` — `{valid, text, reason}`, what a run there would cover |
| `recipe.run` (`runRecipe`) | `id`, `target` — runs on the selection (both channels, on the focused track) or the whole focused track |
| `recipe.stop` (`stopRecipe`) | — stops the run and puts the audio back as it was |
| `recipe.again` (`runLastRecipeAgain`) | — the last recipe again, on the selection, or the whole focused track when there is none |
| `recipe.clear` (`clearRecipeRun`) | — dismisses a finished or stopped run's report |

A step is `{kind, on, ...settings}`; `on:false` keeps the step but skips it.

| `kind` | Settings |
| --- | --- |
| `lowcut` | `frequency` Hz, 20–300 |
| `hum` | `mains` 50 or 60 |
| `voiceCleanup`, `roomEcho` | `amount` %, 0–100 |
| `clicks` | `sensitivity` %, 0–100 (Find and Repair Clicks) |
| `loudness` | `lufs`, −30 to −10: measures the range and changes its level to reach it |
| `limit` | `ceiling` dB, −12 to 0 (OmaAudio Limit Peaks) |
| `evenout`, `sibilance`, `presence` | `amount` %, 0–100 (Calf Compressor, Deesser, Exciter) |
| `level` | `db`, −24 to +12 (OmaAudio Change Level) |
| `width` | `amount` %, 0–200: 0 mono, 100 as recorded |
| `fadeIn`, `fadeOut` | `seconds`, 0.1–10: the first or last seconds of the range |
| `pauses` | `shortenTo` s, 0–3 (default 0.4; 0 leaves only the air each side), `minSeconds` s, 0.2–5 (default 0.5), `moveOthers` 0 or 1 (default 0). Tidy pauses' Shorten on the range, auto level, breaths kept, 10 ms joins; with `moveOthers` 1 the other non-music tracks move too and every voice track must be quiet in a pause. Later steps work on the shorter range. Result "N pauses, X s shorter" or "No long pauses" |
| `effect` | `uri` and `name`, then settings as `"c:<symbol>": value` and/or `controls: [{index, value}]`; a `"c:"` key wins over a recorded control |

`recipe.run` keeps the command busy until the run ends, so the default response
waits for it; with `wait:false` it returns at once and `wait` returns when it has
finished. A step that cannot run is reported and the rest carry on. Read the
progress and result with `get {"property":"recipeRun"}`: `active`, `running`,
`finished`, `stopped`, `id`, `name`, `target`, `targetText`, `current` (1-based,
counting only steps that are on), `total` (steps that are on), `done`, `failed`,
`seconds`, `undoLabel` (empty when stopped) and `steps`, each `{title, summary,
state, seconds, result, reason, progress}` with `state` one of `waiting`,
`running`, `done`, `failed` or `off`. `recipes` lists every recipe, built in first;
`recipeRunning` is true during a run; `lastRecipeName` names the recipe
`recipe.again` would run ("" before the first). `startRecordingSteps` and
`stopRecordingSteps` (`keep`, default true) record the effects applied with
`effect.apply`/`effect.repeat`, Voice Cleanup, Room echo, click repair, fades
and `pauses.apply` with `shorten` or `remove` (a `pauses` step) as steps; `recordingSteps` and `recordedSteps` show them.

```json
[
  {"id":"load","op":"audio.open","args":{"path":"/path/voice.wav"}},
  {"id":"tidy","op":"recipe.save","args":{"recipe":{"name":"Interview tidy","steps":[
    {"kind":"lowcut","on":true,"frequency":100},
    {"kind":"voiceCleanup","on":true,"amount":50},
    {"kind":"loudness","on":true,"lufs":-16},
    {"kind":"limit","on":true,"ceiling":-1}
  ]}}},
  {"op":"recipe.run","args":{"id":{"$ref":"tidy.result"},"target":"track"}},
  {"op":"get","args":{"property":"recipeRun"}}
]
```

Limit Peaks (`urn:omaaudio:limit`: a 5 ms look-ahead sample-peak limiter, controls
`ceiling` −24 to 0 dB and `release` 10–1000 ms) and Change Level
(`urn:omaaudio:gain`: `gain` −48 to +24 dB) are ordinary effects too, for
`plugin.add` and `effect.preview`.

### Clip volume and stereo balance envelopes

`clip.envelope` (`clipEnvelopeTarget`) returns the current reference clip when
called without arguments, or a pinned clip with `{"track":id,"clip":index}`.
Check `valid` and `reason`; the descriptor contains `track`, `clip`, `name`,
`trackName`, `start`, `length`, `stamp` and `curves`. Each curve has `key`, `label`,
`enabled`, `hold`, `minimum`, `maximum`, `base`, `factor`, `available` and `points`.
Volume uses key `volume`, range 0..4, base 1; stereo balance uses `pan`, −1..1,
base 0. Both display with factor 100. Balance is unavailable for mono sources.

`clip.envelope.edit` (`editClipEnvelope`) requires all eight arguments:
`track`, `clip`, `key`, `operation`, `frame`, `to`, `value`, `stamp`.

| Operation | Arguments used |
| --- | --- |
| `point` | `frame:-1` adds/updates at `to`; an existing `frame` moves/updates that point, retaining its interpolation; `value` is the raw parameter value |
| `delete` | `frame` identifies the point to remove |
| `interpolation` | `frame` identifies a point; `value` is 0 (curve default), 1 (Linear), 2 (Hold), or 3 (Smooth) |
| `enabled`, `hold` | `value` is exactly 0 or 1 |
| `clear` | Resets the entire chosen curve, including hidden points |

Pass zero for unused numeric arguments. The initial `stamp` must be current and
nonempty. Positions are clip-local, in the inclusive interval 0..length; the last
endpoint can shape the final audible ramp. Descriptors may contain negative or
past-length positions inherited from trimming; these still define the curve but
cannot be edited until visible. Moving onto another point is refused. Values must
be finite and in range. Adding a point enables the curve; no-op requests succeed
without adding History.

An accepted deferred edit retains the exact clip descriptor and cannot retarget
later navigation or overwrite an overlapping edit. Await command completion before
reading the new stamp. Actual changes are one Undo step and survive Save/recovery
in format 22, or format 23 when Smooth is present. Source audio, clip gain/fades
and unrelated automation are preserved. Smooth points include `smoothStart` and
`smoothEnd` phase bounds; preserve them when drawing inherited portions of a
curve. These are output descriptors, not additional edit arguments.
Volume multiplies clip gain/fades; stereo balance affects only the first two source
channels before track processing, with unity centre. Move/copy keeps local points,
split/trim retains their inherited shape, slip leaves them fixed within the clip,
and stretch/rate conversion scales their times. Rounding collisions, overflow,
4096 points per curve or the combined 65536 project-point budget refuse edits.

### Generated editable ducking

`ducking.analyse` takes a `spec` map with exactly these eleven fields:

| Field | Value |
| --- | --- |
| `track` | Target track ID, integer |
| `triggers` | 1–32 distinct integer track IDs, excluding the target |
| `start`, `end` | Integer sample positions in the project; nonempty `[start,end)` range |
| `thresholdDb` | −80..0 dBFS |
| `reductionDb` | 0..60 dB |
| `attackMs`, `holdMs`, `releaseMs` | Each 0..10000 milliseconds |
| `replaceExisting` | Boolean; explicitly permit replacing volume automation in the range |
| `stamp` | Current `automationStamp` string, read immediately before analysis |
| `rebuild` | Optional boolean. When the target's stored recipe still follows (its points were not edited by hand), analyse from the curve that recipe was first made from, so new settings replace the old ducking instead of adding to it |

IDs and frames must be numeric integers, not strings or booleans; frames must be
exactly representable as JSON integers. Other settings must be finite numbers.
Each trigger's isolated routing graph may contain at most 256 tracks. A target
feeding a trigger, unavailable active effects or exceeded point budgets refuses
the proposal without changing the session.

Call `ducking.analyse` with `{"spec":{...}}`, then read `duckingPreview`. Analysis
only proposes a curve. The default waiting command completes before inspection;
with `wait:false`, observe `generatingDucking` and `duckingProgress` or use `wait`.
Check `valid`, `stale` and `message` before calling `ducking.apply` with
`{"token":"<preview token>"}`. Any new analysis, cancellation or relevant session
change invalidates the previous token. Apply rechecks ownership after any deferred
audio boundary; scripts must inspect the final command response.

Add `"keepUpdated":true` to store the recipe with the curve. The curve is then
generated again whenever a trigger track's sound changes (clips, effects, faders,
automation or incoming tracks). The update joins the edit that caused it, so one
Undo restores both; Undo and Redo never regenerate history. Editing the generated
points by hand stops the updates and keeps the edit. `ducking.state`
(`generatedDuckingState`, `{"track":id}`) reports `stored`, `following`,
`triggers`, `start`, `end`, `thresholdDb`, `reductionDb`, `attackMs`, `holdMs`,
`releaseMs`, `replaceExisting` and `reason`; `ducking.unfollow`
(`stopFollowingDucking`) removes the recipe, keeping the points; `ducking.remove`
(`removeGeneratedDucking`, `{"track":id}`) puts back the curve a following recipe
was made from and removes the recipe, as one Undo step. Projects with a stored
recipe use project format 25.

The preview includes `track`, `name`, `start`, `end`, `triggers`, `points`,
`previousPoints`, `previousEnabled`, `previousHold`, `pointCount`, `activityCount`,
`changed` and `replacementRequired`. `points` is the final proposed stored curve,
with resolved interpolation 1 (Linear), 2 (Hold) or 3 (Smooth). Smooth points also
include their phase bounds. `previousPoints` retains the original points, including
those outside the preview range needed to draw its boundaries. `ducking.cancel` cancels pending work and invalidates
ready previews; it can interrupt a waiting analysis over stdio.

Existing automation affecting the range needs explicit replacement. Disabled
stored volume points must first be enabled or cleared through normal automation
editing. Apply changes only target volume automation, preserves its outside-range
behavior and adds one Undo step; no activity or zero reduction adds none. Detection
uses each trigger's post-effects/fader/automation output, incoming feeds and latency
alignment, honoring Mute and ignoring Solo. The fader and original audio remain.
Generated points are ordinary editable timeline automation saved with the project;
later clip edits do not regenerate them. Realtime Quick Fixes ducking remains
a separate workflow.

### Independent plug-in automation (staged)

`automationTargets` supplies the keys accepted by `automation.edit` and
`configureAutomation`. A dual-mono insert exposes `fx:slot:port` for Both channels,
`fx:slot:port:0` for Left and `fx:slot:port:1` for Right. Query these keys instead of
assuming an insert has two instances. Track volume and pan keys are unchanged.

Left and Right descriptors include `instance` and `inherited`. An inherited curve
follows the shared curve until an edit creates an independent copy. Point editing,
interpolation, disabling and clearing then affect only that channel. `clear` leaves
an empty independent curve using that channel's base value; `inherit` removes the
override and resumes the shared curve. Both operations have an Undo step.

When independent curves exist, Both channels returns them in `lanes` and reports
`editable:false`; choose Left or Right for point edits. Respect `recordable` before
arming Write, Touch or Latch: support for recording both independent curves depends
on the adapter. A selected channel's live automation control writes only that
channel; an ordinary shared rack control still sets both channels.

For example, after querying a valid target:

```json
{"op":"automation.edit","args":{"key":"fx:0:0:1","operation":"inherit","frame":-1,"to":0,"value":0,"stamp":"","track":0}}
```

Independent curves require project format 19. This integration is available in the
private development build; the normal local testing build has not yet been updated.

## Individual commands

```sh
omaaudio cli --project '/path/My session.omaaudio' \
  --save '/path/Edited.omaaudio' split '{"frame":48000}'

omaaudio cli --project '/path/Edited.omaaudio' export \
  '{"spec":{"path":"/path/mix.wav","format":"wav","bitDepth":24,"dither":true}}'

omaaudio cli --project '/path/Edited.omaaudio' master.analyse
```

Supply one JSON object with parameter names, or one JSON array with positional
arguments. Names and types are exact: strings are not automatically turned into
numbers or booleans. Direct track/selection gain changes accept −80 to +48 dB,
also reported in the schema, to reject overflowing gain calculations. Integers outside JavaScript's exact integer range must be
encoded as decimal strings for 64-bit arguments. Timeline frames always refer to
the **project sample rate** unless a field explicitly says seconds, milliseconds,
Hz or dB. Track IDs are IDs, not indices. Many operations accept `-1` for the focused
track and plugin/master operations accept `-2` for the master; signatures and target
queries describe the appropriate operation. Explicit track operations such as
`setTrackGainDb` require an actual track ID.

Omitting optional arguments means omitting a trailing suffix of the signature.
For example, `select` accepts `[start,end]` or `[start,end,snap,track]`. To specify
`track`, also supply `snap`. Defaults are the application's defaults, including
selection snapping. Use `snap:false` for exact sample positions.

The optional `snapGrid` editing preference accepts `none` (default), `bar`, `beat`,
`half`, `quarter`, `eighth` or `triplet`. Enable project musical timing with
`setMusicalTime` and keep `snapping` true to activate it. Divisions follow the
time-signature beat: in 7/8, `half` is half an eighth-note beat. Nearby clip edges,
markers and the playhead still take priority within `snapToleranceFrames`;
otherwise selection edges and moved clip endpoints use the nearest grid point.
Selection ends stay inside available audio. Clip groups keep their relative
spacing. Pointer seeks, numeric edits and slip offsets remain exact.

```json
[
  {"op":"setMusicalTime","args":[true,120,4,4]},
  {"op":"set","args":{"property":"snapGrid","value":"beat"}},
  {"op":"select","args":{"start":25000,"end":47000,"snap":true}}
]
```

`musicalSnapActive`, `snapGridTicks`, `snapGrids` and `snapGridLabel` report the
current availability and choices. Tempo/meter save with the project and are
undoable. The grid choice itself does not edit the project. The GUI remembers it
across launches; set it explicitly in each headless CLI session.

Successful mutation responses include the updated session state. Controls that
clamp values retain the application's behavior; read the property or target back
to obtain the resulting value. A non-const Boolean operation returning false is
reported as `operation_rejected` when the backend supplies no more specific error;
this can also mean there was no change to make.

Every invocation starts a new session. An edit without `--project` starts from an
empty project. Use a recipe or a persistent session for multiple related operations.
`--save path` writes the final project only after the requested sequence succeeds.
A render uses `export`/`queueRender`, with an output path inside its `spec` object;
`--save` saves a project bundle, not an audio mix. `"stems":true` in the spec queues
one job per track that reaches the master (tracks routed into another track are
heard in its stem), writing `<name> - <track>.<ext>` beside the given path, without
the master bus; the call returns the first job ID and the others follow in order.
Add `"tracks":[ids]` to export only those tracks (the desktop's **Export… > Selected
tracks**): each chosen track gets its own file, muted or not and wherever it is
routed. `tracks` needs `"stems":true` and existing track IDs.
Add `"start"` and `"end"` (samples from the project's start, at the project rate)
to export only that time range, for the mix or for stems; or `"range":"selection"`
for the current time selection (refused when nothing is selected). The file holds
exactly `end - start` frames, the same samples the whole export has there (an
effect's sound carried in from before the range included; sound ringing past
`end` is cut off). Past the project's end is silence. Markers outside the range
are left out and the rest move with it; the job's `start`/`end` are reported by
`renderById`.

## Measuring and generating

| Command | Arguments and result |
| --- | --- |
| `analyse` (`analyseAudio`) | `kind` `statistics`, `spectrum` or `phase`; optional `fftSize` (power of two, 64–65536, default 4096) and `window` (`hann`, `blackman`, `rectangular`). Measures the selection on its track, or the focused track's whole length, as the clips play before track effects; up to 20 minutes. `result.ok` false carries `reason`. |
| `generate` (`generateAudio`) | `spec`: `kind` `tone` (`shape` sine/square/triangle/sawtooth, `frequency`, optional `endFrequency` for a logarithmic sweep, `levelDb` peak) or `noise` (`colour` white/pink/brown, `levelDb` RMS, `seed`). Fills the selection on the focused track, else starts at the playhead for `seconds`. One Undo step. |
| `clips.matchLoudness` (`matchClipLoudness`) | `targetLufs` (−60 to −5), `stamp` (`automationStamp`). Sets each selected clip's gain (or the clip at the playhead) to reach the target; clips need 0.4 s above silence. One Undo step. |

`speech.align` (`alignSpeech`) takes `referenceTrack`, `alignTrack`, `tightness`
(0–1) and `stamp`; it uses the clip under the playhead (or the only clip) on each
track, runs in the background (`aligningSpeech`, `speechAlignMessage`; `wait` until
`aligningSpeech` is false), and adds the aligned take on a new track in one Undo
step. `speech.target` describes the clips it would use; `speech.cancel` stops it.

`liveAnalysis` (`fftSize`, 256–8192) returns the spectrum (256 points per channel)
and phase of the newest output while playing with `--audio`; `fresh` says whether
anything new played since the previous call.

`remix` (`remixClip`) takes `targetSeconds` and `stamp`; it re-arranges the clip
under the playhead on the focused track in the background (`remixing`,
`remixMessage`) and replaces it in one Undo step. `remix.target` describes the clip
and the room before the next clip; `remix.cancel` stops it.

`set` on `metronome` (bool) and `metronomeLevelDb` (−40 to 0) controls the beat
click heard with `--audio`; it is never saved, recorded or exported.

Statistics return `peakDb`, `truePeakDb`, `rmsDb`, `integratedLufs`, `loudestRmsDb`,
`quietestRmsDb`, `dynamicRangeDb` and `perChannel` (`peakDb`, `rmsDb`, `minimum`,
`maximum`, `dcOffsetPercent`, `clipped`, `clippedRuns`). Spectrum returns `curves`,
one list per channel of 1024 `{frequency, db}` points from 20 Hz to Nyquist
(a full-scale sine reads 0 dB). Phase returns `correlation`, `balanceDb` and
`sideToMidDb`; it needs two channels.

## Independent-file batches

`batch.preview` and `batch.process` accept `paths` (a list of local audio paths or
file URLs) and `settings`. They operate on complete files and leave the session
and Undo history unchanged. For example:

```json
{"id":"files","op":"batch.process","args":{"paths":["/audio/voice.wav","/audio/music.flac"],"settings":{"directory":"/audio/processed","suffix":"-processed","format":"wav","bitDepth":24,"sampleRate":48000,"channels":2,"normalise":"loudness","targetLufs":-16,"ceilingDbTp":-1,"chain":"none"}}}
```

The output directory must already exist. Supported settings are:

| Setting | Meaning and default |
| --- | --- |
| `directory`, `suffix` | Existing local directory; suffix defaults to `-processed`. Names use the input base name plus suffix and the chosen extension. |
| `format`, `bitDepth` | Container ID from `exportFormats`; defaults `wav`, `24`. Depth is 16, 24 or 32 (float), subject to container capability. |
| `sampleRate`, `channels` | Defaults `0`, `0`: preserve each input. Explicit rate is 8000–192000; channels `1` averages to mono, `2` duplicates mono or keeps stereo. Larger layouts require preservation or mono. |
| `dither`, `bitrateKbps` | Defaults `true`, `0` (automatic AAC bitrate); explicit bitrate is 64–320, subject to rate/channel limits. |
| `normalise` | `none` (default), `peak` or `loudness`. |
| `peakTargetDb`, `targetLufs`, `ceilingDbTp` | Defaults −0.3 dBTP, −16 LUFS and −1 dBTP. Loudness adjustment backs off gain at the ceiling. |
| `includeMetadata` | Defaults `true`; preserve supported input metadata/markers, rescale timing and refresh loudness. Container limitations are warnings. |
| `chain`, `track`, `stamp` | `none` (default), `track` or `master`. Track defaults to the focused track's actual ID. Optional `stamp` must match `automationStamp`. |
| `chain`, `recipe` | `chain: "recipe"` with `recipe` a saved recipe's id: its effects become the chain. `loudness` sets `normalise: "loudness"` and the target (done last); `limit` also sets `ceilingDbTp`. `roomEcho`, `clicks`, fades and `pauses` are not done on files and are named in `effects`; `voiceCleanup` needs `sampleRate` 48000. |

Unknown settings and invalid types are refused. The accepted batch captures active
insert URIs, supported live opaque state and static controls; bypassed inserts
and timeline automation are excluded. Automated controls use their stored base. External sidechain routes are refused. Rate/channel preparation precedes
fresh per-file plugin instances. Missing or incompatible effects fail the file.
Session changes cannot alter an accepted batch. Existing outputs are never replaced,
even if they appear after preview; duplicate or reserved names are per-file failures.

Preview returns `{ok,error,files,effects,settings}` inside `result`; each planned
file has `input`, `output`, `valid` and `message`. A normal process response waits
for completion and adds top-level `batch` and `files`. All-success responses have
the batch ID in `result`. Any failed/cancelled/paused file produces `batch_incomplete`
while retaining every per-file result, including completed outputs. `wait:false`
means acceptance only; inspect `fileBatchJobs` after `wait` to determine the outcome.
Per-file failures live in these rows, rather than asynchronous global error events.

`fileBatchJobs` contains IDs, batch IDs, paths, state, progress, message, warning,
captured `settings` and the output `report`. States are `preparing`, `queued`, `reading`,
`processing`, `paused`, `done`, `failed` and `cancelled`. Reports include frames, rate,
channels, applied gain, measured/delivered loudness and metadata results; lossy
reports describe the encoder input. `fileBatchBusy` includes preparing, queued,
reading and processing files; paused rows are idle.

The local queue persists under `$XDG_STATE_HOME/omaaudio/file-batch` (or the usual
`~/.local/state` fallback), including captured native effect state. Starting the
CLI restores unfinished captured jobs as `paused` without processing them.
Inspect `fileBatchJobs`, then call `batch.resume` with `{id:FILE_ID}` or
`batch.resumeAll` with no arguments. Resume-all validates every paused row before
starting any of them; use per-file Resume when another row has a missing input
or an existing output. Resumption restarts the complete file with its original
settings. Retry and Resume responses include the affected `files` and report
`batch_incomplete` on a per-file failure when waiting for completion.

`fileBatchStatus` reports `{available,error,path}`. A lifetime lock permits one
queue owner; a second instance cannot process or rewrite its queue. A malformed
or unreadable queue is preserved and processing stays disabled. Close all queue
owners before investigating the saved state. `batch.reset` (`resetFileBatchQueue`)
then starts an empty queue: the damaged `queue.json` is kept beside it as
`queue.damaged-<time>.json` (the three newest are kept), outputs are untouched, and
it is refused while another process owns the queue or a file is processing. Each
time the queue opens, temporaries and native-state files that OmaAudio wrote but no
saved job references (left by a crash) are removed, at most 512 per opening; files
it cannot prove are its own are kept. Queue persistence never changes
the session or Undo history. Completed results remain; clearing finished rows
preserves paused work and output files. An interrupted job with an existing
output is blocked on Resume, because the saved journal cannot prove that output
was completed by that job.

Use `batch.cancelFile` with `{id:FILE_ID}` to cancel one file, or `batch.cancel`
with `{batch:BATCH_ID}`; omit `batch` (or use `-1`) to cancel every unfinished file.
These operations remain available during a waiting command. `batch.retry` with
`{id:FILE_ID}` reuses the original settings and paths; inspect that row afterwards
for its result. If preparation failed or was cancelled before settings were
captured, retry is refused: queue a new batch from the intended session. Live
opaque capture refuses while recording; no-effects jobs remain available.
`clearFinishedBatchFiles` removes terminal rows without deleting
outputs. The queue holds at most 512 rows across launches.

## Recipes and result references

`omaaudio cli run recipe.json` executes a JSON array sequentially and stops at the
first failed command. `run -` reads that array from stdin. A failed recipe does not
roll back earlier edits or already completed exports. It does not run a requested
final `--save`; unsaved edits are preserved in recovery unless `--discard` was used.

For a time range across several tracks, call `selection.tracks` (or
`setSelectedTracks`) with `{"tracks":[0,2]}`, using actual IDs from the current
project. `selection.set`, `copy`, `cut` and `deleteSelection` then use that group.
`selectedTracks` reports IDs in display order. `paste` starts at the focused
track and preserves row offsets; `pasteToNewTrack` creates the required lanes at
time zero. Each edit is one Undo step. `focusTrack` or `clearGroupSelection`
returns to one selected track; `focusSelectedTrack` changes focus within a group.
Processing operations continue to use the focused track. The clipboard and
group selection are transient and clear on project replacement or rate change.

For whole clips, call `selection.clips` / `setSelectedClips` with
`{"clips":[{"track":0,"clip":0},{"track":2,"clip":1}]}`. Use actual track IDs and
zero-based clip indices from the current state. An optional `stamp` rejects a
stale project target. The final input clip becomes the focused clip; time fields
and processing use its interval. `selectedClips` returns `track`, `clip`, `start`,
`end` and `group` (`-1` for no membership) in display order. The setter accepts
only `track` and `clip` per entry. With group linking enabled it expands the
selection to include group members; the final input clip remains the focus.
Selection is view state, with no Undo step.

`clips.move` / `moveSelectedClips` takes `deltaFrames`, `deltaTracks` (relative
row offsets) and an optional `stamp`. It moves the entire selection atomically,
refusing collisions, unavailable rows, overflow and stale clip descriptors.
Copy/Cut/Delete use exact selected clips; whole-clip deletion keeps other audio
in place regardless of Ripple. Whole-clip Paste preserves destination audio
between copied clips unless Ripple opens their full span. Close Gap and Delete
Time Across All Tracks require `selection.set` to establish a time range first.
Selection, copy, movement and paste work identically through the UI and CLI.

The local clip-group controls add these aliases:

| Operation | Arguments and result |
| --- | --- |
| `group.create` | `name`, optional `colour` and `stamp`; groups at least two selected whole clips. |
| `group.update` | `group` ID, `name`, `colour`, optional `stamp`; renames/recolours the saved group. |
| `group.detach` | Optional `stamp`; removes just the selected clips' memberships. |
| `group.dissolve` | `group` ID, optional `stamp`; removes the saved group and all its memberships, retaining audio. |
| `group.link` | `linked` boolean; controls automatic expansion of clip selection. No Undo step. |

`clipGroups` reports each group's `id`, `name`, `colour`, `memberCount` and
`members` (`track`/`clip` addresses). Read IDs from this property. Names are
nonempty, at most 512 UTF-8 bytes and contain no ASCII control characters;
`colour` is empty or `#RRGGBB`. The metadata operations are Undo steps and reject
stale targets. Queued operations keep their accepted addresses across automation
boundaries rather than applying to a later selection.

Linking starts enabled and resets on New/Open. For an exact member selection,
call `group.link` with `linked:false` before `selection.clips`. Turning linking
back on expands an existing selection. Copies use fresh group IDs; splits retain
membership. Empty group records remain until explicitly dissolved. Channel
processing, spectral repair and voice cleanup retain destination membership
intervals; gaps stay ungrouped. All-channel Paste introduces independent audio;
partial-channel Paste and Paste and Mix retain destination membership. Stretch and
pitch-and-duration edits preserve their clips' membership.

Crossfade adjustment uses current membership, including later regrouping or
dissolution. Whole-pair movement/copy and Save/reopen retain adjustability. A blend
can span multiple clip descriptors where group membership changes; use current
addresses and stamps after each edit. Selected-clip level and fade edits are
described below; ordinary time-range processing retains its scope.

`clips.apply` / `applySelectedClipProperties` edits the selected whole clips as one
Undo step. First call `clip.properties`; it includes `selectedCount`,
`shortestLength` and a `selectionStamp` when the reference clip belongs to the
current whole-clip selection. Pass that **selectionStamp** as `stamp`, with:

- `gainDb`: −80 to +48; `relativeGain:true` adds this change to every member,
  preserving differences and silent clips. Relative 0 is a no-op. With
  `relativeGain:false`, set a common gain; −80 mutes it. Any resulting gain above
  +48 dB refuses the whole edit.
- `replaceFades`: false preserves existing/inherited fades. When true, `fadeIn`
  and `fadeOut` are nonnegative frame lengths.
- `proportionalFades`: true interprets those lengths against the focused reference
  clip and scales them to each member's duration with sample rounding; false uses
  identical lengths, which must fit the shortest selected clip.

All seven existing arguments are required; the optional eighth argument is
`fadeShapes`, a map with exactly four numeric fields:

```json
{"fadeInShape":1,"fadeOutShape":0,"fadeInCurve":0.5,"fadeOutCurve":-0.4}
```

Shapes are 0 (Linear) or 1 (Smooth); curves are finite −1..1 values. Positive curves
raise intermediate amplitude and negative curves lower it. An omitted or empty
map preserves each member's existing shapes. A supplied map requires all four
keys and is validated even when `replaceFades:false`; it is installed only when
`replaceFades:true`. `clip.apply` / `applyClipProperties` accepts the same optional
trailing map for one clip. `clip.properties` and clip state expose the four fields.
The legacy seven-argument calls remain valid. New nondefault metadata requires
project format 21, including retained crossfade input recipes.

The token also tracks selection changes; the
ordinary project stamp is insufficient. Accepted deferred edits retain their
original target descriptors. Stale/overlapping targets fail without partially
editing other clips. Membership, source audio and pitch-and-duration mapping remain intact;
a no-op adds no History. Single-clip deferred properties also recheck the accepted
descriptor and preserve later selection changes. Arbitrary clip volume/pan/effect
keyframes remain outstanding; shaped fades do not supply that separate workflow.

`clips.stretchTarget` / `selectedStretchTarget` reports the selected whole clips'
`count`, common `start` and current `length`, `minimum`, `maximum`, `stamp`,
`anyElastic`, rendered `valid`/`reason` and realtime `liveValid`/`liveReason`.
`minimum` and `maximum` describe 25–400% of the current span within timeline
bounds; Apply also checks each member's actual rounded duration and collisions.

`clips.stretch` / `stretchSelectedClips` requires `targetFrames`, `realtime`,
`pitchChange` and that exact `stamp`. The earliest start stays fixed; selected
boundaries and spacing scale proportionally. Unselected clips, markers and
automation stay fixed. The token covers project and selection changes.
Realtime pitchChange is relative to each member's current pitch (0 preserves
pitch differences); final pitches must be −12..+12 and durations 50–200% of each
original source interval. Rendered mode requires pitchChange:0 and preserves
current audible pitch, with 25–400% of each current duration. Mono/stereo input
and output must each be at least 50 ms. Gain, scaled fades and membership remain.
One actual edit adds one History entry; unchanged duration/pitch adds none.
Cancelled, stale, colliding or failed edits install no partial result. Re-query
after an edit; later navigation does not retarget an already accepted operation.

A recipe can use IDs and stamps returned by earlier successful requests. An object
containing only `$ref` reads a dot-separated path in that response. Array indices
are decimal path components. References are resolved immediately before a command
runs, so a stamp reflects the earlier query, not a guessed revision.

```json
[
  {"id":"load","op":"audio.open","args":{"path":"/path/voice.wav"}},
  {"op":"select","args":{"start":0,"end":48000,"snap":false,
      "track":{"$ref":"load.state.focusedTrack"}}},
  {"op":"gainSelection","args":{"db":-3}},
  {"id":"clip","op":"clip.properties"},
  {"op":"clip.apply","args":{
    "track":{"$ref":"clip.result.track"},
    "clip":{"$ref":"clip.result.clip"},
    "stamp":{"$ref":"clip.result.stamp"},
    "gainDb":-3,"replaceFades":true,"fadeIn":2400,"fadeOut":2400
  }},
  {"op":"project.save","args":{"path":"/path/Voice edited.omaaudio"}},
  {"op":"export","args":{"spec":{"path":"/path/voice-edited.wav","bitDepth":24}}}
]
```

Use unique string IDs without dots when referring to results. The most recent
successful response for a repeated ID replaces the old reference. Up to 10000
responses and 32 MiB of response data are retained for references; subsequent
uncached references fail explicitly (`referenceStored:false` in that response). Large catalogues need not have IDs.
Input requests and recipe files are limited to 1 MiB and recipes/queues to 10000
requests. Argument/reference nesting is limited to 64 levels. Invalid references,
extra request fields, unknown methods/properties and mismatched argument types fail
before calling the backend. Internal QObject slots and signals are not callable.

## Persistent agent sessions

Start one process and keep its stdin/stdout pipes open:

```sh
omaaudio cli --stdio
```

Send one JSON object per line, and read the matching response before the next edit:

```json
{"id":1,"op":"audio.open","args":{"path":"/path/voice.wav"}}
{"id":2,"op":"get","args":{"property":"tracks"}}
{"id":3,"op":"set","args":{"property":"ripple","value":true}}
{"id":4,"op":"project.save","args":{"path":"/path/Voice.omaaudio"}}
{"id":5,"op":"quit"}
```

Requests accept `id`, `op`, `args`, `wait`, `timeoutMs` and `discardChanges`.
IDs are optional strings or numbers and are echoed. Responses have
`type:"response"`, `ok`, `result` on success or `error:{code,message}` on failure,
and a compact `state`. `--project` first emits its load response with id `open`,
then the requested command responses; consume every line. Import warnings are returned in `warnings`. JSON is the
only stdout output except `--help`/`--version`; Qt, decoder and plugin diagnostics
are routed to stderr. Both streams should be drained by the caller.

All mutation requests are serialized and normally wait for imports, saves,
processing and queued rendering to finish. A render response contains its job ID;
`renderById` returns its final state, report and output path. A save response is
sent after the write, not merely after queuing it. `wait:false` returns after
acceptance; it does not mean the operation succeeded. Use `wait`, the relevant job
query and error events to observe completion. A later ordinary mutation waits for
pending work to settle before it runs.

`get`, `inspect`, `schema`, `cancel` and the batch cancellation commands remain available during a waiting
request; these replies can arrive out of order, so match IDs. `cancel` asks current
cancellable work to stop and stops playback/recording. `wait` settles outstanding
jobs. `--events` additionally emits `type:"progress"` roughly four times per second
while an operation is waiting. Delayed errors after an unawaited command produce
`type:"error"` events even without `--events`, including while a later `wait` is
pending. That wait succeeds once work is idle; consume the error events and the
process exit status to determine the earlier job's outcome.

`wait` also accepts `{"ms":1000}` for a minimum elapsed duration, and
`{"until":"stopped"}` or `{"until":"recording"}` for transport conditions. Its
default `until` is `idle`, meaning no pending background job, not stopped playback.
For example a recording recipe, run with `--audio`, can set `recordInput` to an
explicit device ID, call `record.start`, `wait {"ms":5000}`, `record.stop`, then
`project.save`. Stopping waits for captured audio to be installed before saving.
State-bearing effect saves wait until recording finishes. `stop`, `transport.stop`,
`stopRecording` and `record.stop` may start while such a save is queued; their
normal waited response still follows completion. To stop during another active
waited request, send one of those Stop commands with `wait:false`. Its immediate
reply acknowledges acceptance, and replies can arrive out of order; match IDs.
At EOF the last recording is stopped and completed before pending Save/recovery
or the final `--save`, while explicit queued waits retain their order.

`--timeout ms` sets the default per-command deadline (300000 ms); `timeoutMs`
overrides it on a request. A timeout requests cancellation and returns an error.
Synchronous plugin calls cannot be preempted safely: the deadline is checked when
control returns. Cancellation is cooperative; saves already accepted finish safely,
and some plugin discovery/library calls may take time to unwind. A timed-out
operation may already have changed the session: inspect it before retrying.
SIGINT/SIGTERM request orderly cancellation and recovery, rather than killing a
worker in the middle of a save. A second operating-system hard kill cannot promise
recovery.

## Collect a portable project

`project.collect` / `collectProject` accepts `{ "path": "/existing/folder/copy.omaaudio" }`.
It snapshots the visible session without changing its path, dirty flag or Undo
history. It copies all retained source audio and the reference video, and writes
`requirements.toml` with every track, master and clip insert identifier and bypass state.
Clip entries use `target = "clip"`, the containing track and a zero-based `clipIndex`;
split views sharing one effect rack are listed once at their first view.
Plugins must be installed separately; versions/availability are not verified.
The inventory describes collection time. Originals and existing destinations
are never replaced. The destination parent must already exist.

Waited responses include `collection` with `state` (`done`, `failed`, `cancelled`),
`path`, `published`, `error`, source/plugin counts and whether video was included.
`published:true` with failure means the final parent-directory sync failed after
the complete copy was installed: inspect that path before retrying. Failures and
cancellation make the waited operation fail. A rejected request does not attach
a previous operation's collection result.

Use `wait:false` for acceptance only, then `get` property `collectionStatus` or
`collecting` for live status. `project.cancelCollection` / `cancelCollection` and
`cancel` remain available during a wait. Later edits/project replacement cannot
change an accepted snapshot; app shutdown cancels unfinished copying. Move the
whole resulting folder. Collected video uses format 15 and remains portable
through subsequent Save As and recovery. Relinking returns to an external path.

## Saving, replacement and recovery

No edit is implicitly saved over a project. Use `save`, `saveAs`/`project.save`, or
`--save`. An unnamed project cannot use `save`; it returns an error asking for a path.
A failed `--project` load ends the session before any following commands execute.

`newProject`, `audio.open`, `loadProject` and `recoverSession` refuse to replace dirty
work unless that request explicitly contains `discardChanges:true`. `--discard`
applies to session exit, not to replacement. `quit` refuses unsaved work unless a
final `--save` is configured or `--discard` explicitly allows abandoning it.

At EOF or interruption, unsaved work is written to a private recovery bundle and
a `type:"session"` response reports its recovery index. The original project is not
overwritten. The next session exposes `recoveryAvailable`, `recoveryName` and
`recoveryWritten`; `recoverSession` restores it, and an explicit save makes it a
normal project. Normal CLI sessions disable timed autosave; deliberate
`autosaveNow` and the recovery operations remain available. The desktop and CLI use
the normal OmaAudio XDG locations. Set XDG environment variables for isolated runs
or separate agent workspaces.

Exit statuses:

| Code | Meaning |
| --- | --- |
| 0 | All commands succeeded and no unsaved edits remain, or discard was explicit |
| 1 | An operation failed; persistent sessions still accept further commands |
| 2 | Invalid options, malformed JSON, recipe or input limit |
| 3 | Unsaved edits were preserved in recovery |
| 124 | A command deadline expired |
| 128 + signal | Interrupted (130 for SIGINT, 143 for SIGTERM) |

A persistent session retains the first operation/deadline failure for its final
status, even if later commands succeed. Explicit invalid input and signals take
precedence. Session recovery preserves an existing failure status.
