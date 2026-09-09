# Evolving retained groups — native extension after #34 and policy draft #37

This adds real boundary-changing DSP execution without overwriting #37's Python
policy experiment. The earlier retained-group engine and original retainer stay
unchanged. Two pinned configure-time adapters expose the old test entry point
and replace its numeric-index assumption with prepared member order.

Initial bounded groups are derived from effective-edge topology and the existing
PreparedSignalSchedule: simple serial chains, identically connected parallel
siblings, and complete simple feedback loops. LLVM remains the default from #34.
This is a host boundary policy, not a new Faust arithmetic partitioner or worker
pool. The prior selective ocpp optimizer is preserved, not rerun in this slice.

Later edits retain boundaries. Opening hidden ports or crossing them with new
cables refines only affected groups. Removing a cable does NOT greedily merge
units back. New modules start individual; explicit compact/replan reconsiders
grouping. Compiled group source is cached, bounded at512 entries. Compatible
routing edits reuse both that source and the actual running DSP instances.
Authored object identity matters even when a replacement has identical code.
An authored external feedback cable retains its delay object if unit indices
change around it; delays moving inside/outside a rebuilt kernel are reset.

## State contract

Unchanged compiled units remain the same instances. Splitting, compacting or
changing a compiled unit restarts that unit's state, reported as affected member
IDs. No arbitrary JIT state migration is claimed. Preserve-only mode rejects
such an edit before factory acquisition. Failed edits never install partial
plans. Plan retirement is on this headless caller, not a completed realtime
product reclamation protocol.

The live reference uses independently compiled individual modules and restarts
ONLY members identified by the checked program diff. It is not an uninterrupted
monolithic oracle for affected regions. Fresh whole-product fused oracles check
initial/final topologies. Two-island state tests separately establish whether
an unchanged group's pending impulse stays at1904 while the affected split or
replaced group's impulse restarts at6000. Those resets remain a limitation.

## Executed-test design (results pending at initial commit)

46 original native cases +20 earlier grouped-LLVM cases +26 new native cases.
16 traces cover18 real edits each: exposed input/output, actual endpoint changes,
node insertion/deletion/restore, source changes/undo, explicit regrouping and
routing after regrouping. Serial32 groups4/8, parallel32 groups4/8, feedback8,
control16, nonlinear16 and reversed graph IDs at48kHz/64 and128 frames. Additional
cases cover state, failed transactions, new/deleted cross-group feedback and
monotone refinement. State reset sets and complete emitted programs are retained
for independent verification, alongside full raw audio and timing inventories.

Numerical bounds remain1e-5 +1e-5*abs(reference), with no fit, normalization or
alignment. Planning, compilation/instance preparation, and first computed block
are separated. Full engine-side edit time includes caller work, not GUI or device
latency. Offline throughput arms share the same retained host/observer workload.

Not accepted here: optimal grouping costs, general multiport/CV/VM/MIDI/polyphony,
allocation-free planning, state transfer across a split, background compilation
while real audio runs, GUI/EngineSlot/Core Audio publication, multicore and
production plan retirement. No merge or private production modification.
