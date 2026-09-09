# Tail study 05 — BDM-14, amplitude shape versus drive

9 September 2026. Continues #14 and draft PR #32. **Research, not sonic approval, recovered hardware controls or a release.** The preceding color/body studies and DSP files are preserved unchanged.

## Question and controlled change

BDM-14 has an approximately level early body followed by a long decay. Its 25 ms RMS windows from 1.2 to 2.5 seconds fit a -26.08985 dB/second slope (0.13569 dB residual), equivalent to an amplitude time constant of about 333 ms in that interval. That is measured capture behavior, not a hardware parameter.

`candidates/tail-05.dsp` adds exactly one architectural hypothesis to `color-04.dsp`: delay body-amplitude decay by `body_hold_s`. Attack, pitch, modulation and gate release are not delayed. Zero hold preserves the existing engine. Note-off can release while the body is still held. This tests a plateau mechanism separately from waveform saturation; it is not a decision to add another shipping knob.

## Search and fair-comparison boundaries

First run the existing `fit_color.py --id BDM-14 --iterations 8`: no-PM and PM, each with drive before/after body. Then `fit_tail.py` refines both PM baselines over the supported body-time range up to 2 s and compares the held candidates, initialized at equivalent zero hold. Every objective evaluation is through actual Faust-generated native C++, not Python synthesis.

The unchanged objective is mean 256/1024/4096-frame log-spectral RMSE + 0.15 times 10 ms envelope RMSE + late-pitch penalty. Frequency is fixed to the reference late dominant component (~65.39355 Hz); early/transient pitch is not thereby certified. Whole-hit RMS gain is logged separately. No time/phase alignment, EQ or limiter is applied.

Seed 20260909; initial DE 8 iterations/population multiplier 5 plus at most 400 Powell evaluations; refinement DE 4 iterations/multiplier 5 plus at most 600 Powell evaluations. Initial exploration used 3,311 scored evaluations; refinement used 2,839. These are optimizer work, not 6,150 validation cases. Dimensions/evaluation counts differ; no global-optimum or controlled superiority claim. All six preview references have already been inspected; none is blind validation.

## Results and contrary evidence

Lower spectral/envelope diagnostics are better in their own units; neither is a perceptual scale.

| Candidate | Spectral RMSE dB | Envelope RMSE dB | Late slope dB/s |
| --- | ---: | ---: | ---: |
| Refined color-04, drive before body | 4.39873 | 1.76477 | -27.44021 |
| Tail-05, drive before body | 4.38379 | 1.70032 | -26.85099 |
| Refined color-04, drive after body | 4.56558 | 0.73634 | -27.03241 |
| Tail-05, drive after body | 4.52658 | 0.51358 | -26.16399 |
| Same latter parameters with hold removed | 4.62276 | 0.52406 | -26.19736 |

**After-body comparison:** the 90%-energy error improves from +36.73 ms to +8.19 ms; 99% from +15.24 ms to -0.63 ms. The midpoint remains +49.09 ms late (previously +64.29 ms). The tail slope approaches the reference's -26.09 dB/s. The spectral discrepancy remains substantial; this is not an adequate recreation of its harsh timbre.

**Do not attribute all improvement to hold:** removing hold from the new preset leaves almost all envelope improvement (0.52406 versus 0.51358 dB). It improves spectral error modestly (4.62276 -> 4.52658) at those frozen settings, while the zero-hold version has a slightly closer 90%-energy point (+5.10 ms). Most dynamic improvement came from other fitted body/release settings. This ablation is more informative than comparing differently fitted presets alone.

**Before-body contrary result:** the fitted hold is exactly zero. Its slight improvement comes from further optimization, not a useful hold stage. A supplementary 32-point shape grid found a somewhat better local seed at 250 ms hold/700 ms body time (~4.6133 combined error), demonstrating that the bounded search did not establish an optimum. It did not beat the selected after-body candidate's combined error (~4.6036); preserve the grid rather than hiding it.

The after-body hold lands at its 1.2 s search ceiling, body tau near its 2 s ceiling, and note-off at ~795 ms occurs BEFORE the hold ends. Extending frozen hold to 1.6/2 s makes only small changes (envelope ~0.5094/0.5089 dB). Its exact value is not identified. The sub-sample attack constant and bounded near-maximum coloration likewise are not measurements of hardware settings.

## Decision

Keep the verified body/PM foundation and both drive-placement alternatives. Retain `tail-05` as an experimental option with zero-hold compatibility, **not a new default or mandatory public knob**. Do not add oscillators to solve an envelope discrepancy. Do not silently switch topology based on preset names. A larger waveform-character mismatch remains in this particular reference.

The next product-facing slice should author a reduced-control kick surface and audition continuous sweeps/parameter-locked patterns across the existing clean, punchy, colored and long-tail anchors. Keep unresolved harsh-timbre/aliasing/reference-map questions tracked; do not continue indefinitely fitting unknown-setting samples before assessing playability. This is not authorization to declare the kick finished or begin all other machines.

## Actual verification and failures

DSP commit `d966d58c90126b17d5e939801a0e2fa604f92a52`, successful CI run:
https://github.com/curlcomplex/Faust-expr/actions/runs/34335471993
Artifact 10097556231, SHA256 `cd4d35804fdc1148986e4208e1fd19f3712b82006f4aa047d96ccd9d9e8a5cff`.

The new actual-Faust suite completed 78 renders / 158 checks: 44.1/48/96 kHz, scalar/vector, variable blocks, zero-hold preservation, bounded output, relative-envelope equation, release during hold, onset-latched velocity, persistent retriggers, live hold/note changes and corners. All preceding suites passed separately. Five new synthetic methods check the late-slope analyzer; these are not hardware tests.

The downloaded archive digest, 22 source/build hashes and every score/raw hash were independently checked. Recompiling unchanged generated C++ and replaying all 78 scores produced maximum absolute sample discrepancy `1.4819204807281494e-05`; none was bit-identical across those compiler builds. This is independent C++ replay, not a local Faust compile, Mac or iPhone realtime test. Selected same-build reference renders are identical at 127/128 frames.

Initial run 34335209604 failed before tail renders on unary negation of `max`; explicit subtraction corrected Faust syntax. No numerical threshold or sound normalization was relaxed. A local replay initially encountered a copied executable without its execute bit; correcting that local file mode changed no source or sound.

## Reproduce the frozen audition

With actual Faust/C++/NumPy/SciPy installed:

```sh
python3 -m unittest discover -s tests -v
python3 tools/modules/tail_probe.py --out build/kick-tail-05
python3 tools/modules/replay_tail.py \
  --frozen modules/kick-pm/experiments/tail-study-05.json \
  --references /path/to/verified-reference-directory \
  --baseline build/kick-tail-05/color04/render \
  --candidate build/kick-tail-05/scalar/render \
  --out build/tail-comparison
```

The supplied research package also contains generated headers and a standalone C++ replay script. Raw references and outputs are retained with their hashes; the Git JSON contains frozen parameters, attribution and source hashes, not audio blobs. Full bounded search histories, initial no-PM alternatives, shape grid and hold-only sweep accompany the evidence package. Re-running optimization can vary numerically with compilers/scientific-library versions; the frozen replay does not require another search.

## Reference and listening treatment

BDM-14 from *Syntakt Designer Drums*, Winston Edwards / Particles Into Waves, CC BY 4.0:
https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums
https://creativecommons.org/licenses/by/4.0/

Public MP3 preview decoded to mono float32 at 44.1 kHz, not lossless original. Unknown firmware/knobs/velocity/recording gain/note length/per-hit onboard drive. Its decoded peak exceeds 1.0; these raw values are preserved, not clamped. That does not establish hardware clipping.

The ~11.6 s audition is reference -> fitted current after-body path -> held after-body candidate. One whole-hit RMS match per synth and one common attenuation (about 0.79563), gaps and PCM16 conversion; no EQ/reverb/limiter/alignment. A separate 5.5 s persistent-instance phrase varies gate duration/velocity using the new candidate at fixed kernel gain. Those gates are authored demonstration choices, not recovered recording events. Full attribution and transformations accompany the audio; no endorsement implied.
