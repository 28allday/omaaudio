# Signalsmith dependencies

Approved by Gav on 2026-09-08 for this free, public, open-source MIT program.
Unmodified upstream headers; both MIT notices are included alongside this file.

- Stretch: https://github.com/Signalsmith-Audio/signalsmith-stretch
  commit `57b93f4e9206a089a45387eaa39bdc9f310d3308`
- Linear: https://github.com/Signalsmith-Audio/linear
  commit `ec38d3332c7e9ce88a2fd969003d170baa2316b6`

Only the portable C++ headers are used. No optional FFT backend, plugin,
submodule, binary, build-time download or additional dependency is required.
The engine's private elastic processor adapter owns the integration.
