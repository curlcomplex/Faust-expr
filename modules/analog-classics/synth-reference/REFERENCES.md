# Analog Classics synth reference matrix

Owner: Faust-expr #82 / draft PR #83; consumers CURLOP #274/#275/#271.
Original synth baseline: `fd2b7efae4951d014fed3fed3cd01f295a0af21e`.

The synth family is now **four instruments**: SH-101, Juno-60, Juno-106 and Model D/Mini. Existing source/sound versions remain preserved. Host owns polyphony; every Faust kernel remains one note with canonical `gate`, `freq` (Hz) and `velocity`. Juno chorus remains outside each note kernel.

## Reference hierarchy

Use, in order:
1. Identified hardware recordings or raw/multisampled WAVs from real units, preferably one sample per note and with settings metadata.
2. Inspectable circuit/software models for architecture and bounded component comparisons. Label these separately from hardware.
3. Manuals/service notes for topology, ranges and control semantics.
4. Generic demos only for qualitative sanity checks, never as numerical calibration anchors.

Do not redistribute commercial/reference WAVs without explicit rights. Fetch transiently where permitted; retain URLs, hashes and derived measurements instead.

## Juno-60 — newly added Analog Classics instrument

### Primary hardware/sample references
- Cluster Sound Juno-60: https://clustersound.com/product/synths/juno-60/
  - real Juno-60; 7,275 24-bit/44.1-kHz WAVs; 124 multisampled patches; **one sample per note**.
  - strong broad timbre/envelope corpus; commercial and not treated as redistributable.
- Hornet/scene.org `aq-j60v1.zip` (Anqodia/plastiq, 1997):
  https://files.scene.org/browse/mirrors/hornet/music/samples/
  - archival Roland Juno-60 16-bit/44.1-kHz WAV pack; freely accessible legacy source.
  - capture settings are incomplete, so use for spectral/envelope cross-checks rather than absolute gain calibration.
- Samples From Mars “Junos From Mars”: https://samplesfrommars.com/products/junos-from-mars
  - serviced Juno-60 and Juno-106 hardware, extensively multisampled; some sounds clean and some explicitly processed. Only clean material is relevant to calibration.
- Roland Juno-60 owner manual / factory patch data:
  https://cdn.roland.com/assets/media/pdf/JUNO-60_OM.pdf
  - useful for factory patch/control documentation, not direct dry component measurements.

### Software/circuit oracle
- `jpcima/Hera`, pinned master commit `f6fe5b900f4cf84809686466e0a37de5edf008fd`:
  https://github.com/jpcima/Hera
  - Juno-60 emulation; project explicitly labels itself alpha/inaccurate in places.
  - exposes Faust DCO, HPF and several VCF candidates. DCO/HPF are GPL-3.0-or-later; do not copy them into a differently licensed product. Some VCF files individually declare ISC and may be used only with their attribution/license retained.
  - use Hera for architecture/component experiments, never as hardware truth.

### Current implementation policy
`modules/juno-60/v1/voice.dsp` is an **independent implementation**, not a Hera code copy. It starts from the same CURLOP single-note contract as the 106 while keeping a separate DCO/mixer/envelope voicing. Its current sound is a reference candidate, not yet hardware-approved.

## Juno-106

### Hardware/sample references
- Cluster Sound Juno-106: one-sample-per-note multisample corpus (previously identified).
- Samples From Mars “Junos From Mars”: same capture family as above; useful because the 60 and 106 were recorded by the same vendor, but processed patches must be excluded.
- Hornet/Soundwave Juno-106 archives (`swjuno1.zip`, `swjuno2.zip`), 1997, 16-bit/44.1-kHz, default C3 unless noted:
  https://hornet.org/cgi-bin/scene-search.cgi?search=Roland
  - independent legacy hardware cross-check; settings metadata are limited.
- Particle Sound JNO/Juno-106 raw waveform material remains a useful oscillator-stage lead where accessible.

### Software oracle
- `stevengoldberg/juno106` is structural WebAudio research only, not calibrated hardware. Its HPF map and two-biquad VCF must not be presented as measured Roland values.

The current continuous controls deliberately extend beyond original hardware positions. Do not collapse them simply to match one sample pack.

## SH-101

### Hardware/sample references
- Cluster Sound SH-101 multisample library: real SH-101, one sample per note across a large patch corpus; suitable for oscillator/filter/envelope/timbre comparison, but not absolute output-gain calibration if the library was level-managed.
- RolandClan/MusicRadar legacy SH-101 sample archives can provide independent qualitative checks where source/capture metadata are retained.
- Roland SH-101 Service Notes, 1 November 1982:
  https://manuals.plus/m/e2bf4ea87ccd1c365e55ae9156f3e333a50456d52feaab0c2f9ea127f204575c.pdf

### unresolved resonance/output question
The saved v2 experiment removes the common filter output multiplier `1+0.20*k` (`k=3.85*resonance`). It does **not** change poles or feedback. The earlier claim that this was a confirmed hardware correction is withdrawn.

AMSynths describes the SH-101 as lacking Q compensation, while the same designer's detailed SH-101-derived AM8101 description discusses a resonance-dependent output-stage boost. In-loop compensation and post-filter/output gain are different mechanisms. Neither v1's arbitrary coefficient nor v2's removal is selected until measurements/model evidence resolve this.

Required measurement remains a fixed-recording-gain resonance sweep with harmonics below cutoff measured separately from the resonant peak.

## Model D / Mini

### Hardware/sample references
- JRR Sounds Modern Model D set: all 88 notes across all seven oscillator waveforms; strong oscillator calibration target.
- Cluster Sound Model D: 5,900 WAVs / 112 multisampled patches, one sample per note; broad whole-instrument timbre/envelope corpus.
- Monosounds real Model D one-shots: independent hardware qualitative cross-check.

### Software/circuit oracles
- `t2techno/Faug` remains inspectable Faust research, but no redistribution licence was established; do not copy source. It also contains fixed-44.1-kHz assumptions.
- D'Angelo & Välimäki ladder-filter models and other permissively licensed component implementations may be used as bounded filter oracles with explicit attribution and scope.

## Test goals for the four-synth pass

### Juno-60 / Juno-106
- isolated saw, pulse, sub and noise spectra/levels across several notes;
- PWM static widths before LFO modulation;
- HPF response at representative positions;
- low/high resonance VCF sweeps;
- ADSR attack/decay/release timing and retrigger behavior;
- clean dry voice only; chorus tested separately as an effect.

### SH-101
- oscillator/sub/PWM balance; filter cutoff/resonance/output law; noise; envelope; glide; overload.

### Model D
- waveform/register oscillator calibration; mixer-drive loading; ladder cutoff/emphasis/keytracking; contour timing/amount; glide.

For commercial multisample libraries, numerical fitting must only use files legitimately available to the test environment. Otherwise use them as owner-facing listening/reference targets and rely on accessible archival hardware plus open models for automated CI.

## Acceptance boundary

A software model agreeing with another software model is not hardware validation. A multisample patch library with unknown internal normalization is not an absolute gain reference. Descriptor distances are diagnostics, never authenticity percentages. Preserve previous versions, reject revisions that worsen declared tests, and keep owner listening, CURLOP integration/save-reopen, and named-device qualification as separate gates.
