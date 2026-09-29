# libfaust LLVM scheduler and module-fusion research

Status: active research. This is standalone compiler/runtime evidence, not a Curlop production acceptance result.

## Questions

1. Can Faust's `-sch` work-stealing scheduler be used through the embedded libfaust LLVM JIT path, and does doing so require patching libfaust itself?
2. Does keeping user-authored Faust modules editable require paying a large runtime penalty, or can authored module source be fused into a faster whole-graph build artifact?

## Scheduler reproduction path

The branch builds the same synthetic 64-voice oscillator/filter DSP through `createDSPFactoryFromString` in scalar, `-vec -vs 32`, `-vec -vs 64`, unlinked `-sch`, and linked `-sch` modes.

Faust's generated LLVM for `-sch` references the external scheduler ABI including `createScheduler` and `computeThreadExternal`. Calling `createDSPInstance()` with plain `-sch` segfaults because that runtime is not present in the JIT module.

Faust ships `/usr/share/faust/scheduler.cpp` in the tested distribution. It implements the scheduler ABI and calls the JIT DSP back through `computeThreadExternal`.

However, `scheduler.cpp` is designed for textual insertion into generated C++ and defines its public ABI through an `inline`/`always_inline` `EXPORT` macro. A normal standalone LLVM compilation therefore did not emit `createScheduler` and friends as definitions. Supplying that incomplete module with `-L` still failed at DSP instance creation.

The research harness copies `scheduler.cpp`, changes only the `EXPORT` macro so the public ABI is emitted as externally visible functions, compiles that copy to LLVM IR with the LLVM version matching libfaust, and supplies it using `-L`. With that change, `createDSPInstance()`, `init()`, and `compute()` all succeed. This demonstrates that libfaust itself did not need to be patched for the basic JIT scheduler integration.

## Linux scheduler CI evidence

Successful integration run: GitHub Actions run 34113676868, PR merge-test commit `9af127813de2ec085eca8c7188e4cbb6f565f09f`, using Ubuntu 24.04, Faust 2.70.3, LLVM/Clang 17.0.6.

The linked scheduler mode produced the same checksum (`9.74952`) as scalar and vector controls and exited successfully.

Observed timings on that hosted runner for the 256-frame synthetic workload were:

| mode | ns/frame | realtime factor |
| --- | ---: | ---: |
| scalar | 273.234 | 76.247x |
| vec32 | 380.225 | 54.792x |
| vec64 | 475.394 | 43.823x |
| sch-linked | 814.263 | 25.586x |

These numbers must **not** be interpreted as evidence that the scheduler is slower in real multicore use. During `sch-linked` initialization the Faust scheduler attempted to create realtime `SCHED_FIFO` worker threads and the GitHub runner denied those requests (`Cannot create thread res = 1`). The DSP continued to run, but without the intended worker pool. That measurement therefore includes scheduler/task overhead while receiving no multicore benefit.

Likewise, hosted x86 runners showed inconsistent `SIGILL` behavior between otherwise identical scalar/vector JIT runs, so this CI platform is useful for integration/proof-of-instantiation but not trustworthy as a production performance host.

## Editable-module versus fused-graph experiment

The branch also compares two runtime representations of the same authored Faust DSP:

- **separate/editable runtime modules:** each logical module is independently compiled to a libfaust LLVM DSP instance and connected through prepared float buffers;
- **fused build:** the same logical construction is emitted as one Faust program and JIT-compiled as one LLVM DSP instance.

This is deliberately a runtime-boundary experiment. It does **not** make the source less editable: both forms are generated from Faust source. The question is whether the final live build should preserve authoring boundaries as executable DSP-instance boundaries.

Two graph families are covered at 48 kHz / 256 frames:

1. a 24-oscillator source followed by 2, 4, or 8 serial low-pass module stages;
2. 4, 8, or 16 independent oscillator/filter modules summed together.

The probe reinitializes both representations before an equivalence block and reports maximum absolute output difference, JIT build time, and render ns/frame.

### Apple Silicon evidence

GitHub Actions pull-request run 34120740733, source head `059ddd2a6885cc6f0f7906c344537db36e3f6ec9`, used the standard `macos-15` ARM64 runner (`Apple M1 (Virtual)`), Faust 2.85.9, and libfaust's reported target `arm64-apple-darwin24.6.0:generic`.

The entire comparison was repeated three times in one job. Timings on a hosted virtual machine are noisy, so the table below reports the **median ratio** of separate-module runtime to fused runtime. A value above 1 means the fused graph was faster.

| graph | median separate/fused runtime | interpretation |
| --- | ---: | --- |
| serial, 2 filter stages | 0.917x | too small/noisy to justify fusion from this case alone |
| serial, 4 filter stages | 1.120x | fused about 11% cheaper |
| serial, 8 filter stages | 1.214x | fused about 18% cheaper |
| wide, 4 modules | 0.927x | too small/noisy to justify fusion from this case alone |
| wide, 8 modules | 1.568x | fused about 36% less runtime work |
| wide, 16 modules | 1.799x | fused about 44% less runtime work |

Median separate/fused **JIT build-time** ratios also increased with module count: about 1.55x / 1.40x / 2.01x for the 2/4/8-stage serial cases and 3.13x / 3.98x / 6.00x for the 4/8/16-module wide cases. In this experiment, compiling many independent factories was substantially more expensive than compiling one equivalent fused factory.

Output equivalence held closely: the serial cases had maximum absolute differences of roughly `4e-6` to `5e-6`; the wide cases reported `0` at the printed precision.

### Interpretation

This experiment does **not** support removing user module authoring. It supports separating the authoring representation from the runtime build representation.

For small graphs, preserving independent DSP instances can be competitive and measurement noise is significant. As the number of independent authored modules grows, retaining every authoring boundary as a runtime DSP-instance boundary becomes increasingly expensive in this test. The strongest tested cases favored the fused build by roughly 1.6-1.8x while producing equivalent audio.

The architecture suggested by this evidence is therefore:

`editable Faust/module graph -> off-thread graph lowering -> fused optimized Faust build -> native LLVM DSP instance`

rather than either:

- `editable module -> permanently separate DSP instance per module`, or
- `closed C++ factory modules only`.

A factory module can still ship with a cached/precompiled build, while opening/editing it invalidates that derived build and recompiles an optimized replacement off-thread. Visual authoring can likewise remain an authoring frontend if it lowers to the same Faust construction before build generation.

This is not yet proof that every Curlop patch should always be a single monolithic DSP. Clip/track lifecycle, coarse multicore scheduling, feedback boundaries, dynamic replacement, and graph-size compile costs may justify multiple fused regions. The result only establishes that **editability itself does not require preserving costly executable module boundaries**.

## What this establishes

1. Plain libfaust LLVM `-sch` is not self-contained in the tested Faust release; it expects the external scheduler ABI.
2. Faust's supplied scheduler implementation can satisfy that ABI through `-L` once its public functions are emitted in a standalone LLVM module.
3. A libfaust/compiler fork is therefore not currently required for the basic scheduler integration.
4. The existing "scheduler is slower" observation is not a valid multicore result when worker creation fails.
5. `-vec` is workload-dependent and was slower than scalar on the original synthetic scheduler DSP; it needs graph-specific measurement.
6. User-editable Faust source can be compiled into a fused native LLVM build without retaining module authoring boundaries at runtime.
7. On the tested Apple-Silicon hosted runner, fusion became materially beneficial as the number of runtime module boundaries grew, especially for wide graphs.
8. Independently compiling many module factories also imposed substantially more aggregate JIT build time than one fused equivalent in this experiment.

## Next acceptance work

### Scheduler

Run the linked scheduler on an environment where worker threads can actually be created, ideally macOS on hardware representative of Curlop's desktop target and with the exact Faust/libfaust version used by Curlop. Record worker count and CPU utilization, then benchmark real Curlop-shaped graph families across audio block sizes and complexity/parallel-width sweeps. Only after that should the scheduler be accepted or rejected on performance grounds.

The Apple-specific thread/workgroup behavior also needs explicit review before production use; successful generic pthread scheduling is not by itself proof that Faust worker threads participate correctly in an Audio Workgroup/realtime host setup.

### Authoring/build architecture

Repeat the fused-versus-separate comparison using actual Curlop module source families and graph shapes, including controls, polyphony, feedback, meters/taps disabled and enabled, and representative clip-sized graphs. Measure compile latency as well as p50/p99/max callback time on physical Apple Silicon. Then test hot replacement: edit one source module, rebuild the derived fused graph off-thread, prewarm it, atomically swap it at a safe boundary, and verify continuity/state policy.
