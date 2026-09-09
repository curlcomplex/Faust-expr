# Compact kick control study 06

9 September 2026. Continues #14 / PR #32. This slice moves from fitting individual recordings to an authored instrument surface. It does NOT recover Elektron macro curves or establish a final sound, touch UI or host integration.

## One engine, eight musical controls

Source mapping is in `controls.lib`; `pre.dsp` and `post.dsp` compile the same mapping over the frozen `../candidates/color-04.dsp` kernel using Faust explicit substitution. No kernel copy, preset-name switch, new oscillator network or amplitude-hold control is introduced. Existing research sources, fitted presets and their evidence remain unchanged.

| Control | Mapping and intended musical role |
| --- | --- |
| Pitch | 20–160 Hz, continuous/microtonal. Initial sweep depth scales proportionally with pitch. |
| Decay | Body exponential time constant 18 ms to 1.8 s; gate-release constant 12 to 300 ms. Actual audible duration also depends on gate length, drive placement and level. |
| Sweep | Initial rise of 0–3.5 octaves above Pitch, falling in hertz. |
| Punch | Shortens pitch-drop time from 55 to 5.5 ms and attack from 1.5 to 0.15 ms. It changes timing, not output gain. |
| Color | Squared response into the square-like phase-modulation contribution. |
| Shape | Squared response into triangular phase modulation and its existing feedback. |
| Contour | Moves modulation from persistent to strongly decaying; modulation time shortens from 192 to 12 ms. At zero Color/Shape this intentionally has no audible effect. |
| Drive | Squared response into the existing nonlinear control; exactly neutral at zero. |

Pitch and Decay plus six shaping controls make eight. Gate and velocity are separate event controls, not extra knobs. Wave phase, modulation time, feedback selection and drive placement are not hidden on another compulsory performance page. More detailed lab panels and their arbitrary fitted parameter combinations remain available through the research kernels.

These curves are hand-authored proposals, not hardware measurements. A compact surface deliberately cannot independently recreate every diagnostic parameter. Four new patches named Round, Punch, Colored and Long demonstrate this control space; they are not renamed fitted Elektron presets. The mapping and kernel must be versioned together. Any saved consumer preset must resolve module ID, sound version AND explicit pre/post profile; no in-place legacy-project substitution is authorized.

## Motion and parameter locks

Live changes follow a 3 ms one-pole in normalized control space; pitch follows log-Hz space. On a gate rising edge the controls jump to their exact requested targets before the voice renders that onset. This avoids blurring a freshly locked step with the gesture smoother. Gate remains sample-exact and velocity remains captured at onset. Repeated hits use persistent state.

The actual compiled smoother is probed directly, not inferred solely from hearing the output. A new patch applied on the note-on sample must render like the same patch set before the note; this is tested for both profiles. All shared-sample parameter writes occur before compute.

Smoothing bounds control-step behavior; it is NOT a universal click-free claim. The monophonic kernel resets phase/envelope on a retrigger and can cut an existing tail. Live Decay changes affect the ringing tail through the existing age-based envelope expression and can increase as well as decrease its remaining amplitude. No new energy-conserving envelope integrator is implied. Extreme combinations have large adjacent-sample waveform changes; bounded output does not certify alias-free or attractive sound.

## Two explicit drive profiles

PRE puts drive before the amplitude envelope. POST drives the decaying body. Both keep note-off release separate. They are separately compiled alternatives with identical control meanings, never automatic modes selected by a patch name, pitch or threshold. Both remain available because the reference studies did not identify a universal winner. No profile has received Felix's listening approval.

POST can be louder or longer at high drive. In the authored anchor recording, whole-segment RMS is approximately PRE/POST: Round 0.108/0.108, Punch 0.069/0.078, Colored 0.125/0.154, Long 0.258/0.314. This is recorded output level, not perceptual loudness matching or a claim that POST is better. Auditions preserve fixed kernel level 0.6, without EQ, limiter, reverb or normalization.

## Executed DSP checkpoint

Code checkpoint `96dc06ff0c5d272cd8108695f0e5e119eacc5154` passed hosted run:
https://github.com/curlcomplex/Faust-expr/actions/runs/34356142662

Artifact 10105861903, ZIP SHA256 `51ccc0b36c19f817bd2b367b0000c65cb4180c51b92c0adc737ac09f667952ae`.

**83 actual renders / 194 assertions:** 44.1/48/96 kHz, scalar/vector, 1/32/127/128/512-frame processing, UI/manifest agreement, pre-trigger silence, mapped-kernel parity, velocity and its onset latch, exact note-off boundary, release law, persistent retrigger, no-lag locks, malformed/hidden-control rejection and two persistent corner trajectories. Each corner trajectory covers the 128 endpoints of the seven normalized controls with alternating low/high pitch; it is not every pitch combination or exhaustive dynamic validation. The musical sweep/pattern recordings are included in the 83, not counted again as independent tests.

Static mapped-kernel peak sample differences were 1.45e-5 to 1.73e-5 on the tested colored preset, below the predeclared 1e-3 oracle threshold. Different floating-point evaluation of the mapped constants accounts for a numerical parity test rather than a bit-identity claim. Earlier original/body/color/tail suites and 34 existing Python methods remained passing at that checkpoint.

The downloaded artifact's archive, source/generated/binary/control and score/raw hashes were checked. Its six generated C++ headers were independently recompiled and all 83 scores replayed: three exact files, maximum sample discrepancy `1.7593614757061005e-05`. This is independent C++ replay, not a second Faust build or an Apple/mobile test. Additional repeated-onset sweeps were rendered through these same generated kernels at 127 and 128 frames with exact equality. Eight additional synthetic unit methods check mapping-oracle endpoints/units/relationships; these are not hardware or native-audio tests.

Initial run 34355757273 failed before rendering the new module: Faust 2.70.3's expanded text contained a hyphenated component-metadata identifier that its parser rejected on a second compile. Normal compilation of the ORIGINAL component source succeeds. Expansion remains provenance evidence; it is not promised to be reparsable in that compiler. Neither the DSP design nor test thresholds were weakened to repair the build.

## Reproduce

With the repository at this slice and existing Faust, C++17 and NumPy:

```sh
python3 -m unittest discover -s tests -v
bash scripts/build.sh
python3 tools/modules/control_surface_probe.py --out build/kick-controls-06
python3 tools/modules/replay_compact.py --evidence build/kick-controls-06 --out build/kick-controls-06-replay
```

The first command family compiles actual Faust. The replay script verifies recorded hashes, recompiles the unmodified generated C++, replays all scores, then creates repeated-onset sweeps. It requires C++ and NumPy but not Faust. The evidence directory may be the `build/kick-controls-06` directory from a downloaded artifact or the compact package's evidence directory. It does not run network requests or call an AI model.

Generated-code equivalence, raw hashes, units and bounds are software checks; listening approval and integrated device budgets are separate. Standard library provenance is recorded via expansion and compiler identity, not a new globally enforced dependency lock.

## Listening files

- `profiles-ab.wav`: four anchor pairs in order Round, Punch, Colored, Long; four seconds PRE then four seconds POST for each. It interleaves unchanged segments at fixed gain.
- `pre-pattern.wav` / `post-pattern.wav`: identical seven-second locked phrase, same gain and parameters, explicit profile choice.
- `pre-retriggered-sweeps.wav` / `post-retriggered-sweeps.wav`: 32 seconds, four seconds each for Pitch, Decay, Sweep, Punch, Color, Shape, Contour, Drive. A 100 Hz low-to-high gesture spans repeated onsets so attack controls remain audible.
- The original `pre-sweeps.wav` / `post-sweeps.wav` are sustained-note gesture tests. Punch and pitch-sweep speed naturally have little late effect once their attack has finished; do not confuse that with a broken control.

No hardware reference audio is used or redistributed in this slice. No sample fitting occurred. Listening files are actual Faust-generated output with PCM16 conversion only; the convenience A/B pattern file inserts only a half-second silence between complete PRE and POST patterns.

## Next bounded gate

Use this compact surface for hands-on audition and select/refine control ranges and drive profile with Felix. Before a production integration, investigate high-register aliasing, abrupt retriggers/tail continuity and the extra cost of sample-rate control smoothing. The measured offline timings are not realtime device qualification. Do not start another machine or silently promote this prototype just because it compiles or stays finite.

Faust language source for explicit substitution:
https://faustdoc.grame.fr/manual/syntax/#explicit-substitution
