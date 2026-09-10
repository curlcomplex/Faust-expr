# Tone 0.2 — playable PM voice

Owning issue #19 / PR #44. This separately versioned instrument fixes two design gaps in the retained 0.1 prototype: no held-note articulation/live controls, and hidden saturation at zero Punch. It is independently authored, not recovered Elektron DSP.

## Sound and controls
Two operators with bounded modulator feedback. Pitch, Ratio, Punch, Decay, Feedback, Modulation, Mod Envelope and Drive remain the eight musical controls. Ratio is continuous .25–8; normalized .0/.2/.4/.6/.8/1 map to .25/.5/1/2/4/8. Exact inverse mapping is in the manifest. We do not invent hardware detents, quantize microtonal pitch or hide the principal PM-depth control.

Pitch, Ratio, Feedback, Modulation and Drive move live with a 3 ms smoothing time constant and exact onset snap. Punch, Decay, Mod Envelope, gate_mode and velocity latch at onset. A held gate plus changing pitch is legato; a new rising edge restarts oscillator/feedback/envelope state. Host note priority, overlap allocation and voice-steal crossfades remain host responsibilities. This is not an audio-rate CV contract.

`gate_mode=0` decays immediately and ignores note-off. `gate_mode=1` holds the attack level until note-off then decays from its actual boundary amplitude. Modulation continues evolving independently. A one-sample note-off starts release at zero elapsed time, not a sample early. Early release freezes the current attack amplitude rather than inventing a new full-level sound.

Mod Envelope now runs from sustained coloration toward an increasingly short burst returning to a clean carrier. Punch is a transient dry/wet coloration, exactly neutral at zero. Drive zero is independently neutral. The DC stage is before the final envelope; its state continues advancing. Ordinary retrigger can interrupt a tail; no global click-free claim.

## Explicit version change
The parent `tone.dsp` and its presets remain 0.1, unchanged. 0.2 changes Punch, Mod Envelope, live controls and DC/envelope order; it is NOT sound-compatible. Host projects must pin a version and entry point. The new gate articulation is a performance toggle, not a ninth macro.

## Evidence and reproduction
`python3 tools/modules/tone_playable.py --out build/tone-playable --references build/tone-references` compiles actual Faust scalar/vector and diagnostic graphs, executes musical/closed-form tests, compares the fixed licensed SYTN set, and runs a paired four-voice benchmark. The same script accepts `--replay <golden-directory>` to recompile verified generated C++ without Faust or network. Tests are independent from source string matching.

Raw DSP tests, descriptor coverage, human audition and target acceptance are distinct. Numerical thresholds are declared in source before execution. Direct exponentials avoid the rejected Metal recurrence. The full reference horizon is 10 seconds; no normalization of quiet windows or raw unknown-recording-gain term in timbre distance. Existing previews are development data, not held-out or firmware-controlled references. The older carrier-only coarse-score result stays valid as a caution, not proof that PM is unnecessary.

## Sources and limits
- Manufacturer Model:Cycles manual, section 10.5 (public mirrored original): https://www.manualslib.com/manual/1790470/Elektron-Cycles.html?page=38 . Establishes one-shot versus gate-hold articulation only, not our exact curves or SY Tone firmware behavior. Official CDN fetch was unavailable in this pass.
- Designer account: https://www.elektronauts.com/t/model-cycles-q-a-with-ess/122712/122 . Confirms PM and authored mappings; does not disclose hidden mappings.
- Creator-owned CC BY 4.0 previews: https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums . Unknown settings/gain, public lossy previews, not original lossless hardware captures. Frozen reference files/attribution belong in evidence, not in the source tree.

High-register PM/drive aliases and one-sample-feedback rate dependence remain possible; qualified sample-rate comparisons are diagnostics, not isolated alias-energy measurements. No target device, commercial release or user sonic approval is implied by a passing build.
