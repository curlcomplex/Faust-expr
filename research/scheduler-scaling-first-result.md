# Cached-JIT scheduler scaling — first result

Status: completed preliminary throughput evidence on GitHub-hosted Apple Silicon. This is not Core Audio/realtime acceptance and does not yet include the heavy-task follow-up.

## Environment

- GitHub Actions run 34144670702, completed successfully.
- macOS 15 ARM64 runner, Apple M1 (Virtual), 3 physical / 3 logical CPUs reported.
- Faust 2.85.9, LLVM/Clang 22.1.8.
- Cached LLVM bitcode factories were loaded before timing; source/JIT compilation time was not included in DSP throughput.
- Scheduler runtime used the existing ARM/lifecycle adaptations plus the atomic queue-publication candidate that passed the preceding correctness matrices.
- 9 timed trials per point after 1,000 warmup compute calls.
- Graph family: independent oscillator -> 2-pole lowpass branches summed to stereo, at 32 / 64 / 128 / 256 voices.
- Host block sizes: 64 / 128 / 256 / 512 frames.

## Result

The scheduler demonstrates some internal scaling from one to two configured participants, but remains materially slower than the scalar fused LLVM build at every tested point. Three participants are generally worse than two and frequently worse than one scheduled participant.

Across the 16 graph/block-size combinations:

- `sch(2)` versus `sch(1)` ranged from about **1.03x to 1.30x faster** (best observed 1.296x).
- `sch(3)` versus `sch(1)` ranged from about **0.77x to 1.12x** and was usually near or below 1x.
- `sch(2)` versus scalar ranged only from about **0.40x to 0.58x**. In other words the best two-participant scheduled case still took roughly 1.7x the scalar runtime; many cases took about 2x or more.
- `sch(3)` versus scalar ranged about **0.29x to 0.50x**.
- Explicit Faust `-vec -vs 32` was also slower than scalar throughout this graph family, with scalar/vector ratios around **0.56x to 0.80x**.

Representative medians (ns per audio frame; lower is better):

| voices | block | scalar | sch1 | sch2 | sch3 | sch2/scalar speed ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 32 | 512 | 150.1 | 316.0 | 259.8 | 326.0 | 0.578x |
| 64 | 256 | 334.4 | 683.4 | 592.6 | 675.2 | 0.564x |
| 128 | 128 | 538.4 | 1416.0 | 1093.0 | 1264.9 | 0.493x |
| 256 | 256 | 1110.0 | 2796.1 | 2200.3 | 2702.2 | 0.504x |

The test did verify successful creation of the scheduler helper threads. The preceding traced correctness/liveness work established task participation; this timing harness itself does not measure per-core utilization or Audio Workgroup membership.

## Interpretation

This is evidence against enabling Faust `-sch` as a blanket optimization for Curlop's fused renderer. The failure mode is not that multicore execution is absent: two participants often make the scheduled implementation faster than one. The problem is that the task/scheduler machinery adds enough overhead that the result still loses badly to the default scalar fused LLVM code on these relatively light independent branches.

This graph family may give each generated task too little work to amortize scheduling. Therefore the next discriminating test keeps the same scheduler/runtime and adds a **heavy per-branch graph family** with several serial filters per independent branch. If `-sch` remains slower than scalar when each independently schedulable branch contains substantially more DSP work, the stock Faust work-stealing runtime should no longer be considered the leading Curlop multicore strategy.

Three participants performing worse than two on this 3-core VM also means participant count must be selected rather than assumed to equal available CPUs if the scheduler is pursued.

## Evidence

- Run: https://github.com/curlcomplex/Faust-expr/actions/runs/34144670702
- Artifact ID: `10027503663`
- Artifact ZIP SHA-256 reported by Actions: `9db9dce891decada2846c04ee507e9dbde93f57567fd7f43c03ceeedb2f2a18a`
- Source head checked out by the job: `397df0a6d00537b4b06997103e363409f23ec1de`

No production Curlop code was changed and this result does not measure callback p99/max, missed deadlines, workgroup behavior, or the production graph builder/control segmentation.
