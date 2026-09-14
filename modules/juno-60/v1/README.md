# Analog Classics Juno-60 v1

Single-note Faust reference candidate for issue #86. CURLOP owns allocation/polyphony/voice stealing. Canonical host inputs are `gate`, `freq` in Hz and `velocity`. Chorus is intentionally external to the voice.

## Reference hierarchy

1. Hardware/raw-waveform and multisample captures with identifiable provenance/settings where available.
2. `jpcima/Hera` pinned at `f6fe5b900f4cf84809686466e0a37de5edf008fd` as an architectural/software oracle only.
3. Existing Juno-106 software models as secondary structural references.

Hera explicitly describes itself as alpha and inaccurate in several areas, so it is not hardware truth. The repository is GPL-3.0 overall. This module does **not** copy Hera's GPL-covered DCO/HPF implementation; it independently implements the observed Juno-family architecture. Individual permissively licensed filter research may inform later versions with attribution if selected.

## Current architecture

- single DCO-style saw/pulse/sub/noise voice
- PWM with LFO modulation
- source-level relationships and mixer loading compensation inspired by Juno-family architecture
- stepped-style HPF behavior exposed behind a continuous host parameter
- nonlinear four-pole OTA-cascade reference candidate
- filter envelope and key tracking
- ADSR VCA
- no internal chorus and no internal chord/polyphony engine

## Status

Experimental/reference candidate only. Hardware/sample comparison, oscillator-level fitting, HPF calibration, filter/resonance fitting, envelope timing, owner listening and CURLOP/device qualification remain pending. Do not promote or rewrite existing Juno-106 projects from this module.