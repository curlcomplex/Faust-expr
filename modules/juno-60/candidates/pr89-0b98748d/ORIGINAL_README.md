# Analog Classics — Juno 60 Voice v1

First single-note Faust candidate for a distinct Juno-60 instrument.

## Host contract

- one note per Faust instance
- canonical `gate`, `freq` (Hz), `velocity`
- CURLOP owns voice allocation/polyphony/stealing
- chorus is deliberately outside this kernel so a shared post-polyphony chorus can be used

## Reference boundary

Primary hardware tuning is still pending. The architectural software oracle is `jpcima/Hera`, pinned to commit `f6fe5b900f4cf84809686466e0a37de5edf008fd`. Hera's own README says the emulation is alpha and inaccurate in several aspects, so matching Hera is not equivalent to matching a Juno-60.

Hera's DCO implementation is GPL-3.0 as part of the Hera project. This module does **not** copy that DCO source. The oscillator/mixer code in `voice.dsp` is a clean-room expression of the observable Juno-family architecture using stdfaust oscillators and an independently authored gain-compression hypothesis.

`vcf.lib` is adapted from Hera's standalone `Source/VCF/JpcVCF.dsp`, which explicitly declares the ISC licence. Attribution and modification notes are retained in that file.

## First candidate architecture

- band-limited saw and pulse/PWM
- sub oscillator and noise
- level-dependent DCO mixer compensation hypothesis
- nonlinear four-pole TPT resonant VCF
- independent high-pass stage
- filter envelope amount and key tracking
- ADSR/VCA
- no internal chord stacking or chorus

## Not yet claimed

- hardware authenticity
- exact Juno-60 knob laws
- exact oscillator/mixer gain calibration
- exact HPF positions/response
- exact envelope timing curves
- exact filter/VCA nonlinearity
- Juno-60 chorus behavior

Next gate: compile/render qualification, then compare isolated oscillator/filter/envelope behavior against identified Juno-60 single-note/raw-wave reference material before selecting a classic preset.