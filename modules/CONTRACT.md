# Portable module contract

Design specification for [#13](https://github.com/curlcomplex/Faust-expr/issues/13); not an implemented ABI. Implement the smallest typed representation that the first kernel and test host need.

## 1. Three layers

**Kernel:** persistent DSP state and deterministic signal processing for declared inputs, controls and outputs. No knowledge of Tracker rows, CURLOP graph IDs, JUCE, web commands, file paths, MIDI transport or voltage calibration.

**Instrument definition:** authored mappings from musical controls/macros to kernel quantities, defaults, valid ranges, trigger/voice semantics and sound identity. Mappings are source, not undocumented host glue.

**Adapter:** host presentation, scheduling and unit conversion; voice allocation when the host owns it; buffer/channel wiring; lifecycle and packaging. A hardware adapter handles ADC/DAC/CV units. A different adapter must not silently change the mapping or oscillator law.

A module may legitimately use a composite internal graph. It is not required to expose every internal quantity as a public parameter. Debug-only controls must be distinguishable from the released sound contract.

## 2. Definition fields

The first manifest/schema should represent these concepts without speculative manager classes:

| Field group | Required meaning |
| --- | --- |
| Identity | Neutral stable module ID, release version, sound-compatibility version, immutable source/mapping digest |
| Role and I/O | Instrument/effect/modulation source, input/output counts, mono/stereo policy, declared auxiliary outputs |
| Controls | Stable IDs, semantic units, finite bounds/defaults, normalization/display mapping, categorical values and endpoint treatment |
| Timing | Trigger versus gate, pitch/velocity application time, latching/smoothing policy, supported control rates |
| Lifecycle | Initial state, retrigger/reset/choke semantics, persistent tails, supported state restoration |
| Resources | Latency, tail policy, precision/backend support and measured memory/compute evidence |
| Provenance | Compiler, transitive libraries, build flags, tables, score/reference identities, source/dependency licenses |

A manifest is a contract; a runtime must reject unsupported versions or invalid control IDs rather than guess. It must not wrap invalid enum values or propagate NaNs into DSP. Range policy is explicit: clamping a finite physical control can be valid; silently changing its units is not.

## 3. Units and musical events

Pitch at the kernel boundary has an explicit unit, preferably Hz where appropriate. Semitone offsets and normalized macros remain separately identified. Do not use a normalized MIDI-number ratio as an undocumented universal pitch type. Microtonal frequencies are representable; a host's chosen scale is not secretly applied twice.

The test score uses integer sample offsets after an explicit time-to-sample conversion. At a shared offset, apply declared parameter/pitch/velocity updates before the associated onset; define note-off/retrigger ordering and whether multiple triggers at one sample are representable. Do not let container iteration order choose it.

A trigger-only voice may ignore note-off musically, but its trigger input still needs a real edge or explicit event. A sustaining voice needs gate and release semantics. A one-sample high pulse is not the same as a long held gate. A wrapper must not require an uncomputed zero-gate interval to reset a sleeping voice's edge detector.

## 4. Controls are time-varying contracts

Each control declares whether it is sampled at note onset, held per voice, continuously updated at event boundaries, smoothed over time, or supplied as an audio-rate signal. Faust UI zones normally changed between compute calls do not by themselves implement an audio-rate CV input.

Smoothing durations use time/sample-rate-aware coefficients. Do not smooth all parameter locks and transients indiscriminately, and do not leave live gain/filter movement discontinuous without an intentional policy. The reference may deliberately click or pitch-bend under a particular action; distinguish faithful behaviour, optional polish and accidental errors.

Macro mapping can be piecewise, nonlinear and multi-parameter. Specify units, breakpoints, interpolation, domain bounds and endpoint behaviour. A map must not have unrelated discontinuities caused by an off-by-one bin or integer conversion. Categorical controls can have deliberate detents. Fit and audition the mapping across the range, not only at its default.

## 5. State and polyphony

Repeated hits normally reuse the declared persistent object/voice. Freshly constructing the DSP for every hit is a separate test and cannot conceal stale-state bugs. Reset, choke, hard stop and voice retirement are different operations. Define whether each clears resonators/delay memory, fades, allows a release, or starts a new object.

Never suspend a stateful processor solely because the current output block is quiet; delayed energy or a pending excitation can remain. A sample-time-based tail declaration may be conservative. Infinite tails or self-oscillation require an explicit mode and resource policy.

A Chord module can contain several internal oscillators/voices. Host polyphony is additional and must be counted explicitly. A host note-to-chord generator must not accidentally multiply the internal chord again without deliberate user choice.

Prepare/allocate outside the callback. Publish prepared state using a coherent ownership handoff. Do not mutate published DSP zones from a preparation worker while rendering. Large resets/destruction and compile-time changes belong off-thread or require a demonstrated bounded alternative.

## 6. Sound identity and compatibility

Release/content identity records exactly what was built. Sound compatibility records what a saved project may rely on. An API-compatible change can still change sound: default level, envelope curve, noise seed/law, oscillator table, ratio mapping, feedback, resampler or fast-math mode are examples.

Use a new sound version or module identity for intentional incompatibility. Consumer projects retain the old identity; new projects can select a new default. A manifest resolves missing versions explicitly. Presets store stable control IDs and values with the module/sound version, not whichever knob happened to occupy column three.

No automatic migration may substitute a vaguely similar new machine for an old one. An optional migration preview must be distinguishable from exact legacy playback.

## 7. Reproducible derived artifacts

Record top-level source and all imported library/table hashes or immutable revisions, Faust compiler identity, generator version, options, precision, fast-math/denormal policy, native compiler/target flags and actual generated-content digest. A hash comment embedded in generated C++ is not proof that its computation matches source.

Verification regenerates/compares canonical output with documented treatment of nondeterministic metadata. Mutation tests change generated computation, imported library, table, flags and mapping independently and require failure. Never fix the verifier by merely updating a marker.

Factory/bitcode caches are keyed by all relevant source, mapping, backend and target identities. One module cache hit does not imply a new fused graph topology needs no compilation. Fused and modular renderers must preserve the declared control/state behaviour.

## 8. Consumer acceptance

Use one musical score to compare the canonical test host and each adapter. Distinguish exact deterministic equivalence from justified numerical tolerance. AOT mobile, LLVM desktop, interpreter and future embedded builds are individually qualified; one successful build cannot certify the others.

The first deliverable needs a real module plus one test adapter, not a general framework. Broad distribution, license selection, firmware hardware and mobile executable updates remain separately reviewed decisions.
