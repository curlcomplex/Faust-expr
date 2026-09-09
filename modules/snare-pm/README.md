# Synthetic PM snare — full development batch

Owning issue #16. This is a new shared module, not a repair of Tracker's rejected snare and not an Elektron clone claim.

## Reference basis
Model:Cycles Snare remains the product-behavior inspiration: a small macro surface derived from an FM/PM engine. Elektron does not publish the Snare topology or macro curves. Syntakt SD Basic and SD Vintage supply separately identified comparator recordings and documented control roles. The Syntakt manual describes SD Basic controls including Sweep, Punch, Decay, Inharmonicity, Modulation/Feedback, Modulation Envelope and Overdrive; SD Vintage substitutes Frequency Complexity for Modulation/Feedback. Those labels constrain the musical dimensions, not the hidden equations.

Eight authored controls: Pitch, Sweep, Punch, Decay, Inharm, Shape, Contour and Drive. Gate and Velocity are events. Pitch is Hz; all musical parameters and velocity latch on onset. Note-off does not choke. `Shape` coordinates PM depth and, in the hybrid candidate, dedicated filtered-noise color/amount; `Contour` coordinates modulation decay and noise-tail duration. Zero Drive is exactly neutral.

## Architecture decision under test
`deterministic.dsp` uses only the inharmonic PM body and deterministic crack component. `hybrid.dsp` uses the identical body plus a dedicated filtered-noise tail. The licensed Syntakt references have unknown patch settings, so the batch compares broad descriptor coverage rather than pretending to identify their knob positions or waveforms. Human listening remains the final architectural gate.

Three fixed noise-filter states are crossfaded rather than changing stateful filter coefficients on a parameter lock. This deliberately avoids the large same-sample IIR state spike previously discovered in the analog-kick work. The pseudo-noise state advances continuously while the voice is processed, so repeated hits need not have sample-identical crack tails.

## Verification
The batch compiles scalar/vector actual Faust for both candidates, checks 44.1/48/96 kHz, pitch, silence, velocity, gate semantics, persistent retriggering, block segmentation, rapid locks and a 128-setting endpoint trajectory. It measures same-source scalar/vector performance instead of inventing a sound-changing optimization. Reference preview acquisition is isolated in CI and never required for ordinary module playback.

Auditions are fixed-gain PCM16 only: `snare-anchors.wav`, `snare-pattern.wav`, `snare-controls.wav`, plus a licensed-reference comparison collage generated only in evidence. No EQ, reverb, limiter or per-hit normalization.

## Consumer boundary
Faust-expr owns the sound and stable parameter IDs. A product adapter converts note pitch once, applies controls before the same-sample onset, preserves persistent state, and versions legacy project sounds rather than silently replacing them. A six-knob UI must deliberately choose how all seven non-pitch controls remain reachable; this module does not delete one for layout convenience.
