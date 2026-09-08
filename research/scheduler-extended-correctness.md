# Extended scheduler correctness — 2026-09-07

## Result and decision

**The existing atomic queue candidate passed this broader, finite correctness matrix.** The same candidate plus the previously tested block-completion barrier also passed. This permits moving to controlled physical-machine performance experiments, not enabling the scheduler by default in Curlop production.

Both GitHub jobs completed successfully. Independent inspection of the downloaded artifact summaries and every comparison line found:

| Runtime | Clean child exits | Scheduled exercise cases | Scalar exercise controls | Audio comparisons |
|---|---:|---:|---:|---:|
| Existing atomic queue, no block barrier | 31/31 | 26/26 | 3/3 | 114/114 bit-identical |
| Atomic queue plus existing block barrier | 31/31 | 26/26 | 3/3 | 114/114 bit-identical |

Each job's 31 children include two factory-preparation children which do not render audio; do not describe all 62 children as render stress runs. The total is 52 scheduled exercise cases, six scalar exercise controls and four preparation processes. Each job compares 7,274,496 float sample positions, including repeated references, with maximum measured error zero. These are not millions of independent statistical trials or an uninterrupted album-length soak.

The 114 comparisons per job are 101 scheduled-within-backend comparisons and 13 scalar-within-backend controls. They are **not native-C++ versus LLVM comparisons**. The separate native/JIT numerical drift identified in the preceding investigation remains unresolved; no threshold was changed to obtain this result.

## Existing implementation used

This continues `scripts/scheduler_bitcode_probe.cpp`, `scripts/patch_scheduler_arm64.py`, `run_scheduler_liveness.instrument_runtime` and `scheduler_liveness_queue_controls.queue_control` on the existing research branch. No new partitioner and no Curlop/private source were introduced.

The no-barrier runtime source is **byte-for-byte identical** to `scheduler-untraced.cpp` in the preceding passing atomic-queue artifact. Its SHA-256 is `d64bded629532da81d31a127da6aebac29b93025f64c265e6e25f4c40e9340f1`. The barrier runtime source SHA-256 is `3b05ea0b3eba7e9be43cc2a4d26c208fd67bdeef6d235119398f5292e3e95371`.

The original 64-voice oscillator/filter probe is extracted verbatim. For distinct-factory isolation, a second graph changes only the final voice multiplier from `0.01` to `0.007`. Same-factory concurrent instances use different starting positions (the second is advanced by 37 frames) so independent instance state is exercised. These deliberately small variations extend the reproduction rather than replacing it with a new benchmark corpus.

## What was checked

Each of the following runs at configured participant counts 1, 2 and 3, including the caller:

- Fixed 256-frame processing, compared against an isolated one-participant scheduled reference.
- A repeating irregular positive-block pattern: `1,3,7,15,16,31,32,33,63,64,65,127,128,129,255,256,257,511,512,513`. It includes lengths below, at and above the generated 32-frame vector size.
- Partition invariance: that irregular schedule compared with fixed 256-frame processing of the same total samples.
- Eight create/init/render/destroy cycles using a resident factory in one process.
- Eight load-bitcode/create/init/render/destroy/release-factory cycles in one process.
- Two instances of the same factory rendered concurrently on separate host threads, with independent state and buffers.
- Two different factories rendered concurrently, with independent state and buffers.
- Eight outgoing instance/factory retirements while a surviving graph continues rendering without resetting its state; survivor output is compared across all epochs.

The irregular-block test also runs at 44.1 kHz and 96 kHz using three participants; the main matrix uses 48 kHz. Sample rate is set at instance initialization, not changed during a running callback.

Every compute call checks prefix and suffix output guards, including the unused part of the maximum-size allocation. Requested samples must be written, finite and not equal to the sentinel. Full returned audio arrays are compared in the child, with the existing `1e-7` absolute threshold and an independent bitwise-equality report. No out-of-bounds sentinel change, unwritten sample, nonfinite sample, numerical mismatch, crash or timeout was reported in this matrix.

All six scheduled retirement cases recorded 8/8 retirements beginning while the other host render operation was still active: 48/48 total. This is a host-operation overlap observation, not a measurement that two physical cores were executing instructions at the exact same instant. It tests off-render-thread retirement of an independent factory, not callback-safe destruction of the currently rendered instance.

## Cache and lifetime controls

A separate preparation process compiles each A/B factory, writes the bitcode string, explicitly releases the factories and exits. Every exercise process loads those cached artifacts. The original support `.ll` is physically renamed away for the entire exercise matrix; children receive no scheduler-module environment variable or `-L` option. They therefore cannot accidentally use the writer's in-process factory cache or reopen the original support IR file.

Within an exercise process, one-participant reference instances are rendered and destroyed before setting the tested participant count and creating the test instances. Factory APIs are initialized with `startMTDSPFactories()`. Instance creation, cache loading and factory release remain under host control; independent compute calls run concurrently only in the overlap cases. All accepted children explicitly destroy their instances and release all their factories before `stopMTDSPFactories()` and normal exit. A success marker without exit code zero is not accepted.

Worker-creation return values are checked using the existing candidate's diagnostic checks. The VM reports three CPUs; the stock pool allocates two helpers per instance even for one active participant. This suite does not instrument task utilization or prove that every requested helper materially contributed. The previous traced investigation established helper participation at two participants; real scaling tests must measure it again at each configuration.

The driver allocates capture vectors, checks guards/samples, and emits diagnostic output. **Its elapsed time is not usable as audio-callback performance evidence.**

## Scope still not covered

This is a finite standalone correctness pass, not a proof of race freedom or complete production acceptance. In particular:

- The generated shared vector index and other scheduler shared state, queue epoch transitions/reuse, platform timing globals and lifetime synchronization still require a full memory-model/sanitizer review. Making the queue counter atomic does not by itself prove every shared variable safe.
- No ThreadSanitizer/AddressSanitizer run is claimed here. Sentinel checks are useful but are not general memory-safety instrumentation.
- The DSP has no external inputs or UI zones. Production controls/events, feedback families, taps, the actual graph builder and its compute segmentation remain to test in Curlop's renderer.
- Zero-length and invalid-size callbacks are not included. The smallest tested positive callback is one sample. No sample-rate changes during an active instance are tested.
- CPU counts greater than three, genuine heterogeneous physical-core scheduling, real audio-device deadlines, Audio Workgroups, sustained thermal/load behavior and long-duration soak tests remain open.
- No native-C++ versus JIT comparison is newly accepted. Earlier cross-backend numerical drift remains a separate issue.
- Neither success of the barrier nor success without it proves the barrier unnecessary. Its necessity, synchronization contract and performance need separate evaluation.

These limitations do not require another product rewrite. The next performance baseline remains the actual fused Curlop renderer with its existing cache/build architecture and matching bundled Faust/LLVM toolchain.

## Reproduction and evidence

- [Completed run 34142263087](https://github.com/curlcomplex/Faust-expr/actions/runs/34142263087)
- Source head: `77bcdfb1b32dd33d790cb5126456db4b9d3698f6`
- Actual PR merge-test SHA: `beb3b13a752edc0bd2cf9e6fea36a1b1440d4dd8`
- Environment: GitHub-hosted macOS 15 ARM64, Apple M1 virtual machine, reported physical/logical CPUs 3/3.
- Faust 2.85.9; LLVM and Homebrew Clang 22.1.8.
- Host flags: `-std=c++17 -O2 -g -fno-omit-frame-pointer`.
- Original DSP SHA-256: `f3593f74b0614bd51cdc34a28ce732e3a84763e6ef56d53f41725adf091ca8d9`.
- Installed upstream scheduler SHA-256: `85e01152c0e507792a5188bcaf0ef04e4388182690739d509402e2bfe8cf3a4b`.
- The previously recorded `arm64-apple-darwin24.6.0` versus `arm64-apple-macosx15.0.0` target-triple warning remains present in the evidence.

| Artifact | ID | ZIP SHA-256 |
|---|---:|---|
| Atomic candidate | 10026483324 | `ce5340a80b1bc85f0919f7f5f48be9333c8746cfc1bd2fe1102f73737f62356d` |
| Atomic plus barrier | 10026497817 | `20fa6beb1a160d3aacd799a49371f2d4a07da75a07f832eed6706177099f196e` |

Artifacts retain commands, exact runtime copies, IR, cached bitcode strings, per-child guard/comparison summaries, environment and provenance. Full audio is retained by this driver on a numerical mismatch; successful audio is checked in memory and summarized rather than uploaded. The ZIP hashes and every comparison line were checked after download.

Files added:
- `scripts/scheduler_correctness.cpp`
- `scripts/run_scheduler_correctness.py`
- `.github/workflows/scheduler-correctness.yml`

On the matching diagnostic toolchain:

```sh
CORRECTNESS_THREADS=1,2,3 python3 scripts/run_scheduler_correctness.py
```

The self-hosted physical-Mac setup guide and inert private-control-repository template are in `research/self-hosted-mac.md` and `research/self-hosted-correctness.yml.example`. No runner was installed/registered, no test executed on the owner's Mac, no Curlop production file changed, and no PR merged.
