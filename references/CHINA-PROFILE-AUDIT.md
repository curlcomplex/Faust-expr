# China profile audit — 7 September 2026

Part of Cymbal lab #6, shared references PR #5. The user found the recent upturned-rim demo interesting but not convincingly China-like, with intrusive upper tones. **Do not treat that profile example as validated China synthesis.** The successful original sounds remain preserved.

## Sources and scope

Audited the exact delivered `cymbal-profile-09.zip`, SHA256 `ba485a9ca34682071b9e677f505f5be126f9083f1aabbfca67049f0ae5cf5514`, including its C++ renderer and numerical profile grids. This audit did not change either original physical-model branch or the dense model's fitted coefficients. The diagnostic synthesis was standalone C++, not a new Faust compilation.

Retrieved two strong takes each of a **Paiste 18-inch Innovations China** and **Masterworks 20-inch Custom China** from Alexander Holm's Salamander Drumkit, mirror `studiorack/salamander-drumkit` at `8b6faa8847b4c02f4b5dd42c1836c28ffbe89d33`. Originals passed file-length, Git-blob and SHA256 checks. Mirror files actually decode as 48 kHz stereo, despite the general historical README's 44.1 kHz description.

The creator describes garage recording and post-export sample normalisation: no force-calibrated loudness or velocity inference is justified. The mirror includes CC BY-SA 3.0; the author separately declares public domain from March 2022. Preserve attribution and both provenance statements; do not relabel CC0.

Collection commit `4a74117d1e3e36a2fec403b3b3b6c90b82e98d5c`; successful run https://github.com/curlcomplex/Faust-expr/actions/runs/34162171023 ; artifact `10032947688`; verified ZIP SHA256 `237f4dd298e6447f3a22b705c12930b27fe7136983b19d85fefe3c0cae13a21c`. Standard bounded Linux runner, no model/API calls or private sources.

## Confirmed implementation defects

1. The profile producer used `A = area*E/(1-nu^2)` for the membrane stiffness weight instead of `area*E*h/(1-nu^2)`. Reproducing the omitted-thickness equations matches the archived proxy frequencies to maximum relative error `5.8e-12`. Including h restores the preceding thickness solver's neutral matrices exactly. The erroneous local membrane weight was approximately 321–833 times too large for the assumed thickness range; that is NOT a global frequency multiplier. Neutral ratio tests and validation against the same faulty producer cannot detect this dimensional error.

2. For the prior China-labelled setting (bow 1.25, flange 0.5, flange start 0.76), **2,252/4,207 target resonances exceed 23,040 Hz**. The renderer clamps them all to that frequency at 48 kHz instead of appropriately suppressing above-band energy. An otherwise matched 192 kHz rerender followed by low-pass downsampling removes the false ceiling line. Its measured 23,020–23,060 Hz power share drops from 3.16% to effectively zero. **The audible-band narrow peaks persist**, so this ceiling error alone does not explain the user's listening impression.

## Reference comparison

Compared a single isolated edge hit at the same original profile/strike settings, not the full overlapping three-hit old demo segment. Stereo powers are averaged; waveforms are not mono-summed. Onset analysis is centred and retains 1 ms pre-onset.

- Strongest ~11.72 Hz FFT bin's share of 4–16 kHz power, measured 50–500 ms after onset: **1.06–1.39%** across the four real takes versus **16.95%** in the current profile model. A prominent model peak is near 11.6 kHz.
- Cumulative 90%-energy time within 5.5 seconds: **0.374–0.434 seconds** in these takes versus **1.178 seconds** in the model. This is not T60 or a universal China decay target.
- The units-only correction shortens this energy measure to 0.923 seconds, but strongest-bin concentration rises to 26.6%. It is a diagnostic, **not an accepted sonic improvement**.

Eight audit test methods passed: source hashes, reproduction of faulty producer, dimensional matrix correction, above-ceiling count, actual render lengths/finite peaks, high-rate ablation, listening-file hashes, and level-matched comparison integrity. No realism pass is implied.

## Model limitation, beyond the bugs

The profile experiment redistributes sorted resonance frequencies while retaining the original suspended-cymbal excitation coefficients and decay data. It does not consistently update modal identities, spatial excitation, radiation and nonlinear energy exchange with changed geometry. That is a credible cause of disconnected ringing rather than a new coherent China object, but source inspection is not a unique causal decomposition of every audible difference.

Nguyen/Touze's taper/curvature study and Ducceschi/Touze's nonlinear impacted-plate study support investigating shape, thickness, excitation, loss and nonlinear transfer together. Neither validates our China proxy. China designs themselves have varied sustain and timbre; retain both reference instruments.

## Next gate

Correct and validate the shell assembly and above-band handling, then evaluate coherent profile-dependent excitation/loss/spatial/radiation changes and mode-crossing treatment before adding more surface/damage controls. Do not conceal the mismatch with a fitted EQ, a sampled crash layer or a hidden China mode. Preserve the original good sound and compare real sources alongside controlled transformations.

The chat audit package contains original reference recordings, direct comparison WAVs with cue times, the units-only diagnostic, source, data, tests and spectral plots. No model fitting, merge, force push, or replacement of the original instrument occurred in this audit.

## Primary research and provenance

- Creator: https://rytmenpinne.wordpress.com/sounds-and-such/salamander-drumkit/
- Nguyen and Touze (2019): https://doi.org/10.1121/1.5091013 ; sound/experiment companion https://perso.ensta.fr/~touze/tapercymbals.html
- Ducceschi and Touze (2015): https://www.research.ed.ac.uk/en/publications/modal-approach-for-nonlinear-vibrations-of-damped-impacted-plates/
- Manufacturer reference for China sound variation: https://www.paiste.com/en/products/models/123-china
