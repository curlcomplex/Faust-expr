# PM kick — first audible laboratory slice

Owner: [#14](https://github.com/curlcomplex/Faust-expr/issues/14); useful partial foundation work for #11–#13. This branch is stacked on design PR #31; neither is implicitly merged.

## What this is

An independent actual-Faust hypothesis with a low-frequency carrier, explicit pitch/amplitude trajectories, a band-limited-partial square-like modulation source, and a triangular modulation source with optional one-sample feedback. Two candidates differ only in that feedback path: A is feedforward, B adds feedback. `feedback_mode` is an experimental ablation, not a proposed shipping knob.

This is **not a measured BD Modern reproduction**. `references.json` records the evidence gap honestly: the inspected Science Lab #18 recordings are processed multi-track performances, not a dry, firmware/parameter-controlled capture set. No hardware fitting, reference accuracy percentage or human sonic approval is claimed. Candidate A/B differences are not hardware errors or proof one is better.

The designer's published Cycles discussion motivates triangular shaping/feedback, but does not disclose BD Modern topology or scaling. Frequencies, coefficients, envelope curves and resets in this source are deliberately visible hypotheses. No old Tracker DSP, firmware, samples or proprietary tables are imported.

## First eight musical controls

Frequency in Hz, sweep, punch on/off, decay in seconds, square modulation, triangle modulation, modulation-envelope depth, and drive. Pitch and decay units are laboratory units, not an assertion of Elektron's mapping. Velocity is separately latched at onset and scales amplitude linearly for this experiment. Gate-off does not truncate the one-shot envelope.

All musical controls except velocity update at score events. No click-free live-control or retrigger guarantee is claimed. Phase/time restart on onset; feedback contribution is zeroed on that triggering sample. The engine never sleeps, avoiding frozen gate/delay state. Drive has an exact dry setting; there is no output limiter, EQ, reverb or peak normalization. High-register/extreme modulation aliasing remains unqualified.

## Execute

With Faust, a C++17 compiler, NumPy and the existing smoke-test dependencies installed:

```sh
python3 -m unittest discover -s tests -v
bash scripts/build.sh
python3 tools/modules/lab.py --out build/kick-pm-01
```

The module script expands the actual imported Faust source, generates both scalar and vector C++, compiles the same native runner, inspects the controls, renders scores and writes a fail-closed report. It does not install packages, touch a host application, open an audio device or use an AI API. `FAUST` and `CXX` may select existing toolchain executables.

`results.json` records source, expanded source, generated C++, binaries, scores and raw-render hashes. The full resolved expressions and generated headers accompany artifacts so the build can be inspected and the C++ rerun. This is recorded toolchain identity, not yet a globally enforced compiler-version lock or completed packaging SDK.

## Checks

Exact initial silence; clean sine pitch at 32.7032/52/110 Hz at 44.1/48 kHz; same-score event segmentation at 1/32/64/127/128/256/512 frames; scalar/vector tolerance; note-off independence; onset-latched linear velocity; zero-triangle ablation identity; effective nonzero-triangle ablation; sixteen authored corners; separate actual-Faust sustaining-gate and stereo-input fixtures; native rejection of unknown, duplicate, nonfinite, nonbinary, unordered and out-of-range score events.

`experiment.json` fixes the numerical oracles before execution. These are implementation checks, not auditory-fidelity limits or exhaustive parameter-space proof. The clean sine frequency estimator is explicitly not advertised as a reliable arbitrary-kick pitch tracker. Multi-resolution spectral differences include sub-bass and short attack windows, without secretly aligning away onset errors or normalizing level.

## Listening

`audition.wav`: four anchors (rounded, punchy, harmonic, aggressive), each A then B, four seconds per entry. Each entry has one full-velocity hit and a 0.6-velocity hit. `pattern-0.wav` and `pattern-1.wav` play the same locked phrase for A and B. All use identical fixed gain and PCM16 conversion only. Raw float files remain unchanged. The timeline in `results.json` identifies every anchor.

The anchors are hand-authored experimental patches. They are not identified hardware presets, fitted targets or a held-out dataset. Listen first for body, attack, excessive ringing and whether the feedback variation is worth retaining. Do not choose a topology just because it produces a larger numerical difference.

## Next discriminator

Obtain/register a small controlled dry BD Modern capture set with the required firmware/settings/routing metadata, then fit clean body and pitch trajectories before reworking the modulation network. Preserve this candidate as a falsifiable baseline. Do not expand into another instrument or declare sonic completion while this reference gap remains.
