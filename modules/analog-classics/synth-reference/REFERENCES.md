# Synth reference checkpoint: SH-101 gain experiment is NOT selected

Owner: Faust-expr #82 / draft PR #83; consumers CURLOP #274/#275/#271.
Original synth baseline: `fd2b7efae4951d014fed3fed3cd01f295a0af21e`.
Saved experiment: `3b914e78cefb9e621bf179de713a26349da8a99a`.

## Correction to the earlier interpretation

The saved v2 removes `comp=.20` from the common four-pole filter's
`y4*(1+comp*k)` output multiplier, where `k=3.85*resonance`.
It does NOT change the filter poles, feedback, nonlinear input stage, oscillator,
envelope, glide or controls. At a fixed resonance, after control smoothing settles,
its output is the old output multiplied by `1/(1+0.20*3.85*resonance)`.
At the default resonance .30 this is about -1.805 dB. Variable resonance can also
change the time-varying level envelope, so it is not one global gain for every patch.

**The old source comments and initial PR wording calling this a confirmed
hardware correction were too strong.** Both v1 and v2 stay preserved. Neither
gain law is selected here as an authentic SH-101 response, and the revised
qualification does not promote v2. Compilation and mathematical agreement with
that gain equation cannot establish the hardware gain law.

## Actual evidence, and why the distinction matters

- Roland SH-101 Service Notes, 1 November 1982, circuit diagram page 7:
  https://manuals.plus/m/e2bf4ea87ccd1c365e55ae9156f3e333a50456d52feaab0c2f9ea127f204575c.pdf
  The VCF/VCA area includes the TR26/TR27 resonance/output network. This is a
  primary schematic, not a measured output-versus-resonance dataset.
- AMSynths' designer overview says the SH-101 has no Q compensation:
  https://amsynths.co.uk/2022/04/06/all-about-the-ir3109-chip/
- The same designer's detailed SH-101-derived AM8101 description explicitly
  describes TR26 feeding the final amplifier to boost level with resonance:
  https://amsynths.co.uk/2022/03/21/5631/
  That replica also changes supply rails. These are not interchangeable
  descriptions of a fully specified numerical transfer function. In-loop
  compensation and resonance-dependent output-stage gain must be distinguished.
  This conflict is recorded, not resolved by choosing the sentence we prefer.
- https://bonzaicoin.net/ describes an SH101 circuit/netlist reconstruction and
  reduced models measured against those netlists. Those are not independent
  physical-unit recordings. No original audio/source, executable comparison or
  complete oracle port from that project is part of this checkpoint.

Therefore removing ALL resonance-dependent gain is not justified merely by the
phrase "no Q compensation". Resolve the output path or measure it before selecting
a replacement. The saved v1's arbitrary .20 coefficient is not validated either.

## Juno and Mini remain unchanged

The previously inspected `stevengoldberg/juno106` is a structural WebAudio model,
not calibrated hardware. Its HPF frequency map is that software's map, not proven
original-hardware cutoff values; do not copy its two-biquad filter as hardware truth.
The current continuous controls remain extensions. Juno chorus and all polyphony
stay outside the single-note kernel.

`t2techno/Faug` is inspectable Faust research, but a redistribution licence was not
established and its source contains fixed-44.1-kHz assumptions. No source is copied.
Its existence does not supply an independent measured Model D reference.

No new controlled hardware recording corpus was acquired for any of these three
synths during this checkpoint. They are not a completed synth calibration batch.

## Executed qualification and QA correction

The saved 36 checks / 16 renders were inspected and all their assertions did pass.
However, that runner inherited unrelated hat presets, compared preservation against
its own fresh compiler hash, and returned zero even if `Batch.check` recorded a
failure. These are corrected in the continuation: explicit synth settings,
independently pinned source identities, and a nonzero exit for failed/empty suites.
Guardrail unit tests include an intentionally failing assertion.

The expanded run uses actual Faust scalar/vector builds, rate/block/lifecycle
checks and an explicit output-gain prediction at eight resonance/cutoff settings.
The prediction is mathematics, not a replacement synthesizer or hardware oracle.
The original complete synth suite remains a separate regression, not evidence
that these voices are authentic. Candidate-only WAVs retain their actual levels.
No normalization is used to conceal the gain difference. No hardware WAVs are
repackaged.

## Required next reference experiment

Use fixed recording gain throughout, no chorus/effects, a warmed-up instrument,
documented note/gate duration, all oscillator/noise levels, filter/contour settings,
output jack, interface, sample rate, unit identity and any processing.

| Voice | First controlled captures |
|---|---|
| SH-101 | Same sustained saw note at low and high cutoff, with resonance minimum/quarter/half/three-quarter/near-self-oscillation. Measure harmonics well below cutoff separately from the resonant peak. Add a noise-source sweep and envelope/retrigger captures. |
| Juno-106 | One dry voice; saw/pulse/sub separately, all HPF positions, low/high resonance sweeps, ADSR/release. Record PWM only after the static references. |
| Model D | Oscillator 1 alone across waveforms/registers; fixed-mixer-level cutoff/emphasis sweeps; separate contour curves; then multi-oscillator mixer-drive and glide captures. |

Repeat reference settings and reserve another hit/note or, preferably, another
identified unit for validation. Do not retune to unspecified demonstration patches.
A tested executable circuit model with documented scope can be a separately
labelled intermediate oracle, not a substitute for physical-unit approval.

Owner listening, controlled reference matching, expanded voicing, CURLOP
bindings/faceplates/save-reopen and named-device tests remain distinct gates.
No merge, GUI expansion, new voice allocator or automatic legacy revoicing.
