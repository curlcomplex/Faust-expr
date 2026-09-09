# Consumer handoff — kick-analog-sharp / 0.2.0-experiment

Canonical source is `engine.lib` plus the explicitly selected direct/Clenshaw entry point, `manifest.json` and patch definitions. The reference implementation and old 0.1 engine remain available. Do not silently identify their different parameter sets as the same sound version.

## Required adapter behavior

The kernel has zero audio inputs and one audio output. The host chooses its stereo/track routing explicitly. Pitch is absolute Hz, including fractional values; the currently qualified range is 20–160 Hz. Convert MIDI/semitone pitch once at the adapter boundary. Do not clamp an entire keyboard to this range or transpose silently and still claim pitch equivalence.

Apply parameter and velocity events before the gate onset at the same sample offset. A real computed low gate is needed before another rising edge. Continuous high gates are not repeated triggers. Note-off is ignored musically by the one-shot; explicit choke/voice-steal behavior belongs to a separately tested host policy. Do not clear the instance for every rendered hit just to make tests pass.

Seven non-pitch musical controls remain after keyboard pitch: sweep time, sweep amount, decay, waveform, hold, click and drive. A six-knob host must deliberately expose the seventh rather than hiding an essential parameter or silently combining unrelated controls. No Tracker lane layout is changed by this module PR. Stable IDs, categorical wave values and macro units are in the manifest; do not store presets by visual knob position alone.

The six waveforms each have reset/free phase choices. Free phase only means free phase if its oscillator advances while output is silent. A host that suspends the DSP cannot simply resume stale phase and claim equivalent timing. Keep it running, or implement and verify an equivalent state-advance policy. Reset mode may admit a different idle policy, but must still preserve event-edge, filter and tail state.

Envelope/wave/sweep/click/velocity controls latch at onset. Pitch and drive are live, with internal smoothing and exact onset snapping. A host must not add another mandatory control smoother that smears locks. Monophonic retriggers can truncate a tail; no universal click-free transition or audio-rate CV promise is made.

## Building and sound identity

Pin an immutable Faust-expr commit and include the generated header checksum, all source/mapping files, compiler version, library identity and flags in the consumer package. Generated C++ is derived code, not another editable synth. No copied handwritten DSP variant should evolve in Tracker. Production generation must not silently inherit fast-math settings absent from this study.

The tested flags are single precision, standard C++17 O2 with contraction disabled, and optionally Faust vectorization `-vec -lv 0 -vs 32`. Scalar and vector are separate measured candidates. The Linux winner is not automatically the fastest ARM/iOS backend. Regenerate against a pinned library/compiler and run parity before selecting production flags. Full transitive dependency-lock tooling remains shared-library infrastructure rather than something this README implements.

Do not mutate old projects' module identity. Import as a separate experimental instrument/version, or make any intentional incompatible migration explicit in the Tracker review. The previous PM-kick consumer port and its unmerged branch are independently owned by the Tracker repository; this analog batch does not finish, merge or repair that port.

## Acceptance still owned by each product

Test adapter-vs-canonical output, note/lock timing, instance lifecycle, first paint, memory, real callback latency and thermals on the minimum supported target. Only ordinary C++ new/new[] was instrumented in the offline compute benchmark; other allocation families and the whole host need their own checks. A numerical peak/finite pass does not qualify clipping protection for a complete mix. Keep app limiting separate from dry fidelity tests.

The supplied auditions are authored exploration material. Felix's musical approval, hardware-reference calibration and target-device acceptance are not automatic consequences of an uploaded package or a green CI badge.
