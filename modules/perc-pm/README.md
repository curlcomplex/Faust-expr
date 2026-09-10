# Perc PM — tuned and inharmonic struck percussion

Owning issue [#18](https://github.com/curlcomplex/Faust-expr/issues/18), draft PR [#43](https://github.com/curlcomplex/Faust-expr/pull/43). Consumer handoff: curlcomplex/curlop-tracker#43. Canonical identity: **perc-pm / 0.1.0-experiment**. This is a working, independently authored prototype, not a recovered Elektron algorithm or a released host feature.

## Instrument

The small PM network has three weighted audible tonal paths, a bounded feedback modulator and a short synthesized impact. There is no sample playback, snare-like noise tail or reverb layer. Model:Cycles Perc is the conceptual family; Syntakt PC Carbon provides published control roles and separately identified reference recordings. The ratios and macro curves are our own.

| Stable ID | Musical behavior |
| --- | --- |
| pitch_hz | 35–1800 Hz base tuning; continuous and microtonal |
| sweep | Coupled pitch-rise depth and fall time; 0–3 octaves and 4–79 ms time constant |
| punch | Faster attack, short tonal/noise impact, and early nonlinear emphasis |
| decay | Body exponential time constant, 18 ms–1.53 s; not total audible duration |
| inharmonicity | Stretches the relative oscillator frequencies |
| modulation | PM depth and internal feedback, with finer control near zero |
| mod_envelope | Persistent coloration at zero toward a short initial modulation burst at one |
| drive | Additional nonlinear shaping; Punch can still add transient shaping at zero Drive |

Gate and velocity are separate event inputs. All musical controls and velocity latch at onset: moving a knob during a tail changes the next hit, not the ringing sound. Note-off does not choke. A real computed low gate is required before another rising edge. Oscillator/feedback onset state resets; the pseudo-noise and DC-filter state remain persistent. Recreating the DSP for every hit is not equivalent.

These policies favor predictable per-step sound changes; continuous modulation of ringing notes is not a supported claim. A phase-reset retrigger can interrupt a tail, so finite/headroom tests are not proof of click-free voice stealing.

## What is retained

`perc.dsp` and the eight authored patches are unchanged from the initial verified source `71d088cd9ff37c879677606893a6f377e31ea0df`. The delivery pass improves measurement, replay, source provenance and event checks rather than silently revoicing the instrument. `additive-diagnostic.dsp` removes PM/feedback while preserving the rest of the engine; `envelope-diagnostic.dsp` exposes the actual amplitude calculation. Neither is a new shipping machine.

## Build and replay

With an installed Faust compiler, C++17 compiler, Python, NumPy and SciPy:

```sh
python3 -m unittest discover -s tests -p test_perc_delivery.py -v
python3 tools/modules/perc_delivery.py --out build/perc-delivery --references PATH_TO_FROZEN_REFERENCES
```

Reference acquisition is a separate intentional operation (`fetch_perc_references.py`, requiring ffmpeg/ffprobe and network); normal synthesis never downloads audio. The delivery bundle includes the frozen references and supports `--replay PATH_TO_GOLDEN` to verify hashes and compile its unchanged generated C++ without Faust or network access. That is independent native C++ replay, not a second Faust compilation.

See [QUALIFICATION.md](QUALIFICATION.md) for actual evidence and limits, [REFERENCES.md](REFERENCES.md) for provenance, and [INTEGRATION.md](INTEGRATION.md) for consumer requirements. The PR carries the latest exact-commit run and artifact identifiers. Musical approval and minimum-device realtime/thermal qualification remain separate gates; no automatic merge.
