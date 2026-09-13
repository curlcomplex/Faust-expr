# Analog Classics / 909 — seven synthesized candidates, four assets pending

Faust-expr #68; CURLOP #279 / #271. Sound version `0.1.0-experiment`. Preserves 606 PR #67 and all earlier pack instrument sources. Expanded-control 606 follow-up is deferred under CURLOP #284, not implemented here.

## Renderable candidates

Kick, snare, low/mid/high toms, rim, clap. These are 909-oriented research voices, NOT hardware-approved reproductions. Kick/snare incorporate specifically identified Plaits formulas/architectural ideas. Toms/rim/clap are new uncalibrated synthesis hypotheses. Read ORACLES.md and LICENSES.md; using a component does not mean a full source instrument was ported.

Closed/open hats, crash and ride remain pending selected redistributable assets. `sample-fixture.dsp` is a non-musical diagnostic, not an eighth instrument or a 909 asset. `sample-player.lib` proves bounded interpolated one-shot playback with source-rate/root-Hz and choke only. No consumer resource binding is claimed.

## Controls / lifecycle

One note per kernel; canonical `gate`, `freq` (Hz), `velocity`; custom `accent` requires explicit wiring. Host owns allocation/chords/polyphony. Pitch, velocity, accent, decay and pitch-envelope controls latch on onset. Note-off leaves a one-shot ringing; retrigger needs a computed low gate sample. Tone/noise balances/drive/level are sample-rate-aware 5ms smoothed and live during tails. A transient-level edit after the transient ends must not resurrect it.

5–7 timbre controls per voice, without imposing a permanent knob limit. Source layout metadata are hints, not completed host faceplates. The manifest records control timing; ranges/units/defaults come from actual Faust declarations/defaults.json and are captured by qualification.

Kick: independent pitch amount/time, amplitude decay, transient, tone, drive. Snare: separate body/noise decay, snappy mix, bend, tone, drive. Toms: decay, bend, overtone/noise balance, drive. Rim: decay, spectrum, snap, drive. Clap: burst spacing, tail length/balance, tone, drive; no pitched snare body.

## Qualification

`python3 tools/modules/drums909_batch.py --out build/hats-v2/drums909-batch`

Existing native renderer compiles actual Faust/C++. Scores, raw inputs/outputs, controls, generated/source hashes and results.json retained. Tests include two extracted native Plaits functions, non-musical sample-player bounds/interpolation/retrigger/choke, broken EOF and delayed-onset negative controls. Test stimuli are not substitute synth renders. Existing hats/Trigger Seq and repository smoke/build tests run separately; other branch-guarded suites are not newly counted.

## Auditions

`00_909_synthesis_core_groove.wav`: actual Trigger Seq schedules seven persistent voices, four bars at 120 BPM plus tail, fixed summing gain 0.4. No hats/crash/ride stand-ins.

`01_909_seven_synthesized_voices.wav`: kick, snare, low/mid/high tom, rim, clap, three seconds each. Four presets per voice, three seconds each; exact settings in report. No external EQ, reverb, compression, limiter or normalization. Instrument drive is DSP; offline adapter is not CURLOP audio.

## Open gates

Four sample assets; full reference/hardware calibration; musical approval; actual CURLOP source/package/faceplate/binding/choke/save-reopen; target-device callback/memory/thermal tests. No merge, library promotion or release approval. Archive exact source/score/audio evidence durably before Actions retention expires.
