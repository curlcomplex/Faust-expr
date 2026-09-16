# TX81Z v10 — real patch reconstruction

Owning issue #97 / PR #127 / strategy #111. This step delivers checked single-voice SysEx reconstruction and actual dry Faust playback, not a universal FM architecture or final hardware certification. v1–v9 remain unchanged. Exact execution results belong in #97 and the identified Mac evidence.

## Reference reconciliation and corrections

**Operator ordering: retain OP4, OP2, OP3, OP1 for both VCED and ACED.** Yamaha's printed parameter list labels the middle groups in the opposite order. The initial v10 candidate followed that list and failed against real `Filter2.syx`: it decoded operator levels as90,75,65,82 instead of the author's documented90,65,75,82, and moved the W3 waveform from operator3 to2. `Filter1.syx` similarly put the author's operator3 carrier on2. Edisyn's `vcedParameters` agrees with the actual files and the earlier v9 ordering. The failed Mac run35114310161 is preserved. **The initial claim that v9's operator ordering was wrong is withdrawn; do not reapply the printed order.** Synthetic fixtures alone had failed to catch that bad interpretation.

Other v9/0.9.1 policy defects were real:

- Yamaha D1L is a level:15 is maximum,0 minimum. OPZ SL is attenuation; use **15-D1L**.
- OUT0..19 uses a nonlinear total-level table. **99-OUT** applies from20; OUT0 is TL127, not99.
- Keyboard level scaling uses a nonlinear keyed table, not the unqualified v9.1 line above MIDI48.
- The published TX81Z integer KVS mapping differs from the previous Deicsonze-style interpolation. In particular, KVS7 at velocity127 retains one TL step; do not force all sensitivities to unity.
- BC EG bias needs operator EBS, the voice's BC EG-bias depth and live CC2. The earlier static proxy omitted depth and used the opposite controller interpretation.

These changes have a new sound identity. Previous green tests prove only the previous candidate's tested behavior, not canonical TX81Z accuracy.

## Adopted parameter policy

Yamaha supplies intended controls and parameter meanings, subject to the ordering erratum above. Andrew Evdokimov's pinned reverse-engineering notes supply compact Basic-TL, KLS, KVS and carrier-trim facts. `panel_policy.lib` expresses those numerical facts in Faust. The qualification reads the reference notes separately and compares compiled functions against the complete KLS/KVS grids and bounded level/trim/controller cases.

Two minor inconsistencies in the notes are explicit: Basic-TL prose says the table includes20 but lists only20 entries; use entries0..19 and the affine section from20. KVS prose reverses high/low names once; use its explicit pseudocode and byte table. Neither is presented as our own hardware measurement.

Existing ratio-family/DET translation and the native OPZ core are retained. Fixed frequency remains v6's audible-Hz adaptation; the v5 emulator counterexample is unchanged. Transpose now affects base pitch; envelope key code no longer follows instantaneous vibrato. Level-scaling note and velocity are captured at onset. Live breath changes remain available during release, and zero-velocity onset is silent. ACED reverb rate is mapped to all four existing operator release-extension controls.

## Provisional BC EG-bias magnitude

The working curve is the XCent0.12.7/DSP28 family model:

`TL_bias = floor((127 - CC2) * EBS * BC_depth / 2540)`

Its author describes **DX100** calibration, not TX81Z calibration. Yamaha confirms the controls and their intended use, not this exact formula. This is an explicit prior-art approximation: EBS0 or depth0 disables it; maximum breath removes added attenuation. **Precise TX81Z BC-curve fidelity remains open.** Native comparison cases disable BC instead of pretending the adapter independently validates it.

## Actual patch files

Matt Gregory publishes `Filter1.syx`, `Filter2.syx`, and `FilterBass.syx` with his programming tutorial. The Mac downloads the actual142-byte files, validates both checksums, headers, byte counts and channels, then sends decoded controls to compiled Faust. No hand-entered substitute patch is used.

Independent tutorial anchors include Filter2's algorithm1, unequal levels90,65,75,82 and W3 on operators3/4, plus Filter1's algorithm7/operator4 envelope. The original files and decoded records stay in private execution evidence; public results retain their URLs and hashes. No third-party patch bank or firmware is republished. Delivered WAVs are newly synthesized Faust output. The breath example is explicitly a modified FilterBass variant.

## Qualification boundaries

The test uses the existing renderer at82926f023410ae8367eec3c5d842edbbe34ab439 and existing `ymfm_opz_voice_reference.hpp`; it does not introduce another renderer or host engine. Native pairs run at55930Hz. Both sides use the decoded patch policy, so their agreement is a regression check of oscillator/envelope/routing arithmetic, **not independent firmware or physical-hardware validation**. LFO, BC, EG Shift and fixed-Hz mode are isolated in those pairs because the native adapter does not implement them. Separate behavioral cases exercise modulation and release.

Actual compiler versions/hashes are recorded; historical Faust2.88 results are not relabelled current qualification. Other-rate tests qualify only the implemented adaptation: feedback remains two host samples. Full analog output/DAC, performance allocation, wheel/foot routes, bend/portamento and host GUI remain outside this adapter and visible in its limitations report.

The standalone export embeds bounded local source environments, retaining only the standard Faust library dependency. It is compiled and compared against a multi-file musical render. This does not use `faust -e` expansion or claim the later module-extraction/recomposition milestone. The delivery archive excludes private controller/queue files and downloaded third-party patches.

## Primary sources

- Yamaha TX81Z Owner's Manual, pp.19–22 and71–74 (operator-order erratum above): https://usa.yamaha.com/files/download/other_assets/9/316769/TX81ZE.pdf
- Andrew Evdokimov, ax81z5848832989c7864092a277e9428d44f817c6173f: https://github.com/iflyhigh/ax81z/blob/5848832989c7864092a277e9428d44f817c6173f/Math.md ; blob b23df480991d653deea9ab5434ab5eff3cf1da59. Parameter notes: https://github.com/iflyhigh/ax81z/blob/5848832989c7864092a277e9428d44f817c6173f/VCED_and_ACED.md
- Edisyn61cf9625f4e63b8f77c39f0ba6bcb6896803af8b, `vcedParameters`/`acedParameters`: https://github.com/eclab/edisyn/blob/61cf9625f4e63b8f77c39f0ba6bcb6896803af8b/edisyn/synth/yamaha4op/Yamaha4Op.java
- Matt Gregory's original tutorial and downloadable patches: https://mgregory22.me/tx81z/filter.html
- XCent0.12.7,7May2026,DSP28, DX100-family BC curve only: https://knivesonstrings.com/xcent
- Existing unchanged BSD-3-Clause OPZ engine: https://github.com/aaronsgiles/ymfm/tree/81aec25ccbb98f4873a255f7551ac4dadac59b4a ; full notice retained in `../YMFM-LICENSE.txt`.
