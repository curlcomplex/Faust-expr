# Adaptive retained groups: boundary maintenance checkpoint

This is the next slice after retained compiled groups (#34). It uses the exact
executed retained-group machinery as the runtime, and tests **boundary policy and
edit handling**, rather than introducing another DSP renderer.

## Goal

A real patch cannot assume that every useful compiled group remains fixed forever.
This checkpoint asks whether the host can preserve fast/stateful groups when an
edit is compatible, and split/rebuild only the minimum affected area when an edit
crosses or exposes a hidden boundary.

The policy is deliberately deterministic and conservative:

- Begin from explicit groups of four modules (or the whole declared one-sample
  feedback SCC).
- External edits at exposed group ports retain all group instances.
- Parameter/gain changes internal to one group rebuild that group only.
- A new connection to a hidden internal member **opens that member as a boundary**:
  split the old group into contiguous left/member/right units. Unchanged groups
  elsewhere retain their running objects and state.
- A connection spanning members of two existing groups never merges them during
  the interactive edit. It remains an external route between stable units.
- Automatic merging/re-fusion is explicitly deferred; this avoids moving state
  merely because an optimizer sees a theoretically profitable larger region.
- A boundary may only be closed again by an explicit cold/rebuild operation in
  this slice. Hot undo therefore retains the opened boundary instead of silently
  re-fusing and resetting state.

This is a host policy, not a Faust compiler partitioner. It is intentionally
biased toward continuity and edit latency. The measured ordinary grouped LLVM
backend from #34 remains the default execution backend.

## Native tests

The benchmark extends the same public macOS environment and actual imported
CURLOP GraphState/FaustRuntime/renderer reference. It exercises serial32,
parallel32, control16, nonlinear16 and feedback8 at 48 kHz with 64/128-frame
blocks.

For each applicable graph it executes:

1. exposed external connect/disconnect (no rebuild),
2. hidden-member connect that forces a group split,
3. undo of that connection while keeping the opened boundary,
4. an internal gain/source edit after the split,
5. a cross-group connection that must not trigger an eager merge,
6. state continuity in an unaffected delay/oscillator group,
7. fixed/irregular block equivalence,
8. negative checks for illegal feedback cuts and unsupported partial parallel
   fan-in/fan-out.

Every transition records old/new boundaries, retained/rebuilt instance identities,
factory acquisitions, mutation/plan/first-compute time and full raw audio. A
separately compiled fused oracle verifies audio. A deliberate cold-regroup
negative is allowed to reset state and must differ from the retained path.

The acceptance target is not a universal optimal partition. It is: edits that do
not require changing a boundary remain sub-millisecond engine operations; opening
a hidden boundary rebuilds only the old group, never unrelated groups; unaffected
state survives; and the resulting processing cost remains bounded relative to
individual retained modules and the previous fixed-group baseline.

No GUI/Core Audio, production retirement, CV/VM/MIDI/polyphony completeness,
multicore execution or background auto-merge is claimed by this checkpoint.
