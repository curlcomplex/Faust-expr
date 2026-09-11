# Clap v0.2 — clustered noise and independent PM body

Owning issue #49 / draft PR #50. This continues the selected seven-control design, not a competing machine. V1 and the superseded six-control candidate remain history. Tracker owns private integration/device testing/merge; this repository owns reusable source and evidence.

## Playable surface

Pitch (70–900 Hz), gate and linear velocity are performance inputs. Seven synthesis parameters remain addressable: **Spacing / Punch / Decay / Balance / Body / Body Envelope / Drive**. Proposed six columns: Spacing, Punch, Decay, Balance, paired Body+Body Envelope, Drive. No P8 or native chord generation. The host owns note polyphony.

Spacing separates four sub-attacks, shortens them and adds brighter crunch in the upper half. Punch adds onset emphasis and nonlinear shaping. Decay controls the diffuse noise tail, separately from Body Envelope's pitched-body decay. Balance is an equal-power noise/body mix. Body changes the independent modulator ratio, index and nested PM depth. Drive is a separate output shaper: Drive0 does not disable shaping deliberately added by Punch.

All musical values latch at onset. Knob movement during a tail affects the next hit. Note-off does not choke. Canonical PRNG/filter histories advance in silence; a declared host idle-freeze policy may pause them but must not leave stale gate state or revive unintended tails.

## Source corrections and alternatives

V1 multiplied the carrier's wrapped phase by a noninteger ratio, resetting the modulator at every carrier wrap. V2 gives each operator its own phase accumulator, reset only at note onset. A phase-output probe and a deliberately rejected old-law mutation test this directly. Body0 is now a clean sinusoidal body. These intentional sound changes have a new version identity.

Each sub-attack rises from zero over a finite attack, rather than starting with a hard step. Direct exponential burst/body/tail envelopes remain the accuracy baseline; they terminate after 16 time constants. No recursive float32 decay shortcut is used.

`clap.dsp` uses a 4096-point linearly interpolated sine table; `reference.dsp` uses direct sine with identical noise/envelopes/nonlinearities. `single-burst.dsp`, `no-tail.dsp` and `no-body.dsp` are controlled ablations, not alternate default machines. Single-burst removes only secondary bursts, preserving crunch/filter routing. Diagnostics allow unused controls to be pruned instead of retaining them through artificial inaudible signal dependencies. Envelope and phase-probe entry points are measurement outputs, not product instruments.

## Execute

Requires Python 3, NumPy, SciPy, C++17 and Faust. Only acquisition additionally needs FFmpeg/network; ordinary replay uses frozen references.

```sh
python3 -m unittest discover -s tests -p test_clap_v2.py -v
python3 tools/modules/clap_v2_references.py --out build/clap-v2/references
python3 tools/modules/clap_v2_delivery.py --out build/clap-v2/run --references build/clap-v2/references
```

The delivery package supplies an offline replay against checksum-verified generated C++, without Faust or internet. That is C++ replay, not another Faust compilation. Do not use Python -O: inherited checks use assertions. Completion requires a successful process exit and the complete final report; an intermediate report from inherited test stages is not final qualification.

## References and limits

CP Vintage is behavioral inspiration, not recovered source. CPV-01 through CPV-12 from Winston Edwards / Particles Into Waves' *Syntakt Designer Drums* are licensed public MP3 previews, not original lossless captures. Firmware, knobs, recording gain and per-hit processing are unknown. Reference assets and attribution are test material, not runtime samples. Eight authored anchors supply broad spectral/decay comparisons; no hardware fitting or untouched validation set is claimed.

Creator: https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums
License: https://creativecommons.org/licenses/by/4.0/
Manufacturer-community listening/manual leads: https://www.elektronauts.com/t/syntakt-science-lab-5-cp-vintage/237671
Faust oscillator contract: https://faustlibraries.grame.fr/libs/oscillators/

Standalone official manual endpoints returned errors during this continuation; no newly verified firmware-specific algorithm is inferred. Equations/mappings here are independently authored.

Core auditions use fixed gain without EQ, reverb, normalization or a limiter. The reference-context sequence applies documented whole-hit gains only. Extreme high-register PM/Drive and abrupt retriggers remain listening/device acceptance questions. See QUALIFICATION.md and INTEGRATION.md.
