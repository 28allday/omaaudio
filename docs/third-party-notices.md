# Credits and licences

OmaAudio is © 2026 Gavin Nugent and released under the [MIT licence](../LICENSE).
It is built on the work of the people and projects below. Each component keeps
its own licence and copyright; OmaAudio's licence does not replace theirs.

## Included with OmaAudio

These are part of OmaAudio's source or are built into the program. Their licence
texts are in [docs/licenses](licenses/) and are installed with the package in
`/usr/share/licenses/omaaudio/`.

| Component | Licence | Copyright | How OmaAudio uses it | Licence text |
| --- | --- | --- | --- | --- |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | MIT | Geraint Luff / Signalsmith Audio Ltd. | Unmodified headers in `engine/third_party/signalsmith`, built in: time stretching and pitch shifting | [Signalsmith-Stretch-MIT.txt](licenses/Signalsmith-Stretch-MIT.txt) |
| [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) | MIT | Signalsmith Audio | Unmodified headers, built in, used by Signalsmith Stretch | [Signalsmith-Linear-MIT.txt](licenses/Signalsmith-Linear-MIT.txt) |
| [CLAP](https://github.com/free-audio/clap) 1.2.10 API headers | MIT | Alexandre Bique | Unmodified headers in `engine/third_party/clap`; CLAP effects you install are loaded at run time | [CLAP-MIT.txt](licenses/CLAP-MIT.txt) |
| [VST 3 SDK](https://github.com/steinbergmedia/vst3sdk) 3.8.1 (interface subset) | MIT | Steinberg Media Technologies GmbH | Unmodified interface headers and IDs in `engine/third_party/vst3` (see `engine/third_party/vst3/LICENSE.txt` and its README); VST 3 effects you install are loaded at run time. VST is a trademark of Steinberg Media Technologies GmbH. | [VST3-SDK-MIT.txt](licenses/VST3-SDK-MIT.txt) |
| [Lucide](https://lucide.dev) icons, including icons from [Feather](https://feathericons.com) | ISC (Lucide), MIT (Feather) | Lucide Icons and Contributors; Cole Bemis | SVG icons in `src/icons`, built into the program | [Lucide-ISC-and-Feather-MIT.txt](licenses/Lucide-ISC-and-Feather-MIT.txt) |
| [pyaaf2](https://github.com/markreidvfx/pyaaf2) 1.7.1 | MIT | Mark Reid | Unmodified Python sources in `tools/aaf_vendor`, run in a separate Python process to read AAF files | [pyaaf2-MIT.txt](licenses/pyaaf2-MIT.txt) |
| [RNNoise](https://gitlab.xiph.org/xiph/rnnoise) 0.2 and its speech model | BSD-3-Clause | Jean-Marc Valin, Xiph.Org Foundation, Mozilla, Amazon, Mark Borgerding, Gregor Richards | System library linked at run time for Voice Cleanup; its notices are carried because OmaAudio is built against its header | [RNNoise-COPYING.txt](licenses/RNNoise-COPYING.txt), [RNNoise-header-notice.txt](licenses/RNNoise-header-notice.txt) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 OR MIT (MIT chosen) | The Khronos Group Inc. | API declarations used when building; Qt loads Vulkan at run time | [Vulkan-Headers-MIT.txt](licenses/Vulkan-Headers-MIT.txt) |

## Used from your system

These are installed separately, by your package manager, and keep the licence
texts their own packages install. OmaAudio links to the libraries at run time
and runs the programs as separate processes; none of them is bundled.

| Component | Licence | How OmaAudio uses it |
| --- | --- | --- |
| [Qt 6](https://www.qt.io) — Core, GUI, QML, Quick, Quick Controls, D-Bus, SVG (and the OpenGL and Network modules they load) | LGPL-3.0-only (also offered under GPL-3.0 and commercial terms) | The interface toolkit, linked at run time |
| [PipeWire](https://pipewire.org) / SPA | MIT | Playback and recording, linked at run time |
| [libsndfile](https://libsndfile.github.io/libsndfile/) | LGPL-2.1-or-later | Reading and writing audio files, linked at run time |
| [lilv](https://gitlab.com/lv2/lilv), [serd](https://drobilla.net/software/serd.html), [sord](https://drobilla.net/software/sord.html), [sratom](https://drobilla.net/software/sratom.html), [zix](https://gitlab.com/drobilla/zix) | ISC (some files also 0BSD or BSD-3-Clause) | Finding and hosting LV2 effects, linked at run time |
| [LV2](https://lv2plug.in) headers | ISC | Effect interface declarations used when building |
| [toml++](https://marzer.github.io/tomlplusplus/) | MIT | Reading keyboard-shortcut files |
| [libxcb](https://xcb.freedesktop.org) and xcb-util-wm | X11 / MIT-style | Placing effects' own windows, linked at run time |
| [xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/) | LGPL-2.1-or-later | File dialogs, through D-Bus |
| [Python 3](https://www.python.org) | PSF-2.0 | Runs the AAF and OMF readers as a separate program |
| [FFmpeg](https://ffmpeg.org) (optional) | LGPL-2.1-or-later or GPL, depending on how your distribution builds it (Arch: GPL-3.0) | Run as a separate program for AAC/M4A, rendered stretching and the video reference; no FFmpeg code is part of OmaAudio |
| The GNU C library | LGPL-2.1-or-later | The system C library |
| Inter / Noto Sans fonts (when installed) | SIL Open Font License 1.1 | Interface fonts; no font files are bundled |

## Effects you install

LV2, CLAP and VST 3 effects — for example Calf, LSP or x42 — are installed
separately and keep their own licences. OmaAudio finds and runs them; it does
not include or relicense them. The built-in starting-point presets hold only
OmaAudio's own suggested settings for those effects' controls.

## OmaAudio's own effects

The effects that come with OmaAudio (Low Cut, Hum Removal, Voice Cleanup, Width,
Limit Peaks and Change Level, in `engine/plugins/omaaudio.lv2`) are OmaAudio's
own code under its MIT licence. Voice Cleanup uses RNNoise, listed above.

---

OmaAudio is not affiliated with, derived from, or endorsed by Audacity, Tenacity,
Ardour, Adobe, or the Omarchy project. Product names belong to their owners.
