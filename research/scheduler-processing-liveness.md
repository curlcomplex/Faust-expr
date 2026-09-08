# Scheduler processing liveness — 2026-09-07

## Decision and scope

**The observed processing hang is now localized to an invalid task ID returned by the scheduler queue. A targeted atomic queue-publication candidate eliminated that failure in the completed test matrix. This is not yet realtime/performance or Curlop production acceptance.**

The important architectural result is that this investigation did not require abandoning source authoring, whole-graph Faust, LLVM bitcode, or Faust-generated task partitioning. The intervention is in the supplied scheduler runtime's shared queue, on top of the previously documented ARM/linkage and graceful-shutdown adaptations. No libfaust compiler source or production Curlop code was changed.

This continues the **existing standalone scheduler/bitcode probe**, not a new graph corpus or an imported Curlop renderer. The DSP is extracted verbatim from `scripts/scheduler_bitcode_probe.cpp`; the host is derived from `scripts/lifecycle_diag.cpp`. Host changes are a configurable processing-block count and a progress marker every 128 blocks. Native C++ retains the documented removal of the redundant generated default destructor; the actual scheduler-cleanup destructor remains.

Files added for this investigation:

- `scripts/run_scheduler_liveness.py`: task progress, actual worker-creation checks, original-process hang capture, source/cache/native controls.
- `scripts/scheduler_liveness_queue_controls.py`: applies one selected queue-publication change to that same matrix.
- `.github/workflows/scheduler-liveness.yml` and `.github/workflows/scheduler-queue-liveness.yml`: isolated macOS diagnostics and evidence retention.

## 1. Failure reproduced and localized

The first instrumented run [34138746599](https://github.com/curlcomplex/Faust-expr/actions/runs/34138746599) produced:

| Runtime variant | Child processes | Clean exits | Invalid-task traps | Processing timeouts |
|---|---:|---:|---:|---:|
| Atomic graceful shutdown + runtime trace | 7 | 3 | 4 | 0 |
| Same + earlier block-completion barrier | 5 | 2 | 3 | 0 |
| Atomic graceful shutdown, no runtime trace/watchdog | 5 | 2 | 0 | 3 |

The trapped value is **`-1` returned by `getNextTask()`**, identified as `api=3` in the diagnostic. It occurs in source-JIT, fresh-process JIT reload, and native C++ cases. This is not merely an empty queue: `0` is the valid no-task/work-stealing value, whereas `TaskQueue::Init/InitOne` fills unused storage with `-1`.

The checks stop before the generated dispatcher consumes that invalid result. Some snapshots show unfinished dependency counters, consistent with lost work. Queue/storage snapshots are best-effort observations, not a coherent concurrent snapshot or a reconstruction of the exact memory interleaving.

### Independent confirmation without runtime tracing

The parent attaches LLDB to the **original hung process**, rather than trying to reproduce its timing in a new debugger run. In `untraced-read-0-lldb-attach.txt`:

```text
x14 = 0x00000000ffffffff
...
0x104956c70: cmp w14, #0x86
0x104956c74: b.hi 0x104956c40
```

The generated dispatch loop has task ID `-1` in its 32-bit task register, fails the unsigned valid-case range check, and loops without completing the current audio block. Its current vector index is 128 of 256. This is inside processing, not instance destruction, a join, or factory cleanup.

`untraced-native-0-lldb-attach.txt` also captures a native hang, with symbolic `TaskQueue::PopTail` / `GetNextTask` frames and another thread in generated dispatch. **LLVM JIT or bitcode reload is not a necessary condition for this failure.**

Converting `-1` into `0` would hide the invalid result without establishing that the missing task and its dependencies ever complete; that was not used as a fix.

## 2. Queue-publication hypothesis and narrowly controlled interventions

The supplied queue does approximately:

```cpp
fTaskList[Head(fCounter)] = item; // ordinary slot store
IncHead(fCounter);              // volatile 16-bit head update
```

Other participants claim work with a 32-bit compare-and-swap on the packed head/tail counter. Thus counter accesses mix widths and atomic/non-atomic operations, without a proper release/acquire publication contract for the slot. LLVM's [Atomics guide](https://llvm.org/docs/Atomics.html) explicitly distinguishes volatile from atomic synchronization; volatile accesses are not a substitute for atomic interthread ordering.

A participant observing a published head before the associated slot contents is a plausible explanation for consuming the unused-slot marker. This is a hypothesis supported by source and observed values; the diagnostics do not record the exact hardware visibility order. Two controls test it without changing Faust DSP arithmetic or compiler-generated task partitioning:

**Fence control:** add only `__atomic_thread_fence(__ATOMIC_RELEASE)` immediately before the existing head publication. This is a hardware-ordering diagnostic, **not a complete C++ memory-model repair**: the old mixed counter accesses remain.

**Atomic queue candidate:** change the shared packed counter to `std::atomic<int>`, use a full-word release `fetch_add` after writing the slot, acquire reads and acquire/release compare-and-swap for claims, and an atomic reset. The full-word update preserves a concurrently advanced tail instead of mixing a volatile halfword write with a full-word atomic operation. The queue's existing indexing/claim algorithm and task partition are retained. A producer-index bounds check is added; the existing stop/wake/join adaptation is unchanged.

These are candidate changes to the queue, not a proof that all generated scheduler shared state is race-free. Remaining block/epoch transitions, queue reuse, counter capacity and other shared variables still need review.

## 3. Completed control results

Publication controls: [run 34139527563](https://github.com/curlcomplex/Faust-expr/actions/runs/34139527563). Both jobs completed successfully.

A contemporaneous baseline at the **same source/merge-test commit**, [run 34139527535](https://github.com/curlcomplex/Faust-expr/actions/runs/34139527535), still failed:

| Baseline variant | Child processes | Clean exits | Invalid-task traps | Processing timeouts |
|---|---:|---:|---:|---:|
| Runtime trace | 25 | 23 | 2 | 0 |
| Runtime trace + completion barrier | 5 | 2 | 3 | 0 |
| No runtime trace/watchdog | 5 | 1 | 0 | 4 |
| **Total** | **35** | **26** | **5** | **4** |

The matrix deliberately stops a variant after two failed repeat pairs; writer failures are separately recorded. These are observed counts, **not an estimated failure probability or a statistically balanced rate comparison**.

| Publication control | Source-JIT processes | Fresh-process bitcode readers | Native C++ processes | Total clean exits | Completed audio comparisons |
|---|---:|---:|---:|---:|---:|
| Release fence diagnostic | 3/3 | 70/70 | 70/70 | **143/143** | **137/137 bit-identical** |
| Atomic packed-counter candidate | 3/3 | 70/70 | 70/70 | **143/143** | **137/137 bit-identical** |

Each mode includes traced (61 children), traced plus completion barrier (61), and untraced (21) variants. Each child renders 1,000 blocks of 256 frames at 48 kHz and, to pass, destroys the DSP, explicitly releases the JIT factory where applicable, and exits normally. That is **143,000 completed block calls per control**, across one deterministic DSP and the stated configurations—not an album-scale or real-device soak test.

The untraced variants omit runtime counters/watchdog/noinline tracing, but still retain host progress output and worker-creation checks. Trace/watchdog activity can affect thread timing; this is why untraced controls matter.

### Workers actually existed and participated

All control children reported two successful helper-thread creations. The configured participant count is two **including the calling thread**; the stock pool allocates by the VM's three reported cores, so one helper can remain unused. In all 122 traced atomic-control child logs, helper 1 recorded nonempty task returns (minimum 89,671) and nonzero task activity. This establishes helper participation in this diagnostic, not pinned-core assignment, physical-core utilization, workgroup membership, or speedup.

### Bitcode reload and audio validation

Fresh readers receive neither `-L` nor the scheduler path. The original scheduler `.ll` is physically renamed away for the reader's lifetime. This excludes a RAM factory-cache hit or quietly reopening that original support module.

For each publication mode the 137 comparisons comprise 70 JIT source-versus-reader comparisons and 67 native repeat-versus-native-reference comparisons. Every comparison contains **all 512,000 interleaved float samples** and is bit-identical, meeting the existing `1e-7` absolute threshold with zero error.

Independent checks on the downloaded, hash-verified artifacts also found:

- Each candidate's traced, barrier and untraced source-JIT audio is bit-identical to a successful 512,000-sample capture from the preceding baseline run. Candidate traced source-JIT audio also matches the contemporaneous same-commit baseline capture exactly.
- Fence versus atomic untraced JIT audio is bit-identical.
- Against the earlier scalar JIT reference, the first 102,400 samples retain the previously observed maximum difference `2.384185791015625e-7`, RMS `1.0437272486038331e-8`.
- Native C++ versus JIT over the longer 512,000-sample capture is **not** bit-identical: maximum difference `1.2013688683509827e-4`, RMS `4.901430524944209e-5`, in both controls. Its numerical origin has not been isolated. This longer cross-backend comparison is supplemental, not one of the within-backend comparisons that made the workflow green, and would exceed the preceding short test's `5e-5` cross-backend threshold. Do not claim full cross-backend numerical acceptance or silently relax that threshold.

A failed writer may not produce reference audio. Readers are still attempted to diagnose their behavior, and missing-reference comparisons remain failures in the JSON; they are not evidence of a measured numerical mismatch.

## 4. Interpretation and next gate

The strongest demonstrated result is the causal chain **invalid queue task -> generated dispatch cannot advance -> processing hang**, independently captured without runtime tracing. A same-commit failing baseline and two focused publication interventions passing the full matrix provide strong evidence that queue publication/order is the relevant fault area. They do not formally prove every memory interleaving or rule out all remaining scheduler bugs.

The **atomic queue version**, rather than the fence-only diagnostic, is the candidate to carry into further review. Neither requires a new Curlop partitioner or a change to authored module boundaries. Faust continues to compile the fused graph and determine its tasks; the linked runtime and its cached bitcode carry the adapted queue implementation.

Next bounded acceptance work should review queue and block-generation synchronization, then extend this candidate to multiple active participants, variable block lengths, repeated create/render/destroy in one process, and overlapping independent DSP instances. Keep the failing baseline and require complete audio comparisons and clean lifetimes. Separately isolate the longer native/JIT numerical drift before claiming cross-backend equivalence.

Only after those checks should performance selection use **Curlop's actual production renderer and matching bundled toolchain**, with realistic graph/control segmentation, callback tail latency, active/decaying overlap, verified worker participation and Apple audio-workgroup integration. This report does not assert realtime safety, multi-instance safety, SIMD benefit or multicore speedup.

## Reproduction and evidence

Environment: macOS 15 ARM64 GitHub hosted Apple M1 virtual machine; 3 physical/logical CPUs reported; Faust 2.85.9; LLVM/Clang 22.1.8. Host/native compile flags `-std=c++17 -O2 -g -fno-omit-frame-pointer`. `OMP_NUM_THREADS=2`, `OMP_DYN_THREAD=0`, `DIAG_BLOCKS=1000`. JIT target `arm64-apple-darwin24.6.0:generic`; scheduler IR reports `arm64-apple-macosx15.0.0`. The pre-existing target-triple warning remains recorded.

On a matching macOS development environment:

```sh
python3 scripts/run_scheduler_liveness.py
QUEUE_CONTROL=fence python3 scripts/scheduler_liveness_queue_controls.py
QUEUE_CONTROL=atomic python3 scripts/scheduler_liveness_queue_controls.py
```

Each invocation writes `evidence-liveness/`; preserve/move that directory between local invocations. CI jobs use separate workspaces. The scripts intentionally require the previously tested Faust distribution instead of silently migrating versions.

| Evidence | Source head | Actual PR merge-test SHA | Artifact |
|---|---|---|---|
| First invalid-task/live-hang capture, run 34138746599 | `6e5d0805bba647922ca844cf5bd9375c4583e579` | `85fcbd89f5cc15dd18277410ab5fd6d25242deb0` | `10025123393` |
| Same-commit baseline, run 34139527535 | `bf7e1fd4f55e573ba23a3a4dde1897820769e474` | `7dc09caca6929ca76e48773554ea32acbcb1ca50` | `10025442882` |
| Fence control, run 34139527563 | same | same | `10025492702` |
| Atomic control, run 34139527563 | same | same | `10025465396` |

Artifact ZIP SHA-256 values, checked after download:

```text
10025123393  51087e8fd98950dda7f1fcb1a70fb9e2eeff5e7c898f7e021cd079efcc75784f
10025442882  1d931558d73c6e3d460d40df7a405f2a0afb2a4e5efee32a82b3473ca73f6091
10025492702  62da5c4a57ff605e6fcf0bcdb1df24c2fbca170c0a0be0862aa2c7c5d30853cc
10025465396  acd16f2c2787a81911c8e7aff36da5127f6f7adf0ee7203fb8fca5225bdfbfe9
```

Unchanged DSP SHA-256: `f3593f74b0614bd51cdc34a28ce732e3a84763e6ef56d53f41725adf091ca8d9`.
Installed upstream scheduler SHA-256: `85e01152c0e507792a5188bcaf0ef04e4388182690739d509402e2bfe8cf3a4b`.

Artifacts include exact original/adapted runtime copies, generated native headers, linked factory IR, bitcode strings, full audio captures, provenance, commands, per-child status and captured original-process stacks/disassembly. Failed controls are retained. No PR was merged and no upstream issue or message was posted in this investigation.
