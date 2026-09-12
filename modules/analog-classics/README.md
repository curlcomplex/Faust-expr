# Analog Classics — first standalone slice

Work: [#55](https://github.com/curlcomplex/Faust-expr/issues/55). This is the first grouped lab slice, not the complete pack or a factory release. It contains one closed-hat voice, one open-hat voice and a 1–32-step trigger sequencer. The consumer owns polyphony. No private CURLOP code is included.

## Sources and contracts

Read `first-slice.json`. Hats derive from the unchanged `hats-analog/v2` source at `91b051e81e52e290866f0fe09416a8db51556ec2`; the baseline's paired arrangement remains a comparison oracle only. Each new hat entry reaches a single articulation. Oscillator/filter/envelope equations and onset-latched controls are preserved, not re-fitted. UI labels are `gate`, `freq` and `velocity`. The explicitly chosen A4 reference maps `freq / 440` to the old metallic pitch ratio; 264..748 Hz preserves the old 0.6..1.7 range. This is note transposition of an inharmonic source, not a claim that the hat has that fundamental. Wider note range and live-tail timbre control are NOT claimed in this compatibility slice.

An open voice's `chokeGate` operates only on itself. A host must route closed-hat triggers to active open voices if it wants a cross-voice choke group. The new source does not inspect, allocate or steal other notes. Choke strength zero is off; repeated choke must not resurrect a tail; a simultaneous choke wins over a new hit. Gate release does not truncate a drum's one-shot tail.

The sequencer emits a one-sample gate pulse for an enabled step. Positive clock edges advance; falling/negative input does not. Length is 1..32. Reset is edge-triggered and arms step 1; a simultaneous running clock plays it. Run=0 holds state and suppresses output, but the clock detector keeps tracking, so re-enabling run during an already-high clock does not create a phantom edge. Length edits take effect at the next clock; previous step telemetry may exceed a newly shortened length until that tick. Pattern edits are ordinary values. There is no internal tempo, timer, note allocation or rebuild per edit.

`trigger.dsp` is the source-authored UI/control facade. `audio-clock-test.dsp` is a diagnostic with two real input channels, clock and reset. Both use the same kernel. The first output is `[curlop:cvout]` gate; trailing `[curlop:meterout]` step is observation only. These declarations still need actual host capture/routing/faceplate/save-reopen tests; no private host classifier is copied into the lab.

## Executable evidence

Run:

```sh
python3 tools/modules/analog_classics_slice.py --out build/hats-v2/analog-classics
```

This reuses `tools/modules/render.cpp` and the existing hats lab compiler helper. It compiles actual Faust scalar/vector code, compares single-note output against the original separated channels, checks note labels and Hz conversion, velocity, latching, tails, retrigger/choke, and checks exact sequencer output from real input buffers. A deliberately two-sample-pulse mutant must fail the trace comparison. Pure Python computes expected integer step traces, never replacement audio synthesis.

Audition WAVs are actual new Faust voices. The groove clocks the new Faust sequencer, then uses its output samples to build the score for independent closed/open renderers and explicit open-voice choke. That is offline lab routing, NOT a CURLOP graph capture. No backing music, EQ, reverb, limiter or per-hit normalization. One documented common gain is used only if the combined files need headroom; raw floats remain intact.

The existing `hats-v2-complete.yml` is extended for the new branch. It reruns baseline hats qualification, then the new slice. Original hardware-reference download/analysis stays enabled on the original hats branch, but is not repeated here because this slice tests adaptation, not new hardware fitting. The existing smoke/analyser workflow also still runs. Workflow exit status and exact-commit artifacts, not this README, determine pass/fail.

No speedup, new hardware accuracy, musical approval, target realtime performance, host integration, merge or release is implied. The next consumer gate is real declaration capture, routing, choke broadcast, voice lifecycle, faceplate, recall and Path A evidence. Old projects retain old sources.
