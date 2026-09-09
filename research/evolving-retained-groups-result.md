# Evolving retained groups: executed native result

Date: 2026-09-09. Draft PR #38, extending #34 and preserving policy draft #37.

**PASS: real changing group boundaries now execute in the native retained LLVM engine. All 92 native test processes passed. Compatible exposed-port edits retain their processors. Opening a hidden boundary rebuilds only affected units in the tested domain, but the rebuilt unit's state still restarts. This is not general split-state migration or a production live-patching acceptance.**

## Exact execution

- Native-tested head: `4f410c6a72bf38affdd23c49c7e9cd0eeaab40f9`.
- Branch: `research/evolving-retained-groups`; PR https://github.com/curlcomplex/Faust-expr/pull/38 .
- Successful standard public hosted Mac run: https://github.com/curlcomplex/Faust-expr/actions/runs/34373980877 ; job `102542043887`.
- Artifact `10113622993`, `evolving-groups-34373980877-1`.
- Original artifact ZIP SHA256: `bbd3a87a3c1049ec31195f5280a25b99439b0b6a1c2df57564bb4fe542e6c98c`.
- Executed-source archive SHA256: `67ef2fcd6a193a9113024ee6637c392b811e50775dcfc1628f1fe70e46f21b7f`.

Machine: three-core Apple M1 virtual machine, macOS 15.7.9, installed Faust 2.85.9 / LLVM 22.1.8, Apple clang 17.0.0. 48 kHz, 64/128-sample blocks. NOT the physical M1 Pro. Runner-recorded adaptive executable SHA256 `1a7132c1e7f56a6b0879b4da785a3526d9146bcf4fe48bc99207f63a3a3933dd`; original regression executable `9733fb88f34789e5074b6d3ec3ff74051f0e37b504e83415b120fd1a699456ee`. Executable bytes are not in the artifact; the independent audit does not claim to rehash unavailable binaries.

## Actual engine extension

Initial boundaries are derived from effective graph topology and existing PreparedSignalSchedule: bounded serial chains, identically connected parallel siblings and supported complete simple feedback loops. Later edits keep existing groups and refine boundaries only where exposed/crossing ports require it. New nodes initially execute individually. Disconnect/undo does not greedily merge fragments; explicit compact/replan can reconsider grouping.

Example: opening member 7's input changes `[5,6,7,8]` into `[5,6]` and `[7,8]`, while another group `[1,2,3,4]` remains the same running object. Once that boundary exists, compatible cable changes reuse the processors.

The implementation reuses the previous group source builder, retained DSP executor, FaustRuntime and feedback machinery. A bounded 512-entry source cache avoids regenerating unchanged compiled-group source. Internal signal layout is part of the cache/wire identity because it changes the gain law. Authored object identity matters even when replacement code and index are equal. External feedback history follows authored cable identity when compiled-owner indices change. No arbitrary internal state transfer is claimed.

Two pinned configure adapters expose the old test entry point and replace its numeric-ID ordering assumption with prepared member order. Original imported sources and the preceding experiments are unchanged. Grouped LLVM remains the measured default from #34; the optional experimental C++ fusion pass is preserved there but not rerun in this slice. No new arithmetic partitioner or worker pool.

## Validation executed

**92 native processes:** original unchanged retained-module 46 + preceding grouped-LLVM 20 + new 26. Of the 26 new cases, 24 render audio and two stress native layout refinement. These are not 92 audio-device tests.

Sixteen traces each execute 18 transitions (288 events): serial32 caps4/8, parallel32 caps4/8, feedback8, control16, nonlinear16, and reversed numerical IDs, each at64/128 frames. Events cover exposed connections, hidden input/output exposure, actual endpoint rewires/undo, insertion/removal, source changes/undo, member deletion/restoration, explicit regrouping and no-op reuse. Topology snapshots prove endpoint changes, not merely gain changes. Independent checks confirm endpoint undo restores routes without re-merging the opened groups or creating processors.

Separate two-island state and identity tests, failed-preparation tests, added/deleted external feedback and progressive boundary refinement are included. Twelve rejected-edit checks leave the active plan unchanged and subsequent computed audio matches the continuing reference. Strict no-rebuild mode rejects a required replacement before factory acquisition. Other negatives cover deliberately malformed undeclared cycles, hidden polyphony, invalid Faust source, absent pins and duplicate identities.

## Engine-side edit time

Synchronous caller-side graph mutation + group/source/routing preparation + one completed computed block. Not GUI dispatch, device latency, first perceptually audible change, background-build deadlines or real-time publication. These are sequential warm-session traces: reference factories and earlier variants can be resident. Compilation is included when needed, but this is not an isolated fully cold-cache compilation benchmark.

| Operation | Observations | Median ms | Min–max ms |
|---|---:|---:|---:|
| Connect/disconnect at exposed ports | 96 | 0.313 | 0.092–5.980 |
| New port exposure requiring replacement instances | 28 | 71.089 | 43.044–209.882 |
| Actual endpoint rewire | 16 | 25.875 | 0.426–140.578 |
| Undo endpoint rewire | 16 | 0.335 | 0.167–0.885 |
| Member-source change | 16 | 39.027 | 28.023–43.693 |
| Undo member-source change | 16 | 0.418 | 0.227–0.802 |
| Explicit compact/regroup | 16 | 0.608 | 0.385–56.834 |

All 96 exposed-port routing operations created zero DSP instances and acquired zero factories. A separate 16 no-op reuse checks also did so; they are not counted as cable changes. There were 32 port-exposure events, of which 28 actually required new instances; four already exposed singleton members. Aggregates mix defined shapes and operations, with one observation per operation per trace, not population latency estimates.

Endpoint rewires include both routing-only and further boundary-changing operations. Cheap cached source undo still creates a freshly initialized affected instance; it does not restore that unit's earlier running history. Compact may also reset units despite resident code.

## State: preserved outside the changed group, reset inside it

The two-island tests put state inside both compiled groups. After 4096 samples of prior execution, a pending 6000-sample impulse remains due at1904 in the untouched group. After splitting or authored-identity replacement of the other group, that affected unit instead emits at6000. Uninterrupted product references emit at1904. The untouched group pointer is unchanged and its waveform differs from the actual uninterrupted product tap by at most **7.451e-9**, not bit-identically. In these four tests, the affected original member set is exactly5–8.

Live trace references use independently compiled individual modules selectively restarted according to the checked emitted-program diff. They are NOT falsely uninterrupted references for rebuilt regions. Fresh complete product-fused oracles independently verify initial/final topology, and the state-island tests independently distinguish preserved history from resets.

New/deleted exposed external feedback creates no instances/factories in its two dedicated cases. While the cable exists the host uses its conservative sample-by-sample fallback. This slice does not optimize that path or prove preservation for arbitrary coupled feedback.

## Offline processing cost after evolving edits and compact

Both arms use the same retained host/observer workload. Seven shuffled512-block batches per backend. At128 frames, median nanoseconds per stereo frame (lower is better):

| Shape | Adaptive groups | Individual modules | Adaptive / individual |
|---|---:|---:|---:|
| serial32 cap4 | 166.15 | 200.98 | 0.827 |
| serial32 cap8 | 202.61 | 198.93 | 1.018 |
| parallel32 cap4 | 186.22 | 377.13 | 0.494 |
| parallel32 cap8 | 154.63 | 235.01 | 0.658 |
| feedback8 | 185.71 | 280.87 | 0.661 |
| control16 cap4 | 80.58 | 196.26 | 0.411 |
| nonlinear16 cap4 | 81.15 | 93.78 | 0.865 |
| reversed-ID serial32 cap4 | 164.93 | 202.27 | 0.815 |

Adaptive grouping was cheaper in14/16 configurations across64/128 frames. Serial32/cap8 was approximately2.7% more expensive at64 and1.8% at128. Ratios range0.411–1.027. The topology policy is bounded/heuristic, not a universal cost optimum. This is the evolved graph after compact, not the preceding pristine fixture or a controlled physical-machine callback-tail measurement.

## Independent evidence audit

Separate NumPy/JSON/CSV readers, not importing native evaluators, verified all three suites:

| Suite | Native cases | Raw captures | Float channel samples |
|---|---:|---:|---:|
| Original retained regression | 46 | 94 | 3,160,576 |
| Previous grouped LLVM | 20 | 82 | 2,260,992 |
| New evolving-boundary suite | 26 | 120 | 1,363,968 |
| Total | **92** | **296** | **6,785,536** |

Checks include exact case/block/edit inventories, full waveforms, finite samples, complete topology/emitted-program snapshots, recreation/reuse/reset accounting, endpoint undo, state impulses, negative controls, timing consistency and all raw hashes. Maximum new numerical difference **2.146e-6** is below the unchanged `1e-5 + 1e-5*abs(reference)` bound.

Source verification covers116 imported snapshot SHA256 entries,11 original benchmark/catalogue blob pins and17 new/preceding implementation hashes, plus known published core-file blobs. Artifact and source archive hashes match.

Final trace stimuli use analytic wire gains derived from the existing center-pan law to prevent long-chain attenuation hiding errors. Every event reference peak must exceed1e-4; the independent minimum observed peak was **0.0480**. This is stronger input qualification, not recording normalization, gain fitting, alignment or tolerance relaxation. The original66 native regression signals are unchanged.

Separately,37 original laboratory tests, the real Faust-generated48,000-frame probe build/render, eight preceding Python policy checks and14 new evaluator tests passed. Python checks are not DSP execution substitutes.

## Failures retained

Initial run34370917128 at`a80d872` passed66 original cases and24/26 new cases. Two malformed-cycle negative fixtures failed because normal GraphState ingress automatically inserted a valid one-sample delay. The fixture alone was corrected to strip that boundary in its deliberately invalid copy. Production ingress and numerical thresholds were unchanged. Initial artifact10112355771 SHA256`457dcaaa17306604cfdc23108d270cf95e2308e75582aa818ba1e75eb54e2315`.

Cache layout identity and stronger trace signal qualification were added before the final accepted run. Dedicated experiment concurrency cancelled superseded runs34372728004 and34372947398. The former's native step had completed successfully but its overall run is cancelled; neither is used as final accepted evidence. No other experiment's queue or physical runner was cancelled.

## Decision / remaining work

A real evolving patch can retain compatible compiled units and rebuild affected units rather than the whole graph. Ordinary grouped LLVM, retained-instance state, source caching and actual changing topology now coexist in the tested implementation.

The unresolved hard gate is first-time boundary opening: tens to roughly200ms preparation and a restart of that affected compiled unit. Seamless split-state transfer, safe background preparation/publication, production retirement and actual GUI/Core Audio qualification are not done. Finer/pre-exposed boundaries are a possible mitigation to evaluate, not a completed universal solution.

Optimal automatic grouping, complete multiport/CV/VM/MIDI/polyphony semantics, arbitrary feedback, automatic live re-fusion/history transfer and multicore remain outside acceptance. No main/master or private production source was modified. Nothing merged or enabled in CURLOP. This document is a documentation-only commit after the tested code head.
