# Compiler and numerical diagnostics (#109)

Additive, offline developer diagnostics. These do not change an instrument, the
normal renderer, an accepted reference score, a sonic gate or a performance result.
Issue #109 under #104 owns status; this document owns the measurement contract.

## Run on a reviewed source

```sh
python3 tools/modules/compiler_diagnostics.py module.dsp \
  --faust /path/to/faust-2.88.0/build/bin/faust \
  --faust-libraries /path/to/faust-2.88.0/libraries \
  --faust-archive /path/to/faust-2.88.0.tar.gz \
  --interp-tracer /path/to/trace-capable/interp-tracer \
  --structural --out build/diagnostics/module
```

Requires Python 3.10+ on a POSIX host. NumPy is only needed by the native-output
qualification test, not this driver. Every invocation uses an isolated run folder.
The output directory must not contain the source or overlap the library tree.
Concurrent writers to the same output directory are rejected. Use distinct outputs
for independent jobs. The upstream tracer can write `*rc` files after an exception;
those stay inside the run folder, not beside the source or in a developer worktree.

The CLI returns **0** for completed requested checks without hard findings, **2**
for findings, incomplete execution or invalid requests. A report with runtime
`not_requested` is only a compiler check. Failed reruns cannot leave a stale success
manifest. Inspect `diagnostics.json`, the referenced raw logs and command records.

## Pinning and evidence

The driver requires exactly Faust 2.88.0. It records compiler and tracer binary
hashes, the tracer's actual runtime libfaust version, the entire `.lib` manifest,
resolved compiler dependencies and generated-code hash, commands, exit codes,
stdout/stderr, selected environment and execution origin. With `--faust-archive`,
the complete release archive is hash-verified and every `.lib` is compared against
its content; without it, archive verification explicitly remains false.

Pinned archive SHA-256:
`e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9`.
The CI build records static-library, upstream tracer-source and binary hashes too.
A binary hash/version alone does not attest who built it: consult the retained
build/source evidence. Source and resolved dependency hashes are rechecked after
execution. Local/private paths in diagnostic evidence must remain private.

Logs are captured even on process failure, with 60-second compiler and 15-second
tracer timeouts and an 8-MiB output limit. An OS failure, missing statistics or
interruption is a failed/incomplete check, not fabricated evidence of a numerical
fault. This is resource bounding for trusted code, not a security sandbox.

## What the passes mean

**Compiler:** `-wall -me -single -scal -lang cpp -flist`. Invalid constant math can
already be rejected by Faust without `-me`; do not claim this option creates all
constant checking. `-wall -me` also exposes interval/domain warnings about runtime
expressions. Interval analysis may overapproximate: warnings remain advisory until
a specific correctness impact is established. The generated code is diagnostic
output, not an automatically published instrument build.

**Runtime:** upstream `interp-tracer -trace 4 -noui` in the pure interpreter backend.
This backend sees intermediate numerical operations, even when subsequent clamps
make the final native output finite. It is not native C++ performance evidence.
The wrapper examines positive counters and fault banners, **not only return codes**:
upstream catches some exceptions and returns zero. Zero-valued counter labels are
not findings. Missing/incomplete statistics cannot establish a clean result.

Hard numerical findings: floating NaN/infinity, real/integer division by zero,
invalid float-to-int casts and recognized/unknown interpreter fault traces such
as load/store failures. Compiler errors, crashes, timeouts and incomplete coverage
also fail the requested diagnostic, but remain distinguishable from numerical
findings. Subnormals, integer-overflow warnings (including intentional PRNG wrapping)
and negative-shift warnings are retained as advisories rather than universal release
gates. No signal is normalized, repaired, clamped or rewritten by this policy.

**Input coverage:** effects use the upstream `-input` profile: fresh clones receive
an impulse and Faust noise, 1000 blocks of 16 frames each at 44.1 kHz. Generators use
`-control`: upstream endpoint/zero checks then 10 random-control blocks. The actual
profile is recorded; `--tracer-profile input|controls` can select it explicitly.
The matching `FAUST_LIB_PATH` is supplied because the tracer's internal test-signal
compilations do not forward the caller's `-I` arguments. This avoids dependence on
a globally installed, possibly different Faust library tree.

These are bounded diagnostic stimuli, not exhaustive coverage of every signal,
parameter, sample rate or backend. The control profile retains upstream ordering,
including its random-control implementation; do not claim platform-independent
random schedules or coverage of all control combinations. A stopped fault case
need not complete the later input phases to establish that fault.

**Structure:** `--structural` checks compiler support, then requests an actual
`SS_SIG` line using **`-lang ocpp -sig` only**. The report verifies emitted output and
keeps the signature separate from the normal `cpp` diagnostic pass. These static
counts/bounds are not measured CPU cycles, latency or throughput. Unsupported or
failed structure requests remain visible; no instrument backend is switched.

## Critical build distinction

In Faust 2.88.0, `build/backends/interp.cmake` selects `INTERP_COMP_BACKEND` rather
than the trace-capable `INTERP_BACKEND`. Its `INTERP_COMP_BUILD` path uses TRACE=0;
a binary can print “Using interpreter backend” while collecting no diagnostics.
The workflow explicitly sets `INTERP_BACKEND=STATIC`, `INTERP_COMP_BACKEND=OFF`,
checks `-DINTERP_BUILD` in compiler flags, and qualifies the resulting executable
with both safe and deliberately faulty DSPs. A help/version banner is insufficient.
The upstream source is used unchanged; this is a build configuration, not a fork.

## Qualification and prior research

```sh
COMPILER_DIAGNOSTICS_INTEGRATION=1 \
FAUST=/path/to/faust FAUST_LIBRARIES=/path/to/libraries \
FAUST_ARCHIVE=/path/to/faust-2.88.0.tar.gz INTERP_TRACER=/path/to/interp-tracer \
COMPILER_DIAGNOSTICS_EVIDENCE=build/diagnostic-tests \
python3 -m unittest discover -s tests -p test_compiler_diagnostics.py -v
```

Fast tests use explicitly synthetic parser input and real bounded subprocesses.
The opt-in tests compile/run actual safe effects/generators, compiler-only domain
errors, runtime divisions, runtime square-root errors and feedback overflow. The
hidden-division case is also run through the existing native host: all final output
samples must be finite while the interpreter reports a real internal fault. CLI
exit status, isolated side effects, repeatability, actual `ocpp` signature output
and input immutability are checked. Skipped integrations never count as passes.

Do not repeat the existing scheduler/vector/JIT benchmark studies here. Prior work:
[`research/libfaust-llvm-scheduler.md` on `research/scheduler-vs-tune`](https://github.com/curlcomplex/Faust-expr/blob/research/scheduler-vs-tune/research/libfaust-llvm-scheduler.md).
Its environment-specific measurements remain separate from this structural output.

Execution defaults to GitHub-hosted; the owner's machine has its own independent
queue. This workflow neither uses it nor infers that it is available. See
`AGENTS.md` and `EXECUTION.md`. No runner/permission/billing change is authorized.

Primary references: [Faust debugging](https://faustdoc.grame.fr/manual/debugging/),
[compiler options](https://faustdoc.grame.fr/manual/options/), and the exact 2.88.0
release's `tools/benchmark/interp-tracer.cpp`, `compiler/generator/interpreter/`
implementation and `build/backends/interp.cmake`. Runtime policy is established
against those pinned sources and executed fixtures, not a moving help page.
