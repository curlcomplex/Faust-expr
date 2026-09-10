# Consumer handoff: Tone 0.2 playable

Owner issue #19 / draft PR #44; Tracker record curlcomplex/curlop-tracker#44. Canonical source stays in Faust-expr. Use the exact qualified commit in the final PR checkpoint and entry **modules/tone-pm/playable/tone.dsp**, sound identity **tone-pm / 0.2.0-experiment**. Parent tone.dsp is intentionally the older 0.1 and must not be confused with this graph.

## Controls and musical timing

Convert note to absolute pitch_hz exactly once (20–8000 Hz numerical domain; upper-register nonlinear quality still needs policy/audition). Ratio is continuous .25*32^x. Exact landmarks .25/.5/1/2/4/8 correspond to normalized 0/.2/.4/.6/.8/1; inverse is log(r/.25)/log(32). Display the physical ratio accurately. Do not add undisclosed snapping or quantize microtonal pitch.

Seven non-pitch musical controls: Ratio, Punch, Decay, Feedback, Modulation, Mod Envelope and Drive. Preserve stable IDs and reachability even on a six-knob page. `gate_mode` is a separate articulation toggle, not a silently discarded ninth parameter.

- Live: pitch_hz, ratio, feedback, modulation, drive. 3 ms sample-rate smoothing during a tail, exact requested-value snap on onset.
- Latched: punch, decay, mod_env, gate_mode and velocity. Changing these in a tail affects its next onset.
- Gate rising edge resets oscillator/feedback/envelope contribution; all controls must be written BEFORE computing that sample. Compute at least one actual low-gate sample between distinct rising edges.
- gate_mode=0: one-shot, note-off ignored. gate_mode=1: hold until note-off, then decay from the actual boundary amplitude using Decay. One-sample notes are supported. The modulation envelope continues independently.
- Pitch change without a new gate edge is legato. Host owns overlapping-note priority, note stealing and voice allocation; this mono kernel does not infer MIDI semantics or create chords.

Reuse persistent instances. Do not reset on each score block or freeze filter/phase histories during arbitrary silence. No off-thread zone mutation while compute is running. The startup filter guard is required to keep first-onset locks equivalent to preselected patches. Voice-steal crossfades are a host responsibility; ordinary hard retriggers are not globally click-free.

## Sound compatibility and deployment

0.2 changes Punch neutrality, modulation-envelope mapping, articulation/live controls and DC/envelope ordering. It is NOT sound-compatible with 0.1 or old Tracker TONE. Preserve old project identities or explicitly preview a migration. Never silently substitute the new path for all legacy projects. Source/mapping and generated content must be pinned together; generated AOT/interpreter code is derived, not independently authored.

Numerical scalar/vector parity is tested, but compiler speedups vary by target. The hosted benchmark favors vector here, unlike several drum modules; do not generalize that to iPhone without measurement. Five diagnostic graphs in the evidence are not product machines.

## Acceptance before product merge

Verify canonical-versus-adapter renders, AOT/interpreter behavior, both gate modes, held notes, ratio landmarks, all controls and live/latched distinction, same-sample locks, legato, polyphonic allocation, rapid retriggers, sample-rate/device changes and reload. Do not add EQ/reverb/normalization to reproduce the demo. Check extreme high-register feedback/drive carefully: significant rate sensitivity is recorded, not resolved. Profile actual minimum-device memory, callback and thermal behavior. Offline green tests do not provide Felix's musical approval or authorize a merge.
