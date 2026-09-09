# Metal delivery qualification and corrected evidence

Issue #17 / PR #41; portable identity `metal-pm / 0.2.0-experiment`.
This closes the missing-file/unverified-envelope delivery gap. It does not approve
an iPhone release, replace Tracker code or establish an exact Elektron match.

## Accepted processing path

The playing entry point uses direct exponentials. The multiplication recurrence
rounded a coefficient near one and accumulated approximately 2.1% relative error
through the long 44.1 kHz decay. That attempt is rejected; its 1.5% threshold was
not relaxed. The proposed decrement replacement was never established as an
accepted implementation and is NOT needed to use this module.

At the inspected `02bce27` baseline, the corrected suite passed 217 actual renders
and 337 checks. A separate float64 closed-form comparison of its generated
short/medium/long envelopes at three sample rates found worst relative error
below 9e-7 where the envelope exceeds -60 dB. Independent recompilation of the
unchanged generated C++ replayed all 217 recordings, with worst absolute sample
difference below 5e-7. These are baseline results, not a new Faust compile or
named-device evidence.

The final `metal_acceptance.py` gate reruns the complete suite, adds the separate
closed-form test and compiles/renders the old recurrence as a negative control.
The old error must still be detected; the product's direct envelope must pass.
It also decodes every listening WAV completely. The resulting `report.json` is
the authority for exact render/check counts and current-commit results.

## Architecture comparison: no exaggerated winner

The corrected time/spectral descriptor weights windows by their real energy;
it does not normalize tiny tails into a falsely bright sound. In the corrected
baseline, dense mean development distance was 0.84812 versus sparse 0.85991;
the separately reserved references slightly favored sparse (0.36492 versus
0.36585). These are small, mixed differences, not a decisive dense victory.

Keep `metal.dsp` as the broader three-carrier audition candidate and `sparse.dsp`
as an explicit alternate. Do not remove the alternate based on these numbers.
No new ratios/macros were fitted to make this corrected comparison pass.

Eight fixed licensed CY Alloy MP3 previews support broad coverage only. They
have unknown hardware controls, firmware and gain. The reserved clips had
already been inspected under an earlier biased metric; correcting the metric
does not turn them into untouched validation. The recordings are not Model:Cycles
Metal captures or proof of proprietary topology. Full attribution travels with
the audio evidence; source code contains no samples or extracted tables.

## Performance and remaining quality gates

The historical `fast` build is now an accurate scalar build identical in its
amplitude behavior to `reference`. Ratios between those two measure compiler/
timing variation, not a sonic optimization. Scalar/vector costs vary with
block size and machine; profile the actual consumer. Preserve p50/p95/p99/max,
object size and stack files. Ordinary new/new[] instrumentation is narrower
than an entire callback/all-allocator safety proof.

Feedback uses one sample of delay, so its response is host-rate dependent.
The baseline full dense 48/96 kHz diagnostic differed by approximately -19.92 dB
relative RMS, and the extreme setting by -6.48 dB. Removing feedback improved
the moderate comparison significantly. These comparisons mix phase, filter,
feedback and aliasing differences; they are not measurements of isolated alias
power. Upper-register/strong-drive musical quality remains an audition gate.
Do not claim sample-rate sonic identity, alias-free behavior or a hardware clone.

Explicit choke fades over 8 ms to exact zero; release does not revive the tail,
and a fresh trigger reopens it. Ordinary phase-reset retriggers can interrupt
ringing audio. Peak/DC/finite tests do not promise universally click-free sound.
No numerical issue is hidden by per-hit normalization or a safety limiter.

## Delivery and consumer boundary

Provide three directly playable core files: 9-second pattern, 21-second anchors,
and 25-second controls, plus optional sparse/reference comparisons. A compact
replay package carries full source, generated headers, all scores/hashes,
licensed reference assets and reports; bulk float renders can be regenerated
instead of making the only download hundreds of megabytes. Preserve the full
CI artifact where wanted. A verified package must reproduce the musical WAVs
with the same compiler and rerun the acceptance suite without network access.

Tracker's own issue owns source pinning, pitch conversion, seven non-pitch
control reachability, choke-group routing, sound-version compatibility and
AOT/interpreter/device verification. Do not rebuild the synth independently in
the private host. Nothing here authorizes a merge or replaces legacy METAL.
