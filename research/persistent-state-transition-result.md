# Persistent Faust state: optimized execution, immediate edits, and return to optimized code

Executed September 9, 2026 UTC (the final stages completed after midnight on September 10 in Europe/Berlin). Draft PR #42. This is an additive public experiment; no production default or main/master change and no merge.

## Result

**The central mechanism works in the tested domain.** Existing Faust-generated state objects can continue through optimized A -> editable B -> newly compiled optimized B without a state copy or reinitialization. The stricter cost experiment is broadly comparable to stock whole-Faust LLVM in aggregate, rather than falling back permanently to the per-module execution baseline. Actual changed master output is present before online optimization finishes.

**This is not complete real-time acceptance.** The normal-thread VM has many late scheduled callbacks, and the high-load parallel case exceeds the full block budget during editable execution. The implementation also requires pre-existing unchanged modules with an identical compiler-generated state schema. It is not arbitrary migration from an opaque already-running libfaust object, a completed product backend, or proof that every historical APG speedup is retained.

The user authorized progression through positive gates. The work executed the initial state/cost probe, tightened the output contract, executed continuous state transitions, then advanced to real background compilation and high-load tests. It did not stop at precompiled pointer swapping. High-load deadline failure is retained as the next constraint, not reclassified as success.

## What was implemented

Faust's installed C++ backend generates each unchanged authored module using `-os -ec`: a state object, a control routine, and a per-sample routine. A stable Bank owns those exact objects independently of any graph-specific compute function. An editable executor processes the current routing using them. A graph-specific optimized kernel statically composes the same Faust-generated routines in one sample loop; Clang/LLVM sees the bodies and can inline and optimize across the calls.

Both representations advance the same state, sequentially under one audio owner. Compilation reads code/schema and never mutates live DSP state. Optimized code can be replaced without replacing the state objects. Libraries and plans are retained until the rendering thread joins in this bounded prototype.

This is not a new stock libfaust state-transfer API. The actual installed LLVM backend rejects the tested external-memory/one-sample options. C/C++ generation supports useful paths, with the recorded `-it`/`-ec` restrictions. The selected implementation uses complete compiler-generated state objects rather than parsing `fRecN` fields or guessing raw struct layouts. Exact source, generated headers, compiler and flags participate in the schema fingerprint. No handwritten replacement oscillator/filter/delay DSP or compiler-source fork was introduced.

Independent inspection of all 18 optimized A/B LLVM-IR kernels found **zero remaining module-frame calls and zero indirect calls inside `ps_compute`**. The optimized form is not simply the old per-node host-call loop with a different name. However, it does not invoke all of Faust's whole-program front-end transformations across authored modules; the matched whole-LLVM cost gate specifically tests the consequence of that distinction.

## Exact accepted execution chain

| Stage | Native code/adapter head | Run | Outcome |
|---|---|---:|---|
| Initial four-plane diagnostic cost/state experiment | `976f3fc61e5d5cc18ed9559116938f12d71d9bba` | 34410082115 | 19 static + 19 precompiled continuous-transition cases passed |
| Independent initial evidence audit | auditor `2ff5b20dce33c51c27d6679c7be60e74aa270e4d` | 34411928417 | First diagnostic captures/source/binaries/metrics verified |
| Stricter output-only cost/state and unchanged native regressions | `008be1e5e9b8572ac3f9ef937e5cb391c9566b3c` (documentation-only rerun of native code at `b959be2d88adac4e8b7fc9c8ec8c3b8bdcbdd78c`) | 34414210244 | 46 original + 19 static + 19 continuous cases passed; subsequent live report had a metadata-type failure, so overall run is red |
| Independent output-only, original regression, actual host-output and generated-code audit | auditor `4d6831ee2141f795eb3653de9b46ca1747698a0e` | 34415821471 | All covered evidence checks passed |
| Corrected live report and full repeat of all real online cases | `6c0359889aa0898787ed14815c561217aec40731` | 34415439350 | 8/8 native cases and strict independent live audit passed; high-load deadline acceptance did not |

The final online repeat changes only two report values from an ambiguous literal zero to explicit `juce::var(0)`, plus the generated include selecting that header. Native DSP/kernel source, strict evaluator and numerical thresholds are unchanged. It reuses the exact verified kernel bytes from the accepted earlier cost/state stage. The previous stage's 46/19/19 results are therefore prerequisites at their own exact head, not falsely relabelled as rerun at the later head.

Together the stronger accepted gates cover **92 distinct native cases: 46 preserved regressions, 19 cost/correctness cases, 19 continuous-transition cases and 8 real online/stress cases**. The earlier four-plane experiment is a separate preliminary result, not added again to this count. The 37 laboratory unit tests and actual 48,000-frame Faust-generated toolchain probe also passed; eight generator/evaluator tests are separately identified and are not audio tests.

Platform: standard GitHub-hosted three-core Apple M1 virtual machines, macOS 15.7.9, installed Faust 2.85.9 / embedded LLVM 22.1.8, Apple clang 17.0.0. Audio rate 48 kHz; static/continuity blocks 64/128, plus 512 for BEN64; live cases use 128. These are not measurements on the owner's physical M1 Pro.

## Cost gate against unified Faust, not against a slower invented reference

The nine source/size combinations include the accepted BEN-008 gain/affine/second-order-filter/one-sample-delay family at 4/32/64 stages, plus the established serial32, parallel32, control16, nonlinear16, feedback8 and long-delay8 fixtures. The existing actual product renderer supplies a named-tap and host-output semantic oracle.

Four timed arms use the same graph and required output-tap planes: stock whole-Faust LLVM, stock whole-Faust generated C++, shared-state optimized code, and shared-state editable execution. Common peak/RMS observer work is identical. Bare DSP is separately timed, preventing common observer work from hiding a large kernel slowdown. Every configuration has eleven shuffled 256-block batches per arm/measurement.

The first diagnostic exported each module's input mix as extra outputs, which could constrain optimization. That result was not treated as sufficient. The accepted stricter experiment removes those extra outputs from every compiled timing arm. It retains complete actual product host/tap captures for independent checks.

Predeclared prototype screening limits were geometric-mean shared-optimized/whole-LLVM time <=1.25 for bare DSP and <=1.20 with observers, with no individual bare ratio >1.75, and all numerical cases passing. These are engineering go/no-go screening bounds, not a production performance promise.

| Ratio statistic across 19 stricter configurations | Bare DSP | With common observers |
|---|---:|---:|
| Geometric mean | 0.9821 | 0.9984 |
| Median of configuration ratios | 1.0638 | 1.0189 |
| Worst configuration ratio | 1.3242 | 1.2051 |

Lower is better. The aggregate is approximately equal to the whole-Faust reference; a typical configuration is somewhat slower, and some configurations are materially worse. This does **not** establish a uniform speedup, statistical significance, or zero state-interface cost. Two BEN configurations have large favourable differences and influence the geometric mean. The complete per-case and trial data are retained.

Representative bare-DSP medians, nanoseconds per stereo frame:

| Configuration | Shared-state optimized | Stock whole LLVM | Shared / whole |
|---|---:|---:|---:|
| BEN4 / 128 | 10.148 | 7.664 | 1.324 |
| BEN32 / 64 | 39.597 | 39.408 | 1.005 |
| BEN32 / 128 | 95.587 | 183.286 | 0.522 |
| BEN64 / 128 | 135.382 | 133.202 | 1.016 |
| BEN64 / 512 | 196.230 | 366.577 | 0.535 |
| Serial32 / 128 | 257.889 | 220.252 | 1.171 |
| Parallel32 / 128 | 163.113 | 190.406 | 0.857 |
| Control16 / 128 | 122.787 | 112.343 | 1.093 |
| Nonlinear16 / 128 | 126.325 | 118.752 | 1.064 |
| Feedback8 / 128 | 75.399 | 67.499 | 1.117 |
| Long delay8 / 128 | 8.353 | 9.427 | 0.886 |

Block-size-dependent cliffs in the BEN timings are visible in both stock LLVM and whole C++ and were not causally isolated. VM timing and generated-code/cache behaviour require repetition on physical hardware before interpreting the large favourable points as stable gains.

This measures steady-state topology A in each configuration. Topology B's execution is numerically verified and its actual post-compilation live cost is measured, but a separate equally powered B-versus-stock-B static timing matrix was not run. Do not infer every topology change has the same performance ratio.

The old migration's full APG-versus-Faust numbers are historical evidence and remain untouched. They are not multiplied by the ratios above. The current whole-Faust timing denominator has matching required taps and observers, but is not a replay of every production callback/VM/control cost from BEN-008. The existing product renderer is an independent semantic oracle, not the timed denominator.

## Continuous state transitions

All 19 cases execute:

`optimized A -> editable B -> optimized B -> editable A -> optimized A`

A and B use the same module programs and state schema; B connects an already-present marker to a previously unconnected last-module input. For this stage the A/B optimized kernels are precompiled, isolating state continuity from online compilation. The complete 24,576-sample timeline contains irregular block lengths and gain changes at sample offsets 3,001 and 10,003, not merely block-quantized UI changes.

Independent continuing stock LLVM module instances provide the reference; they do not use the candidate's frame functions. A no-edit reference proves the new cable makes an observable master-output difference. All ten cross-block-size partition comparisons are byte-identical. Maximum continuous-transition difference from the independent LLVM reference is **5.6252e-7**, under the unchanged `1e-5 + 1e-5*abs(reference)` bound. No gain fit, normalization or time alignment is used.

The long delay contains a pending impulse when the first edit happens after 4,096 samples. It remains due at absolute sample 6,000, i.e. **1,904 samples after the edit**, through both representation changes. The explicit restarted-state negative instead waits a new 6,000 samples. The oscillator/filter/tap waveforms also continue; this is not just a pointer-identity assertion.

The actual product host-output captures match the candidate's output within **5.6113e-8** over all 19 static cases. Named taps are checked as well.

## Real compilation while the edited graph runs

The next stage does not quietly reuse precompiled B for its first transition. After the B edit is submitted, a separate compiler process emits and compiles B's new static loop using the already generated module/state definitions. The audio owner immediately follows B's prepared routing using the same Bank. When compilation and loading finish, B's optimized compute function is adopted with that Bank still at its current sample state.

The test then submits an older A compile, supersedes it with B before completion, verifies rejection of that stale result, returns to resident B code, deliberately compiles invalid source, and rejects an incompatible state schema. The independent reference follows the recorded actual revision/sample timeline after capture, so reference computation does not add to the measured live workload.

All eight native cases and the unchanged strict independent live reader passed after the report-type correction. Maximum live waveform error is **5.7742e-7**. The new master signal is present before the compiler completes in every case. Neither a freshly initialized replacement nor a crossfade is used. State creation/copy counts are design assertions verified against executed source and output continuity, not a general allocation-hook or sanitizer proof.

Four normal-load observations, 128 frames / 48 kHz:

| Graph | Request to first completed editable-B block | Actual online compilation | Request to first completed optimized-B block |
|---|---:|---:|---:|
| BEN32 | 4.833 ms | 2,223 ms | 2,230 ms |
| Parallel32 | 16.281 ms | 2,828 ms | 2,843 ms |
| Feedback8 | 2.277 ms | 1,766 ms | 1,772 ms |
| Long delay8 | 10.761 ms | 1,311 ms | 1,321 ms |

These are individual controlled observations, not latency percentiles or speaker-delivery measurements. The completed editable block is tracked separately from the first master sample exceeding a numerical difference threshold in the delivery audit. The functional fact is stronger than a version flag: complete master/tap recordings establish B output before compilation ends.

Compilation is still seconds in this direct Clang prototype. The user does not have to wait for that compilation to obtain the new topology's computed sound, but the transient mode has a real cost. Caching/precompiled source routines avoid regenerating module DSP; optimizing compiler invocation/startup or replacing the AOT loop compiler with a qualified JIT is future work, not a result of this test.

## High-load result: important remaining failure

Four additional cases repeat the online test with independent Banks duplicated to target about 75% of the initial optimized block budget, with a bounded maximum of 96 copies. The achieved load differs from that target and is reported, not assumed. This is synthetic repeated-graph load, not full polyphony acceptance.

For 128 frames at 48 kHz the block budget is 2.667 ms. Recorded median compute-time fractions of that budget:

| Stress graph | Copies | Initial optimized A | Editable B during compile | New optimized B | Editable blocks whose measured compute exceeds budget |
|---|---:|---:|---:|---:|---:|
| BEN32 | 46 | 76.9% | 58.2% | 71.5% | 101 / 715 |
| Parallel32 | 63 | 84.8% | **161.9%** | 80.3% | **554 / 554** |
| Feedback8 | 83 | 51.0% | 88.7% | 39.0% | 199 / 496 |
| Long delay8 | 96 | 30.0% | 22.5% | 15.4% | 2 / 426 |

The parallel case is not usable at that load: median editable computation is **4.319 ms for a 2.667 ms budget**. The normal thread falls behind its synthetic schedule. Returning to optimized code reduces cost, but does not undo missed real-time delivery. The feedback and BEN cases also have significant over-budget tails even where medians fit. Uncontrolled VM scheduling, compiler contention and changing code/cache behaviour prevent a precise causal decomposition of every tail or the surprising BEN ordering.

Many synthetic schedule deadlines are missed even in lightly loaded phases because the VM thread is not a realtime Core Audio thread. Distinguish late wake-up from wall time spent in compute. Neither count is labelled an actual device xrun. A green numerical workflow does not establish glitch-free playback.

**The experiment therefore supports the state/code separation and bounded functional transition, but rejects a blanket assumption that deoptimizing a near-capacity whole patch is safe.** The next performance intervention should preserve optimized execution of unaffected work and limit fallback to the edited portion, or otherwise provide measured headroom. Reducing compilation latency shortens the exposure but does not alone make an over-budget editable block fit.

## Evidence, identities and failures

Core full artifacts (GitHub recorded ZIP digests):

| Artifact | ID | SHA256 |
|---|---:|---|
| First diagnostic native evidence | 10127213339 | `0ef3305ba86f49bceb3fc914a9386d1dd0d63a6493fcc812e4290c57f6f4a962` |
| Output-only native evidence, including preserved metadata failure | 10128768691 | `55f8e8677d891b444c9194e044c207249166c814884ed70c0c7beee4ef750fe5` |
| Successful output-only independent postflight | 10129021941 | `ae8da200368e26ce37f393c2f1ccd8f1051904f403f3f85606673ef056d8f032` |
| Corrected online full evidence | 10129046782 | `c8ab67c2df7fd07a5eb52ceef32badea2effdfb35346cdbcaa92fbd0ceda980c` |
| Corrected online compact summary | 10129032016 | `23f37a1eb73d6fc5969eb4d9bec867e43a481657169ba7b04e5a102516241c59` |

The audits independently reread sample arrays, exact case/block/event inventories, source and generated-header identities, emitted LLVM IR and retained native library bytes. Actual binary bytes are included here, unlike some preceding experiment artifacts. Source archives tie generated adapters to the executed heads. Original imported product files remain hash-checked and unmodified.

Counts distinguish complete numerical comparisons from ancillary scans. The output-only artifact contains 308 raw files, including 24 live recordings from the metadata-failed attempt. The independent static/state audit scans those for finite data/hashes but does not thereby accept the failed live stage. It explicitly compares the 19 static and 19 state cases. A separate reader verifies all 46 original-regression cases and 94 captures. The corrected eight live cases have their own full-array/timeline/state audit and newly recorded raw captures; their result is not pooled with the failed live attempt to inflate success counts.

Retained failures:

1. Initial capability commands without required `-it` failed. The backend restrictions are recorded rather than treated as a missing universal feature.
2. Required-output run `34413214170` built successfully and all 46 original children exited zero, but a strict old paced test missed intermediate revision 9. Postflight localized a 26.510 ms VM callback-start gap covering that revision's 21.817 ms pending interval. It remained a regression failure. A documentation-only unchanged rerun passed all 46; the earlier scheduling failure is not erased.
3. Run `34414210244` completed the new static/state/audio tests, but two live JSON zero counters serialized as empty strings because a literal zero selected a `const char*` overload. Its overall failure remains. An exact report-only adapter uses `V(0)` and reruns all eight live cases; the strict evaluator and DSP are unchanged.
4. An independent script named `inspect.py` shadowed Python's standard `inspect` module during NumPy import in run `34415555685`. Re-running the unchanged checks with Python's safe import path (`-P`) fixed that audit-tool failure; no data criterion changed.

## What is and is not established

Established in the bounded corpus: compiler-generated persistent module state, an actually inlined optimized graph kernel with competitive aggregate cost against a stock whole-Faust reference, state-preserving switches to/from editable routing, actual new-code compilation while the edited graph advances, and rejection of stale/failed/incompatible results. No reversion to APG or permanent per-module default is involved.

Not yet established: safe high-load transient execution, qualified production publication/retirement, realtime Core Audio scheduling, GUI-to-speaker latency, allocation/race sanitizer acceptance of the new backend, arbitrary new/deleted modules and state-schema changes, migration from an existing opaque whole-LLVM instance, arbitrary source-code changes, full multiport/CV/VM/MIDI/polyphony, iOS execution, sustained edit bursts, general new/deleted feedback histories, bitcode reload of this new ABI, or multicore scheduling.

All units are present before the tested cable edit. A fixed Bank layout is deliberately the first compatibility scope. More general ownership can be designed using the same state/code separation, but has not been demonstrated by this record. Table initialization and library lifetimes are controlled in the experiment; dynamically unloading old code while a live state object's class metadata still refers to it is not yet a qualified production operation.

Recommended continuation: treat this as the positive state-interface and transition prototype, not a ready product switch. Target localized optimized fallback/headroom and safe code/state lifetime management next, retaining the unified performance comparison and full state/timeline tests. Keep exact platform results and slower configurations visible. Do not restart an APG campaign or promote a reset-free but permanently slower representation as meeting the original target.
