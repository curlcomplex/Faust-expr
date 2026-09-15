# TR-909 recorded-anchor pass

Work #80 / draft PR #81; consumer CURLOP #279 / #271. The current pass qualifies the five synthesized engines (Kick, Snare, one shared Tom, Rim, Clap) in seven articulations. No GUI, voice allocator, asset-backed metal module, project migration or release is added.

## Acquired recording corpus

The original Jason Baker / Rob Roy Recordings set is dated 20 April 1995, with a header repair noted 12 May 1995. Its TR909SET.TXT credits fEEd at Manna Studios for supplying the hardware. The documented path is original TR-909 -> EMU ESI-32 -> sample dump to Sound Forge 3.0. It is dry mono 44.1-kHz PCM16. The samples were individually normalized. Unit serial, component calibration and output jack are not identified. This is one hardware/capture lineage, not an independent sample of many units.

Archive used: https://mirrors.xmission.com/aminet/mods/smpl/tr-909.lha
Archive SHA256: `36bf70b0e49ba330d105ee0b803afcc634736b083bcb41451588936b5ffa9b2a`.
Original notes SHA256: `9a6de7a5651710029c4fa0f0c493b11357deadf21cc26a9185df1bb0189063ee`.
The public `fluid-music/open-drums/tr-909/TR909all` mirror provided inspection of the filenames and original notes, but the CI measurements use the pinned Aminet archive, not a changing GitHub main branch.

160 WAV paths contain **159 unique byte streams**: RIDED4 and RIDED6 are duplicates. All path/hash/PCM/frame/descriptors are retained in reference-index.json. Crash/ride filenames have D but the original notation identifies their control as tuning, not decay.

The notes permit free distribution only under set-integrity and no-profit restrictions. We do not infer commercial runtime permission. The archive is read transiently in a temporary directory; whitelisted members are decoded from stdout rather than extracted to arbitrary filesystem paths. **Reference WAVs are not committed, embedded in the Faust programs, or included in review artifacts.** CH/OH/crash/ride remain blocked on separately cleared runtime assets and host resource integration. Having comparison recordings does not resolve that blocker.

The sampler/normalization/editing can affect comparisons. Raw reference levels cannot calibrate output gain, velocity or accent. The two clap recordings have numbered names but no unambiguous numerical velocity mapping. The rim filenames specify 63 and 127, but each was normalized, too.

## Targets and selection

Before inspecting the candidate match, primary anchor files were selected as BT3A0D7, ST3T3S3, LT3D3, MT3D3, HT3D3, RIM127 and HANDCLP1. Hardware knobs are recorded at 0/3/7/10 (A=10), not continuous noon settings. Each chosen filename and complete synthesized settings are in selection.json. Source UI defaults are NOT automatically these selected settings.

These seven anchors were fitted with a bounded deterministic search through **actual generated Faust C++**, using the existing renderer. Python supplies control scores and evaluates features, not substitute synthesizer audio. Search method, bounds, every parameter proposal/loss and generated-audio hashes are retained in fit-evidence.json in the conversation review bundle. Search renders are not counted as the frozen qualification's lifecycle checks. The frozen qualification does not optimise or silently adjust settings. Values are rounded to the declared .001 steps before qualification.

The metric weights onset-aligned cumulative-energy times, attack/body normalized spectral bands/centroids, and a tonal-peak term only for Kick/Toms/Rim. Exact windows, weights and onset rule are in tr909_reference.py. It is a coarse shape diagnostic, **not an authenticity percentage or a sample-null test**. Candidate audio is not normalized, gain-fitted, time-warped or corrected with EQ. Per-component adverse differences are retained alongside the aggregate score.

RIM63 and HANDCLP2 are checked after the selected settings are frozen; these are additional captures from the same unit, not independent hardware or fully blinded validation. The remaining control grid is inventoried for later work. **This pass does not establish a complete hardware-knob mapping, fit all 160 files, or validate all original TR-909 units.** Pinned Plaits/Andremichelle sources remain explanatory software references as previously documented, not separately executed full-instrument oracles in this pass.

## Preserved architecture and open questions

Prefer preset changes where existing controls account for the data. No existing DSP source is overwritten. The shared Tom entry is copied from the already-qualified PR71 consolidation; all twelve previous slot presets are tested for sample-identical recall. Single-note `gate`, `freq` in Hz, `velocity` and explicit accent remain; CURLOP owns polyphony, lifecycle and cross-instance choke.

No new controls are required. Extended ranges remain available. Reference settings do not equate literal control values across hardware and synthesis (e.g. hardware Snappy 3 is not our Snappy 0.3). Some fitted controls lie near search bounds: that is a warning about residual model/control mismatch, not proof that an endpoint perfectly recreates the circuit. The kick's fixed 60ms envelope plateau and the snare's held noise envelope remain model assumptions. Exact pitch trajectories, transient waveform/phase, ring texture, high-drive aliasing and full dial behavior still need more detailed evaluation and listening.

Our sample-rate-aware smoothing starts from its existing initial state; startup/onset sensitivity is recorded without refitting. Old versions/projects are preserved. These are laboratory candidates, not CURLOP recordings or installed factory presets. Named-device realtime/thermal/memory tests, typed bindings, GUI/save-reopen, licensed sampled-metal assets and Felix's final sound approval remain separate.
