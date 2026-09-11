# Tracker / CURLOP consumer handoff

Canonical identity: **snare-analog / 0.2.0-experiment**. Preferred small-block candidate `modules/snare-analog/v2/snare.dsp`, importing its local engine. Pin the qualified commit in PR48/#47. Root `modules/snare-analog/snare.dsp` is historical v0.1, not this instrument; no-tail/envelope are diagnostics, single-crack an explicit audition alternative, reference.dsp the direct-sine oracle. Do not select by the shortest matching filename.

## Columns and performance inputs

Source lane-map proposal, independent of final visual grouping:

| Lane | Stable ID | Default | Meaning |
|---|---|---:|---|
|P1|balance|0.55|equal-power body/noise blend|
|P2|crack|0.48|attack plus initial/secondary short excitation|
|P3|decay|0.5|linked body/noise decay; keep Decay in column3|
|P4|noise_color|0.55|convex dark/mid/bright noise-filter mix|
|P5|tone|0.45|second-mode ratio, weight and decay|
|P6|drive|0.12|in-kernel nonlinear shaping, zero neutral|

All six controls range0–1. Separate `pitch_hz`70–500Hz default180, `velocity`0–1 default1 and binary `gate`. P7/P8 deliberately absent; do not fake a signal dependency to retain unused controls. Pitch and velocity are not extra synthesis knobs. All six controls must remain accessible; Tracker#46 owns broader visual conventions and any paired-column arrangement, not the source parameter identity. Its later owner-authorized replacement/idle policy must not be contradicted by older generic lab instructions.

This is a distinct analog-style voice. There is no pre-existing matching Tracker slot assumed and no instruction to replace the approved PM snare. Registry/project ID allocation belongs to the consumer. Existing files remain readable; deterministic lane/default migration is explicit if an old slot is intentionally replaced. Do not renumber unrelated models or silently reinterpret locks by display column.

## Timing and state

One note = one independently gated mono DSP instance. Host owns polyphony, chords and voice allocation. All pitch, synthesis controls and velocity latch on a computed rising gate; gate-off does not choke. Values must be written before computing the onset. A real low sample must be computed before another onset. A held high gate is one hit, not repeated triggers. These contracts are tested with prepared-versus-same-sample locks.

The canonical renderer advances PRNG/fixed-filter histories continuously, including silence. The owner's Tracker policy permits pausing idle voices and resuming stored state. That intentional difference is allowed: test retrigger audibility, no stale gate, no unintended tail revival and bounded switching/retirement; do not claim sample identity against uninterrupted noise during an idle interval. Do not clear or reallocate the whole object on every hit to disguise state bugs.

Noise is deterministic per initialized DSP state; multiple fresh instances with identical timing can have correlated noise. Randomized per-voice seeds are not part of this canonical version. Any future seed/phase policy must be explicit and audio-tested rather than secretly added by the adapter. Fast retriggers can interrupt ringing tails; no universal click-free promise.

## Build and lifecycle

Actual product flags: `-lang cpp -single -cn ModuleDSP`; qualified vector alternative additionally`-vec -lv0 -vs32`. Native comparison uses`-std=c++17 -O2 -ffp-contract=off`, no fast-math. Preserve generated-content verification and dependency/compiler identity. Expanded Faust expressions accompany the final evidence. Do not manually edit generated computation or validate only a source-hash comment.

The sine table is shared by a generated class. Call`classInit` once OFF the audio thread before concurrent rendering starts, then`instanceInit` per voice/rate. Convenience`init` may rewrite shared storage and must not run while another instance uses it. Benchmark object size excludes shared table, stack and host buffers. No per-hit allocation or background publication of mutable DSP zones.

Generated I/O is0in/1out. `envelope.dsp` has2 diagnostic outputs and must never be selected as the instrument. Explicitly verify UI zone IDs/defaults/units and actual output counts in each AOT/interpreter/native adapter.

## Concrete acceptance

`adapter-map.json` and its render test verify parameter-name/score translation ONLY. They are not an implemented private Tracker native adapter. Consumer must implement thin mapping, canonical/adapted and AOT/interpreter comparisons, same-sample locks, velocity, idle/switch/retrigger, save/reload/project mapping, preparation cost and target callback/memory/thermal tests.

Kernel noise is synthesized, not reference-sample playback. Reference assets are test/listening material with separate attribution, not a runtime dependency. Keep authored Drive in the kernel. No new permissions, personal runner configuration, application code or automatic merge is performed by this handoff. Full Fourier Morph v2 remains unrelated and preserved alongside its v3 lookup alternative.
