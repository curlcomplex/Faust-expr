# LLVM region boundary checkpoint

Date: 2026-09-08

Head before this report: `45f8087e93f187f289eb82862c5a37e4a1e7c21f`.
Hosted run: https://github.com/curlcomplex/Faust-expr/actions/runs/34213967560
Artifact: `10050978769`, ZIP SHA256 `65a76f71394e9ae2388838e0d2c6db51580f0c51ee77f1b286160bee846cf3f5`.
Runner/toolchain: Apple M1 (Virtual), macOS 15 arm64, Faust 2.85.9, Homebrew Clang/LLVM 22.1.8.

## Question

Before attempting multi-worker execution, can a bounded LLVM-region interface preserve both ordinary DSP state and an explicit one-sample delayed dependency crossing a region boundary, across arbitrary compute-block boundaries and fresh cached-bitcode factory reconstruction?

## Fixture

The monolithic reference is one Faust LLVM JIT DSP:

```faust
a = fi.lowpass(2, 1200.0);
process = _ : a <: _, mem : + : fi.lowpass(1, 3400.0);
```

The decomposed path compiles two independent LLVM factories:

- region A: `fi.lowpass(2, 1200.0)`;
- region B: `+ : fi.lowpass(1, 3400.0)`.

One calling thread executes A, materializes A's current output plus an explicit one-sample-delayed copy, carries the delayed-edge state across compute calls in the host, then executes B. The input is an 8,192-frame deterministic signal with non-block-aligned impulses. Both fixed 128-frame and deliberately irregular 1–513-frame block partitions are exercised.

Source-created factories are then destroyed. Their bitcode is loaded into fresh factories and the irregular render is repeated from a fresh initialized state.

Predeclared gates in the source are:

- whole-vs-regions max absolute error <= `3e-5`;
- whole-vs-regions RMS error <= `5e-6`;
- block-partition and fresh-bitcode reproduction max absolute error <= `1e-7`.

## Executed result

The hosted job passed.

| comparison | max abs | RMS | max-error frame |
|---|---:|---:|---:|
| whole fixed vs irregular blocks | 0 | 0 | 0 |
| regions fixed vs irregular blocks | 0 | 0 | 0 |
| regions vs whole, fixed blocks | 2.2351741791e-08 | 3.3915003822e-09 | 1062 |
| regions vs whole, irregular blocks | 2.2351741791e-08 | 3.3915003822e-09 | 1062 |
| whole source vs fresh bitcode factory | 0 | 0 | 0 |
| regions source vs fresh bitcode factories | 2.2351741791e-08 | 3.3728339896e-09 | 5200 |
| reloaded regions vs reloaded whole | 2.2351741791e-08 | 3.9035850316e-09 | 1062 |

The region path is therefore block-partition invariant for this fixture and reproduces the monolithic construction to roughly one float ULP at the worst observed sample. Fresh bitcode factories reproduce the fresh-source result within the strict reload gate.

## What this establishes

A host-visible region contract is viable for this bounded linear/stateful case. A delayed inter-region dependency cannot simply be discarded because it is delayed: its state must live somewhere. Carrying that state explicitly in the host preserves the reference semantics across irregular block boundaries. Independent LLVM regions can also remain content-addressed/cacheable through normal libfaust bitcode reconstruction.

This reduces one uncertainty in the proposed architecture: post-fusion regions do not inherently require one monolithic DSP instance merely because a delayed edge crosses a boundary.

## What this does not establish

This does **not** extract or execute the upstream `SuperNodeGraph` / `GroupPlan` fused regions. The two regions were authored explicitly for the test. It does not serialize a live DSP instance state across a factory reload; fresh factories begin from fresh initialized state. It does not cover delayed feedback cycles crossing regions, nonlinear state, table/foreign-resource ownership, UI/control state, shared mutable data, multiple workers, realtime synchronization, Audio Workgroups, callback-tail latency, or arbitrary Faust graphs.

The result therefore validates the execution *contract shape*, not yet the upstream plan-to-LLVM implementation.

## Next bounded checkpoint

Do not jump directly to a general two-worker runtime. First bridge the upstream planner to an executable region representation on one concrete graph.

Preferred next test:

1. take the pinned upstream post-fusion `SuperNodeGraph` for a small stateful fixture whose post-fusion graph has at least two regions and at least one cross-region delayed edge;
2. expose enough structured information to identify each region's explicit current inputs, delayed inputs/state, outputs and persistent local state without parsing generated C++ as the product mechanism;
3. compile those regions through LLVM/libfaust or a minimal compiler-internal LLVM consumer;
4. run them serially through the contract validated here and compare against one monolithic LLVM DSP across irregular blocks and fresh bitcode reconstruction;
5. only after that passes, execute independent ready regions on two workers and measure correctness plus actual p50/p99/max callback cost.

No Curlop production code changed. No merge performed.
