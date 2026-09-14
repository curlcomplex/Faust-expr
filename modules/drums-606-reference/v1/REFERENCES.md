# 606 recorded-anchor tuning pass

Issue #78 / draft PR #79. This is a lab selection, not a CURLOP factory package or sonic approval. Six source entries serve seven articulations: a single Tom engine has Low/High presets. Every instrument is a single note. The host owns note allocation, voice lifecycle and cross-instance choke routing. There are no new GUI controls in this pass; expanded controls remain CURLOP #284's later work.

## Acquired source and limitations

Primary archive: <https://mirrors.xmission.com/aminet/mods/smpl/tr-606.lha>
Original notes: <https://mirrors.xmission.com/aminet/mods/smpl/tr-606.readme>

The notes date the recordings to **28 September 1998**, identify **Ed's TR-606 into an Akai S3000XL**, explicitly state no added effects or EQ, describe top/tail editing in CoolEdit, and identify `acc` files as accented hits. The author is unnamed; serial, unit calibration, tempo and original level settings are not recorded. The sampler signal path may color the recording. This is one useful identified hardware archive, not a metrology-grade dataset or a representative sample of all 606s.

Archive SHA256: `8467ac4c3d3e3d1d298e8b5d3bb567a10b4ea3d99e6f37c5010833d6bc8da59a` (572184 bytes).
Notes SHA256: `9bd04f9d0212ce6ebd36f260fe51646cd953523a1058a1947ebf2b40825890b2`.
All 14 individual mono PCM16/44.1-kHz hashes are in `selection.json`. Filenames pair bass, snare, ltom, htom, chat, ohat and cymbal with their `acc` counterparts.

No redistribution licence is inferred. The scripts fetch the hash-pinned archive transiently, decode it in a temporary directory and retain only provenance/hashes/descriptors. **No recording from this archive is in git, the generated DSP, or the audition bundles.** The audio delivered for review is our previous and revised synthesized output.

Accented recordings sometimes peak lower than normal ones, so levels cannot establish an authentic accent-gain law. Our host-facing velocity and authored accent behavior remain implementation contracts, not newly calibrated voltage models. Existing OH decay remains manual; the archive's unknown tempo cannot establish original tempo coupling.

## Other sources considered

- Wave Alchemy 606 Drums: <https://www.wavealchemy.co.uk/product/606-drums/>. Creator identifies original TR-606, API-preamp capture and multiple hits/accent layers. The accessible download path required checkout, not a directly available archive. No login/checkout bypass; not an acquired validation corpus.
- Roland Clan <https://www.rolandclan.com/library/tr-606/>: sample archive lead, but hosted page retrieval timed out and was not repeatedly retried.
- Tubbutec <https://tubbutec.de/6m0d6/> and Peter Barata <https://www.baratatronix.com/blog/606-cymbal-and-hi-hat-synthesis>: useful architecture/expanded-control context. Barata's ideal calculated oscillator values are not measurements of Ed's machine. No claim of a line-for-line third-party 606 software port.

## Method and selection

Baseline is PR71 `8aa548fc1e108553bf6652a7794db4e02c8be0b2`, retaining PR67 voices and its subsequent Tom consolidation. We rendered the actual baseline through the existing Faust/C++ Lab. Normal recordings were the fitting targets; accented files were checked only after parameters were selected. The latter are another hit/layer of the SAME machine, not an independent device test or a sealed dataset.

The diagnostic objective is explicit in `tr606_reference_fit.py`: logarithmic errors of onset-trimmed 50/90/99-percent cumulative energy times; normalized attack/body band-distribution and centroid differences; a body-peak term only for tonal voices. Fixed windows are 0-25ms and 25-150ms. Tail-window details are also reported. Peak/DC/raw levels remain recorded, not gain-fitted. No waveform-null, time-warp, EQ, or loudness normalization is used to manufacture agreement.

Fitting is bounded and deterministic. Actual generated Faust C++ supplies every candidate waveform; Python computes descriptors/scores, not replacement synth audio. The accepted selection is frozen in `selection.json`; `tr606_reference_qualify.py` does not refit. Keep per-descriptor adverse differences visible. A reduced aggregate shape distance is not an authenticity percentage or an audibility guarantee, especially for the exact metallic partials.

Changes supported by this pass:
- Kick, unified Tom and CH: preset settings only. Their DSP is unchanged.
- Snare: new versioned behavioral candidate includes onset pitch relaxation, band-limited noise, and removes v1's compulsory noise floor at Snappy=0. Noise/body remain independently adjustable via the existing controls. The relaxation is a fit hypothesis, not a transistor-level derivation.
- Cymbal: new versioned strike/quiet-long-tail envelope distribution. The old version's initial energy was too spread out even when total decay was shortened. Tone and metal spread remain live and editable.
- OH: same equations as v1, manual -60dB decay range extended from 2.2 to 4 seconds so the long recording can be approached. Same-parameter comparison verifies the retained range. No tempo-model claim.

The spectral match remains imperfect: selected snare body brightness, OH decay distribution and exact metal partials need listening and additional unit/capture comparisons. The new presets do not remove extended ranges. Old sources/defaults/projects are not rewritten. CURLOP faceplates, typed wiring, startup-control initialization, independent voice/choke behavior, save/reopen and target callback/thermal testing remain separate gates.
