# 808 auxiliary-percussion oracle notes

## TapTools
Pinned revision: `tap/TapTools@e058c30aa7d187611b224b7fafe2b1c868071bca` (MIT).

Relevant kernels:
- `include/taptools/tr808_tom.h` — one tom/conga class with `size` low/mid/high and `model` tom/conga. Documents service-note tuning spans, decay classes, D80/D81 attack pitch fall, tom-only pink-noise reverberation layer and calibration against the Fischer 1994 TR-808 sample set.
- `include/taptools/tr808_rim.h` — one rim/claves class with the panel switch represented as model selection; rimshot combines ~455 Hz and ~1667 Hz resonators, claves uses ~2500 Hz high-Q voicing.
- `include/taptools/tr808_cowbell.h` — circuit-informed cowbell reference.
- `include/taptools/tr808_clap.h` — clap/maracas shared-channel reference.
- shared `bridged_t.h`, `swing_vca.h` and tests/calibration notebook.

This batch does **not** claim a line-for-line TapTools port or whole-instrument match. TapTools is used as an open, pinned circuit/oracle reference and as evidence that Tom/Conga and Rim/Claves are shared architectures rather than separate DSP products.

## Hardware recordings
Michael Fischer's 1994 set contains five tuning positions for each low/mid/high tom and conga plus individual rimshot, claves, maracas and cowbell captures, recorded directly from TR-808 serial 103852. Use these in the later reference-comparison pass subject to the exact source/licence terms of the selected mirror. Do not infer redistribution permission merely from a mirror.

## Current candidates
`tom-conga.dsp`, `rim-claves.dsp`, `maracas.dsp`, `cowbell.dsp` are experimental Faust voices. The shared-engine consolidation is intentional; exact circuit fidelity and hardware matching remain pending.
