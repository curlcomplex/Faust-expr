# Analog Hats v2 — measured original-808 reference, expanded controls

Canonical source: `modules/hats-analog/v2/hats.dsp` plus local `engine.lib`.
Identity: **hats-analog / 0.2.0-experiment**. This is the current candidate in issue #53 / PR #54. V1 is retained as a negative-control baseline, not silently replaced. No runtime samples. Musical approval and device qualification are not complete.

## Why this reference

The original Roland TR-808 is the chosen sonic benchmark for this analog/electro-hat role. This is a design choice, not a claim that one hi-hat is universally best. Synthmania provides WAV recordings taken directly from an identified original unit (serial 209265), explicitly permits downloading/editing/sampling, and states that controls were varied while recording. This is stronger provenance than an anonymous pack or a mixed performance.

- Actual measured source: https://www.synthmania.com/tr-808.htm
- Modern control/interaction lead: https://www.hexinverter.net/mutant-hihats
- Six-oscillator/choke architecture comparator: https://tiptopaudio.com/808-2/

Only the original-808 WAVs were measured. The Mutant and HATS808 documentation inform the design family, not a claim that either module was captured or cloned. Exact reference knob values, gain, and calibrated capture chain are unknown. The source sweeps informed design; repeated-shot checks are NOT untouched or controlled held-out knob validation.

Pinned originals:

- Closed WAV SHA256 `115bc46c9dc65dd7341576665fae8d81712696cb5f3c296c9fd878a2c83ec359`.
- Open WAV SHA256 `9fc7c8847d508c61c244fa20f83bc30324e19022efb4b551b68077c60235dee9`.

The report records exact source sample indices, durations, resampling, patch settings and audition gains. Changed references fail the hash check. Complete reference recordings are not redistributed in the final CI artifact; only brief attributed A/B excerpts remain. The Faust synth never reads them.

## The actual instrument

Six synthesis columns: **Metal / Tone / Decay / Shape / Choke / Drive**, with Decay in column 3. Pitch ratio, articulation, velocity, note trigger and external choke are performance inputs. See `manifest.json` for the ABI.

The metal source is six continuously running polyBLEP square oscillators, with inharmonic spread and optional ring-product color at high Shape. Metal blends this against noise; Tone moves separate articulation-specific filter responses. Closed hats are substantially brighter than open hats in the measured reference, so they are not one noise burst with two durations. Both use finite-attack, hold/decay envelopes, with independent drive and velocity processing. The numeric oscillator ratios and filter mapping are an authored approximation, not recovered circuit constants.

One paired kernel contains separately latched CH and OH signal paths. With identical settings the compiler may share common subexpressions; do not assume a fixed shared-bank allocation without inspecting generated code. A closed hit can use different pitch, tone, velocity or decay without changing a ringing open hit. Noise, oscillators and filter state persist, rather than resetting to a repeated sample at every onset.

The closed envelope has a variable hold and short decay; the open has its own hold and longer decay. Values are envelope time constants, not promised total durations. Both reach silence before the 30-second age clock saturates. Note-off does not choke.

Choke is explicit DSP behavior, not arbitrary voice stealing. Exactly zero is off. Above zero, the release time constant varies from about 100 ms to 0.25 ms; the default 0.78 is about 0.94 ms. Release progress accumulates, so repeated closed hits cannot revive an open tail. A new OH clears prior choke progress. An external `choke_gate` creates no closed hit and wins over a simultaneous OH onset. It is applied after the DC blocker so the blocker cannot leave a residual ringing tail after a hard choke.

## Repairs relative to v1

V1 used global locks that let CH events change the ringing OH's velocity/timbre. Its nominal choke-off still decayed the OH; resetting choke age could revive it. Its capped age clock froze the longest exponential at a nonzero level. It also shared one broad filter profile between articulations. These are preserved as actual negative-control failures, not just notes about hypothetical bugs.

V2 replaces those behaviors and labels `pitch_ratio` honestly. The v1 name `pitch_hz` represented a multiplier, not a fundamental in Hz. This is a breaking sound/ABI revision. Do not silently migrate old playback identities or assume Tracker already routes the new external choke.

## Reproducible evidence

Dependencies: Faust, a C++17 compiler, Python 3, NumPy, SciPy. The native renderer is the existing `tools/modules/render.cpp`, not a Python substitute synth.

```sh
python3 tools/modules/hats_v2_reference.py --references build/hats-v2/references --fetch-only
python3 tools/modules/hats_v2_delivery.py --out build/hats-v2/run
python3 tools/modules/hats_v2_reference.py --references build/hats-v2/references --render build/hats-v2/run --out build/hats-v2/audition
```

Use `FAUST=/path/to/faust` and `CXX=/path/to/c++` to choose compilers. Actual scalar/vector and voice-separated builds, 44.1/48/96 kHz preset renders, block sizes 1/32/64/127/128/256/512, exact silence, velocity, note-off, onset locks, independent articulation state, explicit/chained/simultaneous choke, long tails, 64 control corners, 128 retriggers and invalid-score rejection are covered. Noise-only and single-oscillator variants are rendered without changing unrelated routing. Seven additional reference-patch renders accompany the qualification suite. Counts and source/generated/score/raw hashes are in the executed JSON, not proof of musical quality.

The local full suite currently has **101 actual renders / 146 checks**, plus **7 reference-patch renders**. Final hosted results must be consulted at the pinned commit before making a hosted-pass claim. Maximum observed raw absolute sample in this suite is about 0.659; there is no hidden mastering limiter.

### Measured sound differences

Using the same onset-aligned, 48 kHz-resampled reference excerpts:

| Example | Reference centroid | New centroid | Reference t90 | New t90 |
| --- | ---: | ---: | ---: | ---: |
| Closed | 10.96 kHz | 11.09 kHz | 34.56 ms | 34.69 ms |
| Short open | 8.04 kHz | 7.69 kHz | 55.42 ms | 47.48 ms |
| Medium open | 8.05 kHz | 7.71 kHz | 183.98 ms | 182.88 ms |
| Long open | 8.09 kHz | 7.70 kHz | 347.52 ms | 349.98 ms |

All seven listed excerpts have a smaller seven-band Jensen-Shannon distance than the fixed v1 default. This is development-set descriptive evidence, not a percentage authenticity score. The new closed hat still puts more energy above 8 kHz than the reference (about 85.7% vs 74.4%); the short OH is slightly shorter, and the OH spectral center is slightly lower. Keep these differences visible.

**Counter-evidence:** the single-oscillator ablation beats the full bank on this coarse band-distance metric for the tested CH and medium OH. That metric does not resolve the fine metallic texture and cannot establish that six oscillators sound better. Both alternatives are retained for audition. Noise-only is worse on these two band comparisons, but that is not a general perceptual ranking either.

Noiseless matched-onset 48 vs filtered-96 kHz comparisons give approximately -14.6 dB (clean) and -15.0 dB (extreme) relative residual. This includes oscillator, filter and nonlinear sample-rate dependence, NOT isolated alias energy. PolyBLEP reduces oscillator discontinuity aliasing; the complete drive/ring/filter path is not claimed alias-free or sample-rate invariant. Target-device checks remain outstanding.

## Listening files

- `01_808_reference_then_new.wav`: original then new, in four pairs: closed, short OH, medium OH, long OH. One sound every 1.7 seconds.
- `02_eight_analog_hats.wav`: Classic, Tight, Soft, Dust, Crunch, Low Metal, Glass, Long. Each three-second patch has CH, softer CH, OH, then an OH choked by CH after 150 ms.
- `03_hats_only_groove.wav`: 120 BPM, no backing music hiding the hats.
- `04_808_old_new.wav`: original, v1, v2 for the same four examples.

Comparison clips are RMS-matched with one common headroom scalar and 5 ms end fades only. No corrective EQ, compression, reverb or limiter. Preset bank, individual presets and groove share one global gain; no per-hit normalization hides velocity differences. Float source renders remain in the evidence artifact.

## Consumer boundary

Use one paired kernel per intended choke group, or broadcast external choke events to every ringing OH instance. A separate DSP per note does not automatically implement cross-voice choking. Validate finite/range inputs, send articulation exactly 0 or 1, apply locks before the onset sample, and compute a low gate sample between triggers. Hosts own voice lifecycle, sleeping and stealing. Same-articulation retrigger/steal click quality is not certified by finite-output tests.

No Tracker implementation, source visibility changes, merge or deployment occurs here. Musical approval is Felix's decision. Adapter parity, named-device callback/thermal budgets, and broader rate/retrigger listening remain release gates.
