# TX81Z / OPZ continuation — issue #97, PR #127

## This slice

v1 is retained byte-for-byte as historical draft source. It had two defects:
`ba.select2` is invalid Faust selector syntax, and waves 6/7 duplicated 4/5.
v2 uses the `select2` primitive, rectifies the doubled first-half sine for
waves 6/7, and factors the common sine evaluation before selection. The old
nested alternatives caused heavy compiler expression expansion through four
operators. No official Faust library or renderer is edited.

The comparison called `v1` is NOT a previously qualified recording: it is
an executable control with v1's equations, repaired syntax and equivalent
selector factoring. The runner checks all eight factored-control phase grids
against the syntax-repaired original definitions and checks exact preservation
of v2 waves 0–5. Original and generated control sources remain in the evidence.

## Reproduce

Run `tools/modules/tx81z_wave_qualification.py --help`. Supply the complete
Faust 2.88.0 compiler/libraries/release archive, the unmodified renderer from
main `82926f023410ae8367eec3c5d842edbbe34ab439`, and a clean `ymfm` checkout at
`81aec25ccbb98f4873a255f7551ac4dadac59b4a`. The existing TX81Z 2.88 workflow
executes this on GitHub-hosted Ubuntu; it does not dispatch the owner's Mac.
Output directories must be new. Full logs and failed reports are retained.
An omitted `--ymfm` explicitly records `not_run`; hosted qualification requires
that reference execution. A supplemental sandbox run is not hosted evidence.

The C++ reference adapter invokes unchanged `opz_registers` construction,
register decoding and operator caching, then uses upstream logarithmic-to-linear
conversion. It is a waveform-table reference, **not a complete YM2414 chip
renderer or a hardware TX81Z recording**. Faust is tested at the same 1024
phase indices, without aligning, normalizing or fitting the arrays. Its smooth
analytic curves are compared to quantized, approximately half-indexed log
samples. Maximum absolute tolerance 0.01 was declared before reference
execution, not fitted afterward. Duplicate-wave and gain mutants must fail.
The upstream squared-wave hypothesis itself is not independently confirmed.

Checks also cover all eight carriers and modulators, startup, release,
retrigger, velocity zero, 44.1/48/96 kHz, cold replay and blocks 1/127/256/511.
Scalar/vector comparison uses a declared maximum-sample tolerance 0.0002.
These are bounded tests, not exhaustive stability or device-performance proof.
The default repository smoke/build remains a separately identified check.

## Listening

Every listening file comes from actual compiled Faust. Python only writes
scores, measures results and concatenates rendered samples. All use fixed
playback gain 0.5; no per-example normalization, limiter, EQ or reverb is added.
The voice's existing 15-Hz DC blocker remains part of its provisional DSP.
Waveform-table measurements bypass the voice envelope and DC blocker.

1. Eight carrier wave choices, indices 0 through 7.
2. Eight modulator wave choices, indices 0 through 7.
3. Executable v1-equation control then v2, isolated wave 6, 200-ms gap.
4. Rectified bass phrase.
5. Glass sequence.
6. Hollow keys phrase.

## Still open under #97

This is a provisional serial four-operator voice with generic ADSR and floating
phase/level conventions. It is NOT a completed TX81Z emulation. All eight
algorithms, operator-specific envelopes and frequency/level laws, feedback,
fixed mode, modulation, uncertain ymfm fields, independent hardware evidence
and host/device acceptance remain separate work. Do not close #97, merge,
release or generalize these components merely because this slice passes.
