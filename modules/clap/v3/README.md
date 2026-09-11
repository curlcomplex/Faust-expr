# Clap v3 — clap identity rebuild, pending musical approval

Canonical entry: `modules/clap/v3/clap.dsp`, identity `clap / 0.3.0-experiment`.
This supersedes v2 as the candidate to audition in PR #50. V1/v2 remain preserved for old project identities and negative-control evidence; their numerical qualification is not musical acceptance.

## Why v2 was rejected

The default voice put an equal-power pitched PM body under weak clustered noise, with a noise tail starting at the initial trigger. The body dominated and filled the gaps. Whole-hit descriptors and extensive arithmetic checks did not establish that the result was recognizably a clap.

V3 removes the periodic body entirely. Three short noise pre-attacks lead into a main attack; the separately filtered, decorrelated noise tail starts at that final attack, not at trigger time. Burst attacks and releases are finite. Broad mid-band noise formants provide the body; there is no sine/PM drum oscillator and no runtime sample playback. Tail=0 genuinely disables the tail.

The default onsets are 0, 7.6048, 15.5232 and 23.0496 ms. Spacing controls a 4–28 ms base gap. Snap adjusts transient width/brightness. Decay controls the tail's 18–450 ms exponential time constant; it is not a total-duration value. Noise/filter state persists between hits; controls and velocity latch at onset; note-off does not choke.

## Reference selection and limits

The actual audio comparison uses two clustered-clap examples from VERMONA's isolated DRM1 MKIV CLAP demonstration, at 35.0407708333 and 95.0449166667 seconds. The complete source MP3 is SHA256 `37e0d94eccf9475bcf60f4e6eeced339cf24081f1acfa1713544b38258a9884f`. A changed source fails the capture rather than silently reusing timestamps. Knobs, capture gain and exact signal path are unknown. This is a contextual reference, not a controlled calibration or clone claim.

- Manufacturer instrument/demos: https://www.vermona.com/produkte/drums-percussion/produkt/drm1-mkiv-2/
- Isolated CLAP demo: https://www.vermona.com/fileadmin/user_upload/products/drm1mk4/demos/106_drm1mk4_clap.mp3
- Architectural comparator: Hexinverter Mutant Clap, https://www.ericasynths.lv/hexinverter-mutant-clap-3278/
- Broader timbral comparator: WMD Fracture, https://wmdevices.com/products/fracture (micro-sample/granular instrument, not a sample-free synthesis topology).

Only VERMONA audio was measured. No claim of a head-to-head recording comparison with WMD or Hexinverter. The renderer never reads a reference sample. The final workflow removes the complete manufacturer recording before upload and keeps only short excerpts inside the criticism/A-B audio.

## Evidence and what it does NOT prove

Run `python3 tools/modules/clap_v3_delivery.py --out build/clap-v3/run` with Faust, C++17, NumPy and SciPy. It uses the existing native `tools/modules/render.cpp`, not a Python replacement synthesizer.

43 actual renders / 62 checks: scalar and vector Faust builds; eight presets at 44.1/48/96 kHz; blocks 1/32/64/127/128/256/512; silence, velocity, note-off and latching; 64 six-control corners with alternating extreme note proxies; 96 rapid retriggers; tail settling; collapsed-burst and rejected-v2 negative controls. Audio in the default's two secondary attacks rises about 24 dB over the immediately preceding gaps; v2 fails that gate. This is a contrast diagnostic, not a perceptual classifier. Test coverage does not certify a device callback or click-free voice stealing.

Reference measurements use the same first 200 ms and padded short-window analysis for all sounds. Preliminary independent local results: v2 spectral centroid 787 Hz, v3 2526 Hz, references 2144/2640 Hz. Time to 90% of first-200ms energy: v2 81.6 ms, v3 55.2 ms, references 26.5/35.3 ms. Eight-band Jensen-Shannon distance falls from 0.591/0.510 to 0.232/0.323. Energy-CDF timing distance falls from 13.71/9.75 to 7.78/5.76 ms.

V3 is intentionally not identical: it is leaner below 600 Hz (about 2.3%, references 17.5/26.3%) and its default tail is longer. These contrary results are retained. No parameter optimizer or nearest-reference search was used. These numbers are not a percentage musical-quality score and do not replace Felix's listening decision. Exact final hosted data are in the run's JSON reports.

## Listening files

`tools/modules/clap_v3_reference_report.py` generates:

- `01_old_reference_new.wav`: 0s rejected v2; 2s VERMONA B; 4s v3; 6s VERMONA A; 8s v3. Two hits per two-second block. First-200ms RMS match, one common headroom gain, 5 ms end fades on the 300 ms excerpts; no EQ/compression/reverb/limiter.
- `02_eight_claps.wav`: Classic, Tight, Wide, Dark, Bright, Room (long noise tail, not an actual reverb), Driven, Dry. Pairs every two seconds, second-hit velocity 0.65. One global gain preserves relative preset/velocity levels.
- Individual preset WAVs, source timestamps/hashes, all audition gains, and objective comparisons. Raw float render evidence remains separate.

## Tracker ABI / integration boundary

This is a deliberate breaking sound/parameter revision, not a silent update to v2's body controls. Six columns: **Spacing / Snap / Decay / Tone / Tail / Drive**. Decay stays in column 3. See `manifest.json` for P1–P6 mapping. The old Body/Body Envelope pairing is removed because there is no oscillator body. `pitch_hz` remains a 70–900 note proxy but now shifts a noise formant; it does not specify an audible fundamental.

Never reinterpret v2 automation numerically as the new controls. Keep old project instances version-pinned; create v3 with its own defaults and explicit version. Mono DSP per host voice; host owns polyphony/voice stealing. No Tracker app code or merge is performed by this PR. The consumer issue must point to this source/ABI instead of treating v2 as musically accepted.

Remaining gates: Felix's listening approval, consumer adapter implementation, and named-device real-time/retrigger/thermal validation. Draft/unmerged is intentional.
