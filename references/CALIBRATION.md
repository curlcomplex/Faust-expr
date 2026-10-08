# Shared cymbal reference evaluation

Product decision: retain REF-1 suspended cymbal, REF-2 crash and REF-3 ride; REF-1 is the first fitting target. Umbrella #6; CYM-A #2; CYM-B #4. The older collector manifest records selection status at collection time and is not the current product decision.

## Reproducible comparison

The workflow compiles both actual Faust engines from pinned PUBLIC commits and runs the same C++ driver at 48 kHz, internally double precision, 128-frame nominal blocks, with 350 ms of silent control settling and one sample-exact gate edge. It renders every one of the ten reference scenarios for both default engines, plus four REF-1 scenarios for the CYM-A candidate preset. Initial bodies and controls differ between the engines: this is a factory-preset baseline, not optimally calibrated model ranking or equal-geometry experimental evidence.

Original source audio is downloaded by the existing hash-pinned CC0 collector. The comparison verifies those SHA256s again. It resamples originals to 48 kHz when needed, aligns the first signal exceeding 0.5% of peak with 1 ms context, and examines six seconds from onset (zero-padding the shorter ride bell). All raw recordings remain unchanged.

Stereo powers are averaged, not phase-cancelled into mono. A 2048-sample Hann STFT with hop 512 is summed into 32 log-spaced bands from 80 Hz to 16 kHz, then averaged over [0,.04,.12,.35,1,2,4,6] second boundaries. Log-power cells are floored 50 dB below each signal's largest cell. The mean log-power difference is subtracted as a single gain offset; RMS of the remaining differences is the reported dB mismatch. This is a diagnostic, NOT a calibrated perceptual metric or percentage of realism. It does not resolve the detailed sub-20 ms contact transient.

Also report energy-weighted centroid, power fraction above 3.5 kHz, time containing 90% of the analysis window's energy, and unnormalised RMS. That energy-duration number is neither RT60 nor a full-tail decay constant. Preserve original dynamic differences separately; microphone gain, processing, physical dimensions and beater force are unknown.

## First candidate

`calibration-ref1.json` is the same candidate as CYM-A's `presets/reference-1-initial.json` at commit `5a48018e1dfb59ef28271ccf8e3d0dcc0b2d52b5`. Keep one body/stick configuration for all four REF-1 recordings. Only excitation velocity and strike radius change. Material remains bronze. No arbitrary per-hit mode bank, EQ or sample playback is introduced.

Exploratory search: a default plus 32 seeded Sobol body/stick configurations; 24 seeded local proposals; 32 proposals with unknown soft/hard strike velocities treated as nuisance parameters. Search windows were four seconds. The initial objective combined mean log-band error and a relative-dynamics penalty. A subsequent explicit multi-metric objective added absolute log2 centroid and energy-duration discrepancies; all preceding candidates were rescored. This objective revision is part of exploratory development, not a pre-registered blind trial.

The two stick recordings drive the fit; bell and unspecified general hit are excluded from the optimiser. The bell is a useful out-of-fit articulation check, not an untouched dataset: its initial baseline was inspected. The general hit's beater is unknown, so treat its comparison as descriptive rather than calibrated contact validation. Crash/ride recordings remain initial baselines, not claimed fitted presets. Subsequent versions must not drop these references or confuse them as dynamics of a single object.

The candidate reduces the coarse mismatch but still has a brighter strong strike and longer soft/bell tails than the recordings. A tested extra constant-damping control did not clearly beat it and altered nonlinear sample traces at zero; that DSP modification was not adopted. The source changes in this PR implement evaluation, not a new engine.

## Tests and provenance

Eight metric/schema tests exercise gain invariance, stereo anti-phase robustness, pitch and decay sensitivity, silence/nonfinite rejection and fixed body configuration. Actual renderer checks reject pre-strike activity, nonfinite audio/diagnostics and output clipping. Two comparison assertions require lower coarse mismatch on the fitted stick cases; they are NOT realism acceptance gates. `results.json` and raw floats/WAVs record the tested code, preset, sources and measurements.

No private repository checkout, credentials, model invocation, paid larger runner, deployment or merge. Artifacts expire after three days; keep the source commits and manifests for re-running. Current CPU measurements from a separate benchmark still refer only to CYM-A's prior engine, not CYM-B or host/iPad execution.
