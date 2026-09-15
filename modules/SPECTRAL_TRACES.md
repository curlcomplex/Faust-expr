# Time-varying filter-bank diagnostics (#107)

This additive offline pass measures existing, original float32 renders. It does
not compile an instrument, normalize or resample its audio, or replace any FFT,
multiresolution, hardware-reference score or musical release threshold.

## Run

```sh
python3 tools/modules/spectral_trace.py path/to/original.f32 \
  --rate 48000 --channels 1 --stride 240 --out build/spectral/example \
  --faust /path/to/faust-2.88.0/build/bin/faust \
  --faust-libraries /path/to/faust-2.88.0/libraries \
  --faust-archive /path/to/faust-2.88.0.tar.gz
```

Input is interleaved **little-endian float32**, with explicit actual sample rate
and channel count. Each channel is analyzed independently with fresh state. No
stereo summing or channel-layout assumption is made. Use one distinct output
folder per simultaneous invocation. Source audio and parent reports must live
outside the output folder. Dependencies: Python 3.10+, NumPy, C++17 and the pinned
Faust compiler/libraries. Neither FFT reference tools nor an audio device are
required at runtime.

The existing issue #105 streaming host is reused with an explicit five-output
compile-time contract. Its default six-output loudness ABI remains unchanged.
No instrument runner, `lab.py`, fitting adapter or benchmark wrapper is changed.

## Measurement profile and semantics

The fixed profile is read from the **same literal definitions in
`faust_spectral_descriptors.dsp` that are compiled**:

| Setting | Value |
| --- | --- |
| Band-split filter order | 3 |
| Bands per octave | 3 |
| Total bands, including top and DC | 24 |
| Highest crossover | 10000 Hz |
| Power averaging window | 20 ms rectangular |
| Flux amplitude window and comparison interval | 20 ms |
| Internal arithmetic / I/O | double / float32 |

The crossover must be below Nyquist, so this profile rejects sample rates at or
below 20000 Hz rather than reporting invalid measurements. The upper supported
rate is 192000 Hz. Every report lists the sample-rate-dependent band centers.

`faust_filterbank_centroid_hz` is a power-weighted mean of the band centers;
`faust_filterbank_spread_hz` is their power-weighted standard deviation. These
are finite-resolution filter-bank descriptors, not exact pitch or FFT-bin
measurements. Top/DC band centers and limited resolution matter, especially for
energy near or beyond the outer crossovers.

`faust_filterbank_flux` sums positive increases of rectangular-smoothed band
amplitudes over the stated delay. It is **amplitude-dependent, not normalized,
not Hz and not a rate per second**. Doubling input amplitude should double flux
and quadruple `faust_filterbank_power`, while centroid/spread remain substantially
unchanged above the denominator floor.

The averaging window uses `round(T * rate)` samples (Faust `rint`), and the flux
delay uses `int(HOP * rate)`. Both integers are recorded. Window-fill flags mark
initial zero history; they are **not** proof that IIR filters have settled. All
original frames, including the last partial storage window, are retained. No
pre-roll or end-of-file silence is invented; this IIR analyzer has no finite FIR
flush rule. Its startup or ringing can be diagnostically relevant.

Power at/below double epsilon produces denominator-guarded centroid/spread.
Consult `power_above_epsilon_last` and the active-sample count before interpreting
silent/very-low-level values. This flag only describes numerical conditioning,
not an audibility threshold or a quality gate.

## Trace and report contract

`spectral-trace.json` is the schema-2 **completion manifest**. It identifies the
input, complete library manifest, source/generated/native-binary hashes, exact
compile commands, configuration, reset/window/channel rules, and build/execution
environments. Archive verification is explicit: supplying the release archive
checks its pinned digest and compares every `.lib` to it; omitting it records
`release_archive_verified: false`, never a spurious verification claim.

`spectral-trace.jsonl` is the SHA-256-linked trace. Rows are ordered by channel,
then `start_frame`. Each row describes `[start_frame, end_frame_exclusive)` and
has the exact `last_frame` / `time_seconds` of its final observation.

Every descriptor is calculated for **every input sample**. `--stride` controls
storage only. A row stores minimum, maximum, mean and last over *all* samples in
that interval; the top-level field value is explicitly the last reading. This
retains short flux peaks that naive every-Nth-sample decimation would miss.
Use maxima for transients, and last/mean traces for gradual changes; do not
interpret a storage-window statistic as an instantaneous FFT measurement.

The completion manifest is published last after checking input/parent/binary
identities. A failed rerun removes the previous manifest. Ignore leftover raw
or JSONL files without a successful run and matching completion manifest.
`read_trace(out)` verifies trace hash and row count before returning rows.

Resource bounds: at most 600 seconds, 16 million input frames, 32 channels, and
200000 stored rows across channels. The native host streams; intermediate output
uses a disk-backed mapping. Increase stride or split large inputs rather than
removing bounds. Reusing a runner requires its matching provenance, not an
arbitrary binary plus a claim that it is the pinned analyzer.

## Existing lab reports

An optional `--lab-report path/to/results.json` links the sidecar to existing
`renders` entries **only if raw audio SHA-256, rate, frames and channels match**.
The original report bytes, metrics and pass/fail decisions remain untouched.
This integrates evidence without silently redefining an accepted reference score.
The API returns the sidecar manifest for inclusion by callers as a separately
named diagnostic; no existing batch is forced to run it.

Useful applications include clap/noise attack evolution, cymbal brightness decay,
filter/envelope tails and timbral transitions. Interpret against a matching
configuration and signal class. Do not use these descriptors as a universal
similarity verdict or inherit thresholds from existing FFT metrics.

## Qualification and execution

```sh
SPECTRAL_TRACE_INTEGRATION=1 \
FAUST=/path/to/faust-2.88.0/build/bin/faust \
FAUST_LIBRARIES=/path/to/faust-2.88.0/libraries \
FAUST_ARCHIVE=/path/to/faust-2.88.0.tar.gz \
SPECTRAL_TRACE_EVIDENCE=build/spectral-qualification \
python3 -m unittest discover -s tests -p test_spectral_trace.py -v
```

Ten fast contracts and twelve real Faust/native tests cover the original four
trend hypotheses, 44.1/48/96-kHz direction checks, block/cadence invariance, final
partial windows, channel resets, amplitude behavior, provenance/capture tampering,
invalid inputs, existing-report linkage and the public CLI from another directory.
The window reducer has an independent known-value single-sample spike test.
Tests for synthetic analyzer inputs are not claimed to be instrument renders.

The shared pinned GitHub-hosted workflow retains complete #105/#106 qualification
and their source evidence, then adds #107. Every logging pipeline fails closed
(`bash` with pipefail), including an executed deliberate-failure check. A green
badge alone is insufficient: inspect the retained test logs and actual traces.

GitHub-hosted and owner-machine queues remain independent (see `AGENTS.md` and
`EXECUTION.md`). This path defaults to hosted correctness testing, with separately
identified assistant-sandbox checks where used. It neither claims owner-machine
availability nor runs real-time hardware benchmarks. No merge is implicit.

## Primary references

- Faust analyzer API: https://faustlibraries.grame.fr/libs/analyzers/
- Pinned implementation: `libraries/analyzers.lib` and `libraries/filters.lib`
  in the complete Faust 2.88.0 release archive.
- Release: https://github.com/grame-cncm/faust/releases/tag/2.88.0
