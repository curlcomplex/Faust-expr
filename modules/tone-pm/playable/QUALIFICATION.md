# Tone playable qualification and evidence limits

Issue #19 / PR #44. Version 0.2 preserves the parent 0.1 source and its evidence. Consume the exact verified commit from the PR checkpoint, not a floating branch.

## Changes with musical consequences

The 0.1 draft only decayed after onset, latched every control and still saturated at Punch=0 through tanh(sine)/tanh(1). The 0.2 graph adds explicit held-note articulation and live smoothed Pitch/Ratio/Feedback/Modulation/Drive; guarantees a neutral Punch=Drive=0 path; reverses the modulation-envelope progression toward short bursts; and moves DC filtering before the final envelope. These are a NEW sound version, not an API-compatible silent replacement.

The first 0.2 run (bcfe926, Actions 34484966732) failed same-sample onset equivalence. Inaudible startup oscillation was filling the DC filter before the first note. The accepted e189540 fix gates the filter input with `seen` until first onset and then keeps it processing persistently. The oscillator is not recreated per hit, and thresholds were not changed. Preserve this regression.

## Actual DSP and independent oracles

`tone_finish.py` runs the full `tone_playable.py` suite, then adds the all-eight-control demonstration, identical-setting articulation comparison and documented reference-nearest collage. Actual Faust scalar/vector builds and envelope/control/raw diagnostic graphs are separate evidence from Python fixtures.

The first corrected exact-source run passed 153 renders / 287 checks. The final delivery adds three actual renders and ten checks: expected total 156 / 297 with the fixed eight references. Read the actual report instead of trusting these counts if the suite changes. The ten separate unit methods check the manifest, ratio arithmetic, score validation and analyzer behavior, not sound fidelity.

Tests cover 44.1/48/96 kHz; six clean pitches from 20 to 8000 Hz; ratio landmarks; initial silence; one-shot versus held mode; one-sample early release; release from actual attack level; live smoothing and legato; exact onset locks; velocity; 512 endpoint combinations inside one persistent trajectory; same-score segmentation at 1/32/64/127/128/256/512 frames; scalar/vector output; four independently rendered overlapping voices; and WAV-container decoding.

Independent float64 envelope formula covers longest 6 s exponential time constants over 50-second recordings. Corrected-run worst relative discrepancy was 1.576e-6 above -60 dB, under the predeclared 1e-4 limit. The raw clean-path harmonic test detects hidden saturation rather than merely checking the fundamental's pitch. Diagnostic peaks (e.g. a ratio of 8 or pitch 8000) must not be reported as audio peaks.

## References: useful but not a clone calibration

Eight fixed CC BY 4.0 public SYTN MP3 previews, Particles Into Waves / Winston Edwards, Syntakt Designer Drums. Original and decoded hashes, attribution and unknown metadata stay with the evidence. Firmware, controls, recording gain and per-hit processing are unknown. None is sampled into this instrument.

The older descriptor included recording RMS and a global window over unequal durations; it gave the carrier-only diagnostic a slightly lower score. That remains a caution, not evidence of a recovered hardware algorithm. The revised 10-second-horizon comparison uses energy-pooled spectral windows, energy timing and gain-independent features across the same 32 authored points per architecture. Full PM versus carrier-only is controlled by setting Modulation and Feedback to zero, not replacing other layers.

At e189540 the mean nearest distances were full PM 0.71474 versus carrier-only 1.03076. Several individual clips still favor carrier-only, while SYTN-05 and -07 remain poorly covered. Engine AND metric changed: do not compare with prior ~0.33 scores as an improvement percentage. All references were already inspected; none is claimed as untouched validation. This pool does not identify hardware control curves or establish audible equivalence.

## Cost and numerical limits

A warm, four-voice same-sound scalar/vector benchmark alternates order and repeats four times per block. It uses the one-shot mode; it does not include host UI, audio device or thermal contention. Record p50/p95/p99/max, object size and generated compiler stack files. Ordinary new/new[] interception is not malloc/aligned-allocation or whole-host proof.

The corrected hosted run favored vectorization by about 1.18x at 32/64/128 frames and 1.27x at 512. Independent C++ replay showed only about 1.04x at short blocks and 1.16x at 512. This is a same-sound compiler-mode comparison, not a universal iPhone/MCU speed claim. Retain both modes and measure the consumer.

High-register quality remains unresolved. A properly time-aligned, filtered-polyphase 48/96 kHz diagnostic was -48.25 dB relative RMS for a clean patch and -55.56 dB for Keys, but **+4.26 dB for 4 kHz carrier, 8x ratio and maximum PM/feedback/drive**. This extreme is severely rate-sensitive. It combines operator aliasing, one-sample feedback delay, phase/filter and nonlinear differences; it is not isolated alias energy. Do not present the whole numerical 20–8000 Hz domain as a musically qualified release range. Reference-firmware matching and a production anti-aliasing/range policy remain separate tasks.

Ordinary onset resets can interrupt a ringing signal. Neither peak bounds nor control smoothing guarantee click-free voice stealing. The corrected stress run reached about 0.8592 peak and 0.9002 maximum adjacent-sample change; human judgment is still required. Live zones must be applied by the audio owner at a compute boundary, not concurrently from a GUI thread.

## Delivery evidence

The compact replay package must contain source, exact generated headers, scores and golden raw hashes, frozen references/attribution, listening WAVs and an executed verification command. Bulk raw floats can be regenerated instead of duplicating hundreds of megabytes. An independent C++ replay is NOT a second Faust compilation; exact same-compiler repeatability and cross-compiler tolerance are different claims. Record archive integrity and full decoding of the actually linked files.

Felix's Tone approval, consumer UI acceptance, actual device memory/realtime/thermal behavior and any merge/release remain outstanding until separately demonstrated. This batch does not change Tracker source or credentials.
