# Synthetic PM snare

Canonical shared module `snare-pm / 0.1.0-experiment`, owned by issue #16 and draft PR #39. This is an independently authored electronic snare, not a repair of Tracker's old sound or a recovered Elektron algorithm. Host integration belongs to the consumer repository; Tracker handoff is curlcomplex/curlop-tracker#40.

## Musical surface

| Control | Authored behavior |
| --- | --- |
| Pitch | Absolute body frequency, 60–500 Hz; a host converts its note representation once |
| Sweep | Initial rise from zero to 2.5 octaves, with a decaying frequency contour |
| Punch | Pitch-drop speed, attack timing, short crack and early body emphasis |
| Decay | Exponential body duration; the noise envelope follows through Contour |
| Inharm | Operator-frequency relationships and secondary-body/noise balance |
| Shape | PM amount; in the hybrid, coordinated noise amount and spectral balance |
| Contour | Modulation-envelope and noise-tail timing, from short articulation to longer texture |
| Drive | Nonlinear blend with neutral zero; not an output-level control |

All musical controls and velocity latch at onset. Knob movements during a tail affect the **next hit**, not that tail. Gate-off does not choke the one-shot. A repeated trigger needs an actually computed low gate before its next rising edge. These are explicit versioned semantics, not assertions about Elektron's hidden control laws.

Shape and Contour are multi-parameter paths through the engine, not aliases hiding unrelated parameters. Seven non-pitch controls remain once the host keyboard supplies Pitch. A six-knob layout must preserve access deliberately, not silently remove one.

## Candidate identities and corrected ablation

`hybrid.dsp` is the provisional product-audition candidate: inharmonic PM body, short crack and a dedicated filtered-noise tail. `deterministic.dsp` is the preserved **legacy filename** for the drier alternate. That alternate is **not noise-free**: its crack contains pseudo-noise, and it scales the crack by 0.55 compared with the hybrid's 1.0. Do not describe the original A/B as changing only the noise tail.

`tail-off.dsp` supplies the controlled comparison: it evaluates the hybrid graph with only `noiseSignal` removed, leaving the body, full crack, shaping and output stage unchanged. It is a diagnostic ablation, not a newly selected product voice. At zero tail amount it agrees with the hybrid in the executed regression.

`tonal-diagnostic.dsp` suppresses all pseudo-noise, including the crack's noise, solely to support a meaningful deterministic sample-rate comparison. It is not the shipped sound and cannot qualify the real noise path by itself.

Three fixed noise-filter states are blended rather than changing IIR coefficients at a lock boundary. Pseudo-noise state advances while the instance is processed. Recreating the DSP on each hit changes its behavior and is not an acceptable way to manufacture reproducibility. Zero Drive is neutral; the output uses fixed gain and DC removal, without a hidden limiter.

## Reference evidence

The inspiration is Model:Cycles' focused electronic-machine approach, with separately identified Syntakt SD Basic/Vintage comparison recordings. The equations and macro curves are ours. The eight predeclared CC BY 4.0 public previews are SDB-01/04/07/10 and SDV-01/05/09/13 from Winston Edwards / Particles Into Waves' *Syntakt Designer Drums*. Original preview bytes, decoded audio, attribution and conversion metadata accompany the evidence, not ordinary module playback.

The recordings are lossy and have unknown firmware/settings/gain and per-hit drive. They support exploratory spectral/temporal descriptor coverage, not exact knob fitting, architecture recovery or a fidelity percentage. The fixed 48-setting comparison is reused diagnostic data, **not held-out validation**. The controlled tail-off comparison retains the original descriptor scaling; adding a candidate does not move the measuring scale.

The hybrid covers the four Basic examples better under this diagnostic; the drier candidate remains better on several Vintage examples. The isolated-tail experiment preserves that result, but it does not establish perceptual superiority. Felix's musical approval remains outstanding.

## Execute and inspect

With Faust, a C++17 compiler, Python, NumPy/SciPy and FFmpeg:

```sh
python3 -m unittest discover -s tests -p '*snare*' -v
python3 tools/modules/fetch_snare_references.py --out build/snare-references
python3 tools/modules/snare_delivery.py --references build/snare-references --out build/snare-delivery
```

Reference acquisition is an intentional separate network step, not a DSP runtime dependency. The delivered artifact contains source snapshots and frozen references. From its `source/` directory, a compiler and NumPy/SciPy can replay **unchanged generated C++**, without Faust or network access:

```sh
python3 tools/modules/snare_delivery.py --replay .. --out ../../snare-replay
```

The replay verifies source, reference and generated-header hashes before compiling. This is not a second Faust compilation. Do not modify the frozen source snapshot and then claim it reproduces the recorded build.

[Qualification](QUALIFICATION.md) records the test scope, controlled comparison, performance tradeoff and limitations. [Integration](INTEGRATION.md) records host semantics and versioning. Issues/PRs own the current work state, not this document.
