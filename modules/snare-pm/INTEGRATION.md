# Consumer handoff — snare-pm / 0.1.0-experiment

Canonical module work: Faust-expr #16 / draft PR #39. Tracker handoff: curlcomplex/curlop-tracker#40. Pin an immutable source commit, entry point, manifest and generated-artifact identity. The canonical DSP and authored mappings stay here; host adapters must not maintain their own redesigned snare.

## Entry points and sound identity

`hybrid.dsp` is the provisional product-audition default. `deterministic.dsp` is a preserved legacy filename for the drier alternate, **not** a promise that it contains no pseudo-noise. Its crack gain also differs from hybrid. Preserve the chosen entry-point identity in addition to the module sound version. `tail-off.dsp` and `tonal-diagnostic.dsp` are qualification experiments, not automatically selectable new product machines.

## Events and controls

Pitch is absolute Hz; the declared surface is 60–500 Hz. Convert Tracker/MIDI pitch once at the boundary. Behavior outside the investigated range requires an explicit adapter/qualification decision, not a silent clamp or octave shift. Apply Pitch, Sweep, Punch, Decay, Inharm, Shape, Contour, Drive and Velocity before the Gate rising edge at the same sample. All those values latch for that hit; subsequent changes affect the next hit. Note-off does not choke the one-shot. A computed low gate is required before another rising edge.

The pseudo-noise and filter states persist. Reconstructing the DSP per hit changes its articulation, and sleeping it changes its free-running noise state. Choke/voice-steal behavior is product work requiring a transition test. No graph rebuild, allocation or compilation is needed to apply musical controls to a prepared instance.

Seven non-pitch controls remain. A six-knob Tracker page must preserve reachability through an explicit page/context/lock-lane or approved macro design; do not silently discard a control. Presets use stable parameter IDs rather than visual positions. LFO/automation values can feed the next onset, but this version does not advertise continuous audio-rate modulation of its latched controls.

## Build and compatibility

AOT/interpreter code is derived. Record source/imported-library/compiler/flag/target identities, and compare the consumer rendering against canonical scores. Keep both scalar/vector possibilities: same-source optimization measurements reverse ordering between tested machines, so target profiling decides. Do not enable fast-math or rewrite filters without numerical and listening checks.

Preserve old Tracker SNARE project identity instead of replacing it silently. New projects can opt into the new explicit instrument version. No automatic merge or release is authorized by the research PR. Tracker owns its private build, integration, device test and merge when that environment is available; the shared-library batch does not claim to have run them.
