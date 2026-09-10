# Perc consumer handoff

Tracker issue: curlcomplex/curlop-tracker#43. Shared owning issue/PR: Faust-expr#18 / #43. Module `perc-pm / 0.1.0-experiment`, entry point `perc.dsp`. Consume a verified immutable upstream source pin from the PR; generated C++/interpreter artifacts are derived, not another hand-authored synth.

## Host behavior

Convert note pitch once to Hz. Kernel range is 35–1800 Hz; outside-range policy must be explicit, never an undocumented octave wrap. Eight musical controls include Pitch, leaving seven after keyboard pitch: Sweep, Punch, Decay, Inharmonicity, Modulation, Mod Envelope, Drive. Preserve all seven through a deliberate page/context/lock-lane choice rather than dropping one to fit six knobs. Stable IDs from the manifest back locks/presets.

Write all note parameters and linear velocity before the same-sample gate rising edge. All values latch at onset. Note-off does not choke. A computed low gate is required between onsets; two writes without intervening compute are not a trigger pair. Oscillator and feedback onset state restarts; noise and DC-filter histories remain persistent. Do not construct a new DSP for every hit to conceal lifecycle errors.

Zero Drive does not guarantee a wholly linear attack: Punch deliberately supplies separate early nonlinear emphasis and a small synthesized tonal/noise impact. The body remains free of a sustained noise/sample/reverb layer. Do not silently reinterpret that mapping when adapting the UI.

## Registration and compatibility

**Do not assume Tracker already has a PERC slot.** Choose explicit new registration or an owner-approved replacement/version policy after inspecting the consumer. Preserve all existing machine IDs and old project playback. No automatic re-numbering, old-project substitution or legacy deletion is authorized. The earlier generic handoff phrase 'legacy PERC' did not establish that such a slot exists.

## Before merge

Verify source/mapping/generated-content identity, control units/defaults/reachability, exact note conversion, AOT/interpreter parity as applicable, sample-rate/device changes, onset/velocity/retrigger/persistent-state behavior and saved project identities. Audition the actual UI and parameter locks, then qualify memory/callback/first-paint/thermal behavior on the minimum supported device.

Use scalar as a starting point, not a universal target-performance assertion. Highly driven upper-register behavior is rate-sensitive and needs an explicit quality decision. The raw adjacent-sample jumps do not certify click-free stealing/choking. No kernel choke event is currently defined; host fade/voice allocation is separate.

Faust-expr owns the instrument; Tracker owns product integration, private tests and merge. No consumer source, credentials, repository visibility or runner setup was changed by this module delivery.
