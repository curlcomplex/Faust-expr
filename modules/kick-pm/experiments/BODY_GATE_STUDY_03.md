# Body/gate verification and BDM-01 comparison — 03

9 September 2026. Continues #14 / PR #32. New DSP remains an isolated research body, not a released kick or a replacement for the existing experiments.

## Recovery and source distinctions

The previous chat delivered `kick-reference-study-02.zip` (SHA256 `f0cc091d6fe675adc88d969d2e6d410be2e16bfb3818f5694111d8a813b2dbf2`) with analysis, fitted original-engine presets, and a proposed uncompiled gate-release body. Its frozen BDM-01 estimates are now committed in `body-gate-study-03.json`; they are not refitted here.

At recovery the remote branch had also advanced to `f84c7884ab7fa95824a220068bbc0912958681a4`, containing `candidates/body-02.dsp`: a broader PM candidate with a timed late fade. That implementation and its compiler driver are preserved. The newly compiled `experiments/body-02.dsp` is intentionally different: sine-body isolation and an explicit gate-release layer. Do not confuse their similarly named paths or treat their behavior as interchangeable.

Original `kick.dsp` is unchanged. The study does not remove modulation from the planned final instrument. It isolates the body to test whether its pitch/envelope explanation works before modifying coloration.

## Prior conclusions retained, not promoted beyond the evidence

The earlier study compared three pitch laws against 26 crossing-period estimates in one near-sinusoidal preview. Its in-sample errors were 65.306 cents for the original coupled semitone sweep, 47.250 for an independent log-frequency curve, and 0.804 for additive hertz with exponential decay. The latter yielded a roughly 65.4 Hz body, 354 Hz initial addition and 25 ms time constant. These are observations about this recording, not recovered hardware knob values.

Peak-envelope fitting suggested body decay followed by additional attenuation near 337 ms. Recording note length and track envelope were unknown; therefore the gate-release layer is kept explicit rather than attributed to the hardware's machine kernel. Earlier unrestricted spectral searches sometimes improved a score by detuning; rejected fits and the constrained original-engine searches remain historical evidence in the study-02 package. No new parameter search was run in study 03.

All six preview references had been inspected descriptively. The reference-fit protocol's earlier `reserved_not_inspected` field was corrected; the final two recordings are not blind validation.

## Implemented timing corrections

The isolated body's first gate-off sample has release elapsed time zero. The next sample begins attenuation. This corrects the proposed counter's one-sample-early release. A test compares the gate-off sample to an otherwise identical held note; another checks the complete relative release curve. Retriggering resets the same persistent object's phase, elapsed time and latched velocity.

For constant parameters, the reset phasor uses the sum of frequency samples 1 through n. A continuous exponential-frequency integral does not use exactly the same coefficient. The conversion is:

`native_depth = continuous_depth * sample_rate * tau * expm1(1/(sample_rate*tau))`.

This deterministic conversion (353.711579 -> 353.870784 Hz at 44.1 kHz for the frozen preset) compensates integration convention; it is not another optimization against the recording. Both the unconverted and converted cases are retained.

## Actual execution evidence

DSP/check commit: `c7be3745b3c38f66f53753c346a63b48b29f6eed`.
Hosted actual-Faust run: https://github.com/curlcomplex/Faust-expr/actions/runs/34286553186
Artifact: `10079720619`, SHA256 `f65f918a68747dc0f62339ca37fccb791c7fe4808c97f6628386878960e33d17`.

The new isolated probe produced 52 renders and passed 115 checks: silence, headroom, tail, discrete phase/envelope oracle, exact release boundary, release curve, onset-latched velocity, persistent retrigger, dynamic notes, 44.1/48/96 kHz and 1/32/64/127/128/256/512 segmentation, plus scalar/vector checks. The independently derived double-precision oracle differed by at most 1.77e-5 in the tested cases. These are bounded implementation tests, not full-range or device acceptance.

The artifact's ZIP, source, generated files, binaries, scores and raw outputs were checked independently. Its unmodified generated C++ was rebuilt with the container compiler and all 52 scores replayed bit-for-bit. This is independent C++ replay, not a second Faust compile or a Mac/iPhone result. Original module tests and smoke checks also continued passing in the hosted run. Five new synthetic equation tests separately exercise the oracle and its one-sample mutation sensitivity; final-head CI evidence belongs in the PR.

## Same-recording comparison

`compare_body_gate.py` uses the pinned BDM-01 decoded bytes and frozen estimates. Candidate audio is rendered by the actual generated native executable, never by the Python oracle. It uses the same three-resolution log-spectral diagnostic as the previous study (256/1024/4096 windows, reference-relative -60 dB floor), with one recorded whole-hit RMS match. No time shifting, phase alignment, EQ, limiter, reverb or per-window normalization is applied after rendering.

| Candidate | Mean spectral diagnostic error, dB |
| --- | ---: |
| Previously fitted original engine | 1.121715 |
| New gated body, direct prior estimates | 0.218478 |
| New gated body, discrete-phase conversion | 0.222982 |
| New body with release deliberately omitted | 1.472216 |

Lower is better for this diagnostic only. The phase conversion improves full waveform residual from -30.56 to -33.51 dB relative to the reference, while slightly worsening the spectral score; it also worsens the late-tail phase residual. Neither variant is an across-all-metrics winner or an automatic musical selection. The phase-aware number cannot be read as a perceptual rating or a fair equal-search-budget architecture ranking: the old preset was fitted spectrally, while the new body's phase comes from the prior crossing analysis.

The release ablation supports retaining a separate articulation stage for this capture. The main residual remains in the first milliseconds and fine phase/tail detail. Lossy preview encoding and unknown hardware settings limit what those differences identify. Do not add complexity merely to fit codec artifacts.

Four variants were rendered again with 127 instead of 128 samples per block, with exact equality. The listening sequence repeats reference -> old fitted -> phase-converted body three times. Raw audio remains unchanged; one whole-hit gain per synth, one shared attenuation, gaps and PCM16 conversion are recorded in results.json.

## Attribution and provenance

BDM-01 from *Syntakt Designer Drums*, Winston Edwards / Particles Into Waves, 13 June 2022:
https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums
License: CC BY 4.0, https://creativecommons.org/licenses/by/4.0/

The reference is the public lossy MP3 preview decoded at 44.1 kHz, not the lossless original. Firmware, controls, velocity, gain and per-hit onboard drive are unknown. This comparison edits ordering/levels and adds our synthesis; no creator/Elektron endorsement is implied. The WAV hash is pinned in the preset; no reference asset is committed here.

## Reproduce

With existing Faust/C++/NumPy dependencies:

```sh
python3 tools/modules/body_gate_probe.py --out build/gate-body
```

With SciPy installed and the verified reference directory supplied locally:

```sh
python3 tools/modules/compare_body_gate.py \
  --references /path/to/verified-references \
  --body-runner build/gate-body/scalar/render \
  --baseline-runner build/kick-pm-01/scalar/render \
  --out build/body-gate-comparison
```

Ordinary CI compiles/checks without downloading hardware recordings. The comparison and raw evidence package are separate. No local Mac runner, production host, repository visibility, signing credentials or merge was changed.

## Next bounded experiment

Preserve this body as a baseline. Test how the richer existing candidate explains BDM-05/09 with this pitch law and independently controlled body/articulation, then examine the long BDM-14 tail separately. Keep absolute pitch and dynamics checks alongside spectral loss; do not tune away pitch to lower a score. A small controlled known-settings capture set is still needed to identify macro relationships. Final six-control mappings, feedback/coloration topology, whole-library musical approval and mobile/embedded budgets remain open.
