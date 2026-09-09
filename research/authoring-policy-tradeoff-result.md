# Authoring policy: CPU savings versus edit disruption

Date: 2026-09-09. Draft PR #40, extending #38.

**Executed checkpoint: all 232 native processes completed successfully. Decision: retain individual authored modules at freely editable boundaries as the conservative authoring default. Keep compiled groups for deliberately stable internals; do not automatically compact/refuse a running editable patch.** Grouping still offers useful processing savings, but the tested automatic policies trade them for collateral instance resets. This is an experimental policy recommendation, not a production merge or Core Audio acceptance.

## Exact execution and artifacts

Native-tested head: `073047f3704a44a5c5fd8fbe4dd574b1f79b74e7`.

Successful standard public hosted Mac run: https://github.com/curlcomplex/Faust-expr/actions/runs/34396676216 . Jobs: policy-0 `102618006975`, policy-1 `102618006699`, regression `102618006989`.

Each comparison block ran all five policies on the same three-core Apple M1 VM, macOS 15.7.9, Faust 2.85.9 / LLVM 22.1.8, Apple clang 17.0.0, at48 kHz with64/128-frame blocks. Two policy shards used separate VMs and the regression used a third. This is not the physical M1 Pro or an audio-device test.

| Artifact | ID | Original ZIP SHA256 |
|---|---:|---|
| Policy shard0 | 10122154275 | `cbe4247fa96f53881690ac79c7127b0dd37fe67e306f61b3b8ee6b3aafc55dcd` |
| Policy shard1 | 10122193195 | `7ac80761177cd70f7cb11074f6fcf6c4edb3f2d93b10ed5dcba368d90f755f5b` |
| Previous92 native cases | 10122173682 | `45a02ba66ea8e8ea976e264bc109244111d5a5ca19f8094e6de53a015f5987da` |

## Implementation and fairness

A pinned adapter changes only the initial/recompact boundary selector in the preceding engine. The FaustRuntime, compiled-group source builder, retained DSP executor, source cache, monotone boundary refinement, and existing experiments remain unchanged. Policies are individual modules; complete simple feedback islands only; adaptive non-feedback caps2,4,8. The capped policies retain the preceding exception that a complete supported feedback loop may exceed the cap. None accesses future targets or the edit trace.

120 new native traces cover serial32, parallel32, feedback8, control16, nonlinear16 and reversed-ID serial32, with two block sizes, two predetermined authored target positions and five policies. Each performs the same18 events. Full recorded authored topology/program traces are identical across the five policies in all24 matched comparison blocks. Twenty separate native two-island memory cases complete the140 new cases. The preceding92 native cases are rerun unchanged.

Each policy/trace uses a separate process to avoid cross-policy in-process factory reuse; candidate initial preparation precedes reference construction. Subsequent edits are warm sequential-session observations: old variants and reference factories can remain resident. Policy order and paired CPU-batch order are deterministically shuffled. All policies within a comparison block run on the same VM. No simultaneous competing policies share a rendering thread.

CPU is measured at initial state, after exposure, after the full edit sequence BEFORE compact, and after explicit compact. Each uses nine paired256-block batches against the same-host individually retained reference, with equal processing advancement and observer workload. The actual product-fused renderer remains a fresh initial/final audio oracle, not an unequal-work CPU denominator.

## Main result

CPU ratios below are geometric means of24 per-trace paired-trial median ratios to the individual-module reference. Shapes/block sizes/targets have equal weight; this is not a real-user workload or gesture-frequency model. Lower is better. A ratio near1 for the individual control exposes the ordinary timing/wrapper variation; small percentage differences are not a demonstrated significant gain.

| Policy | Initial CPU ratio | After editing, before compact | Wiring/exposure operations restarting surviving DSP members | Surviving-member reset observations |
|---|---:|---:|---:|---:|
| Individual modules | 1.023 | 1.012 | 0/240 | 0 |
| Feedback islands only | 0.791 | 0.969 | 12/240 | 82 |
| Cap2 | 0.838 | 0.997 | 52/240 | 194 |
| Cap4 | 0.659 | 0.810 | 60/240 | 298 |
| Cap8 | 0.744 | 0.858 | 68/240 | 470 |

Wiring/exposure covers events0–9 only: two external cable operations, two port exposures with their connections/disconnections, and actual endpoint rewire/undo. New processors, source changes, deletion/restore, explicit compact and no-op are excluded. Counts are controlled-test observations, not a prediction that25% of a user's gestures reset sound. Reset-member totals count repeated member/event occurrences, not unique modules.

Cap4 saves about19% of reference processing time after these edits, but60 of240 wiring/exposure operations reset existing DSP members. Cap8 has more resets and less aggregate post-edit saving here. Cap2 is not the hoped-for universal compromise: it offers essentially no aggregate post-edit CPU reduction, yet52 operations reset existing members. **Forty of those52 resets complete in under2ms** because suitable code is resident. Low latency does not prove state continuity.

After editing, geometric-mean ratios by shape illustrate the limits:

| Shape | Individual | Feedback-only | Cap2 | Cap4 | Cap8 |
|---|---:|---:|---:|---:|---:|
| Serial | 0.975 | 1.021 | 1.186 | 0.876 | 0.991 |
| Parallel | 1.032 | 1.022 | 1.101 | 0.657 | 0.644 |
| Control-bearing | 1.014 | 1.027 | 0.815 | 0.821 | 0.900 |
| Nonlinear | 1.013 | 0.991 | 1.091 | 0.952 | 0.905 |
| Reversed-ID serial | 1.001 | 1.010 | 1.182 | 0.869 | 1.022 |
| Feedback | 1.039 | 0.773 | 0.717 | 0.722 | 0.750 |

These are limited fixtures on VMs; the policy is not a universal cost optimum. For an initially intact feedback loop, feedback-only processing uses about0.241 of the individual reference time (~4.15x throughput). That ratio degrades to0.773 after opening/editing its internals. Recompaction returns it to about0.251, but resets the loop's existing state. This is a useful stable-unit optimization, not a free live transition.

## Engine-side edit times

Graph mutation + boundary/source/routing preparation + any required compilation/initialization + one completed computed block. Not GUI, device delivery, first perceptually audible change or callback deadline measurements.

| Policy | Median ms | p95 nearest-rank ms | Largest ms |
|---|---:|---:|---:|
| Individual | 0.300 | 0.724 | 2.176 |
| Feedback-only | 0.318 | 1.446 | 263.762 |
| Cap2 | 0.344 | 1.924 | 281.177 |
| Cap4 | 0.332 | 119.856 | 236.159 |
| Cap8 | 0.332 | 168.246 | 245.038 |

Each row contains240 wiring/exposure observations. Twelve slow feedback-island rebuilds occupy exactly5% of the feedback-only and cap2 rows; p95 therefore misses that last5% under the declared nearest-rank convention. The maxima and reset counts must remain visible.

Individual modules perform the tested input exposure in0.305ms median and endpoint rewiring in0.297ms; cap4 takes77.248ms and90.460ms respectively. Individual source replacement remains a different operation:34.774ms median,75.587ms maximum. Cached source undo is0.382ms median but restarts that changed module. Inserting the simple new processor takes13.238ms median,30.730ms maximum; existing modules are retained. Initial preparation across all policies/fixtures ranges206–2006ms and is not part of the fast wiring claim.

## Memory and regrouping

The20 two-island tests put a6000-sample pending impulse inside the processing chain, run4096 samples, then expose an internal input. Unchanged-island output must agree with uninterrupted product output. A freshly restarted negative control emits at6000 rather than the correct1904.

The potentially affected island preserves the impulse at1904 in all four individual cases and all four feedback-only cases (these state islands are nonrecursive, so feedback-only leaves them individual). Cap2 preserves two and restarts two; cap4/cap8 restart all four each. In every case the actual impulse agrees with independently derived invalidation. The untouched island survives for all policies. Passing these checks means the preservation/reset report is honest, not that every policy is seamless.

Explicit compact is separately measured. It restarts existing members in24/24 capped-policy traces and4/24 feedback-only traces, despite sub-millisecond median preparation with resident factories. The CPU saving can be recovered, but not invisibly while preserving arbitrary running history. All source-change events here occur after those regions have already been exposed; their zero collateral counts are not a general proof about changing code inside an untouched fused group.

## Independent verification

All232 native processes completed:140 new +46 individual-regression +20 grouped-LLVM regression +26 evolving-regression (two of the latter are planner stress, not audio-render cases). The37 laboratory tests and actual48,000-frame Faust probe also passed in each job; these are37 distinct tests, not111. Twelve new evaluator/adapter tests passed separately.

Independent NumPy/JSON/CSV readers, not importing the benchmark evaluators, verified **1,116 raw captures /15,586,304 float channel samples**. The140 new cases contribute820 captures /8,888,320 samples. Checks include finite values, exact case/event/block/timing inventories, sample-clock gaps for CPU batches, same authored traces across policies, source/program diffs, created/reused/factory/reset accounting, surviving/collateral-member separation, pending impulses and reset negatives, waveform comparisons and raw hashes. All2,160 new trace edit observations and8,640 CPU batch cells are retained. Maximum new waveform error is2.667307854e-6 under the unchanged1e-5 +1e-5*abs(reference) bound. Minimum event-reference peak is0.04996, above the required1e-4. No alignment, fitting, normalization or tolerance relaxation.

All160 archived source files are identical across the three jobs. Verified116 imported manifest entries,11 original benchmark/catalogue Git blob pins and24 new/prior implementation source hashes per policy shard (17 for the regression). Executable hashes are recorded by the runner; executable bytes are not exported, so no independent rehash of unavailable binaries is claimed.

## Failure retained and historical-scope correction

First run34395476412 at46e3448 executed all native children but its evaluator rejected all20 new feedback traces: removing/re-adding a forward edge in an already cyclic GraphState can introduce an additional declared one-sample delay. Undo restored endpoints but not the original delay configuration. The new fixture now saves/restores complete GraphState snapshots around endpoint and insertion edits. The strict evaluator, policies, DSP source, thresholds and case count were unchanged. Both failed shards remain failed; their results are not pooled into the accepted comparison.

A stricter re-audit found the same limited endpoint-only undo claim in the two unchanged #38 feedback regression fixtures. Their waveform checks validate the actual resulting graph, but do not prove the whole previous delay configuration was restored. The independent regression audit explicitly records those two differences. The new policy suite verifies full recorded graph restoration, including gain/pan/delay attributes. This correction does not change the imported product engine.

First artifact checksums are preserved in PR40 comment5607831124 and the checkpoint package. No failure or reset was normalized away.

## Decision and next implementation gate

Use retained individual authored units across freely editable cables. Preserve normal Faust/LLVM optimization within each unit. Larger compiled units remain appropriate where the boundary is intentional and stable, including closed instruments/effects and feedback islands whose internals are not being edited. Those integration choices follow from the measured trade-off; they are not a new validated general state-transfer mechanism.

Do not automatically compact a running editable patch merely to regain CPU. Treat grouping/recompilation with a possible local reset as a deliberate transition, or defer it until stopped/restarted, unless explicit state transfer is implemented and qualified. This is not a recommendation to abandon the preceding fusion research: it specifies where its gains can be used without reintroducing the editing problem.

The next gate is safe live publication/retirement and broader product semantics using this conservative policy, followed by measured multicore scheduling of stable units. No GUI/Core Audio, asynchronous deadline, full multiport/CV/VM/MIDI/polyphony, arbitrary split-state migration or multicore result is claimed here. No production default was silently enabled; main/master and private CURLOP code are unchanged, and nothing was merged.
