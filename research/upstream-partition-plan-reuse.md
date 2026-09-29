# Upstream Faust partition-plan reuse

Date: 2026-09-08

## Question

Can Curlop reuse the recent upstream Faust loop-split/fusion planning machinery rather than inventing a new coarse-region partitioner?

## Findings

At pinned Faust commit `3d4baa164c0dd31b7d147617ed84a626495148dd`:

1. `SuperNodeGraph` is explicitly analysis-only. Its contract states that it decides the partition and emits nothing. It builds the finest legal partition from materialized signals, collapses strongly connected components, exposes inter-block dependencies, tests legal contractions, re-topologizes after contractions, and estimates operation counts.
2. `GroupPlan` is also explicitly analysis-only. It records hierarchical execution structure beside the signal graph and emits/re-writes nothing. It represents loop, atomic, adjacent, free and leaf groupings.
3. The profitable `-ls-fuse` path currently consumes those analysis structures from the old scalar/`ocpp` compiler path. The ordinary LLVM backend still selects `InstructionsCompiler` or `DAGInstructionsCompiler`; it does not consume `SuperNodeGraph`/`GroupPlan`.
4. `ocpp` is the only backend path in `libcode.cpp` that routes the recent loop-split machinery through `ScalarCompiler`; `-vec` on `ocpp` explicitly sets `gLoopSplit = true`. This confirms that the present limitation is backend integration, not that the partition representation itself is intrinsically C++ source generation.
5. Faust already exposes `-sng` / `--super-node-graph`. After the fusion/contraction phase and `retopo()`, the scalar compiler can dump the resulting super-node DAG to `<draw-path>-sn.dot`. This gives an existing observability hook for the post-fusion partition plan.

## Interpretation

The next Curlop step should **not** be a new partitioner and should **not** be another worker-pool implementation.

The upstream planner already contains the difficult semantic pieces we care about:

- legal partition boundaries through SCC/dependency analysis;
- legal contractions of the quotient DAG;
- operation-count estimates;
- cost-guided fusion;
- same-loop producer/consumer scalarization in the current emitter;
- an explicit hierarchical grouping representation.

The missing product path is narrower: make a profitable upstream plan usable by Curlop's LLVM/cached-bitcode runtime.

## Reuse options, in preferred order

### A. Expose the upstream plan before backend emission

Best architectural outcome. Add or use a compiler-internal/public export that serializes the post-fusion `SuperNodeGraph`/`GroupPlan` into a stable plan (JSON or equivalent), then let Curlop compile/execute coarse LLVM regions using its existing `RealtimeWorkerTeam`.

This minimizes duplicated graph theory and keeps the fusion oracle upstream-derived.

### B. Add an LLVM consumer of the same plan inside Faust

More invasive but cleaner if region emission requires internal signal trees that cannot be faithfully reconstructed outside the compiler. The planner remains shared; only an LLVM-region emitter is added.

### C. AOT C++ derived build

The current `ocpp -ls-fuse` path already demonstrates a large performance gain, so AOT C++ remains a viable fallback for desktop derived builds. It is less attractive for Curlop's current hot-edit/JIT architecture because compile time in the tested fusion pass is minutes, not interactive.

## Rejected direction for now

Do not design a Curlop-specific cost model from scratch. Do not parse generated C++ to infer regions. Do not treat `-sng` DOT as the final production API; it is a useful probe/observability format, but the preferred implementation is a structured compiler-plan export or direct shared planner integration.

## Next experiment

Use the pinned upstream compiler to emit the post-fusion `-sng` graph for the heavy workload and quantify:

- number of pre/post-fusion super-nodes;
- dependency depth and width;
- op estimates / fused-region size distribution where available;
- whether the profitable fused regions align with the 2-3 coarse independent banks that previously gave ~2x scaling on the M1 VM.

Then inspect whether each fused block can be represented as a self-contained signal subtree plus explicit inputs/state/outputs. If yes, prototype a structured plan export. If not, prototype an LLVM consumer inside Faust rather than recreating region semantics in Curlop.
