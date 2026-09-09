# Authoring boundary policy: processing cost versus edit disruption

Next checkpoint after PR #38. Select an authoring policy from actual native DSP measurements, rather than choose larger groups solely from a clean-patch CPU benchmark.

## Predeclared experiment

Five policies use the exact preceding LLVM retained/grouped runtime: individual authored modules; complete simple feedback islands only; adaptive maximum non-feedback group sizes 2, 4, and 8. The three capped policies inherit #38's exception that a supported complete feedback loop can exceed that cap. The feedback-only policy inspects the initial graph, not a list of future edits. Later maintenance is the same monotone splitting policy for every arm. No automatic re-merging on undo. Explicit compact is measured separately, not counted as ordinary cable editing.

A pinned source adapter changes only initial/recompact seed selection. It neither rewrites DSP nor substitutes a new scheduler. All imported product code, original renderer, source builder, existing experiments and previous tests remain unchanged. Optional C++ cost-guided fusion is not rerun; grouped LLVM was the measured default in #34.

120 new trace processes: six shapes (serial32, parallel32, feedback8, control16, nonlinear16, reversed-ID serial32), two block sizes (64/128 at48 kHz), two predetermined target positions, five policies. Each process performs the same18 authored events for its shape/target: wiring, port exposure, endpoint rewiring/undo, insertion/removal, source change/undo, deletion/restore, explicit compact and one no-op. Targets use authored IDs and never inspect the candidate groups. Complete authored graph snapshots are hashed to verify that all five policies received identical edits. Policies are established before the edit sequence is executed, without pre-exposing its targets.

20 additional native two-island memory cases test hidden input6 or7 with every policy/block size. Pending delay memory in an unchanged island must remain due at1904 after4096 prior samples; a full reset negative emits at6000. The affected island must agree with independently checked instance/program invalidation. Successful execution does not imply no resets; resets are a measured adverse outcome.

The unchanged preceding92 native cases execute as a separate regression job. Total requested execution:232 native processes. Evaluator unit tests are reported separately.

## Measurements and fairness

Candidate initial preparation precedes reference construction. Each policy/shape/target is a separate native process, avoiding cross-policy in-process factory sharing. Later edits are warm sequential traces; previous variants and reference factories may be resident. All five policies for a given shape/block/target run on the same hosted VM, in deterministic shuffled order. Two shards divide complete comparison blocks, never individual arms.

Engine-side edit timing includes GraphState mutation, boundary/source/routing preparation, any required compilation/instance creation, and one completed computed block. It excludes GUI/device delivery and perceptual/filter response. Raw processing cost is measured at four points: initial, after exposure, after the edit sequence BEFORE compact, and after explicit compact. Nine shuffled paired256-block batches compare the candidate to the same-host individually retained reference; both advance the same number of samples. No unequal product observer workload. Preserve all trials, ratios, host identity and failures.

Report routing/exposure events0–9 separately from adding processors, changing source, deletion/restore, compact and no-op. Count surviving authored members reset; newly created objects are not collateral. Source changes intentionally replace their own member, so collateral reporting excludes that member but still counts unrelated members in a replaced group. These are controlled workload counts, not a predicted distribution of real user gestures. CPU ratios across shapes, if aggregated, use stated equal weights and do not claim universal optimality.

All live waveforms are checked against independently compiled individual modules selectively restarted according to the emitted-program diff; they are not mislabelled uninterrupted references for rebuilt units. Fresh complete product-fused oracles verify initial and final graph topology. Every edit reference must have peak>1e-4. Keep the existing1e-5 +1e-5*abs(reference) criterion with no gain fitting, normalization, time alignment or relaxation.

## Decision rule and limits

Choose a conservative authoring default based on the CPU/reset/latency trade-off, not a single CPU winner. A policy that routinely resets unchanged processors during cable edits is not seamless authoring, even if the test passes. Measure the special benefit and first-opening penalty of feedback islands explicitly. No performance threshold is silently converted into a correctness gate.

This does not implement split-state transfer, asynchronous publication/reclamation, full multiport/CV/VM/MIDI/polyphony, actual GUI/Core Audio, or multicore. No production merge is requested. Standard public hosted macOS only, read-only test token, no private checkout or personal-machine changes.
