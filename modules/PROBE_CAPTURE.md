# Probe-aware native rendering (#106)

This extends the existing `tools/modules/render.cpp` and an opt-in `probe_lab.ProbeLab` subclass of `lab.Lab`. It does not
rewrite existing diagnostic audio-output fixtures, retune instruments, or change
frozen exports. The #105 offline loudness analyzer is a separate tool.

## UI and control identity

`render --controls` keeps the original TSV shape (`io`, then name/min/max/default/
boolean) and the original labels/order for unique, score-safe writable controls.
Bargraphs never appear in this writable contract. Duplicate labels in different
groups are supported; their selectors are full paths. An ambiguous bare name is
rejected, not resolved arbitrarily. Metadata on a meter sharing a control label
cannot turn that meter into a writable control or disturb the control's alias.

`render --ui-json` adds a schema-1 catalog with `controls`, `meters`, `groups`,
`probe_count`, and the diagnostic-build status. Descriptors retain human labels,
widget kinds, paths and metadata, including `probe`, `hidden`, `unit`, and custom
fields. A meter has no parameter default. Group metadata is preserved as well.

Paths join UTF-8 percent-encoded components with `/`; spaces, slash, percent and
other unsafe bytes are encoded. `/Left/gain` and `/Right/gain` are separate
controls. `A/B` and `A%2FB` cannot collide. Full paths are always accepted for
writable controls, even when the legacy short alias is available. Duplicate score
events are detected after alias resolution. Duplicate full paths, duplicate probe
IDs and writable/read-only pointer aliasing fail explicitly.

## Diagnostic build boundary

Normal rendering rejects active `[probe:ID]` metadata. A CLI flag alone cannot
turn a normal binary into a diagnostic binary. Build an isolated diagnostic
executable with `-DFAUST_EXPR_DIAGNOSTIC=1` and acknowledge it at runtime with
`--diagnostic`. Diagnostic binaries always report `mode: diagnostic`,
`benchmark_eligible: false` and `instrumented_compute_ns: null`, including when
capture is off. They cannot quietly populate a clean performance timing column.

`ProbeLab.build(..., diagnostic=True)` writes to `diagnostic-builds/`, records mode
and source/generated/native/UI hashes, and rejects vector diagnostic builds.
`ProbeLab.build()` rejects active probes on its clean path. The fitting adapters also
reject tagged probes and diagnostic binaries (`-6`); clean fitting remains intact.
The existing script-export qualification uses the normal runner and therefore
cannot successfully qualify a probed binary as a clean export. This is a guard
within this lab's paths, not a sandbox for arbitrary external Faust compilers.
Existing ordinary output meters without `[probe:ID]` are not assumed diagnostic.

`benchmark_eligible: true` means the clean runner's timing is eligible for the
existing lab pipeline, not target-device qualification or musical approval.

## Capture through the Lab extension

```python
from pathlib import Path
from probe_lab import ProbeLab

lab = ProbeLab(Path("build/probe-study").resolve())
exe = lab.build("stage-probes", Path("tests/fixtures/probes/enabled.dsp").resolve(),
                diagnostic=True)
audio = lab.render("diagnostic", exe, controls=False, input_audio=stimulus,
                   seconds=len(stimulus) / 48000, diagnostic=True, probe_stride=64)
```

Set `FAUST` and `FAUST_LIBRARIES` to the matching pinned Faust 2.88.0 bundle.
Diagnostic audio, score, input and JSONL are kept in `diagnostic-renders/`. The
render record includes capture path/hash, binary hash, header and completion
record. Use unique names/output directories for concurrent calls. `controls=False`
in the example avoids the kick defaults belonging to this original Lab class;
explicit score events may address the fixture's `gain` or full control paths.

Direct native invocation preserves the old positional arguments:

```sh
render SCORE.tsv AUDIO.f32 48000 128 48000 0 INPUT.f32 \
  --diagnostic --probes CAPTURE.jsonl --probe-stride 64
```

Input is omitted for a zero-input generator. Input/score/audio/capture paths must
differ, including symlink/hardlink aliases. All meter writes are rejected.

## Sampling contract (not block snapshots)

Capture executes the scalar diagnostic graph with `compute(1)` and reads every
meter after every sample. Controls scheduled at frame N are applied before sample
N. Requested host block sizes do not determine capture cadence. This is a slow,
deliberate offline diagnostic path, not a proposed audio-engine scheduling change.

A JSONL header carries schema, rate, zero-based frame origin, requested block,
effective compute size 1, stride and full UI catalog. Every following `window`
records `[frame_begin, frame_end_exclusive)`, `last_frame`, `last_time_seconds`,
and a `values` array in exactly the header's meter order. Each entry has `min`,
`max`, `last` over **all per-sample meter readings in the window**. Thus a stride
reduces disk output without missing a one-sample raw-value probe transient. The
last partial window is written. Stride 1 gives one reading per source frame.

These are the probe's own outputs: a peak envelope may decay; a slew probe is
smoothed; an RMS meter has its own window. A window maximum of a smoothed meter
is not an exact peak of the original signal. UI ranges never clamp capture values.
No filter-tail padding or smoothing is secretly added by the collector.

Each run initializes fresh DSP state. Nonfinite meter readings fail even with
finite audio. A final `complete` record is written only after successful audio
output. Consumers must require successful process exit **and** matching completion,
not accept a partial JSONL file. Bounds: existing 60-second renderer, 8–96 kHz,
1–8192 requested block, 1–96000 capture stride, and 20 million meter observations.
Stride does not reduce per-sample compute work. No performance numbers are emitted
for this path.

## Qualification and execution lane

The shared pinned `.github/workflows/faust-analysis-288.yml` builds Faust once,
runs the existing #105 tests/CLI, and then runs:

```sh
FAUST_PROBE_INTEGRATION=1 \
FAUST=/path/to/faust-2.88.0/build/bin/faust \
FAUST_LIBRARIES=/path/to/faust-2.88.0/libraries \
FAUST_ARCHIVE=/path/to/faust-2.88.0.tar.gz \
FAUST_PROBE_EVIDENCE=build/probe-evidence \
python3 -m unittest discover -s tests -p test_render_probes.py -v
```

Seven native collector tests use a labelled stub, not claimed DSP execution.
The opt-in integration tests compile real `debug.lib` fixtures, compare enabled /
`DEBUG=0` / removed audio, exercise duplicate labels and multiband derived IDs,
check sample/event/window semantics and block invariance, reject meter writes and
benchmark contamination, validate Lab evidence, and check clean/diagnostic fit ABIs.
The initial draft fixture is retained and executed; its four source-substring checks
are replaced by the native and real-DSP tests. The initial distribution-Faust probe
workflow is consolidated into the pinned workflow to avoid unavailable `debug.lib`
and duplicate compiler builds. The ordinary smoke workflow continues to test the existing instruments on its
unchanged distribution toolchain. Skipped opt-in tests are not passes.

Default lane: **GitHub-hosted**, independent of the owner's machine queue. Local
sandbox runs must be labelled separately from both. Preserve exact source SHA,
compiler/library identity, generated/native hashes, logs and capture metadata.
No owner-machine jobs, runner configuration, automatic merge or baseline migration
are authorized by this change. See `AGENTS.md` and `EXECUTION.md`.

## Primary references

- https://faustlibraries.grame.fr/libs/debug/
- `libraries/debug.lib` from the SHA-verified Faust 2.88.0 complete release.
- https://github.com/grame-cncm/faust/releases/tag/2.88.0
