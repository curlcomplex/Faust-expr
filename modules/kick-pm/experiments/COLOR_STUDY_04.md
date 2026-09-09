# Color study 04: gated body, PM and drive placement

9 September 2026. Continuation of #14 and PR #32. Research candidate, not a released instrument or recovered Elektron algorithm. The previous BDM-01 body study remains unchanged in `BODY_GATE_STUDY_03.md`.

## Implemented distinction

`candidates/color-04.dsp` combines the verified additive-hertz pitch fall and explicit gate-release layer with square-like and triangular phase modulation. Modulation time is separately adjustable. Two laboratory configurations put the nonlinear stage before or after the body amplitude envelope; gate release remains separate and after either arrangement.

This separation makes drive placement falsifiable without changing note-off timing at the same time. Zero coloration retains the clean body. The forward-path feedback contribution is explicitly reset on the triggering sample; persistent retriggers are tested. `drive_after_body` and `feedback_mode` are diagnostic controls, not a finalized shipping panel or an assertion that hardware switches topology per patch.

## What was fitted

BDM-05 and BDM-09 public preview recordings, not BDM-14 in this slice. These are same-recording, unknown-settings fits. All six acquired references have already been inspected descriptively; there is no blind hold-out claim.

For each recording, compare drive before/after the body first without PM and then with PM enabled. The PM search starts from its corresponding no-PM result and retains that baseline if no improvement is found. Carrier frequency is fixed to the measured late dominant component. A separate full-band late-peak check rejects discrepancies above 15 cents; the search penalizes errors beyond 5 cents. This does not establish every early pitch trajectory or the fundamental of an arbitrary inharmonic sound.

Search: deterministic seed 20260909, differential evolution with 12 iterations and population multiplier 5, followed by bounded Powell search of at most 400 evaluations. Physical bounds/log transforms, initial conditions and improving candidates are retained in each protocol/history. There were 4,100 scored search evaluations for BDM-05 and 4,200 for BDM-09, all through the actual Faust-generated native kernel. These are not 8,300 independently selected validation tests. Different-dimensional no-PM and PM searches do not have identical evaluation counts.

The selection objective is mean three-resolution log-magnitude RMSE + 0.15 times 10 ms envelope RMSE + late-pitch penalty. Spectra use 256/1024/4096 samples and reference-relative floors. Envelope errors and energy quantiles are also reported separately. One whole-hit RMS gain is allowed for timbre comparison, with raw level and the gain recorded. The objective and its weight are diagnostic choices, not a calibrated perceptual scale.

## Results, including contrary evidence

Lower spectral/envelope error is better within this diagnostic. Values are dB RMSE; the columns measure different properties and are not interchangeable.

| Recording and model | Spectral | Envelope |
| --- | ---: | ---: |
| BDM-05, previous fitted original engine | 1.99910 | 4.72106 |
| BDM-05, no PM / drive before body | 1.73966 | 1.42280 |
| BDM-05, no PM / drive after body | 1.77817 | 1.17690 |
| BDM-05, PM / drive before body, selected for audition | 1.67082 | 1.20354 |
| BDM-05, PM / drive after body | 1.76730 | 0.96868 |
| BDM-09, previous fitted original engine | 0.73738 | 1.72875 |
| BDM-09, no PM / drive before body | 0.73857 | 0.92129 |
| BDM-09, no PM / drive after body | 0.75716 | 0.64913 |
| BDM-09, PM / drive before body | 0.73095 | 0.84164 |
| BDM-09, PM / drive after body, selected for audition | 0.75621 | 0.63877 |

**BDM-05:** the selected new fit improves both diagnostics over the prior fit. The 90%-energy point is 134.4 ms versus the reference's 149.6 ms; the prior fit was 85.8 ms. It still concentrates energy too early. The after-body alternative has lower envelope error but higher spectral error; retain it.

**BDM-09:** the selected new fit improves envelope error but slightly worsens spectral error versus the prior fit. The before-body PM alternative has a better spectrum than both, with worse envelope error. Its combined objective is only about 0.005 higher than the selected alternative, so this is not a robust universal winner. The 90%-energy timing error also worsens in absolute value: previously about 17 ms early, now about 21 ms late. The 50%-energy point improves from about 20 ms early to 5 ms early. Do not summarize these results as all metrics improving.

The selected late-component discrepancies are approximately -0.031 and -0.021 cents. This guards against detuning to lower spectral error, not proof of hardware-identical pitch over the whole transient.

The original engine's comparison uses its previous fitted presets; it was not exhaustively reoptimized with this new weighted objective and budget. The new model has more independent controls. These results show a useful candidate and identifiable tradeoffs, not a controlled proof of an intrinsically superior architecture.

## Interpretation and identifiability limits

1. Retain the verified body/articulation foundation in the richer candidate; the neutral-path regression must continue passing.
2. Do not freeze one shared drive placement yet. The two selected configurations differ, alternatives remain competitive, and per-hit onboard drive/routing is unknown. Do not introduce arbitrary automatic topology switching to explain two presets.
3. BDM-09's selected PM depths are very small. This recording does not justify adding a complicated modulation network; that does not imply PM is unnecessary for the whole machine.
4. BDM-09's fitted note-off is two samples before the recording ends, so release time is not identified by that fit. BDM-05's fitted attack constant is below one sample and similarly must not be presented as a measured hardware time constant. The decimal precision in a preset is reproducibility, not measurement certainty.
5. Human sonic approval, macro-map fidelity, high-register aliasing, full control-range behavior and mobile/embedded realtime suitability are still open. The source computes both diagnostic drive paths; production specialization is later measured work.

## Tests and exact build identity

DSP source commit `6605eb051115ada779541fe6d4516d8a2b2c9fb3`, successful actual Faust build/run:
https://github.com/curlcomplex/Faust-expr/actions/runs/34326940729

Artifact 10094169594, ZIP SHA256 `11f703aa9616a07ef71a7cbffc439ff420e6b8be5ae3622ea55d52268101b218`.

The new suite passed **100 actual score renders and 211 assertions**, covering 44.1/48/96 kHz, both drive placements, scalar/vector, blocks 1/32/64/127/128/256/512, clean-body preservation, release boundary/law, latched velocity, persistent retriggers, dynamic locks, selected corners and fitting-interface rejection/parity. Older original and isolated-body suites remain separate and passing. Six additional synthetic diagnostic tests verify error detection; they are not hardware or Faust-render evidence. Code commit `38ee67e68b21714e73709555c3fdc399450854b8` also passed CI run 34328314239 after those tests and fitting/replay scripts were added.

The downloaded artifact's hashes, source, generated headers, scores and raw recordings were checked. Recompiling the unchanged generated C++ and replaying all 100 captures in the container produced six bit-identical files and maximum absolute sample discrepancy `2.1379441022872925e-05`. This is a second C++ compile/replay, not a second local Faust compilation or an Apple device result. Final selected preset replays are bit-identical between 127 and 128 samples per block on the same local build.

Initial run 34326741330 failed before color rendering because `waveform` is a reserved Faust identifier. Renaming it to `carrierWave` fixed the build. No threshold was relaxed or output normalized to manufacture a numerical pass.

## Reproduce without another search

With the repository at this checkpoint and existing Faust/C++/Python NumPy/SciPy dependencies:

```sh
python3 -m unittest discover -s tests -v
bash scripts/build.sh
python3 tools/modules/build_pilot.py --out build/kick-pm-02
python3 tools/modules/color_probe.py --out build/kick-color-04
python3 tools/modules/replay_color.py \
  --presets modules/kick-pm/experiments/color-study-04.json \
  --references /path/to/verified-reference-directory \
  --runner build/kick-color-04/scalar/render \
  --old-runner build/kick-pm-02/baseline/render \
  --out build/kick-color-04-comparison
```

The reference directory must contain the original verified manifest and decoded files, not newly normalized replacements. The accompanying research package also contains unchanged generated headers and a tested `replay.sh`, allowing a C++ replay without a Faust installation. Compiler differences may change low-order bits; the numerical/sonic evidence is tied to the recorded build.

To reproduce a bounded fitting search rather than the frozen comparison:

```sh
OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 python3 tools/modules/fit_color.py \
  --kernel build/kick-color-04/scalar --references /path/to/references \
  --out build/fit-bdm05 --id BDM-05 --iterations 12
```

Repeat separately for BDM-09. Do not relabel in-sample optimization as validation. BDM-14 remains a separate next experiment.

## Listening, provenance and next action

`reference-old-new.wav` plays reference -> prior fit -> selected new fit twice for BDM-05, then twice for BDM-09 (about 16.8 seconds). Each synth gets one documented whole-hit RMS match; the whole file then receives one common attenuation, gaps and PCM16 conversion. No post-render timing/phase alignment, EQ, limiter or reverb. Raw audio remains unchanged. The five-second pattern uses one persistent instance, the two fitted anchors, soft repeats and selected triangle locks; its 90 ms gates are authored musical choices, not inferred hardware capture settings.

References: BDM-05/09 from **Syntakt Designer Drums**, Winston Edwards / Particles Into Waves, 13 June 2022.
https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums
License: CC BY 4.0, https://creativecommons.org/licenses/by/4.0/

These are publicly served lossy MP3 previews decoded to float32 WAV, not the offered lossless originals. The creator describes dry recording with onboard drive on some hits; firmware, controls, gain, velocity and per-hit drive/note length remain unknown. Acquisition archive SHA256: `ec411bed8b2bd473ab043f0e9e27e03cc7dba97c16135ce795e7c084b90b3a14`. Full source/decoded hashes and attribution accompany the package and frozen presets. No creator or manufacturer endorsement is implied.

Next: isolate BDM-14's long-tail discrepancy while keeping this body/PM candidate and the BDM-01 neutral regression fixed. Separate amplitude shape from drive before changing oscillator complexity. Preserve both drive-placement alternatives for a later listening/control-range decision; do not start the analog kick or declare this machine finished yet.
