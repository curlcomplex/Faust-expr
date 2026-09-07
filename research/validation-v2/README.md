# Percussion validation v2 — calibration 01 rejected

The user rejected the first calibration audibly. Its lower coarse spectral score does not constitute improvement. Preserve the original CYM-A and CYM-B audio and render conditions as separate, immutable listening anchors. Do not call a newly rendered factory preset under different conditions the original demo.

This directory introduces independent diagnostics for: first-sample/30 ms attack; four spectral resolutions; 1 ms energy envelope; 20/50/90/99-percent energy times; changing spectral centroid/flatness/high-frequency share; gain and clipping. Anti-phase stereo uses channel power, not a mono sum. No automatic gain fitting occurs in the metrics. Full compared durations and any gain/onset transformation belong in each result manifest.

The ten adversarial tests include the previously missed sample-zero spike, altered pitch, excess gain, tail truncation, extended decay, non-finite values, stereo cancellation and unrelated noise. These are tests of the analyser, NOT tests demonstrating realistic synthesis.

The numeric gates are provisional engineering tolerances, not a calibrated listening scale. Additional same-velocity round-robin crash/ride recordings estimate genuine hit-to-hit variation; the suspended-cymbal medium-stick and extra bell recordings provide validation not used in the initial dense-contact research settings. No candidate can be promoted merely because an average improves, and no threshold passing proves subjective near-identity.

Preserve raw files. Use the same sample rate and observation chain for direct comparisons. A fixed common gain for a whole instrument/articulation set is distinct from per-hit normalisation; document both physical/excitation settings and listening transformations. Do not conflate a fitted excitation velocity with a measured impact force.

Related: issue #6; model PRs #2 and #4; shared reference PR #5. No original model defaults changed here.
