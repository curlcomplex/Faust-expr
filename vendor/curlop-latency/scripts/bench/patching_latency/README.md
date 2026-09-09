# Patching latency checkpoint (#254)

This standalone measurement target links the **unchanged current product** `GraphState`, `FaustGraphRenderer` and `FaustRuntime` implementation, with legacy APG disabled. It is not a replacement executor or a product latency fix. No current live app/session or personal checkout is mutated.

Run on the existing private physical Mac workflow, or from an isolated checkout:

```
python3 scripts/bench/patching_latency/run.py --output evidence/patching-latency
```

The driver uses the native dev CMake preset with RelWithDebInfo, the installed Faust package, pinned public submodules, and eight build jobs. It installs no packages. Every child has a timeout and an isolated process group. The original source hashes, build commands, logs, cases and raw audio are retained. Benchmark failure fails the job; state-reset observations are reported separately from successful execution.

## Measured

Twenty-four independent native processes cover serial and parallel graphs of 4/16/32 stereo filter modules, an eight-module one-sample feedback graph, a sixteen-module control-bearing graph, an ordinary bound parameter update, an unrelated-connection state-continuity probe and paced build/render overlap, at 48 kHz with 64/128-frame blocks.

A new cable joins an already present constant source to the output. Its exact expected contribution is derived from the current native equal-power pan law (not fitted from the recording). Actual first-block floats verify the changed computation. Matrix cases distinguish fresh-process preparation, a previously unseen topology after authored module factories are warmed, and nine revisits with both graph factories retained. Resident factories are not running-instance preservation. The first two phases have one observation per case; the revisit nearest-rank p95 of nine observations is the maximum, not a population estimate. Cases are in fixed order; background load and thermal state are not experimentally controlled.

Production stage timers separate graph planning, factory creation and instance initialization. Unattributed preparation includes Box/source construction, locks and other setup; it is not silently classified as compiler time. Whole-process/build durations are never called edit latency. No new persistent-bitcode reload measurement is included in this checkpoint.

The continuity probe compares an unchanged source tap against both a continued instance and a newly initialized anchor. A 6000-sample delayed impulse is due at frame 1904 after 4096 samples of prior history; restarting it instead moves the impulse to frame 6000. Oscillator phase is checked in the same raw recording.

The paced case keeps the old renderer processing on a normal scheduled thread while the caller builds, then uses a minimal atomic pointer publication seam with both owners retained until the thread joins. It records first changed computed audio and synthetic schedule misses. It does **not** use EngineSlot publication, an audio device, an Audio Workgroup, or GUI input. These observations are not hardware delivery latency or real xruns.

## Still required for interactive acceptance

Actual USER_CONNECT/USER_DISCONNECT GUI dispatch and acknowledgement; EngineSlot publication/retirement; Core Audio callback and device evidence; stale-build suppression; incremental compiler-region reuse; live-state preservation across changed regions; polyphonic and song-sized workloads. No claim of completing these follows from a successful headless run.
