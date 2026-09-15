# Offline Faust analysis (#105)

This is an additive measurement path, not an instrument compiler migration.
The analysis DSP, native runner and Python driver live in `tools/modules/faust_analysis*`.
Existing lab metrics and instrument-generation workflows are unchanged. Analyze
original float renders, not level-matched audition files, when measuring levels.

## Run

Use the complete Faust 2.88.0 release and its matching `libraries` directory.
The dedicated hosted workflow verifies archive SHA-256
`e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9`,
builds the compiler and explicitly supplies the library include path. The CLI
requires that version and records the complete `.lib` manifest. Supplying the
archive also verifies that every library matches the release contents; without
it the report explicitly says archive verification was not performed.

```sh
python3 tools/modules/faust_analysis.py path/to/original-render.f32 \
  --rate 48000 --channels 1 --out build/analysis/my-render \
  --faust /path/to/faust-2.88.0/build/bin/faust \
  --faust-libraries /path/to/faust-2.88.0/libraries \
  --faust-archive /path/to/faust-2.88.0.tar.gz
```

The input is headerless, interleaved little-endian float32; rate and channel count
must describe the actual render. The file stays unchanged. Use a distinct output
directory outside the input path for each concurrent invocation. Dependencies:
Python 3.10+, NumPy and a C++17 compiler. SciPy/FFmpeg are qualification references,
not production analyzer dependencies. The native runner is bounded to 600 seconds,
8–192 kHz and streams its buffers rather than retaining the entire six-output trace
in RAM. Python processes up to 32 channels independently using a mono DSP instance.

## Measurement contract

- `sample_peak` and `rms` remain linear-amplitude measurements of the original frames.
- `faust_true_peak_estimate_max` is the maximum of Faust's 4-phase, 12-tap-per-phase
  FIR estimate. Eleven zero frames flush the complete FIR tail; no source frames
  are discarded. `faust_true_peak_hold_final` must equal the samplewise maximum.
  This remains an estimate, not a claim of exact reconstruction or certification.
- Momentary (400 ms) and short-term (3 s) loudness are read at the original final
  frame, before FIR padding. The DSP starts from zero; full-window flags distinguish
  short clips that still include zero-state startup. No pre-roll is invented.
- Integrated loudness is explicitly the Faust **streaming approximation**. Its
  evolving relative gate does not re-gate earlier samples as an offline two-pass
  implementation would. It is not a standards-conformance or release gate.
- Every file/channel starts with fresh DSP state. Silence uses Faust's -100 LUFS
  floor. Multichannel output is independent mono diagnostics, **not aggregate
  programme LUFS**. Do not sum per-channel integrated LUFS or infer LFE/surround
  layouts. A programme-layout meter requires a separately qualified N-channel path.
- The analyzer uses double internal arithmetic, with float32 input/output, to avoid
  float accumulator limitations on longer clips. This does not change instruments.

`analysis.json` schema 2 includes input identity, reset/window/tail/channel policies,
source/generated/binary hashes, complete library manifest, compile commands, Faust
and C++ identities, and execution environment. It does not normalize audio or add
universal musical acceptance thresholds. Keep private local paths/evidence private.

## Qualification

```sh
FAUST_ANALYSIS_INTEGRATION=1 \
FAUST=/path/to/faust-2.88.0/build/bin/faust \
FAUST_LIBRARIES=/path/to/faust-2.88.0/libraries \
FAUST_ARCHIVE=/path/to/faust-2.88.0.tar.gz \
FAUST_ANALYSIS_EVIDENCE=build/analysis-qualification \
python3 -m unittest discover -s tests -p test_faust_analysis.py -v
```

Six fast input/provenance tests do not claim DSP execution. The seven opt-in
integration tests compile real Faust/C++, check 44.1/48/96-kHz calibration,
independent-channel reset/silence, an intersample-peak fixture, final-frame impulse
and tail flushing, four block sizes, a separately implemented NumPy/SciPy FIR,
FFmpeg loudness on a stationary sine, and invalid native arguments. The FFmpeg
comparison has a stated fixture-specific tolerance, not a promise for all material.
The hosted workflow requires integration mode and also executes the public CLI.
No passing result may be claimed from skipped integration tests.

## Upgrade and execution boundaries

The existing instrument toolchain remains available and is not upgraded here.
Before any later instrument-compiler migration, explicitly compare old/new renders
and review changes; never regenerate and accept baselines silently. That migration
is not required to measure existing renders with this separate analyzer.

The default execution lane is GitHub-hosted. The owner's machine has its own queue
and is not blocked by this workflow. See `AGENTS.md` and `EXECUTION.md`. A local
sandbox is not the owner's machine and is not evidence of its queue availability.
All runs, failures and evidence must be tied to their actual immutable source SHA.

## Primary references

- Faust 2.88.0 complete release: https://github.com/grame-cncm/faust/releases/tag/2.88.0
- Analyzer API and approximation semantics: https://faustlibraries.grame.fr/libs/analyzers/
- Pinned implementation: `libraries/analyzers.lib` in the verified release archive.
- Independent FFmpeg reference: https://ffmpeg.org/ffmpeg-filters.html#ebur128
