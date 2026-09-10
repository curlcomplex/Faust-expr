# Morph: conventional wavetable playback

This is a playback optimization of the established v2 design, not a new
instrument. Controls, tuning, stereo unison, phase, gain and envelopes stay
unchanged. V2 direct Fourier and Clenshaw remain the independent reference
implementations. Check PR #46 for the latest actually qualified source commit;
source presence is not a build or fidelity pass.

## Reproduce

With Faust, C++17, Python/NumPy/SciPy installed:

```
OPENBLAS_NUM_THREADS=1 python3 tools/modules/morph_wavetable_batch.py --out build/morph-lookup
```

This generates tables and native artifacts only. It does not install packages,
open an audio device, modify a private host or dispatch a personal runner.
`--replay PATH` recompiles the recorded unchanged generated C++ without Faust
or network access. A final report identifies source, tables, headers, scores and
raw audio. Source compilation and independent C++ replay are distinct evidence.

## Sound representation

All eight frames, clean/driven endpoints and Shape brightness law are retained.
Shape and Drive blend spectral endpoints linearly, so their generated waveform
endpoints can be blended linearly too. Morph retains its original smoothstep
mapping. This is conventional interpolated cycle playback; the audio callback
contains no FFT, table generation or per-sample harmonic-series evaluation.

The old harmonic roll-off has breakpoints at 16000/n and 19000/n Hz for every
harmonic n. The bank uses every breakpoint: 124 distinct frequency knots including
range sentinels. Linear-Hz interpolation reproduces the existing roll-off between
knots mathematically, apart from rounding and phase interpolation. A coarse
logarithmic band bank was faster but changed high-register spectra too much.

A small 1-Hz index table plus at most two exact boundary comparisons locates the
band. The generator verifies that at most two knots lie in any such bin; tests
check exact and neighboring boundaries. The initial recursively expanded binary
search timed out in the Faust compiler and is not the accepted implementation.

Cycles are 128–1024 samples according to harmonic content and deduplicated.
Only a half-cycle plus its midpoint is stored; the other half is reconstructed
using the exact odd symmetry of a sine series. Mixed even/odd harmonics obey this
reflection too. Four-point cubic interpolation reads within each cycle. No
harmonics are removed relative to v2. Drive remains pre-band-limited spectral
coloration, not an added post-stack nonlinear stage.

## Shared memory and initialization

Count Shape and Drive dimensions as well as frames/bands. Do not reuse a memory
estimate that counts only eight generic waveforms. `tablebank.json` describes the
actual samples and metadata; the report inventories generated shared arrays.

Faust may keep both a read-only waveform initializer and an initialized rdtable
copy. Both are shared costs, not per-voice memory, but both count. sizeof(DSP)
does not include them. Separately generated classes/copies can duplicate banks.

**Call the generated classInit exactly once off the audio thread, before any
voices render. Use instanceInit per voice/sample-rate preparation thereafter.**
The convenience init() can refill shared arrays and must not run concurrently
with existing rendering voices. Table contents are sample-rate independent for
the supported 44.1/48/96 kHz profiles; instance coefficients are not.

All four oscillator paths still compute at Stack1. No inactive-state skipping,
control-rate reduction or fast-math is introduced. Additional active-path
specialization is separate measured work. Stereo output requires two buffers.

## Acceptance and interpretation

New fidelity cases declare max absolute sample error below 3e-4 AND relative RMS
error below -65 dB before execution. The inherited musical tests remain active:
tuning, centered stacks, zero-detune collapse, same-sample locks, live movement,
held release, persistent retriggers, endpoints and three sample rates. These are
numerical requirements, not a claim of perceptual indistinguishability.

The three-way benchmark uses direct Fourier, the previous Clenshaw and new
lookup at identical settings/compiler flags, with repeated rotating execution
order. It varies Stack1–4 and 1/4/8 host voices; no speedup threshold is allowed
to redefine correctness. Initialization time is reported separately.

The old/new audition uses the same score and fixed gain, with no compensating
EQ, alignment, per-note normalization or limiter. No new Nord matching claim.
Minimum-device CPU/memory/thermal qualification, arbitrary modulation/retrigger
quality and owner listening approval remain separate. Preserve prior project
sound identities; use the explicit v3 version and exact generated bank.
