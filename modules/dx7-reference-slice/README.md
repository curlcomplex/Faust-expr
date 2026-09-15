# DX7 H1: three diagnostic cases, Faust 2.88

Owns the first slice of #96 / #112 under #111; PR115. This is a baseline comparison, not a finished DX7 or a fidelity approval.

This pass reuses the already exercised PR114 patch, renderer and measurement approach, restricted to three cases. The earlier 2.70 evidence remains on its branch; it is not overwritten or relabelled as 2.88. The faulty preliminary runtime wrapper is replaced by reproducible per-case wrappers, generated from `cases.json` through `faust_source()` in `tools/modules/dx7_slice1.py`. Each generated `baseline.dsp` imports the unchanged release DX7 operator. No oscillator/envelope formulas are copied or modified.

## Reproduce

```sh
python3 tools/modules/dx7_slice1.py \
  --out build/dx7-h1/evidence/slice \
  --faust build/dx7-h1/toolchain/build/bin/faust \
  --faust-libraries build/dx7-h1/toolchain/libraries \
  --faust-archive build/dx7-h1/faust.tar.gz
```

The workflow obtains/verifies the complete Faust 2.88.0 archive and builds its compiler. The driver checks the compiler version and every regular release `.lib` file against that archive before and after rendering. The exact MSFA commit is fetched and its working tree must remain unchanged. Source/build/patch/event/audio hashes and the resolved expanded Faust source are retained. Use a fresh output directory.

## Restricted cases

C01: one carrier at 220 Hz, output level 80. C02: the same carrier plus an OP2 modulator at 2:1 and level 70. C03: both operators have distinct documented envelopes and release. All cases are 44.1 kHz, 65536 frames, gate on at 2048 and off at 32768, velocity 100 with velocity sensitivity disabled. Factory samples/patches and firmware are not required.

The native packed patch stores OP6 first and OP1 last. Upstream `UnpackPatch` is checked against a separately encoded 156-byte expected state for every render. Algorithm 1's OP2 -> OP1 subgraph is used; other operators are inactive. The Faust operator call includes the oscillator-sync argument. Both the release library root and `dx7/` are explicit include paths. Unused velocity is optimized out and is not written to a nonexistent UI zone.

Each engine runs primary, cold-repeat and caller-block-127 variants: 18 actual render executions for three settings. MSFA's 64-frame processing cadence is preserved across caller chunks. Unsupported event offsets are rejected, not rounded. C++11 plus `stddef.h` supplies legacy MSFA build compatibility without modifying its sources.

## Interpretation

Raw Q24 MSFA output is divided by 2^24, before its Android-app filter/output pad/clipping. Faust keeps its own per-operator 0.5 factor. Preserve absolute levels and startup material; no fitted gain, latency alignment, extra gate mute or compensating effect is applied. Related Faust/Dexed/MSFA lineage means this is not independent hardware confirmation.

The report separates raw residual, carrier pitch, steady relative level, unit-normalized spectral shape and envelope trajectory. Residuals are diagnostics, not fidelity gates. Amplitude, delay and mute mutants are tested against a self-baseline rather than an already mismatching oracle. Nonzero output, native patch decoding, repeatability and segmentation are execution gates.

Auditions play MSFA first, then Faust, with 100 ms silence and one fixed gain of 0.2 on both throughout. Raw float WAV/f32 pairs, case files, patch bytes, plots, source archives, JSON and hashes remain alongside them. The native-rate/output conventions and differing note-initialization policies must be considered before diagnosing a physical DX7 defect.

No full six-operator coverage, feedback, scaling, cross-rate/vector, hardware, VDX7, target-device or host-product acceptance is implied. Keep subsequent phase/index and startup investigations separate from this frozen baseline.
