# Analog Classics synth candidates — 0.1.1-experiment

Owner: Faust-expr #62 / draft PR #65; consumers CURLOP #274/#275 under #271.
The preflight revision at 9e24c6c remains in history. Not a factory release,
hardware match or CURLOP integration.

## Three single-note instruments

- `mono-101`: saw/pulse/PWM/sub/noise, idealized four-stage OTA-style filter,
  shared amp/filter ADSR, LFO pitch/filter/PWM and incoming-slide-controlled glide.
- `juno-106`: DCO-style mix/PWM, cleaner four-stage filter gain staging, separate
  HPF and ADSR. No per-note chorus, detuned note bank or internal polyphony.
  Continuous waveform levels and HPF are prototype extensions, not original
  hardware control claims.
- `minimoog`: three oscillator components making ONE received note (saw/saw/
  square with tunings), fixed-headroom mixer saturation, separate filter decay,
  library ladder, output saturation and incoming-slide-controlled glide.
  The ladder is linear internally. Nonlinear mixer/output stages are not a
  calibrated transistor circuit. Wave selectors, osc3-as-LFO, hardware contours,
  keytracking and overload matching remain refinements.

The previous preflight used a generic two-pole filter for both Roland-inspired
voices. This revision uses an algebraically resolved four-one-pole cascade.
This corrects filter order/topology, not hardware fidelity. The independent
bilinear transfer-function check validates mathematics, not IR3109 calibration.
No Hera/Faug code is copied. Faug remains research, not a matched synth oracle.

## Contract

Canonical UI labels: `gate`, `freq` (20–8000 Hz), `velocity` (0–1). Host owns
allocation, priority, overlap, stealing and sleeping. Gate rising triggers ADSR
and samples velocity. New pitch during a held gate does not retrigger. A fresh
onset requires an actual low sample then rise or a fresh host-allocated voice.
Frequency follows while gated and holds at gate-off; timbre remains live in tails.

Mono101/Mini: `slide` alone enables glide on already-gated notes; `glideTime=0`
and ordinary notes hit the target directly. Juno has no portamento. Mini tuning
controls are oscillator-component tuning, not a note bank. Oscillator safety
clamps are explicit; extreme Mini tuning/high notes can reach them. No alias-free
claim. Timbre uses 5ms sample-rate-aware smoothing, cutoff/HPF in log Hz. Gate,
pitch and slide are not generically smoothed. Envelope times have nonzero floors.
Control/default/preset maps in `tools/modules/synth_batch.py` are asserted against
actual Faust UI capture. These laboratory controls are not a CURLOP faceplate.

## Run and evidence

`python3 tools/modules/synth_batch.py --out build/hats-v2/synth-batch`

Existing `tools/modules/render.cpp`, real scalar/vector Faust C++, diagnostic and
signal wrappers. Ordinary/legato/slide/zero-glide, velocity/silence/retrigger/
release, live-tail automation, corners, FFT tuning, 44.1/48/96k, block sizes and
vector residuals. Actual Bassline Seq routing to the mono voices, four separately
rendered Juno instances, and an actual delayed-gate source negative control.

Auditions use raw Faust audio: no EQ/reverb/compression/limiter or per-preset
normalization. The four-note chord sum has a fixed 0.25 gain; it is offline
adapter polyphony, NOT CURLOP. Common-phrase order Mono101/Juno/Mini, 8 seconds
each; presets 8 seconds each. Scores, raw outputs and hashes accompany the WAVs.

Generation/native compilation/compute times are separately reported, not device
qualification. High-register 48k/downsampled-96k residuals conflate phase/filter/
alias differences; they are NOT pure alias metrics or an alias-free pass.

Pending: controlled hardware captures/hold-outs, sonic approval, faithful
oscillator/filter/envelope/HPF/overload behavior, actual CURLOP binding/faceplate/
save-reopen and named-device callback/thermal qualification. No automatic merge
or promotion; retain sound/source identities during refinement.
