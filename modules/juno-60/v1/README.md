# Analog Classics Juno-60 v1

First clean-room single-note reference candidate for Faust-expr issue #84.

## Contract

- one note per Faust kernel
- canonical `gate`, `freq` in Hz, and `velocity`
- host owns allocation/polyphony/stealing
- chorus is intentionally outside this note kernel

## Reference hierarchy

Primary sonic approval still requires controlled Juno-60 hardware/raw-waveform references. This candidate uses `jpcima/Hera` only as an implementation oracle, pinned at commit `f6fe5b900f4cf84809686466e0a37de5edf008fd`.

Hera is GPL-3.0 overall and its README explicitly describes the emulation as alpha/inaccurate in several respects. No GPL-only Hera DCO/HPF code is copied into this candidate. Architectural observations used here are the Juno-family DCO source set (saw/pulse/sub/noise), stacked-source mixer compensation, a dedicated HPF, and a nonlinear four-pole resonant low-pass structure. Some Hera VCF source files separately declare ISC; none is copied verbatim in this first candidate.

Relevant Hera files inspected:

- `Source/HeraDCO.dsp`
- `Source/HeraHPF.dsp`
- `Source/VCF/JpcVCF.dsp`
- `Source/VCF/DangeloVCF.dsp`

## Status

Reference candidate only. It is not a validated Juno-60 emulation and must not replace the Juno-106 candidate or be promoted to the factory catalogue without hardware/sample comparison, owner listening, and CURLOP host/device qualification.
