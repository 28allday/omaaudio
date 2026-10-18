# OmaAudio's own live effects, shipped with the application (see denoise.cpp,
# filters.cpp and dynamics.cpp).
denoise_plugin = shared_module('denoise', 'denoise.cpp', dependencies: [lv2_dep, rnnoise_dep],
  name_prefix: '', install: false)
filters_plugin = shared_module('filters', 'filters.cpp', dependencies: [lv2_dep],
  name_prefix: '', install: false)
dynamics_plugin = shared_module('dynamics', 'dynamics.cpp', dependencies: [lv2_dep],
  name_prefix: '', install: false)
own_plugins = [denoise_plugin, filters_plugin, dynamics_plugin]
foreach ttl : ['manifest.ttl', 'denoise.ttl', 'filters.ttl', 'dynamics.ttl']
  configure_file(input: ttl, output: ttl, copy: true)
endforeach
