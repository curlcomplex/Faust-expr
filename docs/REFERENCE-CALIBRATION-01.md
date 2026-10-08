# First reference calibration — CYM-A

Related: umbrella #6, shared reference/evaluation PR #5. This is an optional candidate preset for the existing CYM-A engine, not another cymbal implementation or a change of factory defaults.

All three source sets remain comparison targets. The first fit uses only the VCSL suspended cymbal's soft and strong wooden-stick recordings. One shared object and stick configuration is applied to both. The bell and unspecified general-hit recordings are excluded from the optimiser; the bell is the more useful out-of-fit articulation check because the general-hit beater is unknown. This is exploratory fitting, not a blinded experiment.

Apply `body_and_stick` from `presets/reference-1-initial.json`; set `strike_radius=0.82` for the two stick hits and `0.13` for the bell; choose the corresponding velocity. Keep all other controls at their factory values. For listening, one fixed output gain can be chosen for the complete set, not an independent gain/EQ for each hit. The reference recording's original gain and strike force are unknown; the fitted velocities are nuisance assumptions, not measured impact speeds.

## Local executed result

Actual unmodified Faust-generated C++ for commit `43f39b60b7b239f8e460d1d8ee7b6f027a0532fb` was compiled with g++ -O2 and rendered at 48 kHz, using a common 350 ms parameter-settling interval before one sample-exact gate edge. The shared procedure also rendered CYM-B at `821736879bdea6c1c58ce9f0d932e0ebe5196840`. Their initial factory presets are different; this is not a matched-geometry or optimally fitted model-ranking experiment.

The six-second log-band/time RMS mismatch (dB) for CYM-A changed as follows:

| Recording | Factory | Candidate |
|---|---:|---:|
| Soft stick (fit) | 15.219 | 12.949 |
| Strong stick (fit) | 12.827 | 10.901 |
| Bell (excluded from optimiser) | 11.716 | 10.829 |
| General hit (beater unspecified) | 12.511 | 8.912 |

The metric uses stereo power (not phase-cancelling mono summation), onset alignment, 32 log-spaced bands from 80 Hz to 16 kHz and seven time intervals. A constant gain offset is removed for timbre comparison. A smaller mismatch is NOT a percentage of realism. Relative dynamics, brightness and energy-decay duration must be examined separately. The fit used four-second windows, while this table includes six-second windows; the factory and candidate in each reported comparison use the same window.

## Remaining discrepancies

The candidate's strong hit is still brighter than the recording: energy-weighted spectral centroid about 4.37 kHz versus 3.54 kHz. Its soft hit retains 90% of its six-second-window energy by about 2.90 s versus 1.79 s for the recording. Its bell reaches that point at about 1.51 s versus 0.43 s. Thus the improved summary metric does not establish a faithful attack or decay.

An added frequency-independent modal-damping term was tested locally but NOT adopted. It did not clearly improve the multi-metric fit, and inserting the control changed compiled nonlinear trajectories even at zero while leaving the tested linear response identical. The failed strict default-waveform-equivalence check is retained in the downloadable research evidence; no failing DSP change is included in this commit.

## Scope

No samples, arbitrary per-hit resonance tables, fitted output EQ, or reverb were added to the instrument. No core DSP, geometry generator, factory preset or existing test was changed here. Crash/ride references are baselines for subsequent separate object configurations within the same engine, not asserted to be calibrated already. The parameter-search source/logs and raw before/after recordings accompany the chat evidence; reproducible cross-model evaluation is being added to PR #5. Inspect its actual exact-commit CI results before treating the new shared evaluation as CI-verified.
