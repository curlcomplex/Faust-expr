# Analog Hats v0.1 — Mutant-led six-control electro-hat candidate

Canonical entry: `modules/hats-analog/v1/hats.dsp`, identity `hats-analog / 0.1.0-experiment`.

## Reference decision

Primary design/reference lead: **Hexinverter / Erica Synths Mutant Hi-Hats**. It is a better fit for this six-column machine than a plain 808/909 recreation because it deliberately expands the electro-hat architecture: open and closed hats, analogue metallic source, separate drive behaviour, resonant filtering, alternative source possibilities and OFF/FADE/EXCLUSIVE interaction modes. This project does not claim recovered circuitry or a hardware clone.

Reference pages:
- Hexinverter product information: https://www.hexinverter.net/mutant-hihats
- Erica Synths legacy product page: https://www.ericasynths.lv/hexinverter-mutant-hi-hats/
- AMAZONA review with dedicated choke/filter/external-source demos: https://www.amazona.de/test-hexinverter-mutant-bd9-snare-hihats/

The review's direct MP3 URLs are recorded in the workflow manifest. At the current hosted GitHub runner they return an anti-bot/HTML response rather than valid MP3 bytes, so no numerical reference-distance claim is made. The synth qualification does not depend on silently substituting another recording.

## Six controls

1. **Metal** — blend from noise-led hats toward the six-oscillator metallic source.
2. **Tone** — coordinated high-pass corner and resonant high-band focus.
3. **Decay** — useful coordinated CH/OH length range. Unlike the hardware lead, closed decay is deliberately variable because a six-column sequencer benefits much more from per-step length control than from reproducing a fixed hardware limitation.
4. **Shape** — alters oscillator-frequency spread, summed-versus-ringed metallic topology and the bright transient contribution. It is intended to move between familiar electro-hat and more glassy/inharmonic territory rather than being another EQ knob.
5. **Choke** — continuous shared-state open→closed interaction: effectively OFF through a musical fade toward hard/exclusive choke.
6. **Drive** — post-envelope saturation for clean through broken/acidic hats.

Performance inputs are `pitch_hz` (a 0.60–1.70 metallic-bank ratio), `articulation` (0 CH / 1 OH), velocity and gate. They are not counted as six synthesis columns. Decay remains column 3.

## Architecture

Six free-running square oscillators use deliberately inharmonic electro-hat ratios around 205, 304, 370, 523, 541 and 800 Hz before the performance ratio. Shape perturbs alternate frequencies and crossfades a simple summed bank toward pairwise ring combinations. Metal crossfades that source with noise. Tone drives high-pass and resonant-band filtering. CH and OH have separate onset clocks and envelope laws, while sharing the source/filter state.

A closed onset does **not** merely steal the host voice. It arms an explicit attenuation law over the still-running open envelope. Choke therefore has audible OFF, fade and hard/exclusive regions. A fresh open onset clears stale choke state.

## Current qualification

Hosted run `34637270955` at `a514091199eb3ede4976f487635e909f1423c25e` passed **67 actual renders / 82 checks** using the repository's native Faust/C++ renderer.

Default 48 kHz descriptors:
- closed t90 ≈ **29.98 ms**, spectral centroid ≈ **10.38 kHz**;
- open t90 ≈ **469.58 ms**, spectral centroid ≈ **10.11 kHz**.

The OH→CH diagnostic measured post-choke energy of **12.316 / 0.897 / 0.466** for Choke OFF / mid-fade / hard respectively. This verifies the ordering and shared-state effect; it is not a psychoacoustic quality score.

Every non-choke synthesis knob is required to make a >5% measured spectral/temporal/level change from the default probe. The actual probes passed: Metal 18.6%, Tone 30.5%, Decay 224%, Shape 38.1%, Drive 33.6% by the largest normalized diagnostic dimension. Choke has its separate interaction test.

Qualification covers scalar/vector Faust builds, exact scalar/vector parity in the default render, block sizes 1/32/64/127/128/256/512, linear velocity, open-vs-closed duration, eight authored CH/OH presets at 44.1/48/96 kHz, and the choke ordering. It is offline Linux evidence, not named-device realtime/thermal qualification.

## Audition bank

`Analog_Hats_8_presets.wav` contains eight two-second blocks:

**Classic / Soft / Crunch / Glass / Dust / LowMetal / Acid / Long**.

Each block plays **CH, CH, OH, then OH→CH** so both articulation and choke can be judged. One global output gain is applied; there is no EQ, reverb, compressor or per-hit normalization.

## Acceptance boundary

This is an audition candidate, not accepted merely because CI passes. Felix's listening approval is still required. If the musical result is weak, preserve this branch and replace the synthesis architecture rather than tuning tests around it. Tracker integration and target-device performance remain later consumer work.
