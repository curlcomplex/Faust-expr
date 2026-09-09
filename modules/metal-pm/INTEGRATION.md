# Metal consumer handoff

Canonical owner: Faust-expr #17 / PR #41. Working identity `metal-pm / 0.2.0-experiment`.
No automatic merge, Tracker code change or legacy METAL replacement is authorized.

## Consume, do not rewrite

Pin the exact tested source commit, `engine.lib`, manifest and chosen entry point.
`metal.dsp` is the dense/readout candidate with recurrent amplitude evaluation;
`reference.dsp` preserves the same sound with explicit amplitude exponentials;
`sparse.dsp` is a musical architecture alternate, not a cheaper equivalent voice.
`no-feedback.dsp` and `envelopes.dsp` are diagnostics, not additional released machines.
Generated AOT or interpreter artifacts are derived and must retain source/build
identity. Neither the word "reference" nor a green build establishes hardware fidelity.

## Control/event wiring

Pitch is Hz, 70–3000 in the current contract. Convert host note/scale pitch exactly
once. This is a base carrier frequency, not necessarily the loudest spectral peak.
Do not clamp or octave-fold a wider host range silently; explicitly choose/test any
extension and identify it as an adapter or sound-version change.

Seven non-pitch controls remain: Decay, Color (A tuning), Shape, Sweep (B tuning),
Contour, Punch, Drive. In this machine Sweep is NOT a decaying pitch envelope.
Preserve all seven controls and stable IDs in a six-knob host through an explicit
page/context/lock-lane decision. Do not hide an unrelated mandatory parameter or
silently alias a displayed knob to a different function.

All musical controls and velocity latch at onset. Apply pitch/macros/velocity before
rendering that same onset sample. Gate-off does not truncate the one-shot. Between
triggers the gate needs at least one computed low sample. Do not create a new DSP
instance per hit to conceal stale state or make samples match.

Choke is a separate rising-edge event, not a saved decay macro or note-length mode.
It fades output over eight milliseconds using a smoothstep. Releasing the input
does not resurrect the old tail. A new trigger reopens the voice. On simultaneous
onset and choke, onset wins. Host choke groups can target another voice, but their
routing and group membership belong to the host, not this single-voice kernel.

The fixed DC filter and oscillator feedback keep persistent state while processing.
A normal onset resets oscillator phases and amplitude but does not clear the fixed
DC filter. The kernel itself does not suspend processing. A sleeping/retired-voice
optimization must preserve reset/choke semantics and prove equivalent onset behavior.

## Qualification before product merge

Compare canonical versus adapter audio with the identical sample-exact score;
qualify AOT/interpreter differences explicitly. Check all parameter IDs, defaults,
range policy, velocity and gate/choke ordering. Exercise rapid locks and voice
replacement in a real pattern. Old project identity must remain resolvable; this
new module is not a silent substitution for the old Tracker METAL slot.

Measure the chosen compiler/backend on the minimum target device with realistic
concurrent voices, effects, graph transitions and thermal load. Hosted offline
p50/p99 timings and an ordinary-new hook do not certify phone callbacks, every
allocator or private application lifecycle. Keep the scalar arithmetic reference
available when an optimized path fails a sonic tolerance or performs worse.

This prototype's feedback and nonlinear upper-register sound remain rate-sensitive.
That is logged evidence, not permission to claim alias-free operation or reproduce
an unknown Elektron clocking implementation. Felix's listening decision is separate
from numerical acceptance. The first delivered presets are descriptive audition
examples, not eight matched hardware patches.
