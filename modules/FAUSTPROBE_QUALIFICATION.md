# Optional second-runner comparison (#130)

The normal Faust compiler and the experimental `faust-rs`/Cranelift runner now
have an executable comparison path. It is **additional diagnostic evidence**, not
a replacement compiler or a new dependency of the native instrument workflows.
No instrument DSP, accepted sound, normalisation, or release criterion changes.

## Run it

Use the verified complete Faust 2.88.0 release and the existing #108 command:

```sh
python3 tools/modules/harmonic_qualification.py --out build/comparison \
  --faust /path/to/faust-2.88.0/build/bin/faust \
  --faust-libraries /path/to/faust-2.88.0/libraries \
  --faust-archive /path/to/faust-2.88.0.tar.gz \
  --faustprobe /path/to/faustprobe \
  --faustprobe-revision fbe0e282343cca2980642aa6dc81e2e9681bdc65
```

Build the external runner from that exact upstream revision with
`cargo build --locked --release --jobs 2 -p cranelift-ffi --bin faustprobe`.
The dedicated `faustprobe-qualification.yml` workflow does this on GitHub-hosted
Ubuntu; it does not install software or dispatch work on the owner's machine.
Its queue is independent of owner-machine work.

The workflow records the checked-out source/tree, clean tracked-file state,
Cargo.lock hash, actual Rust/Cargo versions, build command, executable hash and
version, repository commit and run ID. Rust/Cargo and the hosted operating-system
image are recorded, not claimed to be immutable pins. Dependencies are locked.
The source-to-binary record is checked before qualification, but is not a signed
supply-chain attestation. The ordinary optional CLI still records its revision as
caller-declared; do not mistake a string alone for a verified build record.

## What is compared

Three stationary mono cases at 48 kHz: an eight-partial oscillator with Nyquist
removal, the same oscillator without that removal, and a sine-driven cubic stage.
Native and external executions use matching fixture sources, verified libraries,
controls, excitation bytes and time windows. Each starts fresh and renders 65536
frames; the first 32768 are startup, the remaining 32768 are measured.

Both use double internal arithmetic. External CSV is converted to float32 to
match the native raw output; this quantisation is explicit. No resampling,
normalisation, phase alignment or waveform fitting is performed. The exact same
#108 harmonic reducer processes both outputs. Do not compare the upstream built-in
SFDR directly: its window and harmonic exclusions have different semantics.

The tests check the original samples (maximum difference <= 2e-6 for these
fixtures), analytical harmonic/fold power (existing #108 tolerance 2e-5), saved
CSV/report integrity and repeated execution at blocks 1, 127, 256 and 511. These
are controlled fixture tests, **not universal equivalence or musical thresholds**.
Below-floor dB differences remain diagnostic; a generic tight dB comparison of
numerical noise is not used as the qualification decision.

## Failure and evidence contract

Raw stdout, stderr and a versioned execution record are retained before parsing,
including nonzero exits, malformed output and timeouts. Stale execution success
cannot survive a new failed attempt. Native score hashes and parameter values
are checked before comparison. Reports include source/input/library/score hashes,
configuration, sample residuals and separately named metric differences.

A requested optional comparison error is explicit and nonzero; it is never
silently skipped. The native qualification report is retained with its own result
and a separate `optional_faustprobe.status = failed`, so an unavailable external
runner does not erase completed native evidence. Native-only commands still work
without Rust or faustprobe. Investigate disagreements rather than automatically
assuming either compiler is the reference truth.

Primary upstream source and CLI contract:
https://github.com/grame-cncm/faust-rs/tree/fbe0e282343cca2980642aa6dc81e2e9681bdc65
and `docs/faustprobe-user-guide-en.md` in that revision. Upstream explicitly labels
this compiler port experimental. Passing our selected fixtures is not a claim
that it supports every instrument or every Faust feature.
