# 606 family — 0.1.0-experiment

Lab issue #66; consumer curlcomplex/CURLOP#279 under #271. Seven source candidates,
not hardware-calibrated TR-606 reproductions. Cross-reference and listening approval
remain separate future gates. Issues own current status.

## Contract
Seven mono single-note entries: kick, snare, low-tom, high-tom, closed-hat, open-hat,
cymbal. No internal kit, chord stack, allocator, MIDI or voice stealing. The six
metal oscillators form one percussion sound, not independently allocated notes.

Common faceplate source hints: **Decay / Tone / Character / Level**. Character is
Click, Snappy, Noise or Metal Spread. `gate`, `freq` (Hz), `velocity`, `accent` are
performance inputs; OH adds `chokeGate`. Only gate/freq/velocity have the verified
host naming compatibility; custom controls require explicit adapter routing.
`defaults.json` records initial values. Decay is nominal primary-envelope -60dB
seconds, not a guaranteed complete-waveform decay. Pitch/decay/velocity/accent latch
on rising gate. Gate release does not stop a one-shot. Tone, character and level
use sample-rate-aware 5ms smoothing and remain live in tails. Click remains an
onset transient: later movement cannot regenerate it. At least one computed low
gate sample separates retriggers. OH choke wins collisions; subsequent onsets rearm.

## Research / hypotheses (13 September 2026)
No third-party implementation or sample copied. Original lab equations use the
standard Faust library; compilation captures library/compiler provenance.

- https://tubbutec.de/6m0d6/ — designer documentation of modified 606 circuits:
  dual damped kick oscillators; snare body/noise; low-passed tom noise; metal/choke.
  A related hardware product, not a measured source oracle.
- https://www.baratatronix.com/blog/606-cymbal-and-hi-hat-synthesis — author's
  calculated nominal metal frequencies 246.416, 308.0202, 367.00285, 418.161,
  440.403, 627.241 Hz; bandpass anchors 3440 and 7100 Hz. Calculations/simulations,
  NOT measured reference-machine values; tolerances remain unresolved.
- https://www.roland.com/global/products/tr-06/ — manufacturer 606-derived sound
  roles and modern control context; not silently treated as original 606 audio.

Kick uses two impulse-excited resonators without an 808-style pitch sweep. Snare
uses one tonal body with high-passed noise. Toms add low-passed decaying noise;
LT/HT justifiably share architecture with different defaults/ranges. Hats/cymbal
use the 606-specific six-oscillator source and separate filtering/envelopes.
Output saturation/accent laws are authored approximations, not circuit-fitted VCAs.

## Differences and remaining gates
Independent voices do not replicate shared analogue metal/noise state or accent
bus. Original tempo-dependent OH decay is replaced by manual decay in this version;
verified host tempo mapping remains future work. Host routes CH choke to all
relevant ringing OH instances. New pitch/decay can retune existing state within a
reused single voice; host owns independent overlap. Ideal damped-sine recursion is
not component-level twin-T simulation. Constants, gains, envelopes, accent and
nonlinearity need controlled comparison. PolyBLEP/analytic filters do not establish
alias-free operation. GUI hints are not a completed CURLOP faceplate or package.

## Execute
`python3 tools/modules/drums606_batch.py --out build/hats-v2/drums606-batch`

Uses existing render.cpp, SynthLab build helper and Batch score renderer. Python
provides scores/routing/analysis only; all sound comes from actual compiled Faust.
The ring comparison checks our equation, not hardware. A source-delayed onset must
fail the same exact trace test that the real source passes. Existing workflow runs
this family plus hats/Trigger Seq regressions; other branch-guarded suites can be
skipped and must not be counted as new passes. Smoke/unit-test CI is separate.

Auditions: 00_606_seven_voice_groove.wav; 01_606_isolated_voices.wav (kick, snare,
LT, HT, CH, OH, cymbal); seven four-presets files (3 seconds/preset); stems;
08_open_hat_choke_comparison.wav. Four-bar 120BPM groove uses actual Trigger Seq
outputs and persistent instances with fixed 0.45 summing gain. No normalization,
EQ, compression, limiting or extra FX. Lab renders, not CURLOP captures.

Retain exact source, raw audio, scores, generated-code hashes and contrary results.
No merge, promotion, device acceptance or final sound approval is implied.
