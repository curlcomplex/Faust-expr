# Tone PM consumer contract

Canonical identity: `tone-pm / 0.1.0-experiment` at the exact source commit named by the eventual qualified PR checkpoint.

Host note pitch maps exactly once to `pitch_hz`. Seven non-pitch controls must remain addressable by stable IDs: Ratio, Punch, Decay, Feedback, Modulation, Mod Envelope, Drive. A six-knob Tracker page must not silently discard or repurpose one; use a secondary/hidden-but-lockable lane or an explicit product mapping decision.

Write pitch, velocity and all latched controls before the same-sample rising gate. A real computed low gate is required before another onset. Note-off does not choke the internal exponential decay. The DSP instance persists across hits; onset resets oscillator phases and the feedback contribution. Do not reconstruct a DSP instance per step.

Feedback uses a one-sample recurrence and therefore needs actual target/sample-rate audition; do not claim rate-identical sound. Upper-register modulation/drive requires aliasing judgment. Preserve legacy Tracker TONE sound identity rather than silently changing old projects.

No EQ/reverb/limiter is part of the machine. Scalar/vector backend is chosen from target measurements, not a hosted-Linux label. Human approval and target-device qualification remain separate from passing offline tests.
