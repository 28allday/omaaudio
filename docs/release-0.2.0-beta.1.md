# OmaAudio 0.2.0 beta 1

The first beta of OmaAudio: record, edit, mix, repair and export audio on Linux.
Everything in the earlier 0.1.0 release candidate is here, with a great deal
added. Please try it on your own recordings and
[report anything that doesn't behave as you'd expect](https://github.com/28allday/omaaudio/issues).

## Install

Download `omaaudio-0.2.0beta1-1-x86_64.pkg.tar.zst` from this release, then:

```bash
sudo pacman -U ~/Downloads/omaaudio-0.2.0beta1-1-x86_64.pkg.tar.zst
```

The package is unsigned, so download it first and install the local file. The
Calf, LSP and x42 effects come with it. The
[README](../README.md) has optional extras and uninstalling.

## What's new since 0.1.0 RC 1

### Effects

- Each effect opens in a **panel of its own** in the middle of the window, live
  while you play: knobs for compressors and other dynamics, sliders with real
  scales elsewhere, readable units (dB, Hz, ms), grouped controls, and an
  **EQ curve** measured from the effect itself.
- **Meters** show what an effect is doing — input, output and gain reduction —
  in a box that stays in view, and say why they rest (stopped, bypassed or
  effects off).
- **Starting-point presets** for common jobs (podcast voice, broadcast limit,
  room reverb, rumble cut and more) for Calf, LSP, x42 and OmaAudio's own
  effects, beside the effect's own presets and yours. Each is one Undo step.
- **CLAP and VST3 effects**, as well as LV2, with their own windows, presets and
  sidechain inputs; VST3 is found when *Find VST3 effects* is on in Preferences.
- **Clip effects**, with their controls and automation, and **effects on the
  final mix** reachable from the mixer's Master strip.
- Apply an effect to a **selection**, written into the audio as one Undo step.
- **Sample-accurate automation** for LV2 and CLAP effects.

### Sound, Quick Fixes and repair

- The **Sound** tab: say what a track is and switch on plain fixes — even out a
  voice, reduce noise, hum and room echo, de-ess, brighten, duck music under
  speech.
- **Voice Cleanup**, OmaAudio's own speech noise reduction, live on a track or
  written into a selection.
- **Quick Fixes**: dynamics, noise gate, de-esser, EQ, brightness, pitch
  correction and ducking, each in its own simple panel.
- **Tidy pauses**: find the gaps between words and shorten, remove, mark or
  split them.
- **Recipes**: save a chain of fixes and run it on other recordings in one
  step, including a *Shorten pauses* step.
- Analysis: level statistics, frequency and phase, live while you play.

### Editing and arranging

- Drag a clip's **edges to trim** it, its **corner handles to fade** it and its
  **level line** to change its volume.
- **Delete** removes the tracks you pick by their headers; *Delete Track* in the
  right-click menu of a track or a mixer strip.
- J/K/L **shuttle**, I/O to mark a range, Q/W to jump to the selection's start and end, and *Hear Without
  Effects* (Alt+B).
- Clip groups, clip envelopes, generated ducking that follows later edits,
  match clip loudness, fit music to a length, automatic speech alignment, tone
  and noise generation, a metronome, and project tempo and meter.
- The Arrange page keeps the track you're working on in view.

### Recording

- Several inputs at once, punch-in, count-in and loop takes.
- A new take goes on a track **just under the one you're working on**.
- **Audio Setup** shows what PipeWire sees, with JACK applications through
  PipeWire.

### Import, export and picture

- **AAF session import** from DaVinci Resolve and Avid Media Composer, and
  **OMF** import: tracks, clips, fades, dissolves, clip levels and timecode.
  The timeline's **picture comes in as the reference video** — an edited
  picture is built into one video, frame for frame.
- **Video reference**: open a video beside the project and it plays in time,
  with timecode.
- Export the **whole project, a time selection, or chosen tracks**, with
  delivery presets; **BWF and RF64** with broadcast metadata and markers;
  **AAC/M4A** through FFmpeg; up to 192 kHz.
- **Collect Project** makes a portable copy with its audio and video.

### The agent command line

- `omaaudio cli` gives AI agents and scripts the app's editing, mixing,
  effects, analysis and export with no window, in single commands or a
  persistent session. An **agent skill** comes with the package
  (`omaaudio skill --link`).

### Help and the interface

- Help on every control (hover; F1 for more), a searchable user manual inside
  the app, and plain wording throughout.
- Clearer hover, press and selection; labels centred; long names no longer
  stretch the window; nothing hidden under scrollbars; one font throughout.
- Large projects stay responsive, with a memory budget for playback and
  displays, and long exports and repairs in bounded memory.

## Known limitations

- VST3 and CLAP effects have only been tried with test effects so far.
- OMF import brings in audio only, not picture, and hasn't been tried with real
  OMF exports yet.
- In an AAF's picture, dissolves become cuts and video effects show their
  source shot. A built picture stays in `~/.local/state/omaaudio/pictures/` if
  you undo the import, until you delete it.
- One tempo and meter per project.
- Intel graphics hasn't been tested yet.

## Reporting problems

Open an issue at <https://github.com/28allday/omaaudio/issues> with what you did,
what you expected and what happened. **Help › User Manual** has troubleshooting
first.
