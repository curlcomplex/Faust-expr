# TX81Z / OPZ native-law candidate

Owning issue **#97**, implementation **PR #127**, strategy #111. This remains
TX81Z-only work until it produces useful FM architecture/reuse evidence.
`0.5.0-opz-laws` is a new research sound identity, not a replacement for v1–v4.

## What this establishes

The previous v4 demonstrated eight connections around continuous analytic
waveforms and one shared ADSR. This candidate adds **independent operator
attenuation envelopes, key-rate scaling, logarithmic waveform/output tables,
ratio/fine/DT1/DT2 frequency laws, the pinned fixed-mode accumulator, and the
two-sample feedback path**. All synthesis and state live in Faust. The C++
adapter executes the unchanged native `ymfm` engine as a reference; Python only
prepares tables/scores, measures outputs, resamples and packages recordings.

Reference: `aaronsgiles/ymfm@81aec25ccbb98f4873a255f7551ac4dadac59b4a`,
`src/ymfm_opz.cpp`, `src/ymfm_fm.ipp`, and `src/ymfm_fm.h`. BSD-3-Clause
attribution is retained in `../YMFM-LICENSE.txt` and the standalone export.

The oracle is the **actual four-operator FM core before its guessed DAC path**,
not a tagged routing fixture or a second Faust rendition. Tests compare every
sample without delay alignment, gain fitting, tolerance fitting or normalization.
The integer phase/envelope/state probes require exact equality; the whole-voice
limit is 1e-7 to allow only final floating-point velocity/output multiplication.

## FM reuse findings

| Boundary | Evidence and disposition |
| --- | --- |
| Topology | The v4 `in3/in2/in1/carriers` coefficients are reused **unchanged**. Operator evaluation stays local to this machine. |
| Operator bus units | OPZ emits a signed roughly 14-bit sample and uses `bus >> 1` as a 1024-point phase offset. A unit-normalized operator magnitude would imply about **four cycles**, not the old continuous voice's one-cycle convention. A compiled wrong-unit mutation is rejected against native audio. |
| Frequency controls | OPZ uses key-table lookup, additive key-dependent DT1, coarse DT2 offsets and an x.4 multiplier with integer truncation. This is not the bundled DX7 operator's 0–31 coarse / 0–99 fine/log-frequency mapping. Keep parameter translators machine-specific. |
| Envelope and level | OPZ uses 10-bit attenuation, rate tables, an EG tick every three native samples, AR/D1R/D2R/SL/RR and KSR. The bundled DX7 has four rates/four levels and Q24-domain output. Share gate/state-contract ideas, not one unqualified envelope implementation. |
| Feedback | This OPZ implementation uses two previous operator samples. The delay and numeric bus scale must be part of an interface, not silently assumed equivalent merely because both engines call it FM. |
| Host contract and evidence | `gate`, Hz input and normalized velocity; one voice per instance; existing renderer and evidence structures are reused. No universal FM module catalogue or host voice allocator is introduced. |

The DX7 observations above are from the unchanged operator/env files in the
verified Faust 2.88 release. This is a boundary decision backed by the second
architecture, **not a claim that a shared implementation was extracted and both
whole instruments requalified**.

## Fixed-frequency counterexample — acceptance remains open

The pinned native `compute_phase_step` labels its assembled fixed register
number as Hz but accumulates only `75 * number / 4096` phase units per tick.
The real operator consumes a **10.10** phase accumulator. At 55,930 ticks/s,
its resulting frequency is therefore

`55930 * 75 * number / (4096 * 1048576)`.

For register number **8192**, that predicts **8.00085 Hz**, not 8192 Hz.
The suite renders an isolated native/Faust carrier and measures its full-spectrum
peak to retain this counterexample. It does not repair the reference, multiply
the candidate frequency secretly, or mark physical fixed-mode behavior accepted.
The existing register-only frequency probe could not reveal this because it
checked mean phase steps without checking their actual audio units.

The fixed-mode musical file is labelled **oracle study**, not an authentic
TX81Z fixed-frequency preset. Resolving the reference scaling against documented
panel/firmware semantics and hardware is still part of #97.

## Clock and host-rate boundary

The parity clock is **55,930 Hz**, the upstream integer `sample_rate(3579545)`
result for this setup, not a measured TX81Z crystal clock. Whole-native parity
is tested at that cadence only. At 44.1/48/96 kHz the candidate adapts phase and
EG cadence; it is **not a resampled chip emulator**. Feedback remains two host
samples, so its physical delay changes. Rate tests establish candidate pitch,
finite output and scalar/vector consistency, not cross-rate timbral equality.

Hz input is bounded to **20–4400** and quantized to the nearest native
1/64-semitone position. `blockFreq=-1` uses Hz; nonnegative values directly
expose the raw research key register. The native register range saturates.
Native-style waveforms and high ratio/feedback corners are not alias-free.
Velocity is an external linear, onset-latched output factor, **not** the full
TX81Z velocity-to-operator-level/panel mapping. Public OP1 is the final serial
carrier, retaining v4's convention; this is not a SysEx operator-number promise.

LFO/AM, noise, EG shift, guessed channel volume/DAC behavior, firmware/panel/SysEx
translation and physical hardware verification remain outside this candidate.
Reverb here means the native **envelope tail-rate stage**, not an audio effect.
Control changes and monophonic retriggers can click; no smoothing or limiter is
inserted to hide reference behavior. CURLOP still owns polyphony and GUI.

## Build and evidence

Use the complete pinned Faust 2.88 release and unchanged reviewed-main renderer
from `82926f023410ae8367eec3c5d842edbbe34ab439`:

```sh
python3 tools/modules/tx81z_laws_qualification.py \
  --out build/tx81z/native-laws-results \
  --faust /path/to/faust-2.88.0 --libraries /path/to/faust/libraries \
  --archive /path/to/faust-2.88.0.tar.gz \
  --renderer /path/to/reviewed/render.cpp --ymfm /path/to/pinned/ymfm
```

`tx81z_prepare.py` generates the small lookup library from hash-verified native
source. The generated library is deliberately not hand-maintained in Git;
the evidence contains both its exact bytes and hash. Ready multi-file source,
standalone `TX81Z_OPZ.dsp`, original program definitions and full sources/logs/
scores/raw outputs accompany results. Compile the single-file export with only
standard Faust libraries. The suite verifies its controls and sample identity.

The new build compiles the written source directly. Faust `-e` expands its
nested state expressions into approximately **518 MiB** before reducing them
back to approximately **56 KiB** of generated C++; it is not an appropriate
source-delivery format here. The scoped export preserves the original
expressions rather than shipping that expansion. Earlier v2/v4 build routes
and all original source files remain unchanged and are separately requalified.

Five original musical programs and a live-release example compare native first,
Faust second with a 250-ms gap. Both halves use the same 55,930→48,000-Hz
polyphase conversion and fixed playback gain **0.8**. Raw native-rate evidence
is retained separately. No effects, limiter, per-example normalization or factory
samples. Every PCM sample is checked after writing the WAV container.

Memory reports distinguish the per-instance object from shared generated lookup
arrays. Call `classInit` once off the audio thread before concurrent rendering,
then initialize instances without rewriting shared tables. Ordinary generated
`init` must not be called concurrently with existing renders. Offline compute
measurements are not device callback/thermal qualification.
