# Retained compiled groups: the next engine slice

This extends the exact #33 public native benchmark. The original 46 cases and
vendored snapshot stay unchanged. No private repository or personal machine is
needed. New work is confined to this experiment and its hosted workflow.

## Question

Can we retain compiled groups while editing, and actually use the earlier
upstream cost-guided fusion pass inside those retained units?

The outer boundaries are **explicit, fixed test contracts**, not a new automatic
partitioner. Serial and parallel groups contain 4 or 8 original authored modules;
a declared 8-module feedback loop is one unit. Internal source is reconstructed
from the original effective edges, including the original stereo pan/gain law.
Externally visible ports stay fixed. An attempted connection to a hidden internal
port is rejected, not silently rerouted. Parameter names are made member-specific;
only the controlled fixture label form is accepted.

`make_runtime.py` verifies the original retained-runtime Git blob and generates a
header with one optional native DSP factory seam. The routing/render method is
byte-for-byte unchanged. External library ownership lives on each running node.
The ordinary path still uses the existing FaustRuntime LLVM compiler/cache.

## Comparisons

1. The original individually retained modules.
2. The same modules compiled as retained groups through ordinary Faust/LLVM.
3. The same retained groups compiled with the earlier pinned upstream `ocpp`.
4. Those same groups with `-ls-fuse -ls-sched cs2` enabled.

The last two use exact upstream commit
`3d4baa164c0dd31b7d147617ed84a626495148dd`, generated C++, identical C++ flags,
`dsp` interfaces, group boundaries, source inputs and host routing work. They are
loaded as native libraries before the edit measurements. This preserves the
actual experimental C++ backend optimization rather than claiming that exporting
its region membership to ordinary LLVM reproduces all its code-generation gains.
There is no handwritten-DSP replacement. Offline compile times and library-load
preparation are reported separately; precompiled internal edits are not reported
as just-in-time C++ compilation latency.

Processing cost comparisons use the same retained host without per-module UI
observers in **all** measured arms. The full unchanged product fused renderer is
an independent audio oracle, not the performance denominator. Lower-cost missing
meter work is therefore not counted as a benefit of grouping. The per-module
feedback arm retains its prior conservative whole-graph sample-serial behavior;
putting the full feedback loop inside one compiled unit allows ordinary block
processing. That change is explicitly reported, not attributed entirely to fusion.

## Coverage and predeclared checks

20 processes per phase at 48 kHz / 64 and 128 frames: 14 graph/edit cases, two
state/invalidation cases and four block-partition cases. Seven shapes include
serial32 at group sizes4/8, parallel32 at4/8, feedback8, control-bearing16 and
nonlinear16. The 32 cable edits per matrix case affect an exposed **internal
group input**, not merely the output mixer. Candidate routes start absent; only
the separately compiled fused oracle predeclares them and uses sample-scripted
activation. Every legal cable edit must create/acquire zero DSP factories or
instances and retain every running object.

A delay lives **inside** the unchanged first group, not just a separate source.
After4096 prior samples, its6000-sample impulse must arrive at1904; a deliberate
whole-plan reset must move it to6000. The same group must survive an unrelated
external connection, an internal-wire edit in the second group, and a member-code
edit in the second group. Internal edits must replace only the second group.
Changed-group state is explicitly restarted; no migration is claimed.

All master and group-state captures use the unchanged cross-backend criterion
`abs(error) <= 1e-5 + 1e-5*abs(reference)`. No fitting, normalization or alignment.
Within each backend, fixed versus irregular call sizes must be bit-identical.
Nine shuffled512-block batches per backend measure offline processing time.
Case order is deterministic/shuffled. Full raw captures, source identities,
compiler commands, failed steps and per-event timing rows are kept.

## Limits

This slice does not auto-partition arbitrary patches, automatically split groups
when hidden ports are edited, migrate running fused state across new boundaries,
provide background live re-fusion, integrate GUI/EngineSlot/Core Audio, or enable
multicore execution. It is not a production memory-retirement protocol. These
limitations are distinct from whether retained compiled groups actually work.

Run `python3 experiments/retained-groups/run.py --phase jit --output evidence/groups-jit`
and then `--phase aot --output evidence/groups-aot` on the public hosted macOS
runner. The second phase builds the pinned compiler and native kernels. Twelve
local evaluator/patch checks are not DSP execution evidence. No merge requested.
