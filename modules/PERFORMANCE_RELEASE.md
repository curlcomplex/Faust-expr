# Performance, fidelity and release qualification

Owned by [#27](https://github.com/curlcomplex/Faust-expr/issues/27), with packaging in [#13](https://github.com/curlcomplex/Faust-expr/issues/13) and adapters in [#28](https://github.com/curlcomplex/Faust-expr/issues/28).

## Evidence ladder

1. A synthetic fixture verifies the analyzer or schema, not a synth.
2. Actual Faust generation and rendering verifies that particular implementation path.
3. Controlled reference comparison describes measured sonic/behavioural differences.
4. Felix's approval establishes the selected musical candidate.
5. Integrated target tests establish suitability for a named device/scenario.
6. An explicit promoted release establishes availability to a consumer.

These are separate claims. A compilation badge is not a musical verdict. A successful desktop benchmark does not qualify a phone or unspecified embedded CPU.

## Test matrix

Initial investigation covers 44.1 and 48 kHz, with 96 kHz useful for diagnosis. Test production float32 and a higher-precision reference where relevant; development LLVM/interpreter and production AOT are distinct paths. Include scalar/vector comparisons when enabled, without assuming vector code is faster for sample-exact event fragmentation.

Render the same score with 1, 32, 64, 127, 128, 256 and 512-frame segmentation as applicable. Test end-of-block and adjacent-sample events, rapid retriggers, tail automation, idle/resume, reset/choke and repeated use. Static parameter corners do not cover hostile time-varying trajectories.

Include realistic concurrent voices, Chord-internal oscillator counts, shared and inserted effects, and old/new renderer overlap during publication. Benchmark bypass and quiet paths for correctness as well as speed. Never freeze a delay or gate detector merely because it is not currently audible.

## Resource accounting

Record compile/init time separately from compute; first-paint constraints concern host integration, not only kernel speed. Measure per-instance memory, shared table memory, stack maxima, scratch buffers and tail residency. No callback allocation, locks, compile, I/O, ordinary logs, destruction of large objects or unbounded solve loops.

Select the minimum supported device and workload before setting a release budget. For B frames at sample rate Fs, the callback deadline is B/Fs seconds. Report p50/p95/p99/max callback time and deadline misses, along with power/thermal state and other workload. Reserve headroom for the rest of the product; a module occupying nearly the whole deadline is not a viable multi-track product merely because an isolated average is below it.

No universal percentage is specified in this document. The first real target baseline supplies the context for an explicit budget. A developer Mac is useful for fast regression and capacity tests; device measurements are still necessary. Hardware ports need their own MCU, memory, codec rate and latency constraints.

## Optimization method

Freeze the accepted accurate candidate and evidence. Change one mechanism at a time. Candidate changes include waveform tables, interpolation, selective oversampling, feedback formulation, precision, SIMD layout and idle policy. Keep the training/held-out split and musical score fixed while comparing. A faster implementation that changes intended character becomes an explicitly separate candidate, not a silent replacement.

Choose fidelity tolerances from the particular deterministic/stochastic behaviour and reference repeatability. Exact hashes are appropriate for identical deterministic builds; non-bit-identical floating-point paths may need bounded spectral/time differences. Chaotic or noisy paths need distributional checks. Do not relax tolerances after observing a failure simply to turn the check green.

Fast-math and denormal handling require finite-output and numerical-regression checks. Oversampling must include correct input/output resampling and latency accounting. Avoid one global oversampling factor for all modules before profiling.

## Release contents and storage

A release identifies source and mapping, manifest/sound version, transitive dependencies, generation/native compilers, flags/target, generated-output hashes, scores, references, raw-render hashes, test reports and listening decision. It need not place large audio files in the Git history.

Evidence referenced by hash must remain recoverable from backed-up storage; an expired Actions artifact alone is not a durable experiment record. Upload compact reports and selected audition clips intentionally. Never embed credentials or sensitive workstation configuration in evidence.

Promotion to a released library version is explicit and separate from automatic tests. Consumer integration PRs pin that release and verify canonical-versus-adapter output. Old sound identities remain available according to the consumer's compatibility policy. Signing, distribution and mobile executable/content policies are validated in the consumer release process, not promised by this portable DSP plan.
