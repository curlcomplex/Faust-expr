# Reference-led instrument design protocol

Work: [#11](https://github.com/curlcomplex/Faust-expr/issues/11), [#12](https://github.com/curlcomplex/Faust-expr/issues/12), [#14](https://github.com/curlcomplex/Faust-expr/issues/14). This is a proposed research method. No new module or measured fidelity result was produced by the documentation pass.

## Evidence hierarchy

A manufacturer's technical disclosure establishes only the disclosed detail. A manual supplies observable controls, not necessarily their internal equations. A controlled direct recording establishes measured behaviour under its capture conditions. A demo with unknown routing is useful discovery evidence, not automatically a calibration target. Our candidate topology is a hypothesis until experiments support it.

Log the distinction in every experiment. Descriptions such as analog, FM, physical or reference-matched are not evidence on their own. Electrical circuits and DSP models can produce similar outputs through different mechanisms; black-box fitting rarely identifies a unique internal architecture.

## Reference manifest

Record creator/source and retrieval date, rights/storage treatment, original byte hash and format, hardware/firmware/machine, full known parameter state, note pitch/velocity/gate sequence, routing, effects, level, capture path and conversion history. Unknown values remain unknown. Keep processing-free references where possible and mark all deviations.

Pin firmware because algorithms and available parameters can change. Keep Syntakt and Model:Cycles recordings separately identified even for similar roles. Keep Basimilus Alter and Alia separate. Do not combine them into a single target that no physical instrument actually produces.

Do not publish third-party recordings, extracted tables, firmware or private user material merely because it can be heard online. Store references in a permission-appropriate location. A public manifest may contain hashes and bibliographic information rather than the audio. Private storage is not a substitute for evaluating distribution rights.

## First PM-kick experiment

Choose rounded/sub-heavy, short punchy, harmonic and aggressive anchors. This is a small coverage set, not four targets to overfit independently. With one baseline state, obtain single-control sweeps and selected two-control interactions, plus low/mid/high notes and velocities. Capture repeated hits to determine onset variation; include short/long gates and changes while the tail rings.

Register training and held-out sets before parameter search. Hold-outs include intermediate control settings, unseen parameter combinations and at least one dynamic phrase. When a hold-out influences later tuning, record that it is no longer untouched validation and reserve fresh data. Demo inspection is not the same as an untouched sample.

Repetitions establish a reference's variability. A candidate error below the instrument's own repeat variability can be informative, but does not certify perceptual indistinguishability. Do not suppress hardware noise/drift in the target and then claim to have modeled it.

## Parameter identification, not patch roulette

Fit identifiable components in stages: low-color carrier/tuning; amplitude envelope; pitch trajectory; harmonic evolution; attack; nonlinear placement and level; joint control mapping. Use ablations to hear what each part contributes. If a complex attack prevents robust pitch estimation, flag uncertainty rather than letting an octave error dominate the fit.

Candidate formulas may include a carrier phase accumulation, an amplitude envelope, a pitch envelope in log-frequency coordinates, and a PM network. These are starting hypotheses, not exact Elektron equations. A ratio table, feedback law or time constant must not be asserted merely because it makes one preset attractive.

Prefer a small number of alternative explanations and discriminating captures. Examples: reset versus free-running phase predicts different repeated attacks; static shaping versus envelope-dependent feedback predicts different spectral decay; drive before versus after an envelope predicts different level/time behaviour. Keep rejected hypotheses and parameter-search bounds with the result.

## Macro surface fitting

A released macro is a path through a higher-dimensional kernel space. Define the path in source, with stable IDs, units and endpoint handling. Fit its behaviour at more than the default. Test the interaction of paths, not only one knob at a time. Avoid a collection of individually matched presets with discontinuous interpolation between them.

Do not reduce the reference's degrees of freedom to fit six visible knobs before learning the reference. A later compact page can select, combine or contextualize controls after listening tests. The lab panel may expose internals; it is not the shipping UI.

## Rendering

Render the actual Faust implementation. Keep a sample-exact score and explicit ordering. A generic runner must support one-shot and sustaining instruments and effects with inputs, rather than reusing the cymbal's one-sample gate for everything.

Check varied render segmentation including one-sample fragments, non-power-of-two sizes and host-sized blocks. Use persistent state across intended repeated hits; also test fresh/reset instances separately. Validate control names/ranges and finite values. Record seed, source, mapping, score, compiler, dependencies and output hashes.

## Metrics

Use several views of the error, not one success number:

- Bass: include below 80 Hz, long enough windows for low fundamentals and meaningful pitch-trajectory estimates.
- Attack: short windows and time-domain envelopes; long spectral windows alone smear the very feature being fitted.
- Timbre/tail: multi-resolution spectra, spectral evolution and bandwise energy decay.
- Dynamics: absolute/relative gain, peak, RMS, velocity relationships and crest behaviour without per-hit normalization.
- Noisy/inharmonic voices: spectral/temporal statistics over repetitions, not mandatory sample correlation.
- Effects: repeatable impulses, bursts, sine/multitone tests and music; modulation/nonlinearity may require several responses rather than one LTI impulse model.

Phase-aware comparison requires aligned deterministic signals. Do not artificially align away the transient/timing error being measured. Pitch estimators must not search only near the expected note when the concern is an octave/register mistake. State estimator confidence and inspect the full spectrum where appropriate.

A gain-invariant spectral loss can help separate timbre from level, but pair it with gain-preserving results. No scalar loss is a realism percentage or a substitute for a user's auditory judgment.

## Listening package

Deliver a small side-by-side sequence: raw-relative-level anchors, separately level-matched timbre comparisons, velocity accents, held-out settings and a musical pattern with locks. Record every audition transformation. Do not apply compensating EQ, reverb or per-hit gain secretly. Long raw outputs may stay in backed-up local evidence storage; short audition artifacts can be uploaded deliberately.

Ask for a concrete sound decision, not a vague 'does it work?' Preserve Felix's selected version and the reason. A numerical improvement rejected in listening is rejected musical evidence, not a reason to silently replace the selected baseline.

## Completion language

Report separately: source implemented, actual DSP built/rendered, numerical checks, reference differences, listening approval, target performance and release availability. Missing controlled reference metadata limits fidelity claims; it does not prohibit clearly labelled exploratory synthesis. No claim of native/iPhone/Eurorack performance without that specific evidence.
