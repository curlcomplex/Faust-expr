# TX81Z v10 — real patch reconstruction

Owning issue #97 / implementation PR #127 / strategy #111. The deliverable for this step is usable single-voice SysEx reconstruction and actual dry Faust playback, not a new FM architecture or final hardware certification. No prior version is overwritten. Execution and acceptance results belong in #97 and the identified Mac artifact, not inferred from this document.

## Corrections made before evaluating patches

The v9/0.9.1 candidate was not an adequate reference for patch translation. In particular:

- VCED and ACED edit-buffer order is **OP4, OP3, OP2, OP1**. Packed VMEM order is different (OP4, OP2, OP3, OP1). The old decoder swapped operators 2 and 3.
- Yamaha D1L is a **level**: 15 is maximum, 0 is minimum. OPZ SL is attenuation, so the conversion is **15 - D1L**, not a direct copy.
- Operator OUT 0..19 uses a nonlinear table; the inverse-linear **99 - OUT** rule starts at20. OUT0 therefore maps to TL127, not99.
- Keyboard level scaling is a nonlinear, keyed lookup, not the unqualified v9.1 24dB/octave line above MIDI48.
- The earlier Deicsonze-style velocity interpolation is not the published TX81Z integer KVS policy. The TX81Z reconstruction retains that policy's offsets at full MIDI velocity instead of forcing every sensitivity to unity.
- A BC EG-bias route needs **both** operator EBS and patch BC EG-bias depth, plus the live breath-controller value. The prior `egBias` proxy omitted the patch depth and reversed the breath-control interpretation.

These are sound-affecting changes; v10 is a new identity. Prior passing v9 tests establish only their former candidate behavior, not correctness of those mappings.

## Adopted parameter policy

Yamaha documentation provides parameter order, ranges, D1L direction, EBS/depth separation, transpose and reverb-rate meanings. Andrew Evdokimov's published TX81Z reverse-engineering notes provide the compact Basic-TL, KLS and KVS numerical facts and carrier trim. `panel_policy.lib` expresses these in Faust; the qualification script reads the pinned source notes independently and checks the compiled functions over the complete KLS grid, complete KVS grid and bounded BC/level/algorithm cases.

The notes contain a small boundary inconsistency: prose says the Basic-TL table extends through20, but the table has only20 entries (0..19). We explicitly use the table for0..19 and the affine section from20. The KVS prose also reverses high/low names once; this implementation follows its explicit pseudocode equation and enumerated byte table. Neither discrepancy is hidden as an independently measured hardware result.

Frequency ratio-family and DET encoding retain the existing native OPZ frequency engine. Fixed frequency remains the separate audible-Hz adaptation introduced in v6; the preserved v5 native-emulator counterexample is not changed. Transpose is now applied to the voice's base pitch. Envelope key code is derived from that unmodulated pitch, not from instantaneous LFO vibrato. Keyboard level scaling uses the transposed MIDI note captured at onset; velocity is likewise captured at onset. Live BC changes remain effective during release. A zero-velocity onset is silent.

## Explicitly provisional part: BC EG-bias magnitude

Yamaha establishes that EBS sets per-operator sensitivity to breath-controlled EG bias and that the voice has a separate BC EG-bias depth. It does not disclose the complete numerical curve. The bounded working implementation adopts the published XCent0.12.7/DSP28 family formula:

`TL_bias = floor((127 - CC2) * EBS * BC_depth / 2540)`

That source describes a **DX100** hardware calibration, not a TX81Z calibration. It gives the correct controller polarity and a concrete prior-art magnitude model; it is **not proof of numerical TX81Z fidelity**. EBS0 or depth0 disables the effect; maximum breath gives no added attenuation. Native OPZ comparison cases disable this controller path rather than claiming that the native adapter independently verifies it. Precise TX81Z BC-curve acceptance remains open in #97.

## Real patch cases

Matt Gregory publishes the original single-voice files `Filter1.syx`, `Filter2.syx` and `FilterBass.syx` with his TX81Z programming tutorial. The qualification route downloads the actual142-byte files, validates both Yamaha checksums, sizes, headers and channels, decodes them, then applies the resulting controls to the actual Faust instrument.

Filter2's documented unequal operator output levels (90,65,75,82), algorithm1 and W3 assignments on operators3/4 are independent anchors for the decoder. They specifically expose the earlier operator-order bug. The original files and parsed data remain in private execution evidence; their URLs and measured hashes are retained. No third-party patch bank or firmware is added to the public repository or delivery archive. The delivered sounds are newly generated Faust recordings. The breath demonstration is an explicitly labelled modification, not an unmodified author patch.

## Qualification and scope

`tools/modules/tx81z_patch_qualification.py` reuses the established, hash-identified `tools/modules/render.cpp` from reviewed lab commit82926f023410ae8367eec3c5d842edbbe34ab439 and the existing native `ymfm_opz_voice_reference.hpp` adapter. It does not replace the lab with another runtime/renderer. The source remains ordinary composable Faust.

The native pairs verify the current oscillator/envelope/routing calculation at55930Hz using translated controls. **Both sides use the decoded patch policy**, so these are not an independent validation of controller firmware or a physical-TX81Z A/B. PM/AM, BC, EG Shift and fixed-Hz mode are explicitly isolated for those pairs because the retained adapter does not implement them. Separate behavioral checks exercise current PM/AM/BC and release paths.

The Mac's actual Faust/compiler versions and hashes are recorded. Historical v5 Faust2.88 parity is not silently relabelled current-head qualification. Other sample-rate tests demonstrate the implemented adaptation only; feedback is still two host samples. No analog DAC/output-stage model, multitimbral performance engine, MIDI allocator, complete wheel/foot routing, bend/portamento or host GUI is added in this step. Reserved/unsupported patch functions are retained in the decoder report, not silently discarded.

Stand-alone export embeds the bounded local source dependency closure, leaving only the standard Faust library external. The exported instrument is compiled and compared against the original multi-file musical render; `faust -e` textual expansion is not used. The delivery ZIP excludes private controller/queue files and downloaded third-party patches.

## Primary references

- Yamaha, TX81Z Owner's Manual, pp.19-22 and71-74: https://usa.yamaha.com/files/download/other_assets/9/316769/TX81ZE.pdf
- Andrew Evdokimov, `ax81z`5848832989c7864092a277e9428d44f817c6173f, `Math.md`: https://github.com/iflyhigh/ax81z/blob/5848832989c7864092a277e9428d44f817c6173f/Math.md ; Git blob b23df480991d653deea9ab5434ab5eff3cf1da59.
- Same source, parameter/register descriptions: https://github.com/iflyhigh/ax81z/blob/5848832989c7864092a277e9428d44f817c6173f/VCED_and_ACED.md
- Matt Gregory, original patch tutorial and downloads: https://mgregory22.me/tx81z/filter.html
- Knives on Strings, XCent0.12.7,7May2026,DSP28: https://knivesonstrings.com/xcent (DX100-family BC approximation only).
- Existing unchanged BSD-3-Clause OPZ implementation: https://github.com/aaronsgiles/ymfm/tree/81aec25ccbb98f4873a255f7551ac4dadac59b4a . Full existing notice remains `../YMFM-LICENSE.txt`.
