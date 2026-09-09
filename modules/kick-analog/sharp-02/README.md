# Analog-style kick: Sharp-family prototype

Portable module `kick-analog-sharp`, sound version `0.2.0-experiment`. Owning issue #15, consolidated implementation PR #36. This is a separate instrument from the PM kick. The preceding `../kick.dsp` version 0.1 and its clean baseline remain unchanged; they are alternative research, not silently replaced saved sounds.

## Scope and reference boundary

This prototype follows the *observable control dimensions* documented for Syntakt BD Sharp. It does not reproduce a discovered circuit, proprietary waveform tables, exact knob curves or a measured hardware sound. See `references.json` for the sources and rejected calibration leads. No qualified dry BD Sharp recordings were obtained; no optimization here was fitted to hardware audio.

The earlier `kick-analog` draft had a different generic control surface and only a cost comparison against a stripped-down sine body. This version investigates waveform/phase choice, independent pitch-sweep depth/time, amplitude hold/decay, transient and drive. It adds a same-engine, same-controls optimization comparison. Neither a document match nor a green test suite establishes an Elektron clone or human sonic approval.

## Eight musical controls

| Parameter | Authored mapping |
| --- | --- |
| Pitch | 20–160 Hz, continuous; pitch-relative sweep |
| Sweep Time | 3–150 ms pitch-envelope time constant, exponential macro mapping |
| Sweep | 0–4 octave initial rise, falling exponentially in hertz |
| Decay | 20 ms–1.8 s amplitude time constant; about 6.908 times tau to -60 dB after hold |
| Wave | Six waveform roles, each paired with reset/free oscillator phase |
| Hold | 0–2 s before amplitude decay, quadratic mapping; does not postpone pitch motion |
| Click | Short independent noise/tone excitation, squared level mapping |
| Drive | Dry-to-shaped blend, squared mapping; exactly neutral at zero |

Wave categories are sine, asymmetric sine, triangle, sinetooth, saw and square. The asymmetric and sinetooth mixtures are *our authored approximations*, not manufacturer equations. The numeric ordering and ranges are also ours. The documented square addition does not establish that these tables, transition laws or drive placement match a particular firmware.

Gate and velocity are event inputs, not extra macro knobs. A rising edge triggers the one-shot and captures amplitude-linear velocity. Note-off does not choke. Envelope, waveform, sweep and click settings are captured on onset; moving them changes the next hit instead of rewriting an existing tail. Pitch and Drive move live through a 3 ms smoother and snap to their target on onset, including same-sample locks.

## DSP and the actual optimization

One oscillator reads an independently authored 16-harmonic Fourier series. Partial contributions fade as they approach Nyquist; nonlinear drive can still create frequencies above Nyquist. The finite series itself is an approximation to ideal sharp-edged waves. There is no mandatory oversampling or claim of alias-free output.

`engine.lib` is canonical for both candidates. `reference.dsp` evaluates the series with individual sine calls. `optimized.dsp` uses a statically unrolled Clenshaw recurrence to evaluate the same series with one sine and one cosine. Both have the same envelopes, noise, drive and DC-removal path. Float32 rounding is tested explicitly. The vectorized build is another derived code-generation choice, not another sound design.

The name `optimized.dsp` identifies an optimization *candidate*, not a guarantee of speed. Measure scalar reference, scalar Clenshaw and vector Clenshaw under the same workload. The first paired measurement found scalar Clenshaw slightly slower for 64-sample blocks, despite gains at larger blocks. A later three-build benchmark rotates execution order and records distributions; select a target backend from that target's results rather than a file name.

No expensive circuit solver is justified without captures that distinguish it from simpler behaviour. Faust virtual-analog/wave-digital libraries remain options for a later measured need; a generic ladder or tanh function does not by itself constitute analog-circuit fidelity.

## Verification and auditions

Run from the repository root with Faust, C++17, NumPy and SciPy:

```sh
python3 -m unittest discover -s tests -p test_sharp_contract.py -v
python3 tools/modules/sharp_qualification.py --out build/analog-complete
```

The complete suite generates and compiles actual Faust for direct, Clenshaw and vector variants. It checks all waveform/phase categories at 44.1/48/96 kHz, clean pitch, persistent state, note-off semantics, velocity, held controls, same-sample locks, event segmentation, and selected rapid gestures. One persistent trajectory visits 1,536 endpoint settings; that is not 1,536 independent captures or exhaustive continuous-space verification. Arithmetic/manifest unit fixtures are separate from DSP renders and hardware evidence.

Sample-rate comparison uses equal onset times and a filtered polyphase downsample, not `high_rate[::2]`. The residual also contains integration/phase/filter differences, so it is a diagnostic rather than an isolated alias-energy measurement. Aggressive nonlinear waveforms remain a release-quality question.

Audio files: `analog-sharp-anchors.wav` (16 s, Round/Sharp/Triangle/Rubber/Driven/Long with full and softer hits); `analog-sharp-pattern.wav` (9 s, one voice with locks/accents); `analog-sharp-controls.wav` (25 s, eight stepped control traversals). Fixed kernel gain and PCM16 conversion only. No per-hit normalization, EQ, reverb or limiter. Patches are authored, not hardware presets.

The report identifies source, generated code, scores and raw audio by hash. The ordinary `new` allocation hook covers tested compute calls; it is not a malloc/aligned-allocation or whole-host real-time audit. DSP instance size and compiler stack-usage files are preserved. Offline four-voice timing is not iPhone/MCU acceptance. CI uploads expire, so retain the separately backed-up evidence package.

## Replay and integration boundary

With saved evidence, repeat the full suite using unchanged generated C++ without Faust:

```sh
python3 tools/modules/sharp_qualification.py --replay /path/to/evidence --out build/replayed-analog
```

`INTEGRATION.md` defines the consumer contract. Tracker implementation/merge is explicitly delegated to its repository. This PR does not alter Tracker, its pending PM port, credentials, privacy, runner registration or release settings. Human audition, measured BD Sharp fidelity and minimum-device qualification remain separate gates; nothing merges automatically.
