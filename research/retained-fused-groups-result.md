# Retained compiled groups: executed integrated-optimizer result

Date: 2026-09-09. PR #34, stacked on #33.

**PASS: retained compiled groups work, including the earlier upstream cost-guided fusion pass inside the retained units. Ordinary grouped LLVM remains the measured default: its processing-time median beat both C++ group variants in all 14 integrated comparisons.** This is a tested authoring candidate, not an enabled product path or merge request.

## Exact execution

Native-tested head: `82a297b22cb04ec7464bba42084b2a34886e001c`.

Successful public standard hosted macOS run: https://github.com/curlcomplex/Faust-expr/actions/runs/34357883309 . Job `102487073822`.

Apple M1 virtual machine, three cores, macOS 15.7.9, Apple clang 17.0.0. Installed ordinary runtime Faust 2.85.9 / LLVM 22.1.8. Experimental C++ compiler: exact earlier upstream commit `3d4baa164c0dd31b7d147617ed84a626495148dd`, Faust 2.88.0. This is not the physical M1 Pro.

Host executable SHA256 (identical in both phases): `213d00cd13cf3c79a401f6ec312d31c4d64af403a90ab359fd23c223de13c81f`.

| Artifact | ID | ZIP SHA256 |
|---|---:|---|
| Original 46 and grouped LLVM | 10106986454 | `37f9b929555cccee1f2b36e91ad57fd70ed2719b29986c238927b124d1d0f70a` |
| Integrated optimized groups | 10107195154 | `4b3dd49966d747c66314bf4846178b19c38a74283234ec8e562081572c2d8bec` |
| Retained first failed build | 10106211039 | `880921d77c393b362e624d3f1e19bc4076954a96a56eac8876d1de450d83f168` |

## Actual implementation

Fixed, explicit outer group boundaries contain four/eight authored modules; a complete eight-module feedback loop is one group. This is not a new automatic partitioner. Exposed-port wiring edits retain each group's running instance and replace routing. Internal computation changes replace the affected group only.

The original retainer is extended by a hash-checked optional native-factory adapter; its render method is byte-for-byte unchanged. Native library ownership is retained with its DSP instance. The imported source snapshot and original benchmark are unchanged.

Four measured arms use the same host routing and observer workload: individual LLVM modules, grouped LLVM, ordinary upstream `ocpp` groups, and the same groups compiled with `-ls-fuse -ls-sched cs2`. The last two use identical source/compiler/header/C++ flags (`-O3 -DNDEBUG -ffp-contract=off -fvisibility=hidden`), isolating that experimental pass. The full real product fused renderer is the independent audio oracle, not the processing-time denominator.

The compiler executable itself was built at `-O0 -DNDEBUG` to bound diagnostic setup. Generated DSP kernels still use `-O3`; source-generation timing is not a benchmark of a production-optimized compiler executable.

## Executed checks and independent verification

- **86 native benchmark processes passed:** original unchanged 46, 20 grouped-LLVM cases, 20 integrated four-arm cases. The 37 existing laboratory tests, real 48,000-frame Faust-generated probe build/render, and 12 new evaluator/adapter tests also passed. Evaluator checks are not DSP execution substitutes.
- Independent NumPy/CSV readers, not importing the benchmark evaluators, verified **326 raw captures / 9,197,824 float channel samples**, exact case/edit/block/timing inventories, negative controls, state and hashes. This includes the original regression captures.
- All 116 snapshot SHA256 entries, 11 original benchmark/catalogue blob identities, nine new implementation-file hashes and 37 exported kernel source SHA256/fingerprint pairs match. All 74 native library compilations (37 plain, 37 cost-guided) succeeded. Binary/generated-header hashes are recorded by the runner; their bytes were not separately included in the artifact, so the second audit does not claim to rehash unavailable binaries.
- All 16 same-backend fixed/irregular-block comparisons are byte-identical. New integrated cross-backend comparisons have maximum absolute error **1.565e-7**, below the unchanged predeclared `1e-5 + 1e-5*abs(reference)` bound. No fitting, time alignment, normalization or tolerance relaxation.
- There are 2,688 repeated edit/backend observations across the new phases, of which 1,792 use compiled groups. All legal exposed-port edits create/acquire zero DSP instances/factories and retain the same objects.

The candidate cable starts absent and enters the final group's input. Only the independent fused oracle predeclares it, with sample-scripted activation. A separately running no-edit negative must differ by more than `1e-4`, preventing a falsely reassuring comparison after long-chain attenuation. Hidden internal ports are rejected, not silently remapped.

## Processing cost: integrated phase, 128 frames / 48 kHz

Median of nine shuffled 512-block batches; **ns per stereo frame, lower is better**.

| Shape | Individual LLVM | Grouped LLVM | Plain C++ groups | Cost-guided C++ groups |
|---|---:|---:|---:|---:|
| serial32, groups4 | 190.1 | 192.8 | 381.2 | 201.1 |
| serial32, groups8 | 197.8 | 200.3 | 278.0 | 258.1 |
| parallel32, groups4 | 312.6 | 193.0 | 239.5 | 294.6 |
| parallel32, groups8 | 256.5 | 202.0 | 237.1 | 264.8 |
| feedback8, one group | 219.0 | 62.1 | 86.1 | 90.6 |
| control16, groups4 | 128.5 | 91.4 | 135.2 | 133.2 |
| nonlinear16, groups4 | 85.8 | 74.6 | 133.1 | 84.7 |

The actual earlier cost-guided pass is about **1.90x faster than plain C++** for serial32/groups4 and **1.57x** for nonlinear16/groups4 at128 frames. It wins against plain C++ in nine of14 configurations across64/128 frames. But parallel32/groups4 becomes approximately23% more expensive at128 frames and34% at64 frames. Most importantly, grouped LLVM beats both grouped C++ variants in all14 median comparisons. The old roughly1.8x result is not a speedup relative to these LLVM groups.

The feedback improvement is approximately **3.53x** versus individually retained modules at128 frames. This includes eliminating the old candidate's whole-graph sample-serial host execution by putting the declared loop inside a compiled group; do not attribute it entirely to the arithmetic fusion pass.

Serial grouping is inconsistent: the first phase showed approximately1.20x for groups4, but that benefit did not persist in the integrated128-frame comparison. Both phases and paired trials remain retained; no favorable-case-only aggregation. VM variation, limited fixtures and offline batching prevent a production callback/physical-machine performance guarantee.

## Engine-side edit time

Includes graph mutation, group/source reconstruction, routing preparation and one computed block. Does not include GUI dispatch, audio-device delivery or filter response/group delay.

| Group backend | Range of case medians, ms | Largest observation, ms |
|---|---:|---:|
| Grouped LLVM | 0.035–0.491 | 2.009 |
| Plain C++ | 0.036–0.425 | 1.173 |
| Cost-guided C++ | 0.036–0.587 | 1.691 |

The optimization is therefore compatible with fast retained-state external patching. Compiling source is not performed during these wiring edits.

## State and invalidation

The pending6000-sample delay lives **inside the first four-member group**. After4096 samples, its impulse arrives at1904 for all three grouped backends; a deliberate reset moves it to6000. Its source/group-exit waveform differs from uninterrupted product module4 by at most1.863e-9, not bit-identically.

The same group survives an unrelated external connection, an internal-wire gain edit in group2, and a member-code edit in group2. Each internal edit creates exactly one group and retains the other four nodes. The wire test changes **gain, not endpoints**. Changed-group state restarts; there is no state migration, arbitrary internal rewire, or concurrent product-publication claim.

Those simple LLVM group replacements took approximately11.5–23.1ms across the phases. Precompiled C++ variant loading/initialization took roughly0.37–0.91ms; this is not source compilation time. Per-unit code generation plus C++ compilation reached about1.50s with the experimental pass. All compilation is retained separately from hot-edit timings.

## Decision / limits

Retained compiled groups are the next authoring-engine candidate. **Ordinary grouped LLVM is the measured default; keep experimental C++ fusion selective and require it to beat the relevant LLVM version before adopting it.** Integration of the earlier pass was demonstrated, universal superiority was not.

Fixed test boundaries are not an automatic grouping policy. Dynamic regrouping, opening hidden ports, arbitrary internal topology, state transfer when boundaries change, CV/VM/MIDI/polyphony completeness, GUI/EngineSlot publication, qualified retirement, Core Audio and multicore execution remain open. No automatic live re-fusion or seamless conversion from a running fused monolith.

The first run34356854776 failed at C++ compilation on my EdgeEntry/EdgeEffective type mismatch. The adapter was corrected and the edit-signal negative strengthened before any native group pass. The reference renderer, original46-case suite and numerical thresholds were unchanged.

No private production source, main/master branch or personal machine was changed. Nothing was merged. This result document may be committed after the tested code head without changing the code identity above.
