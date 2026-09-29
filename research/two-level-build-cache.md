# Curlop-style two-level Faust build/cache experiment

Status: research evidence, not yet production acceptance.

## Question

Can Curlop keep reusable user-authored Faust modules compiled/cached while also running an optimized fused clip build, and what happens to authoring latency when only graph topology changes?

This experiment was added after reviewing Curlop's existing `FaustRuntime` architecture rather than treating editable modules as uncached source. The baseline therefore reflects the established model:

- module source is content-addressed;
- LLVM factories can be serialized with `writeDSPFactoryToBitcode` and reconstructed with `readDSPFactoryFromBitcode`;
- authored module artifacts can already be cached independently of the graph using them;
- the fused clip/session graph is a separate derived program whose hash changes when topology changes.

The standalone probe uses the same libfaust LLVM primitives as that cache design. It is intentionally not a copy of Curlop's private code.

## Cases

For 4, 8, 16, and 32 serial Faust filter stages, the probe first warms bitcode artifacts for every authored module. It then measures:

1. first fused graph build (`graph_A_cold`);
2. persistent whole-graph bitcode reload (`graph_A_cached`);
3. a wiring edit that bypasses one module but changes no module source (`wire_edit_new_topology`);
4. returning to an already-seen topology (`wire_edit_return_cached`);
5. changing one authored module's source and rebuilding its module artifact;
6. rebuilding the fused graph after that source edit.

The entire sequence ran three times on GitHub's standard `macos-15` ARM64 runner, reported as `Apple M1 (Virtual)`, using the libfaust machine target reported by the installed Faust package.

Important: the persistent-cache cases below deliberately reconstruct a factory from serialized bitcode. Curlop's existing June architecture also keeps hot factories RAM-resident. A RAM-resident factory hit should avoid this bitcode reconstruction and reduce the revisit path essentially to instance/init/prewarm plus publication. The persistent-bitcode numbers therefore represent a colder cache tier, not the best achievable hot-path switch latency.

## Results

Median of three runs:

| stages | new wiring topology: fused compile | persistent graph bitcode load | return-to-cached-topology total* | edited module source compile | fused rebuild after module edit |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 118 ms | 58 ms | 61 ms | 32 ms | 117 ms |
| 8 | 132 ms | 61 ms | 52 ms | 26 ms | 156 ms |
| 16 | 235 ms | 86 ms | 81 ms | 31 ms | 226 ms |
| 32 | 444 ms | 172 ms | 159 ms | 32 ms | 473 ms |

`*` persistent bitcode tier, including bitcode factory reconstruction and instance init; not a RAM-resident factory hit.

Bitcode serialization itself was cheap in these cases: roughly 0.5-0.9 ms for the fused graphs. DSP instance creation/init was also small, generally around 0.2-0.4 ms. The dominant new-topology cost was factory compilation.

## Main finding

**Warming every authored module cache does not make a never-before-seen fused topology cheap.**

The wiring-edit cases reported zero module-cache misses, yet a new topology still incurred roughly 0.12-0.44 seconds of fused LLVM compilation on this graph family. That is expected: a changed cable topology describes a different whole Faust program. Existing per-module bitcode is still valuable for module reuse, versioning, fast module instantiation, export, and fallback execution, but libfaust does not automatically compose already-compiled module bitcode into a newly optimized whole-graph factory.

This matches Curlop's current source path: `GraphStateApplier::applyFromGraphState` calls `buildFaustGraphRenderer(state, ...)` for each supported topology revision, including connect/disconnect, and the unified renderer is prepared before publication.

## What the two cache levels are for

The evidence supports keeping both:

### Module cache

`module source -> module hash -> module bitcode / RAM-resident factory`

Useful for reusable user library modules, source editing/validation, module versioning, module/plugin export, fast independent execution, and an incremental authoring fallback if needed.

### Fused graph cache

`module identities + source revisions + topology + relevant compile options -> graph hash -> fused bitcode / RAM-resident factory`

Useful for the optimized live renderer. Previously seen clip/topology builds can be kept resident or reconstructed from persistent bitcode without recompiling source.

A module-cache hit and a graph-cache hit are not interchangeable.

## Product implication

Do **not** remove editable user modules. The performance issue is not editability; it is synchronously rebuilding a new whole-graph artifact during an authoring gesture.

The preferred authoring model to test in Curlop is:

1. mutate the authoring graph immediately;
2. keep the currently valid live renderer running;
3. build the new fused graph away from the audio callback and without blocking the UI;
4. prewarm it;
5. atomically publish it when ready;
6. cache the fused artifact by graph hash, retaining hot factories under a bounded residency policy.

The remaining UX question is what the user should hear during the build interval. Two candidates need production testing:

- **deferred-audio topology:** UI reflects the edit immediately while the previous optimized renderer continues until the new fused build is ready;
- **incremental authoring renderer:** reuse cached module factories to make the cable edit audible immediately, then replace that temporary modular execution with the optimized fused graph when compilation completes.

The second option has more complexity and should only be built if measured fused rebuild latency makes the first feel unacceptable. The present M1 numbers (hundreds of milliseconds on larger graphs) make that experiment worthwhile.

## Limits

This is a synthetic graph family and hosted VM timing is noisy. It does not yet include Curlop's Box-building time, control lowering, polyphony, feedback, taps, publication locks, or real UI dispatch. It therefore establishes the compiler/cache relationship, not the exact end-user stall duration in current Curlop.

Issue `CURLOP#254` owns the production-path instrumentation needed to break down the real edit from dispatch through graph planning, factory compile, init/prewarm, publication, and caller-visible/UI latency.
