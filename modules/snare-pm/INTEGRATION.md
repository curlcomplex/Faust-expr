# Consumer handoff — snare-pm / 0.1.0-experiment

Pin an immutable Faust-expr commit and generated artifact identity. `hybrid.dsp` is the expected product candidate only if the full-batch evidence and Felix's audition select it; `deterministic.dsp` remains an explicit architecture alternative.

Pitch is absolute Hz, qualified 60–500 Hz. Convert Tracker/MIDI pitch once at the host boundary. Apply Pitch, Sweep, Punch, Decay, Inharm, Shape, Contour, Drive and Velocity before the Gate rising edge at the same sample. Those values latch for that hit; subsequent changes affect the next hit. Gate-off does not choke the one-shot. A host needs a computed low gate before another rising edge.

The dedicated noise generator has persistent state. Do not reconstruct the DSP for every hit to manufacture reproducibility. A sleeping/retired voice must preserve or deliberately replace this behavior. Choke/voice-steal policy is product work and requires its own transition test.

There are seven non-pitch musical controls. A six-knob Tracker page must preserve reachability via page/context/lock lanes or an explicitly approved macro combination. Presets use stable IDs, not visual knob positions.

Generated AOT/interpreter code is derived from the canonical source. Re-run parity, note/lock timing, persistent-state and target realtime/memory checks in each product. Do not automatically rewrite old Tracker SNARE projects to this version.
