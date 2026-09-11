# Analog snare v2

Issue #47 / PR #48. Distinct conventional two-mode body, shaped noise tail and clustered crack; no PM carrier network and no samples in the instrument. Six synthesis columns: Balance / Crack / Decay / Noise Color / Tone / Drive. Pitch, velocity, gate are separate. No chord or internal polyphony.

The initial root v0.1 draft is retained but is not this delivery entry. Its preset JSON was not valid JSON, it had mandatory noise even at minimum Balance, and its delayed crack events began discontinuously. V2 has valid authored patches, full body/noise mix endpoints, individually decaying body modes, convex noise-color weights and finite-attack sub-excitations. All values latch at onset; gate-off does not choke. Noise and fixed filter state continue across notes; a sleeping host changes that state policy and must qualify it.

`reference.dsp` evaluates three sine functions directly. `snare.dsp` uses ordinary 4096-point linearly interpolated sine lookup, preserving all other processing. `no-tail.dsp` removes only the sustained noise branch. `single-crack.dsp` removes only the delayed sub-impacts. `envelope.dsp` is a two-channel diagnostic, not an instrument. Never ship diagnostic paths accidentally.

## References and claim boundary

Manufacturer prior art: [Erica Synths / Hexinverter Mutant Snare](https://www.ericasynths.lv/hexinverter-mutant-snare-3277/) documents two analog tonal oscillators, a separate noise/snappy section and tone/decay/filter controls. [Erica Synths / Moritz Klein EDU Snare](https://www.ericasynths.lv/news/edu-diy-snare-drum/) provides manufacturer demonstration media. [Roland TR-808 controls](https://support.roland.com/hc/en-us/articles/201963539-TR-808-Technical-Specifications) documents Tone/Snappy as distinct controls. These are architectural/listening leads, not source of captured sample matches. No circuit values, firmware or implementation copied.

The eight CC BY 4.0 Winston Edwards / Particles Into Waves SD Basic/Vintage preview recordings are broad electronic-snare comparison context only. They are DIGITAL machines with unknown settings, firmware, levels and per-hit processing. They are NOT SD Classic, TR-808 or analog-snare calibration data. The acquisition script checks the creator license; retain hashes, source manifest and attribution. No claim of waveform matching or recovered macro curves follows. Do not fill missing hardware captures with mislabeled digital samples.

The PM comparison uses four authored counterparts, same note timing and documented phrase-level gain matching. It is not an exhaustive search proving Tone/PM cannot recreate a particular spectrum. Felix's listening choice decides whether this separate conventional instrument earns its place.

## Qualification commands

```sh
python3 -m unittest discover -s tests -p test_analog_snare_delivery.py -v
python3 tools/modules/fetch_snare_references.py --out build/snare-analog/references
python3 tools/modules/analog_snare_delivery.py --out build/snare-analog/run --references build/snare-analog/references
```

Source builds use actual Faust, scalar and vector C++, independent envelope math, sample-exact scores and malformed-score rejection. Watchdogs are configurable engineering budgets, not a fundamental Faust limit. Reference acquisition is separate from core qualification; a missing comparison dataset must be reported explicitly. `--replay` recompiles verified generated headers; it is not a second Faust compilation.

No output limiter or audition normalization manufactures finite-output passes. Equal-power Balance coordinates body/noise gain; Crack may remain audible with Balance at either endpoint. Set Crack0 and Balance0 for pure body tests. Drive0 is exactly neutral shaping. Long envelopes use direct exponentials and cut after16 time constants. Noise sample-rate scaling approximately preserves fixed-band power, not sample-for-sample random realizations across rates.

Tested compile/render success, human musical approval, hardware fidelity, and actual Tracker/iPhone acceptance are separate gates. Source and table lifecycle need consumer handling: initialize shared sine table once before rendering, use instanceInit for subsequent voices, do not call classInit concurrently with audio. Preserve previous saved sound identities. The full Fourier Morph is unrelated and remains intact.
