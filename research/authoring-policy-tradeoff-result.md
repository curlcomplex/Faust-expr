# Authoring policy: CPU savings versus edit disruption

Date: September 9, 2026. Draft PR #40, extending #38.

**All 232 native processes completed successfully. Decision: keep individual authored modules at freely editable boundaries as the conservative authoring default. Keep compiled groups for deliberately stable internals; do not automatically compact or re-fuse a running editable patch.** Grouping still offers useful processing savings, but the tested automatic policies trade them for collateral instance resets. This is an experimental policy recommendation, not a production merge or Core Audio acceptance.

## Exact execution

Native-tested head: `073047f3704a44a5c5fd8fbe4dd574b1f79b74e7`.

Successful standard public hosted Mac run: https://github.com/curlcomplex/Faust-expr/actions/runs/34396676216 . Jobs: policy-0 `102618006975`, policy-1 `102618006699`, regression `102618006989`.

Three-core Apple M1 VMs, macOS 15.7.9, Faust 2.85.9 / LLVM 22.1.8, Apple clang 17.0.0; 48 kHz, 64/128-frame blocks. Two policy shards used separate VMs and the regression used a third. Every matched five-policy comparison stayed on one VM. These are not physical M1 Pro or audio-device measurements.

| Artifact | ID | Original ZIP SHA256 |
|---|---:|---|
| Policy shard 0 | 10122154275 | `cbe4247fa96f53881690ac79c7127b0dd37fe67e306f61b3b8ee6b3aafc55dcd` |
| Policy shard 1 | 10122193195 | `7ac80761177cd70f7cb11074f6fcf6c4edb3f2d93b10ed5dcba368d90f755f5b` |
| Previous 92 native cases | 10122173682 | `45a02ba66ea8e8ea976e264bc109244111d5a5ca19f8094e6de53a015f5987da` |

## What was changed and compared

A pinned adapter changes only initial/recompact boundary selection in the preceding engine. FaustRuntime, the group source builder, retained DSP executor, source cache, monotone refinement and previous experiments remain unchanged. The five policies are individual modules; complete simple feedback islands only; and adaptive non-feedback group caps of 2, 4 and 8. Capped policies retain the preceding exception that a complete supported feedback loop may exceed the cap. None accesses future edit targets.

There are 120 new native traces: six shapes (serial32, parallel32, feedback8, control16, nonlinear16, reversed-ID serial32), two block sizes, two predetermined authored target positions, and five policies. Each performs the same 18 events. Recorded authored topology/program traces match across all five policies in all 24 comparison blocks. Twenty separate two-island memory cases complete the 140 new cases. The preceding 92 native processes run unchanged.

Each policy/trace uses its own process, avoiding cross-policy in-process factory reuse. Initial candidate preparation precedes reference construction. Later edits are warm sequential-session observations; old variants/reference factories may be resident. Policies and paired CPU batches are deterministically shuffled. All policies in a matched comparison run on the same VM.

CPU is measured initially, after port exposure, after the full edit sequence BEFORE compact, and after explicit compact. Nine paired 256-block batches compare against the same-host individually retained reference, with equal sample advancement and observer workload. The actual product-fused renderer is a fresh initial/final audio oracle, not an unequal-work CPU denominator.

## Main result

CPU ratios are geometric means of 24 per-trace paired-trial median ratios to the individual-module reference. Shapes, block sizes and targets receive equal weight. This is not a real-user workload/frequency model. Lower is better. The individual control's deviation from 1 shows normal wrapper/timing variation; small differences are not established significant gains.

| Policy | Initial CPU ratio | After editing, before compact | Wiring/exposure operations restarting surviving DSP members | Member-reset observations |
|---|---:|---:|---:|---:|
| Individual modules | 1.023 | 1.012 | 0/240 | 0 |
| Feedback islands only | 0.791 | 0.969 | 12/240 | 82 |
| Cap 2 | 0.838 | 0.997 | 52/240 | 194 |
| Cap 4 | 0.659 | 0.810 | 60/240 | 298 |
| Cap 8 | 0.744 | 0.858 | 68/240 | 470 |

Wiring/exposure means events 0–9: external cables, input/output exposure and associated connections/disconnections, endpoint rewire and undo. New processors, source changes, deletion/restore, compact and no-op are excluded. Counts describe controlled tests, not a prediction that 25% of user gestures reset sound. Member totals count repeated member/event occurrences, not unique modules.

Cap 4 saves about 19% of reference processing time after editing, but 60/240 wiring/exposure operations restart existing DSP members. Cap 8 has more resets and less aggregate post-edit saving here. Cap 2 provides essentially no aggregate post-edit CPU reduction yet causes 52 resets. **Forty of those 52 resetting operations finish in under 2 ms in these warm traces. Low latency does not establish state continuity.** Factory acquisitions and instance creation are recorded separately; this is not an isolated cache-hit timing benchmark.

Post-edit CPU ratios by shape (geometric means across two blocks and targets):

| Shape | Individual | Feedback-only | Cap 2 | Cap 4 | Cap 8 |
|---|---:|---:|---:|---:|---:|
| Serial | 0.975 | 1.021 | 1.186 | 0.876 | 0.991 |
| Parallel | 1.032 | 1.022 | 1.101 | 0.657 | 0.644 |
| Control-bearing | 1.014 | 1.027 | 0.815 | 0.821 | 0.900 |
| Nonlinear | 1.013 | 0.991 | 1.091 | 0.952 | 0.905 |
| Reversed-ID serial | 1.001 | 1.010 | 1.182 | 0.869 | 1.022 |
| Feedback | 1.039 | 0.773 | 0.717 | 0.722 | 0.750 |

For initially intact feedback loops, feedback-only uses about 0.241 of reference processing time (approximately 4.15x throughput). After opening/editing the loop, the ratio becomes 0.773. Compact restores about 0.251, but resets the existing loop state. This is a useful stable-unit optimization, not a free live transition. Limited fixtures and VM noise do not establish a universal cost optimum.

## Engine-side edit times

Timing includes mutation, boundary/source/routing preparation, required compilation/initialization, and one computed block. It excludes GUI, device delivery, first perceptually audible change and callback scheduling.

| Policy | Median ms | p95 nearest-rank ms | Largest ms |
|---|---:|---:|---:|
| Individual | 0.300 | 0.724 | 2.176 |
| Feedback-only | 0.318 | 1.446 | 263.762 |
| Cap 2 | 0.344 | 1.924 | 281.177 |
| Cap 4 | 0.332 | 119.856 | 236.159 |
| Cap 8 | 0.332 | 168.246 | 245.038 |

Each row covers 240 wiring/exposure observations. Twelve slow feedback-island rebuilds occupy exactly 5% of the feedback-only and cap-2 rows; nearest-rank p95 omits that last 5%. Keep maxima and reset counts visible.

Individual input exposure takes 0.305 ms median and endpoint rewiring 0.297 ms; cap 4 takes 77.248 ms and 90.460 ms respectively. Individual source replacement remains different: 34.774 ms median, 75.587 ms maximum. Source undo takes 0.382 ms median but restarts that changed module. Inserting the simple new processor takes 13.238 ms median, 30.730 ms maximum, retaining existing modules. Initial preparation across all policies/fixtures ranges 206–2006 ms; it is not part of the fast-wiring claim.

## Actual memory and regrouping

The 20 memory tests place a 6,000-sample pending impulse in each of two processing chains, run 4,096 samples, then expose an internal input. The correct remaining delay is 1,904 samples; a freshly restarted negative control emits at 6,000.

The potentially affected chain preserves its pending impulse in all four individual cases and all four feedback-only cases (these particular chains are nonrecursive and therefore ungrouped by feedback-only). Cap 2 preserves two and restarts two; caps 4 and 8 restart all four each. Actual impulses agree with independently derived invalidation. The untouched chain survives under every policy. A numerical pass establishes honest preservation/reset reporting, not that every policy is seamless.

Compact restarts existing members in 24/24 capped-policy traces and 4/24 feedback-only traces, despite sub-millisecond median preparation with resident factories. Processing savings return, but not arbitrary running history. Source-change events occur after their regions have already been exposed; their zero collateral counts do not prove the same behavior for code edits inside an untouched fused group.

## Validation and provenance

All 232 native processes completed: 140 new, 46 retained-module regression, 20 grouped-LLVM regression, and 26 evolving regression. Two of the latter are planner stress rather than audio-render cases. The 37 laboratory tests and actual 48,000-frame Faust probe passed in each job; these are 37 distinct tests, not 111. Twelve new evaluator/adapter tests passed separately.

Independent NumPy/JSON/CSV readers, not importing benchmark evaluators, verified **1,116 raw captures / 15,586,304 float channel samples**. New cases contribute 820 captures / 8,888,320 samples. Checks cover full waveforms, finite data, case/event/block/timing inventories, sample-clock gaps for CPU batches, identical authored traces across policies, program diffs, instance/factory/reuse/reset counts, surviving versus collateral resets, pending impulses and negative controls. All 2,160 new edit observations and 8,640 CPU batch cells are retained.

Maximum new waveform error: 2.667307854e-6, under unchanged `1e-5 + 1e-5*abs(reference)`. Minimum event-reference peak: 0.04996, above required 1e-4. No fitting, alignment, normalization or tolerance relaxation.

All 160 archived source files match across the three jobs. Verified 116 imported manifest entries, 11 original benchmark/catalogue blob pins and 24 implementation source hashes per policy shard (17 for regression). Executable hashes are runner-recorded; executable bytes were not exported, so no independent rehash of unavailable binaries is claimed.

## Retained failure and historical correction

First run `34395476412` at `46e3448` executed all native children, but its evaluator rejected all 20 feedback traces: remove/re-add can introduce an extra one-sample boundary in an already cyclic GraphState. Undo restored endpoints but not the old delay configuration. The new fixture now saves/restores complete GraphState snapshots around endpoint and insertion edits. The strict evaluator, policies, DSP source, thresholds and case count were unchanged. Both failed shards remain failed and are not pooled into the accepted results.

A stricter re-audit found the same endpoint-only limitation in two unchanged #38 feedback regression fixtures. Their waveform checks concern the actual resulting graph, not proof that the old full delay configuration was restored. Their independent audit records both differences. The new policy suite verifies full recorded graph restoration, including gain, pan and declared delay attributes. No imported product code was changed. First artifact checksums are retained in PR40 comment5607831124 and the delivery package.

## Decision and next gate

Retain individual authored units across freely editable cables. Preserve Faust/LLVM optimization within each unit. Larger compiled units remain appropriate where the boundary is intentional and stable, including closed instruments/effects or feedback islands whose internals are not being edited. These integration choices are recommendations from the measured trade-off, not a new validated state-transfer mechanism.

Do not automatically compact or re-fuse a running editable patch solely to regain CPU. A grouping transition that can reset sound must be deliberate or deferred until an acceptable restart, unless explicit state transfer is implemented and qualified. The earlier fusion research remains useful; this result defines where its gains can be used without reintroducing the editing problem.

The next gate is safe live publication/retirement and broader product semantics under this conservative policy, then measured multicore scheduling of stable units. No GUI/Core Audio, asynchronous deadline, complete multiport/CV/VM/MIDI/polyphony, arbitrary split-state migration or multicore result is claimed here. No production default was enabled; main/master and private CURLOP remain unchanged. Nothing was merged.
