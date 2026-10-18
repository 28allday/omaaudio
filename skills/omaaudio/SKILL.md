---
name: omaaudio
description: >
  Edit, mix, repair, analyse, record and export audio in OmaAudio through its
  headless command line. Use for audio files, stems, Resolve AAF imports and
  .omaaudio projects: trim or arrange clips, apply fades and effects, mix tracks,
  edit automation, check loudness, save sessions and render audio. Commands are
  `omaaudio cli schema`, `inspect`, `run` and `--stdio`. Use when asked to operate
  OmaAudio or work on an OmaAudio audio project. Not for developing the app's
  source code.
---

# Operating OmaAudio

Use `omaaudio cli`, which opens an independent session using the desktop app's
editing and rendering engine. It does not attach to an open desktop window.
Editing, processing and export need no display or audio server. Add `--audio`
only for PipeWire playback, monitoring or recording.

## Discover, edit, check, save

1. Read `omaaudio cli --help` and `omaaudio cli schema`. The schema supplies exact
   operation names, aliases, argument types, defaults and readable/writable
   properties. Look up unfamiliar operations instead of guessing their arguments.
2. Open an existing project with `--project '/path/session.omaaudio'`, then
   `inspect`. For audio, use `audio.open` to start a session or `audio.import` to
   add another track. `aaf.import` adds a multitrack session.
3. Use one recipe or persistent process for related edits. Every separate command
   invocation otherwise starts a fresh session, with its own undo and clipboard.
4. Inspect the changed targets and analyse or export as appropriate. Read `ok`,
   errors and import warnings; an accepted job is not necessarily a finished job.
5. Save the project explicitly. `--save` writes a project bundle; `export` writes
   a rendered audio file. Report the actual saved/exported paths.

```sh
omaaudio cli --project '/path/session.omaaudio' inspect
omaaudio cli --project '/path/session.omaaudio' get '{"property":"tracks"}'
omaaudio cli --project '/path/session.omaaudio' --save '/path/edited.omaaudio' \
  split '{"frame":48000}'
omaaudio cli --project '/path/edited.omaaudio' export \
  '{"spec":{"path":"/path/mix.wav","format":"wav","bitDepth":24,"dither":true}}'
```

## A complete recipe

Write this JSON array to `recipe.json`, substitute the input/output paths, then
run `omaaudio cli run recipe.json`. `run -` reads the array from stdin.

```json
[
  {"id":"load","op":"audio.open","args":{"path":"/path/voice.wav"}},
  {"op":"track.gain","args":{"track":{"$ref":"load.state.focusedTrack"},"db":-3}},
  {"id":"clip","op":"clip.properties"},
  {"op":"clip.apply","args":{
    "track":{"$ref":"clip.result.track"},
    "clip":{"$ref":"clip.result.clip"},
    "stamp":{"$ref":"clip.result.stamp"},
    "gainDb":0,"replaceFades":true,"fadeIn":2400,"fadeOut":2400
  }},
  {"op":"project.save","args":{"path":"/path/Voice.omaaudio"}},
  {"id":"mix","op":"export","args":{"spec":{
    "path":"/path/voice-mix.wav","format":"wav","bitDepth":24,"dither":true
  }}},
  {"op":"renderById","args":{"id":{"$ref":"mix.result"}}},
  {"op":"inspect"}
]
```

An object containing only `$ref` reads a previous successful response. Use unique
string request IDs without dots. Query edit stamps immediately before an edit;
a stale stamp is a reason to inspect again, not invent a newer value.
The fades above are 2400 project frames each (50 ms at 48 kHz); choose lengths
that fit the actual clip and project rate.

## Persistent sessions

Run `omaaudio cli --stdio` with stdin/stdout pipes kept open. Send one JSON object
per line; wait for its response before the next dependent edit:

```json
{"id":1,"op":"audio.open","args":{"path":"/path/voice.wav"}}
{"id":2,"op":"get","args":{"property":"tracks"}}
{"id":3,"op":"project.save","args":{"path":"/path/Voice.omaaudio"}}
{"id":4,"op":"quit"}
```

Responses are JSON lines with `type:"response"`, `id`, `ok`, `result` or `error`,
and a compact `state`. `--project` emits an initial load response with id `open`
before the requested command; read all lines. Drain stderr too; it carries diagnostics. Most commands
wait for background work to finish by default. Prefer that default. `wait:false`
means acceptance only; follow it with `wait` and the job query. `--events` adds
progress messages. Match response IDs because read-only queries and `cancel` can
reply while a long command is waiting. `--timeout` and request `timeoutMs` are
milliseconds; cancellation is cooperative. Inspect after a timeout before retrying.

## Targets, units and useful operations

- Timeline positions and durations are **frames at the project sample rate**,
  unless the field explicitly says seconds, milliseconds, Hz or dB. For exact
  selection use `select` with `start`, `end`, `snap:false` and `track`.
- Track and clip IDs are not list indices. Read them from `inspect`, `tracks`,
  `trackById` and `clip.properties`. `-1` usually means focused track; `-2` means
  master only on operations that support that target. Explicit track setters
  need real track IDs.
- Arguments are a named JSON object or positional array. Optional arguments can
  only be omitted from the end of a signature. Do not pass strings for numbers.
- Use the schema's aliases for `track.*`, `clip.*`, `selection.*`, `plugin.*`,
  `effect.*`, `automation.edit`, `marker.*`, `master.analyse`, `spectral.*`, `video.*`,
  `recipe.*` and `record.*`. Query plugin ports, routing and automation targets before changing
  them. Read controls back to check the values actually applied.
- Read `exportFormats` and `deliveryPresets` with `get` to choose export settings.
  `export` returns a job ID; `renderById` supplies its status, report and path.
  Add `start`/`end` (project samples) or `"range":"selection"` to the spec to export
  only that time range, exactly; it works for the mix and for stems.
  `revealRender` is desktop-only; use the returned path instead.
- Clip effects belong to one clip and its split pieces: `clip.effects` reads them
  (with the clip's `stamp` and `status`), `clip.effect.add|remove|move|bypass|control|automation`
  edit them, `clip.effects.bake` renders them into the clip's audio. Each edit needs
  the current `stamp`. Status `rendering` means the sound is still being prepared;
  exports wait for it, so never bypass an effect just to finish a render.
- Trim a clip's edges over its recording with `clip.trim {"track","clip","start","end"}`
  (frames on the timeline; the audio keeps its place). Ask `clip.trimLimits` first:
  `earliestStart`, `latestEnd` and `minimumLength` say how far it can go.
  `clip.gain {"track","clip","gainDb"}` and `clip.fades {"track","clip","fadeIn","fadeOut"}`
  set one clip's level or fade lengths. Each is one Undo step.
- `analyse {"kind":"statistics"|"spectrum"|"phase"}` measures the selection (or the
  focused track) as its clips play; use it to check levels, clipping, DC offset, a
  tone's frequency or mono compatibility after an edit. `generate` adds a tone, sweep
  or noise clip; `clips.matchLoudness` sets clip gains to one LUFS target. Add
  `"stems":true` to an `export` spec for one file per track without the master bus,
  and `"tracks":[ids]` with it to export only those tracks (muted ones included).
- To process just part of a track, write the effect into the audio: `selection.set`
  `[start, end, false, track]`, `effect.preview {"uri":...}`, read `get effectPreview`
  for its `slot`, set controls with `plugin.control {"slot":..,"port":..,"value":..,"track":..}`,
  then `effect.apply` (one Undo step) or `effect.cancel`. `plugin.add` instead puts
  the effect on the whole track, live. `get effectMenu` (after `plugin.list`) lists
  installed effects once each by group, with a short `core` set; use an entry's `uri`.
- To check what an equaliser or filter does, `plugin.response {"slot":n,"target":track}`
  measures its frequency response at the current settings (`frequencies`, `db`;
  call again while `pending` is true) and lists its `bands` with their
  `freqPort`/`gainPort`/`qPort` for `plugin.control` (`gainLinear`: the gain is a
  factor, 1 = 0 dB). `plugin.kind` says `eq`, `dynamics` or `other`.
- For a known-good setting, `preset.list {"track":id,"slot":n}` lists `builtIn`
  starting points (Podcast voice, Vocal plate, Rumble cut, Podcast peaks at −1 dB…),
  the effect's own `pluginPresets` and saved `presets`; `preset.apply
  {"track":id,"slot":n,"stamp":...,"id":"builtin:…"}` applies one as one Undo step
  (unnamed controls go back to their defaults). Clip effects: `preset.clipList` /
  `preset.clipApply` with `clip`.
- To clean up a voice track in plain terms, `sound.get {"track":id}` lists its fixes
  (`rumble`, `hum`, `noise`, `sibilance`, `evenout`, `presence`) with `on` and an
  `amount` from 0 to 1; `sound.set {"track":id,"stamp":...,"changes":{"noise":{"on":true,"amount":0.45}}}`
  changes any of them as one Undo step. `noise` is live at 48 kHz only.
  `sound.roomEcho {"track":id,"amount":0.4}` reduces room echo in the selection
  (or the whole focused track), written into the audio. `sound.choices {"track":id}`
  reads the panel's remembered choices (`target.<kind>`, `roomEcho`, `duckUnder`,
  `duckReduction`, `duckRelease`) and `sound.choice {"track":id,"key":"roomEcho","value":0.6}`
  stores one; they save with the project and are not Undo steps.
- To tighten speech, `pauses.find {"scope":"voices"}` (or `"track"`/`"selection"` on
  the focused track; optional `auto`, `thresholdDb`, `minSeconds`, `paddingSeconds`,
  `keepBreaths`) lists the pauses where every voice track is quiet; nothing changes.
  `pauses.keep {"index":n,"kept":true}` protects one; `pauses.plan` previews an action
  and `pauses.apply {"then":"shorten","shortenTo":0.4}` does it as one Undo step
  (`then`: `shorten`, `remove`, `mark` with `markAs` `marker`/`pause`/`speech`, or
  `split`; `moveOthers` moves non-music tracks too, music never moves). Read
  `get pauseResult`. Find again after any other edit: old pauses go `stale`.
- Saved recipes (the desktop's Recipes tab, not `run` files) are named lists of
  steps written into the audio in order, one Undo step `run <name>` per run, kept
  on this computer for every project. `get recipes` lists them (built-in ones,
  `builtIn:true`, cannot be changed; saving one makes a copy); `recipe.kinds` gives
  each step kind's settings and ranges. A step is `{kind, on, ...settings}`:
  `lowcut` `frequency` Hz, `hum` `mains` 50/60, `voiceCleanup`/`roomEcho`/`evenout`/
  `sibilance`/`presence` `amount` %, `width` `amount` 0–200 %, `clicks` `sensitivity` %,
  `loudness` `lufs`, `limit` `ceiling` dB, `level` `db`, `fadeIn`/`fadeOut` `seconds`,
  `pauses` `shortenTo`/`minSeconds` s + `moveOthers` 0/1 (shortens pauses; later steps follow),
  `effect` (from `recipe.effectStep {"uri":...}`) `uri`, `name`, `"c:<symbol>"` values
  and/or `controls:[{index,value}]`. `recipe.save {"recipe":{"name":"Tidy","steps":[{"kind":"lowcut","on":true,"frequency":100},{"kind":"loudness","on":true,"lufs":-16}]}}`
  returns the id (`id` in the map replaces an own recipe); `recipe.duplicate`,
  `recipe.rename`, `recipe.remove` and `recipe.get` take that `id`. Check
  `recipe.target {"target":"selection"|"track"}`, then `recipe.run {"id":...,"target":"track"}`:
  it keeps the command busy, so the response (or `wait`) comes when the run ends.
  Read `get recipeRun` for each step's `state` (`done`, `failed` with a `reason`,
  `off`) and `undoLabel`; a step that cannot run is skipped and the rest carry on.
  `recipe.stop` puts the audio back, `recipe.again` reruns `lastRecipeName`,
  `recipe.clear` dismisses the report. For many files, `batch.process` takes
  `"chain":"recipe","recipe":id` (loudness becomes the delivered target; room echo,
  clicks, fades and pauses are skipped; Voice Cleanup needs `"sampleRate":48000`).
- To change generated ducking that still follows its triggers, analyse again with
  `"rebuild":true` in the spec (it starts from the original curve, so it does not
  stack); `ducking.remove {"track":id}` restores that curve.
- `speech.align {"referenceTrack":A,"alignTrack":B,"tightness":0.7,"stamp":...}`
  re-times a re-recorded line on B to A's timing onto a new track; it runs in the
  background, so `wait` until `get aligningSpeech` is false, then check `speechAlignMessage`.
- `remix {"targetSeconds":30,"stamp":...}` re-arranges the music clip under the
  playhead to a new duration (background; wait for `remixing` false, read `remixMessage`).
- With `--audio`, `transport.shuttle {"speed":4}` plays at 4× (whole numbers −8 to 8;
  negative plays backwards, and the pitch follows the speed); read `playSpeed`.
  It is refused while recording, and any speed but 1 while writing automation.
- For recording, start with `--audio`, inspect available inputs, set `recordInput`
  to the chosen device ID, call `record.start`, `wait {"ms":5000}`, `record.stop`,
  then save. Stop waits for the captured audio to be installed.

## Saving and recovery

Edits do not implicitly overwrite a project. Use `project.save` with a path,
`save` for an already named project, or `--save path` after a successful sequence.
Recipes stop at the first error but do **not** roll back earlier edits or exports.
A failed recipe skips the final `--save`.

Replacing dirty work with `audio.open`, `project.open`, `project.new` or
`recoverSession` requires saving first or `discardChanges:true` on that request.
`--discard` permits abandoning unsaved work on exit only. At EOF, unsaved work
normally produces a recovery bundle and exit 3. Read the `type:"session"` result;
`recoveryAvailable` and `recoverSession` can restore it in another session.
Exit 0 means success, 1 an operation failed, 2 invalid input, 124 a timeout, and
128 + signal an interruption. A persistent session retains an earlier error's
exit status even if a later command succeeds.

## Installation and full reference

`omaaudio skill` reports discovery links; `omaaudio skill --link` sets them up for
the current user using Omarchy's agent folders. Start a new agent session
afterward. The links point at the packaged copy, so app upgrades update this
skill too.

The full protocol guide is `/usr/share/doc/omaaudio/agent-cli.md`; the audio
manual is `/usr/share/doc/omaaudio/manual.md`.
