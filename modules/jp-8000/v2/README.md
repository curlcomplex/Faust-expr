# Supersaw wide-setting auditions — #100 / PR #126

v1 remains unchanged. v2 is a measured-law candidate, not a bit-accurate JP-8000 recreation. It uses the frequency offsets and center/side gains published by Adam Szabo, *How to Emulate the Super Saw* (2010), printed pp. 9/11/15. URL: https://www.adamszabo.com/internet/adam_szabo_how_to_emulate_the_super_saw.pdf

The 17 measured detune points are interpolated monotonically (0,7,15,...127). This deliberately avoids the large float cancellation of the 11th-order polynomial; intermediate points are an approximation. The six offsets are relative frequency offsets, not semitone distances. MIX uses the complete published polynomials, including the side intercept. Signed 24-bit phase output is wrapped before both feedback and observation. A two-pole pitch-tracked high-pass and reproducible per-instance free-running phases are candidate choices, not verified original firmware behavior. No overflow is introduced at the final voice mix to invent crunch.

`tools/modules/jp8000_wide_auditions.py` builds actual v1/v2 Faust, using `tools/modules/render.cpp` from reviewed main `82926f023410ae8367eec3c5d842edbbe34ab439`. It runs only on macOS ARM64 through the existing trusted machine queue. It makes held wide notes, four-note chords, maximum-setting register comparisons, upper detune/mix sweeps and repeated notes, retaining raw audio, scores, hashes and same-gain listening WAVs. No Python oscillator model is used. The previous chat's Python-generated previews are not validation or current-build audio.

Run inside the existing Mac queue with separate clean checkouts for this PR and the pinned main lab:

```
python3 tools/modules/jp8000_wide_auditions.py --lab-root /path/to/pinned-main --out /path/to/new-evidence --faust /path/to/faust-2.88.0
```

Checks: finite/non-silent real output, 44.1/48/96 kHz, block 64/257, scalar/vector, cold replay, startup/release, and seven distinct fundamental peaks at maximum settings. These tests establish implementation properties; they do not establish physical JP equality. A/B is explicitly previous Faust then v2, not a hardware recording. Every user delivery must link the newly rendered listening files. Execution results belong in #100/PR #126; this source document alone does not mean tests ran.
