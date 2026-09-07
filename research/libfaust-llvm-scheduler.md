# libfaust LLVM scheduler research

Status: active research. This is standalone compiler/runtime evidence, not a Curlop production acceptance result.

## Question

Can Faust's `-sch` work-stealing scheduler be used through the embedded libfaust LLVM JIT path, and does doing so require patching libfaust itself?

## Reproduction path

The branch builds the same synthetic 64-voice oscillator/filter DSP through `createDSPFactoryFromString` in scalar, `-vec -vs 32`, `-vec -vs 64`, unlinked `-sch`, and linked `-sch` modes.

Faust's generated LLVM for `-sch` references the external scheduler ABI including `createScheduler` and `computeThreadExternal`. Calling `createDSPInstance()` with plain `-sch` segfaults because that runtime is not present in the JIT module.

Faust ships `/usr/share/faust/scheduler.cpp` in the tested distribution. It implements the scheduler ABI and calls the JIT DSP back through `computeThreadExternal`.

However, `scheduler.cpp` is designed for textual insertion into generated C++ and defines its public ABI through an `inline`/`always_inline` `EXPORT` macro. A normal standalone LLVM compilation therefore did not emit `createScheduler` and friends as definitions. Supplying that incomplete module with `-L` still failed at DSP instance creation.

The research harness copies `scheduler.cpp`, changes only the `EXPORT` macro so the public ABI is emitted as externally visible functions, compiles that copy to LLVM IR with the LLVM version matching libfaust, and supplies it using `-L`. With that change, `createDSPInstance()`, `init()`, and `compute()` all succeed. This demonstrates that libfaust itself did not need to be patched for the basic JIT scheduler integration.

## CI evidence

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

## What this establishes

1. Plain libfaust LLVM `-sch` is not self-contained in the tested Faust release; it expects the external scheduler ABI.
2. Faust's supplied scheduler implementation can satisfy that ABI through `-L` once its public functions are emitted in a standalone LLVM module.
3. A libfaust/compiler fork is therefore not currently required for the basic integration.
4. The existing "scheduler is slower" observation is not a valid multicore result when worker creation fails.
5. `-vec` is workload-dependent and was slower than scalar on this particular synthetic DSP; this says nothing conclusive about production Curlop graphs.

## Next acceptance work

Run the linked scheduler on an environment where worker threads can actually be created, ideally macOS on hardware representative of Curlop's desktop target and with the exact Faust/libfaust version used by Curlop. Record worker count and CPU utilization, then benchmark real Curlop-shaped graph families across audio block sizes and complexity/parallel-width sweeps. Only after that should the scheduler be accepted or rejected on performance grounds.

The Apple-specific thread/workgroup behavior also needs explicit review before production use; successful generic pthread scheduling is not by itself proof that Faust worker threads participate correctly in an Audio Workgroup/realtime host setup.
