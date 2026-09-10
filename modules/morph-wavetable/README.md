# Morph shared instrument

The current audition and consumer candidate is **morph-wavetable / 0.2.0-experiment**, under [v2/](v2/README.md). Use `v2/morph.dsp` with its exact generated bank, not the original `morph.dsp` draft. The original draft remains preserved as experiment history; v2 corrects its detune-center and zero-detune phase behavior and adds stereo output, an expanded spectral bank and complete qualification.

This is **not a chord generator**. One host note owns a voice with optional same-note unison; Tracker's polyphony/chord locks allocate independent notes. See [prior art/audio](v2/REFERENCES.md), [consumer contract](v2/INTEGRATION.md) and the exact tested checkpoint in Faust-expr #20 / draft PR #46. Musical approval and named-device acceptance are separate from offline checks. No merge or release is implied by this directory.
