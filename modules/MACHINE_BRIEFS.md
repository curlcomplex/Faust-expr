# Instrument and effects design map

This is the compact architecture map. The linked issues hold full experiment requirements and work status. Working IDs are neutral internal names, not final product branding. Structures below are our proposed candidates unless explicitly supported by a source in [SOURCES.md](SOURCES.md).

## Core set

| ID / issue | Musical role and first reference | Candidate emphasis | Failure to avoid |
| --- | --- | --- | --- |
| `kick-pm` / [#14](https://github.com/curlcomplex/Faust-expr/issues/14) | BD Modern; Cycles Kick comparator | Carrier, amplitude/pitch contours, PM coloration and feedback | Generic FM patch matching only one kick |
| `kick-analog` / [#15](https://github.com/curlcomplex/Faust-expr/issues/15) | BD Sharp reference lead | Oscillator/resonator, transient, contours, measured nonlinear path | Calling reduced PM an analog model |
| `snare-pm` / [#16](https://github.com/curlcomplex/Faust-expr/issues/16) | Cycles Snare; separate Syntakt comparators | Inharmonic body, attack and noise/modulation alternatives | Quiet noise pasted onto a tonal voice with no useful mapping |
| `metal-pm` / [#17](https://github.com/curlcomplex/Faust-expr/issues/17) | Cycles Metal; CY Alloy comparator | Interacting oscillators and spectral/decay transitions | Heavy cymbal physics by default, narrow whistles and level jumps |
| `perc-pm` / [#18](https://github.com/curlcomplex/Faust-expr/issues/18) | Cycles Perc; PC Carbon comparator | Pitch motion and controlled inharmonicity | Renamed kick/snare preset |
| `tone-pm` / [#19](https://github.com/curlcomplex/Faust-expr/issues/19) | Cycles Tone; SY Tone comparator | Compact operator pair, ratio, feedback and modulation envelope | Hidden primary depth control and unmeasured gate assumptions |
| `chord-wavetable` / [#20](https://github.com/curlcomplex/Faust-expr/issues/20) | Cycles Chord; SY Chord comparator | Independent tables, oscillator balance and voicing transitions | Stacking Tone presets or accidental double chord expansion |

The first sequence is #14 then #15. The other core roles do not all need to begin simultaneously. Matching a product family is not a claim that its two different hardware members use identical code.

## Complementary roles

[#21](https://github.com/curlcomplex/Faust-expr/issues/21) preserves three separate experiments: conventional electronic snare (SD Classic lead), clap (CP Vintage), and open/closed hats. Proposed structures respectively emphasize independent body/noise envelopes, clustered transient excitation plus tail, and oscillator/noise excitation with explicit choking. Activate one bounded issue/PR per experiment; none requires a general acoustic drum simulator.

[#22](https://github.com/curlcomplex/Faust-expr/issues/22) preserves dual-oscillator bass/lead and a separate ensemble voice. SY Dual VCO/SY Raw and SY Swarm are reference leads. Oscillator interactions, detuning, correlated motion and level behaviour must be measured; 'analog' and 'supersaw' labels are not complete algorithms.

## Partial percussion

[#23](https://github.com/curlcomplex/Faust-expr/issues/23) owns `partial-percussion`, inspired by Basimilus. Keep the partial frequency/envelope system, interaction modes, fold and post-fold dynamics independently testable. Treat Harm/Spread-style controls as authored multi-parameter relationships. Variant-specific reference capture and bounded folding/clocking behaviour are essential. Do not bolt this broad engine onto the focused PM kick or silently substitute Alia recordings for Alter.

## Effects

| Issue / ID | Proposed model to investigate | Required distinguishing evidence |
| --- | --- | --- |
| [#24](https://github.com/curlcomplex/Faust-expr/issues/24) / `delay-stereo` | Fractional delays, filtered feedback and stereo routing | Echo timing, colour by repeat, time-change behaviour, wet/dry gain and non-frozen tails |
| [#25](https://github.com/curlcomplex/Faust-expr/issues/25) / `reverb-stereo` | Compact diffusion/feedback-delay-network candidate | Buildup, bandwise decay, density, modulation and stereo character |
| [#26](https://github.com/curlcomplex/Faust-expr/issues/26) / `drive`, `filter` | Behavioural or justified virtual-analog/wave-digital structure | Level dependence, transfer/memory, resonance, dynamic changes and aliasing |

The choice of model is provisional. Stock Freeverb, generic tanh or a named analog filter is not an automatic match. A measured dry instrument and the processing around it must remain separable. The product safety limiter is not part of the fidelity metric and must not conceal unstable output.

## Wider catalogue

[#29](https://github.com/curlcomplex/Faust-expr/issues/29) preserves the named Syntakt reference families: additional kicks and snares, rimshots, metallic/classic hats and cymbals, cowbells, Bits/Toy/Chip, acoustic-leaning drums, noise/impulse utilities and later sample-machine leads. It is a research inventory, not a firmware-certified complete list or a commitment to build every entry. Confirm primary manuals and exact firmware when activating an entry.

LFO/modulation is retained as future work. A reusable modulation kernel, mapping interface and host scheduling are separate responsibilities. Audio-rate CV is not obtained by changing a block-rate knob more often.

## Common audition rule

Each machine must earn its identity through a useful range, not just an attractive default. Include low/high notes, velocity changes, short/long articulation, repeated hits and aggressive locks where meaningful. Match behaviour first, then explicitly identify intentional extensions. Keep parameter IDs and sound versions stable across hosts; select a new version for incompatible sonic changes.
