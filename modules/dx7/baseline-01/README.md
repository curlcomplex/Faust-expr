# DX7 baseline 01: H0/H1, not a finished instrument

Issues #96 / #112; strategy #111. This slice measures **unchanged upstream Faust operators** against the original Google MSFA core. It does not tune either engine, activate JP-8000, build a universal host, or promote a shared FM API.

## One command

On the existing GitHub-hosted Ubuntu queue (or a compatible prepared Linux environment):

```sh
python3 -m unittest discover -s tests -v
python3 tools/modules/dx7_baseline.py --out build/dx7-baseline
```

Requires git, Faust, a compiler supporting C++11 and C++17, NumPy and Matplotlib. The driver downloads only two public repositories at immutable commits. No firmware, factory patch bank, private checkout, personal-machine job or model/API call. Existing `faust-smoke.yml`, `Lab.build`, `render.cpp` and lab measurements are reused. Choose a fresh output directory; existing evidence is not overwritten.

Queue policy: this is deterministic GitHub-hosted work, independent of the owner's authorized local-machine queue. It is based on the preserved synth-infrastructure branch, not stacked on the separate Faust-2.88 analysis experiment. Its compiler/library identities are recorded rather than silently changing the compilation baseline of existing instruments.

## The three families

| Family | Minimal question | Cases |
| --- | --- | --- |
| C01 | Isolated carrier frequency, waveform and output-level response | Notes 45/57/69 at level 80; note 57 at levels 60/99 |
| C02 | OP2 -> OP1, ratio 2:1; effect of modulator level | Levels 50/70/90, carrier 80 |
| C03 | Same pair with separate operator envelopes and key-up/release | One articulated patch |

All run at 44100 Hz for 65536 frames, gate on 2048, gate off 32768, velocity 100. Velocity/key scaling, detune, LFO and feedback are neutral; C03 has the separately declared envelope rates/levels. No optional effects. These are diagnostic presets we author, not proprietary factory presets.

The Faust adapter calls the actual `dx.operator` functions used by `dx.algorithm(1)`, projecting only OP2 -> OP1 and omitting inactive operators. This avoids duplicate UI leaf labels in the current native renderer without changing its UI contract or taking over #106. It is **not a full six-operator algorithm/patch-import test**. The generated wrapper adds no oscillator, envelope, output attenuation or corrective DSP. It uses a relative `dx7/dx7.lib` import resolved through the pinned include directory; expanded-source library paths are checked for the expected operator/envelope files.

The wrapper declares `gate`, `freq` in Hz and normalized `velocity`, but **the compiler eliminates velocity because operator velocity sensitivity is explicitly zero**. Each built fixture's actual controls must be exactly `freq` and `gate`; the captured control report is retained. The score does not write a nonexistent control, and no artificial output-gain expression is added just to preserve that slider. These are diagnostic subgraphs, not the final host instrument contract or a test of velocity response.

The MSFA side uses its unchanged `UnpackPatch` and `Dx7Note`/`FmCore`. The authored 128-byte voice is decoded by upstream code and checked against an independently constructed 156-byte expected state on every render. A 155-byte-payload SysEx is also exported for later hardware work; the native adapter does not pretend to exercise a MIDI SysEx transport it does not implement.

## Pinned references and limitations

- Faust Libraries `271228a08981fa10b07732f0861421d1e20d4022`: https://github.com/grame-cncm/faustlibraries/tree/271228a08981fa10b07732f0861421d1e20d4022/dx7 . Per-function DX7 headers identify Apache-2.0; surrounding library components use their respective headers, including GRAME LGPL + compiled-code exception. Preserve the complete source headers/archives rather than infer a blanket license.
- Google MSFA `f67d41d313b7dc85f6fb99e79e515cc9d208cfff`: https://github.com/google/music-synthesizer-for-android/tree/f67d41d313b7dc85f6fb99e79e515cc9d208cfff/app/src/main/jni . Apache-2.0; source archive includes the original license. This is the **original scalar core**, not current Dexed's MkI engine or VDX7. No firmware execution or hardware-equivalence claim.

Both have shared lineage. Their agreement is useful differential evidence but **not two independent hardware confirmations**. VDX7, real DX7 hardware and other architectures are not run in this slice.

The original MSFA amplitude envelope increments do not compensate sample rate. Therefore initial cross-engine comparisons deliberately use 44100 Hz only. MSFA updates gains/envelopes in 64-frame quanta; the adapter preserves that cadence regardless of caller block size and rejects nonaligned gate events rather than quantizing them silently. Faust's per-sample envelope and phase behavior can differ without this being a transport error.

**Gain boundary:** MSFA raw output is the core integer sum divided by 2^24. It excludes the Android application's added filter, output pad and 16-bit clipping. The Faust library's own operator contains a 0.5 factor, also inside its modulation path. No fitted gain correction is made. A raw level difference is not a claim about physical DAC voltage. Compare output scaling, modulation spectra, envelope shape and timing separately before diagnosing or fixing DSP.

The raw residual is recorded, not used as a universal fidelity threshold. Fixed-point versus floating phase, the different envelope cadence and shared lineage are explicit. Native MSFA is compiled in C++11 with an explicit `stddef.h` preinclude and defined signed-wrap behavior (`-fwrapv`), without fast-math. Faust-generated code retains the existing lab's C++17 build. All source, compiler, expanded/generated-code and binary hashes are retained.

## Evidence and acceptance

The workflow uploads a separate commit-labelled DX7 artifact containing `REPORT.md`, `results.json`, original dependency and lab source archives, case and expected patch definitions, scores, generated Faust/C++, binaries, float32 raw/WAV pairs and an immutable file-hash index. Upstream files must remain unmodified. Existing smoke/build/module qualification still runs.

Infrastructure checks cover actual builds/renders, patch decoding, compiled control contracts, repeat renders, host block 127 versus 128, finite/nonempty audio, actual parameter sensitivity and rejection of an unsupported event offset. Negative gain/delay/mute cases exercise the actual comparison functions on rendered audio; their complete diagnostics are retained. Passing these means the measurement path works, **not that the two synth engines match**. Baseline disagreements remain diagnostic data rather than being fitted away to turn CI green.

Three audition WAVs play **MSFA first, then Faust**, with a 100 ms gap and one fixed gain of 0.2 for every file. No individual normalization, EQ, reverb or limiter. Raw float WAVs preserve values above unity. Envelope plots label the engine scaling explicitly.

## Retained bring-up failures

The original failures remain in commit/run history; none required a synthesis edit or a weaker fidelity threshold:

1. `c94d046`: original MSFA failed under C++17 because legacy global min/max collided with the newer standard-header imports, and `size_t` was undeclared. The narrow build uses C++11 and an explicit standard header instead of editing upstream DSP.
2. `e9323f5`: Faust's expanded source emitted an invalid metadata identifier derived from a hyphenated absolute import path. A pinned relative import fixes the source transport; the library expressions are unchanged.
3. `b4cc1c3`: the renderer correctly rejected a write to the optimized-out velocity slider. The diagnostic score now follows the verified compiled control contract.

No final listening/device acceptance, polyphony, complete algorithm coverage, dynamic patch editing, cross-rate fidelity, full-envelope calibration or release readiness is claimed. The next slice must be chosen from measured baseline discrepancies, not automatically expanded to a complete DX7.
