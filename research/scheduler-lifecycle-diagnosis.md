# Scheduler lifecycle diagnosis — 2026-09-07

## Decision / current status

**Scheduler-enabled LLVM bitcode is demonstrably serializable and reloadable in this tested environment. The adapted scheduler runtime is NOT approved for Curlop production: intermittent scheduled-compute hangs remain.**

This investigation separates three previously conflated problems:

1. Retaining an LLVM factory until normal process exit triggers an LLVM JIT-debugger cleanup mutex failure even with **scalar DSP and no scheduler**. Explicit instance-then-factory cleanup avoids that failure in the controls.
2. The ARM-portable scheduler retaining the original macOS thread-termination code fails during JIT instance/factory cleanup. Backtraces show worker threads faulting while the main thread destroys the JIT factory. Graceful stop/wake/join avoids these teardown failures in the tested explicit-cleanup cases.
3. An intermittent hang inside scheduled `compute()` persists. Changing the shutdown flag to an atomic does not establish liveness. A separate atomic block-completion barrier also hung once in repeated JIT reload testing. Neither patch is a production solution yet.

**Do not infer that module authoring, unified Faust fusion, or bitcode caching must be abandoned. Do not infer that multicore speedup has been demonstrated.** The remaining immediate question is scheduler/JIT execution liveness, not whether the bitcode format can carry a scheduled build.

## Scope and reproducibility

The new driver uses the DSP extracted verbatim from the existing `scripts/scheduler_bitcode_probe.cpp`: a 64-voice oscillator/filter bank, with stereo output. It exercises the existing libfaust APIs `createDSPFactoryFromString`, `writeDSPFactoryToBitcode`, `readDSPFactoryFromBitcode`, and `deleteDSPFactory`.

This is a **standalone ABI, serialization and lifecycle diagnostic**, not an imported Curlop renderer, real-device callback test, authoring-latency benchmark, or full production cache implementation. It deliberately stays on the previously failing probe rather than inventing another graph corpus.

Files:

- `scripts/lifecycle_diag.cpp`: common native C++ / LLVM JIT driver, stage markers, full audio capture, explicit cleanup, terminate-handler backtrace.
- `scripts/run_lifecycle_diagnostics.py`: paired controls, exact runtime-copy selection, fresh-process reload, timeout stack sampling, LLDB crash replays and evidence collection.
- `.github/workflows/scheduler-lifecycle.yml`: macOS ARM64 diagnostic job; artifacts are retained for 30 days.

Final completed diagnostic run:

- [Run 34136562886](https://github.com/curlcomplex/Faust-expr/actions/runs/34136562886)
- Source head: `7b0ff2ef8f201a788adeb38943b9f3e6e6ce2446`
- Actual PR merge-test SHA: `31308fd343a6ecf65c1acf45cd010fa011eab765`
- Artifact: `10024298698`, `scheduler-lifecycle-31308fd343a6ecf65c1acf45cd010fa011eab765-1`
- ZIP SHA-256: `4bb8558710f1659a5eb78c1a865a9a2a42ca340d9f77af79e44a3eb23c0b6b7e`
- Environment: macOS 15 ARM64 GitHub hosted runner, Apple M1 virtual machine; reported physical/logical CPUs: 3/3.
- Faust 2.85.9, built with LLVM 22.1.8; Homebrew Clang 22.1.8.
- Host/native code flags: `-std=c++17 -O2 -g -fno-omit-frame-pointer`.
- JIT target: `arm64-apple-darwin24.6.0:generic`.
- Scheduler support IR target printed by Clang: `arm64-apple-macosx15.0.0`; the linker warning is retained, not silently suppressed. Whether it matters to the remaining issue has not been demonstrated.
- `OMP_NUM_THREADS=2`, `OMP_DYN_THREAD=0`. This is requested participation, **not measured physical-core utilization**. The stock pool still allocates by reported CPU count.
- Each child initializes at 48 kHz, computes 200 blocks of 256 frames and captures all 102,400 interleaved float samples.
- DSP source SHA-256: `f3593f74b0614bd51cdc34a28ce732e3a84763e6ef56d53f41725adf091ca8d9`.
- Installed upstream scheduler source SHA-256: `85e01152c0e507792a5188bcaf0ef04e4388182690739d509402e2bfe8cf3a4b`.

All runtime copies, generated native headers before/after diagnostic adaptation, IR, bitcode strings, audio captures, commands, provenance, JSON summaries, crash backtraces and live timeout samples are in the artifact. The `.bc.txt` files contain the string returned by Faust's bitcode API; their string byte count is not a native-machine-code size measurement.

## Controlled variants

| Variant | Changes relative to installed scheduler |
|---|---|
| `arm_portable` | Emit the C ABI functions and supply ARM timestamp/atomic helpers; retain original macOS termination. |
| `graceful_prior` | Existing research script's graceful stop/wake/join, including its original `volatile bool` stop flag. This is a historical control, not the recommended implementation. |
| `graceful_atomic` | Identical graceful runtime except stop flag is `std::atomic<bool>`. |
| `graceful_atomic_barrier` | Atomic graceful runtime plus atomic worker-completion accounting and an explicit `SyncAll()` completion wait. Faust's generated task partitioning is unchanged. |

The barrier variant restores worker acknowledgements that were commented out in the supplied runtime. It tests a block-to-block coordination hypothesis. It is diagnostic code with a busy wait, not a reviewed realtime executor.

## Final-run outcome matrix

These are **clean process exits**, not performance measurements or overall audio-equivalence acceptance. JIT explicit totals combine two source-compilation children, two initial fresh-process readers, and (for the atomic variants) ten additional readers. Native counts include two initial and ten additional children for those variants.

| Case | JIT, explicit cleanup | Native C++, explicit cleanup |
|---|---:|---:|
| Scalar | 4 / 4 clean | 2 / 2 clean |
| ARM portability only / original termination | 0 / 2 clean; faults during factory destruction | 1 / 1 clean |
| Prior graceful shutdown | 4 / 4 clean | 2 / 2 clean |
| Atomic graceful shutdown | 14 / 14 clean | 12 / 12 clean |
| Atomic graceful shutdown + block barrier | 13 / 14 clean; one compute timeout | 12 / 12 clean |

Both final-run retained-factory controls (scalar and atomic-graceful scheduler) aborted after `main_return` with the recursive-mutex error. In an **earlier** run, the atomic-graceful retained control instead hung in `compute()`, before it reached the retain/explicit-cleanup branch. That earlier hang must not be discarded just because the final atomic-graceful repeats were all clean.

Native scalar and scheduled cases use the same driver/render sequence and no libfaust linkage in the native executable. Native scheduled results require the documented code-generation workaround below. A short native run exiting cleanly does not prove the original termination code is safe: unlike the JIT path, it does not immediately unload its generated executable code when the instance is destroyed.

## Finding 1: the mutex failure is not scheduler-specific

The scalar retained-factory control computes, destroys its DSP instance, returns from `main`, then aborts. LLDB records this path:

```text
std::__1::recursive_mutex::lock()
GDBJITRegistrationListener::notifyFreeingObject(...)
llvm::MCJIT::notifyFreeingObject(...)
llvm::MCJIT::~MCJIT()
llvm_dsp_factory_aux::~llvm_dsp_factory_aux()
llvm_dsp_factory::~llvm_dsp_factory()
dsp_factory_table<...>::~dsp_factory_table()
__cxa_finalize_ranges
exit
```

Evidence files include `jit-scalar-retain-0.txt` and `jit-scalar-retain-0-lldb.txt`.

This localizes the failure to exit-time JIT object/debugger deregistration, and rules out `-sch` as a necessary condition for this error. A static-lifetime ordering problem is a plausible explanation; the controls do not independently prove every detail of that ordering.

The previous probe's decision to omit `deleteDSPFactory()` introduced this extra failure into our investigation. Keeping a factory cached during normal application operation is compatible with explicitly releasing the cache during orderly shutdown, before library/global teardown. The controls do **not** demonstrate a corresponding bug in Curlop's actual `FaustRuntime` shutdown implementation.

Relevant primary sources:

- [Faust Box API lifetime example](https://faustdoc.grame.fr/tutorials/box-api/): delete the DSP instance, then release its factory.
- [Faust embedding / factory APIs](https://faustdoc.grame.fr/manual/embedding/).
- [LLVM 22.1.8 GDBRegistrationListener.cpp](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/lib/ExecutionEngine/GDBRegistrationListener.cpp): `notifyFreeingObject` locks the listener's member `JITDebugLock`; the listener is a function-local static instance.

## Finding 2: original termination is unsafe in the tested dynamic lifetime

With only the ARM/linkage adaptations, source and reloaded JIT children repeatedly fault during cleanup. One LLDB capture shows the main thread in `deleteDSPFactory()` / MCJIT module destruction while worker threads are stopped on `EXC_BAD_ACCESS` at JIT addresses.

The original code calls `thread_terminate()` and then destroys worker/semaphore storage without joining. Graceful stop/wake/join plus explicit factory release removes the observed teardown failures in the successful controls.

This is considerably stronger evidence than the earlier inference from source inspection alone. It still does not establish a universal upstream root cause across versions/platforms, or exclude effects of our ARM adaptation. The isolated native comparison helps show why a static C++ application might appear fine while dynamic factory destruction exposes the problem.

## Finding 3: the bitcode roundtrip itself works

Successful scheduled cases execute:

```text
source -> -sch with linked scheduler IR -> LLVM factory
       -> writeDSPFactoryToBitcode
       -> create/init/compute instance
       -> destroy instance -> deleteDSPFactory -> normal process exit

fresh process:
readDSPFactoryFromBitcode
       -> create/init/compute instance
       -> destroy instance -> deleteDSPFactory -> normal process exit
```

The reader receives no scheduler environment path and no `-L`. During the reader's entire lifetime the **original scheduler IR file is physically renamed out of the way**. Therefore the successful reader is not a RAM-resident factory cache hit or an implicit reload of that original IR path.

For every successful scheduled initial source/read pair in the final run, all **102,400 captured float samples are bit-identical**. Successful additional scheduled readers also match their source reference. A matching Faust/LLVM host environment is still required; this is not proof of a universally portable or dependency-free binary format.

Two important numerical caveats are preserved rather than hidden:

- Scalar source/read output differs by up to `2.384185791015625e-7` (RMS `1.0046903196065836e-8`). It fails the deliberately strict `1e-7` absolute comparison even though lifecycle succeeds. Its numerical origin has not been isolated.
- The largest observed native-C++ versus JIT-scalar difference is `2.4186214432120323e-5`, below the separate predeclared `5e-5` cross-backend tolerance. Native and JIT should not be described as bit-identical.

## Finding 4: a separate processing hang remains

Final run case `jit-graceful_atomic_barrier-repeat-read-5` timed out at `compute_begin`. It did not reach `compute_end` or cleanup. Its parent captured live stacks before terminating it:

- main thread repeatedly in JIT addresses;
- one worker repeatedly in JIT addresses;
- another allocated worker asleep in a semaphore wait (not intrinsically unexpected given the pool size and requested active-thread count).

Evidence: `jit-graceful_atomic_barrier-repeat-read-5.txt` and `jit-graceful_atomic_barrier-repeat-read-5-sample.txt`. The sample lacks symbolic names for those JIT instructions, so it does **not** yet identify the exact stuck task/counter/queue.

The same class of compute-stage timeout occurred without the completion barrier in run 34135349622 (`jit-graceful_atomic-retain-0`). Consequently:

- explicit factory cleanup solves the observed exit-mutex control, not the processing hang;
- graceful worker shutdown is not sufficient to establish execution liveness;
- adding the tested block barrier did not eliminate all hangs;
- native C++ passing in this finite sample does not prove LLVM is at fault: it could expose different timing/interleavings in shared scheduler logic.

**Do not start interpreting scheduler speed benchmarks as valid until this liveness blocker is resolved.**

## Native C++ code-generation and harness caveats

The first native scheduled controls were blocked by an incorrect harness assumption: bare Faust class generation does not embed `scheduler.cpp`, and in this installed distribution `-A` did not select our intended replacement for the automatically embedded scheduler. The guard caught the mismatch rather than silently comparing different runtimes.

The final harness archives the generated unadapted header, verifies that it contains exactly one byte-for-byte copy of the installed upstream scheduler, and replaces only that copy with the same runtime used to build the JIT support IR.

A separate Faust 2.85.9 `-lang cpp -sch` code-generation defect then became visible: it generated both:

```cpp
virtual ~ProbeDSP() = default;
// ...
virtual ~ProbeDSP() { destroy(); }
```

Clang correctly rejected the duplicate destructor. The final native diagnostic removes only the redundant defaulted declaration, retains the real scheduler-destroying destructor, and records before/after header hashes. No generated DSP computation or task partitioning was changed. The unadapted generated source and the failing compiler logs from the preceding run remain evidence of this extra problem. This is **not** a claim that untouched upstream native `-sch` compiled successfully.

## Run history / do not discard negative results

| Run | Source head | Result / purpose |
|---|---|---|
| [34134892987](https://github.com/curlcomplex/Faust-expr/actions/runs/34134892987) | `b48580b0b8fbed1d2abca932e972430d78442ece` | Scalar cleanup control isolates mutex error; graceful JIT roundtrips succeed; scheduled native guard blocks comparison. |
| [34135349622](https://github.com/curlcomplex/Faust-expr/actions/runs/34135349622) | `40e1672bf7ead7a04c198d79e3b613443e0ba9c5` | Physically remove original IR during reload; preserve first intermittent atomic-graceful compute hang. |
| [34136146717](https://github.com/curlcomplex/Faust-expr/actions/runs/34136146717) | `b800cdad31eab41d1234772dbb7c1f002cd5631c` | Exact native runtime substitution; duplicate generated destructor exposed; initial barrier JIT roundtrips succeed. |
| [34136562886](https://github.com/curlcomplex/Faust-expr/actions/runs/34136562886) | `7b0ff2ef8f201a788adeb38943b9f3e6e6ce2446` | Native scheduled comparison completed with declared workaround; repeated JIT/native controls; barrier variant still hangs once. |

All four workflows are red because this is a diagnostic matrix containing failing historical/negative controls and unresolved failures. A printed stage marker is never a substitute for a clean child-process exit. The final successful mechanism tests must not be relabelled as a blanket green acceptance result.

Earlier artifacts:

- Run 34134892987: artifact 10023589478, ZIP SHA-256 `b89d765d62c507a22a294d06e4aca141c91e014d876d69bd671b389735aeb35c`.
- Run 34135349622: artifact 10023798385, ZIP SHA-256 `60c6296de3a221f272164a3259cc55dd72c7c6c66e3dea2e080e95e91646a178`.
- Run 34136146717: artifact 10024090250, ZIP SHA-256 `bd2af59ff83e924358ee9a6f424d83b6b0eb34eddcaedc69824bd1dbf17bef5d`.

## Next bounded diagnostic target

Capture the remaining hang with per-worker/task progress and block-generation counters, plus a symbolic/disassembled JIT location. Check worker-creation results and actual task participation rather than trusting requested thread counts. Compare the failing JIT execution against the native control under the same input and configuration; examine both generated-code and runtime synchronization before assigning upstream blame.

After liveness passes repeated create/render/destroy, fresh-process reload, multi-instance overlap and variable-block tests, evaluate performance using the **actual Curlop production renderer and matching bundled toolchain**. That later acceptance needs callback tail latency, worker participation/workgroup integration and actual graph/control segmentation, not the diagnostic wall-clock durations recorded here.

No production Curlop code was changed, no new graph partitioner was introduced, no PR was merged, and no upstream issue or message was posted as part of this investigation.
