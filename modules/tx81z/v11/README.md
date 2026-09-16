# TX81Z: usable Faust blocks and retained voice

Issue #97 / PR #127. v10 is the existing sound baseline. `recomposed_voice.dsp`
rebuilds it using these functions; `alternate.dsp` is a small, independently
patchable two-operator instrument. These are ordinary Faust sources, not runtime
processor instances or a proposed universal FM framework.

## Use

With the delivered source tree, standard Faust libraries are the only external
source dependency. The delivery includes the hash-verified generated OPZ table
library. A source checkout can prepare that file with the existing
`tools/modules/tx81z_prepare.py` against the pinned ymfm revision.

```
b = library("modules/tx81z/v11/blocks.lib");
bf = b.fromHz(freq);
tick = b.egClock : (_,!);
count = b.egClock : (!,_);
env = b.envelope(gate,tick,count,bf,31,10,0,6,8,0,0);
p = b.phase(b.phaseStep(bf,0,1,0,0,0,0,0),gate);
process = b.toAudio(b.op(p,env,8,0));
```

The complete `alternate.dsp` adds a second operator, waveform selection and
modulation depth. It imports only `blocks.lib`, not the full voice or its UI.
The full recomposition reuses the existing v7 control declarations/LFO wiring;
its operator, envelope, frequency, level and routing calculations call the blocks.

## Signal and state contracts

| Block | Meaning |
| --- | --- |
| `fromHz(hz)` | Packed OPZ octave/note/fraction key code, nearest 1/64 semitone; not a MIDI note or radians. |
| `phaseStep(bf,mode,coarse,fine,dt1,dt2,range,fixedCrs)` | Phase increment in 20-bit-cycle units per host sample. Mode0 is native ratio-law adaptation; mode1 is the explicit panel-Hz adaptation. Coarse0..15/fine0..15/DT1 0..7/DT2 0..3/range0..7/fixedCrs0..63. Ratio mode retains fractional carry state at non-native rates. |
| `phase(step,gate)` | Stateful accumulator; output0..1023 is its upper ten bits. Rising gate resets phase; falling gate does not discard release. |
| `egClock` | Pair `(tick,count)` at the existing 55930/3-Hz envelope cadence, with count modulo16384. Share one pair across operators. Supported host rates here are44100/48000/55930/96000Hz. |
| `envelope(...)` | Attenuation0..1023, not linear gain. AR/D1R/D2R0..31, SL0..15 attenuation, RR0..15, KS0..3, reverb0..7. SL is **15 minus panel D1L**. State is internal; off/retrigger behavior is preserved. |
| `totalTL(...)` | Integer OPZ total level0..127, nominally0.75dB/step. Includes the adopted KVS/KLS/BC/carrier policy. Note13..108, velocity/CC2 0..127, algorithm0..7, operatorIndex1..4. It does not latch events: the voice does so explicitly. |
| `op(phase,atten,tl,wave)` | Stateless logarithmic waveform/operator evaluation; waveform0..7; signed integer amplitude bus. |
| `feedbackOp(...,feedback)` | Same output units; feedback0..7; owns the two stored previous outputs. Do not add another `~` around it unless deliberately changing the sound. |
| `modulatePhase(phase,bus)` | Native bus conversion: `phase + (int(bus)>>1)`. Sum buses before this conversion when reproducing the existing algorithms. |
| `attenuation(eg,shift,am,enable)` | Preserved post-envelope EG Shift/AM combination. Shift0..3; operator1 uses0 in the TX81Z voice. |
| `pmDelta(...)`, `amOffset(...)` | Existing TX81Z LFO policy; PM output in1/64-semitone units, AM output in attenuation units. These are not normalized CVs. |
| `in3/in2/in1/carriers` | Existing topology, zero-based algorithms. Public OP4 is the first serial modulator and OP1 the final carrier. |
| `toAudio(bus)` | Convert the integer carrier sum to float with1/32768 scaling; no limiter, automatic normalization or clipping. |

Keep initialization/table preparation off the audio callback and serialize
class-table initialization before concurrent instances. There is no per-block
heap allocation requirement. A function's declaration is not a separate DSP
object: Faust composes it in the unified graph. Metadata/control zones are
host-control inputs, not automatically audio-rate CV inputs.

## Declared clock/output scope

Native arithmetic comparisons use55930Hz, the historical upstream setup, not a
measured TX81Z crystal. Other supported sample rates preserve intended pitch and
envelope cadence but retain **two host samples** of feedback. They are deliberately
host-rate adaptations, not silently resampled chip-clock emulation. Fixed mode
uses the documented panel Hz range8..32640; values above Nyquist alias normally.
Do not infer anti-aliasing or chip-rate invariance from finite/non-silent output.

The completion suite records clock counts, complete fixed-field arithmetic at
four rates, actual low/high fixed-frequency audio, envelope timing and audible
cross-rate residuals. These are implementation measurements, not a hardware
calibration. No new native-clock runtime is needed to use the declared baseline.

The source produces one mono pre-analog-output voice. S&H LFO is the applicable
noise-like control source; no undocumented standalone TX81Z audio-noise generator
is inferred from a generic Yamaha core. Analog DAC coloration, multi-part
performance effects, allocator, bend/portamento and the complete wheel/foot
controller implementation remain outside this scoped source delivery.

**Known fidelity qualifications:** BC magnitude is the disclosed DX100-family
approximation in v10/REFERENCE.md. Also Yamaha's manual p23 describes release
extension triggered by OP1 for all operators, whereas the inherited native-core
adapter models per-operator release thresholds. Nonzero reverb therefore remains
an explicit firmware/hardware discrepancy, not a proven whole-instrument match.
The patch decoder maps the control; that is not proof of its controller firmware.
Sample-and-hold noise sequence, complete LFO calibration and exact hardware clock
remain uncalibrated. Prior v10 results are preserved rather than silently adjusted.

## Acceptance/evidence

`tools/modules/tx81z_completion.py` checks full-patch and dynamic recomposition,
bounded all-algorithm/waveform cases, four host rates, fixed arithmetic/audio,
clock counts, clean offline compute and declared generated-memory sizes. It reuses
the same-run v10 qualification builds instead of compiling the baseline again.
Numerical failures are retained; original dry audio levels are not fitted away.

Final exact source/run/results and delivery hashes are recorded in #97 and
`completion-result.json` when available. A passing source-block suite is separate
from Felix's listening, actual CURLOP/device realtime acceptance and merge/release.

Primary references: Yamaha TX81Z Owner's Manual pp14,16-18,23
(https://usa.yamaha.com/files/download/other_assets/9/316769/TX81ZE.pdf);
Faust composition/recursion semantics (https://faustdoc.grame.fr/manual/syntax/);
pinned ymfm81aec25ccbb98f4873a255f7551ac4dadac59b4a. Exact policy provenance and
contradictions remain in v10/REFERENCE.md. This document adds no hardware claim.
