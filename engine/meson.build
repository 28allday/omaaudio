# Omawave engine. Plain C++20; no Qt anywhere in this build (BRIEF.md amendment A3).
project(
  'omawave-engine',
  'cpp',
  version: '0.2.0-beta.1',
  license: 'MIT',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',   # -Wall -Wextra -Wpedantic
    'werror=true',
    'b_ndebug=if-release',
  ],
)

# include_type: 'system' routes third-party headers through -isystem so their warnings
# cannot fail our -Werror build (BRIEF.md section 8).
pipewire_dep = dependency('libpipewire-0.3', include_type: 'system')
spa_dep = dependency('libspa-0.2', include_type: 'system')
sndfile_dep = dependency('sndfile', version: '>=1.1.0', include_type: 'system')
# BRIEF.md amendment A8: toml++ joins the licence table for project files. MIT,
# header-only, so there is nothing to link.
tomlplusplus_dep = dependency('tomlplusplus', include_type: 'system')
# BRIEF.md section 5, Phase 5: LV2 hosting. lilv (and the serd/sord/sratom it pulls in)
# is ISC and was in the licence table from the start. suil is not used -- amendment A9.
lilv_dep = dependency('lilv-0', include_type: 'system')
lv2_dep = dependency('lv2', include_type: 'system')
threads_dep = dependency('threads')
# W09: CLAP hosting. The headers are pinned and vendored (MIT, see
# third_party/clap/README.md); plugins are loaded with dlopen.
clap_inc = include_directories('third_party/clap/include', is_system: true)
vst3_inc = include_directories('third_party/vst3', is_system: true)
# Unmodified SDK implementation files have their own warning policy. All OmaAudio
# adapters/tests retain the project's -Werror policy. Keep a single engine archive
# for the existing qmake consumer via link_whole below.
vst3_sdk_lib = static_library('vst3-interfaces',
  sources: ['third_party/vst3/pluginterfaces/base/funknown.cpp',
            'third_party/vst3/pluginterfaces/base/coreiids.cpp',
            'third_party/vst3/public.sdk/source/vst/vstinitiids.cpp'],
  include_directories: vst3_inc, pic: true, gnu_symbol_visibility: 'hidden',
  override_options: ['warning_level=0', 'werror=false'], install: false)
dl_dep = meson.get_compiler('cpp').find_library('dl', required: false)
# A79: upstream RNNoise, dynamically linked with its built-in model (BSD).
rnnoise_dep = dependency('rnnoise', include_type: 'system')

engine_inc = include_directories('src')

session_sources = files(
  'src/session/audio_buffer.cpp',
  'src/session/elastic.cpp',
  'src/session/clip_reader.cpp',
  'src/engine/elastic_processor.cpp',
  'src/engine/elastic_runtime.cpp',
  'src/session/disk_audio.cpp',
  'src/session/project.cpp',
  'src/io/resampler.cpp',
  'src/io/audio_file.cpp',
  'src/io/time_stretch.cpp',
  'src/io/process.cpp',
  'src/io/ffmpeg_audio.cpp',
  'src/io/metadata.cpp',
  'src/video/timecode.cpp',
  'src/video/reference.cpp',
  'src/io/peak_builder.cpp',
  'src/engine/transport.cpp',
  'src/engine/transport_latency.cpp',
  'src/engine/transport_shuttle.cpp',
  'src/engine/render_plan.cpp',
  'src/engine/clip_effect_runtime.cpp',
  'src/engine/stream_cache.cpp',
  'src/engine/loudness.cpp',
  'src/ipc/protocol.cpp',
  'src/ipc/server.cpp',
  'src/io/png_writer.cpp',
  'src/io/peak_texture.cpp',
  'src/io/peak_pyramid.cpp',
  'src/io/timeline_peaks.cpp',
  'src/session/zero_crossing.cpp',
  'src/session/edits.cpp',
  'src/session/clip_envelope.cpp',
  'src/session/automation.cpp',
  'src/engine/automation.cpp',
  'src/session/sample_edit.cpp',
  'src/session/voice_cleanup.cpp',
  'src/session/channel_edit.cpp',
  'src/session/multitrack_clipboard.cpp',
  'src/session/project_rate.cpp',
  'src/session/command.cpp',
  'src/session/render.cpp',
  'src/session/state.cpp',
  'src/session/routing.cpp',
  'src/engine/routing_render.cpp',
  'src/engine/transport_routing.cpp',
  'src/session/mixdown.cpp',
  'src/session/uuid.cpp',
  'src/io/peak_cache.cpp',
  'src/io/project_file.cpp',
  'src/io/plugin_state.cpp',
  'src/session/plugin_state.cpp',
  'src/io/project_storage.cpp',
  'src/io/project_collect.cpp',
  'src/lv2/urid.cpp',
  'src/lv2/world.cpp',
  'src/lv2/host_features.cpp',
  'src/lv2/instance.cpp',
  'src/lv2/chain.cpp',
  'src/lv2/worker.cpp',
  'src/clap/catalogue.cpp',
  'src/clap/instance.cpp',
  'src/vst3/stream.cpp',
  'src/vst3/gui_iids.cpp',
  'src/vst3/module.cpp',
  'src/vst3/component.cpp',
  'src/vst3/host_context.cpp',
  'src/vst3/scan.cpp',
  'src/vst3/catalogue.cpp',
  'src/vst3/bus_layout.cpp',
  'src/vst3/scan_inspect.cpp',
  'src/vst3/parameters.cpp',
  'src/vst3/instance.cpp',
  'src/engine/pw_output.cpp',
  'src/engine/pw_capture.cpp',
  'src/engine/recording.cpp',
  'src/render/master_render.cpp',
  'src/render/clip_effects.cpp',
  'src/render/apply_effect.cpp',
  'src/render/analysis.cpp',
  'src/render/speech_align.cpp',
  'src/render/remix.cpp',
  'src/engine/mix_benchmark.cpp',
  'src/session/generate.cpp',
  'src/render/ducking.cpp',
  'src/render/pauses.cpp',
  'src/render/export.cpp',
  'src/render/file_batch.cpp',
  'src/spectral/fft.cpp',
  'src/spectral/stft.cpp',
  'src/spectral/tiles.cpp',
  'src/spectral/repair.cpp',
  'src/spectral/restoration.cpp',
  'src/spectral/mask.cpp',
  'src/spectral/detect.cpp',
  'src/spectral/harmonics.cpp',
)

# PipeWire is in here now: the application links this library directly and owns the
# audio output itself, rather than talking to a separate engine process (A12).
session_deps = [sndfile_dep, tomlplusplus_dep, lilv_dep, lv2_dep,
                pipewire_dep, spa_dep, threads_dep, rnnoise_dep, dl_dep]

session_lib = static_library(
  'omawave-session',
  sources: session_sources,
  include_directories: [engine_inc, include_directories('third_party/signalsmith', is_system: true), clap_inc, vst3_inc],
  link_whole: vst3_sdk_lib,
  dependencies: session_deps,
)

session_dep = declare_dependency(
  link_with: session_lib,
  include_directories: [engine_inc, clap_inc, vst3_inc],
  dependencies: session_deps,
)

executable(
  'omawave-engine',
  sources: ['src/main.cpp'],
  include_directories: engine_inc,
  dependencies: [session_dep, pipewire_dep, spa_dep, threads_dep, rnnoise_dep],
  install: true,
)

# OmaAudio's own live effects (an LV2 bundle the application loads from beside
# itself, see lv2::World::addBundle).
subdir('plugins/omaaudio.lv2')

# --- tests -------------------------------------------------------------------------
vst3_scanner = executable('omaaudio-vst3-scan', sources: ['src/vst3/scan_main.cpp'],
  include_directories: engine_inc, dependencies: session_dep, install: false)
# The test programs live in tests/; a source tree without them builds the
# engine and the application only.
if import('fs').is_dir('tests')
test_inc = include_directories('tests')

test_session = executable(
  'test-session',
  sources: ['tests/test_session.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('session', test_session)

executable('bench-audio-memory', sources: ['tests/bench_audio_memory.cpp'],
  include_directories: engine_inc, dependencies: [session_dep], install: false)

# Generated callback measurements; intentionally outside the default test suite.
executable('bench-playback', sources: ['tests/bench_playback.cpp'],
  include_directories: engine_inc, dependencies: [session_dep], install: false)

test_routing = executable('test-routing', sources: ['tests/test_routing.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)

test_resampler = executable(
  'test-resampler',
  sources: ['tests/test_resampler.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('resampler', test_resampler)

test_audio_file = executable(
  'test-audio-file',
  sources: ['tests/test_audio_file.cpp'],
  link_args: ['-Wl,--wrap=sf_open'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('audio_file', test_audio_file)

test_ffmpeg_audio = executable(
  'test-ffmpeg-audio',
  sources: ['tests/test_ffmpeg_audio.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('ffmpeg_audio', test_ffmpeg_audio, timeout: 90)

test_peak_builder = executable(
  'test-peak-builder',
  sources: ['tests/test_peak_builder.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('peak_builder', test_peak_builder)

test_transport = executable(
  'test-transport',
  sources: ['tests/test_transport.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('transport', test_transport)

test_protocol = executable(
  'test-protocol',
  sources: ['tests/test_protocol.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('protocol', test_protocol)

test_ipc_server = executable(
  'test-ipc-server',
  sources: ['tests/test_ipc_server.cpp'],
  link_args: ['-Wl,--wrap=poll', '-Wl,--wrap=accept4', '-Wl,--wrap=read', '-Wl,--wrap=send'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('ipc_server', test_ipc_server, timeout: 30)

test_peak_texture = executable(
  'test-peak-texture',
  sources: ['tests/test_peak_texture.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  cpp_args: [
    '-DOMAWAVE_VALIDATE_PNG="' + meson.current_source_dir() / 'tests/validate_png.py' + '"',
  ],
  install: false,
)
test('peak_texture', test_peak_texture)

test_spectral = executable(
  'test-spectral',
  sources: ['tests/test_spectral.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  cpp_args: [
    '-DOMAWAVE_VALIDATE_PNG="' + meson.current_source_dir() / 'tests/validate_png.py' + '"',
  ],
  link_args: ['-Wl,--wrap=pwrite64'],
  install: false,
)
# The long-source storage regressions also run without optimization in debug.
test('spectral', test_spectral, timeout: 120)

test_pyramid = executable(
  'test-pyramid',
  sources: ['tests/test_pyramid.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('pyramid', test_pyramid)

test_selection = executable(
  'test-selection',
  sources: ['tests/test_selection.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('selection', test_selection)

test_edits = executable(
  'test-edits',
  sources: ['tests/test_edits.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('edits', test_edits)

test_multitrack = executable(
  'test-multitrack',
  sources: ['tests/test_multitrack.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('multitrack', test_multitrack)

test_mixdown = executable(
  'test-mixdown',
  sources: ['tests/test_mixdown.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('mixdown', test_mixdown)

test_project_file = executable(
  'test-project-file',
  sources: ['tests/test_project_file.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('project_file', test_project_file)

test_plugin_state = executable('test-plugin-state', sources: ['tests/test_plugin_state.cpp'],
  link_args: ['-Wl,--wrap=fsync'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('plugin_state', test_plugin_state, timeout: 60)

test_metadata = executable(
  'test-metadata',
  sources: ['tests/test_metadata.cpp'],
  link_args: ['-Wl,--wrap=fread'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('metadata', test_metadata)

test_project_save = executable(
  'test-project-save',
  sources: ['tests/test_project_save.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  link_args: ['-Wl,--wrap=fsync', '-Wl,--wrap=write', '-Wl,--wrap=rename',
              '-Wl,--wrap=unlink', '-Wl,--wrap=sf_open', '-Wl,--wrap=sf_close'],
  install: false,
)
test('project_save', test_project_save, timeout: 120)

test_lv2_world = executable(
  'test-lv2-world',
  sources: ['tests/test_lv2_world.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('lv2_world', test_lv2_world, timeout: 120)

test_lv2_instance = executable(
  'test-lv2-instance',
  sources: ['tests/test_lv2_instance.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)

test_lv2_chain = executable(
  'test-lv2-chain',
  sources: ['tests/test_lv2_chain.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('lv2_chain', test_lv2_chain, timeout: 120)

test_lv2_worker = executable(
  'test-lv2-worker',
  sources: ['tests/test_lv2_worker.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('lv2_worker', test_lv2_worker, timeout: 120)

test_undo_render = executable(
  'test-undo-render',
  sources: ['tests/test_undo_render.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('undo_render', test_undo_render)

test_loudness = executable(
  'test-loudness',
  sources: ['tests/test_loudness.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('loudness', test_loudness)

test_export = executable(
  'test-export',
  sources: ['tests/test_export.cpp'],
  link_args: ['-Wl,--wrap=sf_close', '-Wl,--wrap=_Znwm', '-Wl,--wrap=pwrite64', '-Wl,--wrap=pread64'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('export', test_export, timeout: 120)

executable('bench-render-memory', sources: ['tests/bench_render_memory.cpp'],
  include_directories: engine_inc, dependencies: [session_dep], install: false)

test_sample_edit = executable(
  'test-sample-edit',
  sources: ['tests/test_sample_edit.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  install: false,
)
test('sample_edit', test_sample_edit)

test_recording = executable(
  'test-recording',
  sources: ['tests/test_recording.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc],
  dependencies: [session_dep],
  link_args: ['-Wl,--wrap=sf_writef_float', '-Wl,--wrap=fsync'],
  install: false,
)
test('recording', test_recording)

subdir('tests/fixtures/gain.lv2')
test_denoise = executable('test-denoise', sources: ['tests/test_denoise.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep, rnnoise_dep], install: false)
test('denoise', test_denoise, depends: own_plugins, timeout: 60,
  env: ['OMAAUDIO_EFFECTS_BUNDLE=' + (meson.current_build_dir() / 'plugins/omaaudio.lv2'),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures'),
        'CLAP_PATH=' + (meson.current_build_dir() / 'tests/empty-clap')])
test_filters = executable('test-filters', sources: ['tests/test_filters.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('filters', test_filters, depends: own_plugins, timeout: 60,
  env: ['OMAAUDIO_EFFECTS_BUNDLE=' + (meson.current_build_dir() / 'plugins/omaaudio.lv2'),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures'),
        'CLAP_PATH=' + (meson.current_build_dir() / 'tests/empty-clap')])
test_dynamics = executable('test-dynamics', sources: ['tests/test_dynamics.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('dynamics', test_dynamics, depends: own_plugins, timeout: 60,
  env: ['OMAAUDIO_EFFECTS_BUNDLE=' + (meson.current_build_dir() / 'plugins/omaaudio.lv2'),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures'),
        'CLAP_PATH=' + (meson.current_build_dir() / 'tests/empty-clap')])
subdir('tests/clap-fixture')
subdir('tests/vst3-fixture')
test_vst3 = executable('test-vst3', sources: ['tests/test_vst3.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('vst3', test_vst3, depends: vst3_fixture, timeout: 60,
  env: ['OMAAUDIO_VST3_FIXTURE_LIBRARY=' + vst3_fixture.full_path()])
test_vst3_scan = executable('test-vst3-scan', sources: ['tests/test_vst3_scan.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: session_dep, install: false)
test('vst3_scan', test_vst3_scan, depends: [vst3_fixture, vst3_scanner], timeout: 60,
  env: ['OMAAUDIO_VST3_FIXTURE_LIBRARY=' + vst3_fixture.full_path(),
        'OMAAUDIO_VST3_SCANNER=' + vst3_scanner.full_path()])
test_vst3_catalogue = executable('test-vst3-catalogue',
  sources: ['tests/test_vst3_catalogue.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: session_dep, install: false)
test('vst3_catalogue', test_vst3_catalogue, depends: [vst3_fixture, vst3_scanner], timeout: 180,
  env: ['OMAAUDIO_VST3_FIXTURE_LIBRARY=' + vst3_fixture.full_path(),
        'OMAAUDIO_VST3_SCANNER=' + vst3_scanner.full_path(),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir'),
        'CLAP_PATH=' + (meson.current_build_dir() / 'tests/empty-clap')])
test_vst3_parameters = executable('test-vst3-parameters',
  sources: ['tests/test_vst3_parameters.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: session_dep, install: false)
test('vst3_parameters', test_vst3_parameters)
test_vst3_instance = executable('test-vst3-instance',
  sources: ['tests/test_vst3_instance.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: session_dep, install: false)
test('vst3_instance', test_vst3_instance, depends: vst3_fixture, timeout: 60,
  env: ['OMAAUDIO_VST3_FIXTURE_LIBRARY=' + vst3_fixture.full_path()])
test_legacy_app = executable('test-legacy-app', sources: ['tests/test_legacy_app.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('legacy_app', test_legacy_app, depends: automation_fixture, timeout: 60,
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures'),
        'CLAP_PATH=' + (meson.current_build_dir() / 'tests/empty-clap')])
test('lv2_instance', test_lv2_instance, depends: delay_fixture, timeout: 120,
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])
test_clap = executable('test-clap', sources: ['tests/test_clap.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test_video = executable('test-video', sources: ['tests/test_video.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('video', test_video, timeout: 600)

test_plugin_timing = executable('test-plugin-timing', sources: ['tests/test_plugin_timing.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('plugin_timing', test_plugin_timing, depends: [clap_fixture, automation_fixture, transport_fixture], timeout: 300,
  env: ['CLAP_PATH=' + (meson.current_build_dir() / 'tests/clap-fixture'),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])
test_clip_effect_runtime = executable('test-clip-effect-runtime',
  sources: ['tests/test_clip_effect_runtime.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('clip_effect_runtime', test_clip_effect_runtime, depends: [clap_fixture, automation_fixture, delay_fixture], timeout: 120,
  env: ['CLAP_PATH=' + (meson.current_build_dir() / 'tests/clap-fixture'),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])
test('clap', test_clap, depends: [clap_fixture, automation_fixture], timeout: 120,
  env: ['CLAP_PATH=' + (meson.current_build_dir() / 'tests/clap-fixture'),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])
test('routing', test_routing, depends: [sidechain_fixture, delay_fixture],
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])
test_track_loudness = executable('test-track-loudness', sources: ['tests/test_track_loudness.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('track_loudness', test_track_loudness, depends: [sidechain_fixture, delay_fixture],
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])
test_automation = executable('test-automation',
  sources: ['tests/test_automation.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('automation', test_automation, depends: automation_fixture, env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])

test_latency = executable('test-latency',
  sources: ['tests/test_latency.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('latency', test_latency, depends: delay_fixture, timeout: 120,
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])

test_clip_envelopes = executable('test-clip-envelopes', sources: ['tests/test_clip_envelopes.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('clip_envelopes', test_clip_envelopes, timeout: 120)

test_ducking = executable('test-ducking', sources: ['tests/test_ducking.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('ducking', test_ducking, depends: delay_fixture, timeout: 120,
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])

test_pauses = executable('test-pauses', sources: ['tests/test_pauses.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('pauses', test_pauses)

test_stretch = executable('test-stretch', sources: ['tests/test_stretch.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('stretch', test_stretch, timeout: 120)

test_restoration = executable(
  'test-restoration', sources: ['tests/test_restoration.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('restoration', test_restoration)

test_channel_edit = executable('test-channel-edit', sources: ['tests/test_channel_edit.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('channel_edit', test_channel_edit)

test_project_rate = executable('test-project-rate', sources: ['tests/test_project_rate.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('project_rate', test_project_rate)

test_disk_audio = executable('test-disk-audio', sources: ['tests/test_disk_audio.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep],
  link_args: ['-Wl,--wrap=pwrite64'], install: false)
test('disk_audio', test_disk_audio)

test_stream_cache = executable('test-stream-cache',
  sources: ['tests/test_stream_cache.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep],
  link_args: ['-Wl,--wrap=pread64'], install: false)
test('stream_cache', test_stream_cache, timeout: 60, depends: [delay_fixture, sidechain_fixture],
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])

test_audio_cleanup = executable('test-audio-cleanup', sources: ['tests/test_audio_cleanup.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep],
  link_args: ['-Wl,--wrap=unlinkat'], install: false)
test('audio_cleanup', test_audio_cleanup)

test_process = executable('test-process', sources: ['tests/test_process.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('process', test_process, timeout: 15)

test_elastic = executable('test-elastic', sources: ['tests/test_elastic.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('elastic', test_elastic, timeout: 180, depends: [delay_fixture, sidechain_fixture],
  env: ['LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])


test_voice_cleanup = executable('test-voice-cleanup', sources: ['tests/test_voice_cleanup.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('voice_cleanup', test_voice_cleanup)
executable('bench-voice-cleanup', sources: ['tests/bench_voice_cleanup.cpp'],
  include_directories: engine_inc, dependencies: [session_dep], install: false)

# Optional compatibility matrix and externally supplied decoder fixtures.
executable('bench-format-matrix', sources: ['tests/bench_format_matrix.cpp'],
  include_directories: engine_inc, dependencies: session_dep)

# Reproducible editing sequences compared with an independent flat-sample oracle.
test_edit_sequences = executable('test-edit-sequences', 'tests/test_edit_sequences.cpp',
  include_directories: engine_inc, link_with: session_lib, dependencies: session_deps)
test('edit_sequences', test_edit_sequences, timeout: 120)

test_multitrack_clipboard = executable('test-multitrack-clipboard', sources: ['tests/test_multitrack_clipboard.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('multitrack_clipboard', test_multitrack_clipboard)

test_file_batch = executable('test-file-batch', sources: ['tests/test_file_batch.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('file_batch', test_file_batch)

# Portable snapshot publication, relocation and copy fault injection.
test_project_collect = executable('test-project-collect', sources: ['tests/test_project_collect.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep],
  link_args: ['-Wl,--wrap=write', '-Wl,--wrap=fsync', '-Wl,--wrap=renameat2'], install: false)
test('project_collect', test_project_collect, timeout: 120)

test_clip_effects = executable('test-clip-effects',
  sources: ['tests/test_clip_effects.cpp', 'tests/alloc_counter.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('clip_effects', test_clip_effects, depends: [clap_fixture, automation_fixture, delay_fixture], timeout: 300,
  env: ['CLAP_PATH=' + (meson.current_build_dir() / 'tests/clap-fixture'),
        'LV2_PATH=' + (meson.current_build_dir() / 'tests/fixtures') + ':' + lv2_dep.get_variable(pkgconfig: 'lv2dir')])

test_analysis = executable('test-analysis', sources: ['tests/test_analysis.cpp', 'tests/test_speech_align.cpp', 'tests/test_remix.cpp'],
  include_directories: [engine_inc, test_inc], dependencies: [session_dep], install: false)
test('analysis', test_analysis, timeout: 120)
endif
