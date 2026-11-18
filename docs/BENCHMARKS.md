# Preliminary idle process measurements

18 November 2026. Windows x64, AMD Ryzen 7 9800X3D (16 logical CPUs), NVIDIA RTX 4070.

Both applications used a separate 1280x720, 60 FPS static-color scene, preview disabled and all outputs stopped. OBS used an isolated portable profile; the user's existing OBS instance and configuration were untouched. Each process warmed up for five seconds, then supplied 24 half-second CPU/RSS samples. Runs were sequential, OBS first.

| Process | Mean CPU (% of whole 16-thread machine) | Peak sampled working set |
|---|---:|---:|
| OBS Studio 33.0.0 Release | 0.073% | 115.6 MiB |
| FeatherCast 0.1.0-dev.1 Release | 0.105% | 88.1 MiB |

This small idle sample showed lower sampled RSS and slightly higher sampled CPU for FeatherCast. It does not establish production or gaming performance improvements. Background user applications remained running. Child processes, GPU VRAM, disk caches and startup time are excluded. Individual GPU engine counter samples were collected, but no total GPU utilization claim is made.

Streaming, recording, replay, delay, average gaming FPS, 1% lows and frame-time comparisons remain unmeasured. Raw evidence is in the downloadable `FeatherCast-benchmarks.json` artifact. These process measurements are separate from the unavailable utilization counters in the application UI.
