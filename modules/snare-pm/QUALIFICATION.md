# Snare qualification and limits

Instrument source and original patches are unchanged from `b6d27218321e227ba3d30d31a31d66209271c2f8`. The extended evidence below was generated at `54cd2534c688adde0ab56b65d78074e8b7eb657e` on 9 September 2026. Later documentation edits do not imply a new sonic candidate. Read PR #39 for the current tested head and attached artifacts.

## Executed implementation evidence

The extended delivery suite contains **227 actual Faust-rendered validation scores / 299 checks**, including the original 162 scores / 208 checks. It also renders two alternate dry audition scores outside that reported validation count. Ten separate snare-specific unit methods check parameter/manifest and score validation; these are synthetic boundary tests, not audio-fidelity evidence.

Coverage includes 44.1/48/96 kHz, scalar/vector compilation, initial silence, clean-body pitch, pulse/held-gate equivalence, linear and onset-latched velocity, mid-tail controls, persistent retriggers, blocks 1/32/64/127/128/512, rapid locks, and a single endpoint trajectory with 128 settings. The endpoint trajectory is neither 128 independent renders nor exhaustive testing of every pitch and intermediate parameter combination. Additional direct oracles compare controls written on the onset sample with the same controls prepared one sample earlier at all three rates.

The new event validator rejects duplicate authored changes to one parameter at one sample rather than silently overwriting them in a dictionary. Simultaneous changes to different parameters are permitted. Native rendering remains sample-exact.

Worst tested raw peak: **0.81991947**; worst adjacent-sample jump: **0.78592551**. Finite/headroom success is not a click-free claim. Large same-sample changes can be legitimate transients or undesirable retrigger artifacts and still need musical judgment.

Hosted delivery run: https://github.com/curlcomplex/Faust-expr/actions/runs/34396445299 . Artifact 10121748571, 58,770,028 bytes, SHA256 `1e9ddeac9f3d1717c0de6e988c0330dee2ba37a5e57c6cfa483a7b7e664558d4`.

The downloaded source/generated/score/audio hashes were checked. Unchanged generated C++ was independently recompiled and all 227 validation scores replayed; 15 were byte-identical and the largest absolute sample discrepancy was **6.556510925292969e-7**. This establishes bounded cross-compiler output agreement for these scores, not a second Faust compiler run or device result. PCM16 files can differ at rounding boundaries despite tiny floating-point differences.

## Corrected architecture comparison

The old `deterministic.dsp` and `hybrid.dsp` differ in crack gain as well as the dedicated tail. Both contain pseudo-noise in the crack. Preserve their source identities, but describe them accurately.

A third candidate removes **only** the hybrid's noise tail. With the same 48 settings, same eight references and the original frozen feature scaling:

| Candidate | Median nearest descriptor distance | Mean nearest distance |
| --- | ---: | ---: |
| Original drier alternate | 1.535073 | 1.432312 |
| Hybrid with dedicated tail | 0.985446 | 1.015720 |
| Hybrid with only tail removed | 1.535081 | 1.432331 |

The tail remains the main contributor to the measured Basic-family coverage advantage. That does not prove a better-sounding instrument or recover hardware internals. Several Vintage examples remain closer to the drier alternative. Many references select the same nearest point in the small pool, emphasizing that this is coarse architecture coverage, not eight individually fitted replicas or validated macro mappings. The references and descriptor scales influenced the design: do not label this held-out testing.

## Performance: no universal winning backend

A stronger warmed four-voice benchmark alternates scalar/vector order, repeats each pair four times and measures the same settings. Median p50 compute microseconds at 48 kHz:

| Frames | Scalar | Vector | Median paired scalar/vector ratio |
| --- | ---: | ---: | ---: |
| 32 | 24.633 | 23.099 | 1.067 |
| 64 | 50.799 | 46.554 | 1.091 |
| 128 | 101.640 | 93.483 | 1.087 |
| 512 | 410.709 | 379.074 | 1.083 |

On this hosted machine vector is modestly faster. **The independent container reverses that ordering**, with scalar/vector ratios approximately 0.780, 0.781, 0.782 and 0.866. The earlier single-instance instrumentation also did not establish a vector advantage. Preserve both builds; do not prescribe vectorization from its name or transfer these results to iPhone or embedded hardware. The portable source has no fast-math approximation or hidden sound change selected by these measurements.

Object sizes in these builds are 368 bytes scalar and 600 bytes vector, excluding host state, audio buffers and stack. Compiler stack-usage files and full p50/p95/p99/max results are retained. No ordinary new/new[] calls occurred in the measured compute regions. The hook does not cover malloc or aligned allocation and does not establish whole-host realtime safety. The benchmark is offline, not an audio callback running under UI/device/thermal load.

## Sample-rate diagnostic, not alias-energy certification

The deterministic diagnostic explicitly suppresses all noise. Equal-time 48/96 kHz renders use filtered polyphase downsampling, not raw every-other-sample comparison. At a 420 Hz body, the clean/moderate/driven cases yield relative RMS differences of approximately **-51.18, -51.23 and -47.11 dB**. These residuals include oscillator integration and filter response as well as nonlinear sampling effects. They are not isolated alias energy, do not exhaust high pitches/control combinations and do not qualify the stochastic path.

## Listening and remaining gates

Fixed-gain, actual-Faust audition files: eight authored anchors, a persistent locked pattern and an all-control traversal, plus drier alternate anchors/pattern. These have PCM16 conversion only, no added EQ, reverb, limiter or normalization. Reference-nearest comparisons separately document one whole-excerpt candidate RMS adjustment and one shared attenuation. They must not be confused with raw-level evidence or a fitted clone comparison.

Still required: Felix's musical choice, retrigger/click judgment in real patterns, intended-register/nonlinear quality, controlled firmware/settings captures for closer matching, and target-device first-paint/memory/realtime/thermal checks. Integration and merge remain Tracker's responsibility. The next independently scoped shared instrument is Metal (#17), not further unbounded snare topology exploration without new evidence.
