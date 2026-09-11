# Analog snare qualification and decisions

Owning issue #47, draft PR #48. The exact current qualified source/run pin and final replay package are recorded in those threads. This document records the fixed design, first successful measurements and contrary evidence; it is not a promise that every CI run has identical performance.

## Instrument choice

Keep a conventional two-mode body, separately shaped noise and a short synthesized impact. This offers direct body/snappy/colour controls rather than the PM snare's operator/feedback relationships. Both machines remain available. The final listening comparison uses matching pitch, gate sequence, velocities and body decay time constants; attack and noise laws still differ. Four authored counterparts are not an exhaustive proof that the PM instrument cannot approximate a given snare. Felix's musical approval remains separate.

Six source controls are Balance / Crack / Decay / Noise Color / Tone / Drive. Pitch and velocity are not counted as synthesis knobs. At Balance0/Crack0 the body is isolated. At Balance1 the sustained body is removed, but Crack can still supply a short tonal/noise impact. Drive0 is exactly neutral shaping. All musical values latch at onset, with a directly tested same-sample lock contract. No native chord engine or internal note polyphony.

## What changed from the starting draft

Root v0.1 is retained as history, not the consumer entry: its patches were invalid JSON, body/noise balance could not reach a clean endpoint, and delayed excitation began discontinuously. V2 uses valid patches, full mix endpoints, finite attacks for every sub-excitation, separate second-mode decay, convex fixed-filter noise colour and direct exponential envelopes.

The first v2 build exposed restricted unary-minus syntax in a function expression; the next exposed a collision with stdfaust's `de` namespace. Those source errors were fixed, not attributed to a compiler capability or timeout. A later independent NumPy2 replay rendered the complete audio set but exposed a non-native scalar in JSON listening gains; explicit native-float conversion and regression tests fix the report path. No DSP or fidelity threshold was weakened to resolve it.

## First complete source qualification

At `b34bd21b9213f2810232f7e1bb128c4594c5c568`, Actions run34570453811 passed **151 actual renders / 289 checks**, plus13 separate arithmetic tests. The refreshed delivery adds two NumPy-safe gain tests and matched pitch/body-decay listening comparisons; verify its exact-head run in PR48.

Actual builds include direct sine,4096-point interpolated lookup, vectorized lookup, two controlled ablations, independent envelope outputs and the existing PM snare. Three rates44.1/48/96kHz; pitches70/180/500Hz; direct float64 body/noise envelope oracles at shortest/longest decay;20-second envelope tails; blocks1/32/64/127/128/256/512; all8 onset-lock patches; held/pulsed gate; latched velocity and tails;128 endpoint settings inside ONE persistent recording;96 rapid retriggers;27-second silence/tail sequence; malformed scores; explicit lane-map translation.

First-pass maximum direct/lookup sample difference5.3644e-7 against unchanged3e-5 limit. Worst envelope-oracle relative error1.4268e-6 above1e-4 amplitude, against unchanged2e-5 limit. Non-diagnostic peak0.605011 and maximum adjacent-sample change0.959474: the latter is a warning about noisy/extreme transitions, NOT click-free acceptance. Do not conceal it by normalization or limiting.

## Reference evidence and controlled ablations

The8 licensed SDB/SDV previews are DIGITAL Syntakt Basic/Vintage context, not SD Classic/analog calibration. Unknown firmware, control settings and per-hit recording/drive levels; lossy codec. Twenty-four authored candidates are reused identically across full, no-tail and single-impact variants. Descriptor scales are fixed, windows power-weighted, and every candidate renders5 seconds. No waveform/knob fitting or held-out-validation claim.

First-pass mean nearest descriptor distances (lower within this study): full0.402225; dedicated tail removed1.378446; only secondary crack pulses removed0.402890. The noise tail clearly contributes to this coarse coverage; the secondary pulses have a very small effect on this metric. Do not claim the measurement proves clustered attack superiority. Keep `single-crack.dsp` as an audible alternative, not a renamed optimized build. Several Vintage references remain poorly represented; many select the same candidate in this small pool.

External architectural/listening prior art is linked in README. No proprietary circuit/source or recordings are embedded in the kernel. Frozen third-party references retain manifest, hashes and attribution separately from source.

## Same-sound performance

The optimization changes only three sine evaluations to ordinary4096-point interpolated lookup. It does not simplify envelopes/noise/drive. The direct implementation remains an oracle. Repeat a warmed four-host-voice workload, rotate direct/lookup/vector order and report p50/p95/p99/max, not just one best result.

First hosted p50 microseconds (direct / lookup / vector):32frames13.22/12.108/12.739;64frames26.88/24.337/25.900;128frames59.07/48.754/51.778;512frames311.993/312.754/317.973. Lookup benefits the tested smaller blocks, but it is essentially tied/slightly worse at512. No universal backend winner or iPhone claim. Retain scalar lookup as the conservative small-block candidate and measure the consumer device.

Generated lookup declares a shared4096-float sine table (16,384bytes), not a per-voice wavetable bank. Include per-object, code, stack and buffers separately. The benchmark checks ordinary new/new[] during compute only; it does not prove absence of every allocator or whole-host realtime safety. Initialize shared tables once before audio and use instanceInit thereafter.

Proper filtered48/96kHz body-only comparisons were -66.07dB with Drive0 and -60.68dB with Drive1. These remove the stochastic noise/crack to compare deterministic paths; they mix phase/filter/interpolation differences and are not isolated alias-energy or full-noisy-instrument equivalence measurements.

## Deliverable and remaining gates

The compact offline package must be run from its own source directory, not just zipped after a CI pass. Include exact source/expanded expressions, generated headers, scores, references/attribution, reports, listening files and content manifest; regenerate bulky float renders. C++ replay is explicitly not a second Faust compiler invocation.

Product gates remain musical selection, intended-register/strong-drive and retrigger/voice-stealing quality, exact canonical/consumer/AOT/interpreter audio, named-device memory/thermal/callback evidence, and project-ID/lane integration. Do not create another round of prerequisite platform work before audition. Tracker owns its private integration and merge.
