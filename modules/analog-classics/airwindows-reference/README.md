# Airwindows reference pass — Vintage Tape and Retro Ensemble

Owner: Faust-expr #63 / original candidate #64; reference PR #72. Consumer: CURLOP #283 under #271. No new modules/controls/GUI, host work, merge or release. V1 sources remain unchanged. Versioned v2 entries are reference candidates, not listener-approved sounds.

## Reference

`airwindows/airwindows@03c9931839881bae6dfd4e36bfd3cced79f54b4a`, LinuxVST IronOxideClassic2 and Ensemble. Download exact header, constructor and processing files. Archive bytes/SHA256. The test wrapper extracts the complete original double processing method, state declarations and reset statements. No equation-by-equation replacement oracle. VST program/UI glue is not DSP and is omitted.

The original double method has output dither commented out. Primary seeded-zero runs exclude its near-zero-input noise injection; separately seeded native runs quantify the omission. Wrapper float inputs/outputs are converted to original double processing. Tape controls map physical dB/ips back into original normalized float A/B/C. Ensemble maps integer head count to original A, with fullness/brightness/mix unchanged. Same inputs, scores, sample rates and state resets; no alignment, EQ, gain fitting or normalization.

## Tape

Restores alternating lean/fast states, original clipping, and high-rate biquads/reconstruction. At this exact revision, gcount starts at zero and never advances: weighted history addresses remain unwritten, so their contribution and slow memories stay zero. The candidate eliminates that dead path but does not silently fix the upstream counter. Source assertion plus native whole-method renders validate this reduction for the inspected reset contract. Do not generalize it to different revisions or an idealized physical tape machine.

## Ensemble

Restores initial pi/2 and per-head phase offsets, frozen inactive-head phases, original three-point interpolation/correction and ordered alternating Air state. One stereo buffer is shared by all read heads in generated code. Maximum 48 lanes still execute; input Voices changes the active output/state update, not compiled topology. No claim of runtime work scaling with selected Voices.

The first v2 comparison exposed a precision problem: Faust algebra canceled the nominal Kahan correction `(total-p)-y` to zero. Non-binding arithmetic guards preserve the intended rounding steps. A 20-second drift test and actual uncompensated-source mutation prevent this shortcut returning. Double and single precision results are reported separately. Float phase/coefficient/delay rounding and the original interpolation correction's boundary discontinuities mean ordinary single precision is NOT a bit-identical port. Full error statistics, including peaks, remain visible.

Controls match upstream's block/event changes without v1's extra smoothing. Thus Fullness/head-count changes can produce the original pitch/timing jumps. Any production smoothing/crossfading must be an explicitly tested mode/adapter decision, not silently included in the equality oracle.

## Criteria and evidence

First comparison only required improvement over v1; success was not absolute fidelity approval. Follow-up adds explicit relative-RMS numerical criteria: Tape 2e-5 (single/double), Ensemble .005 (single) and 1e-5 (double) for static mixed stimuli and longer/extra specified cases. These are numerical engineering tolerances, not a percentage of authenticity or listener sign-off. Automation/impulse peaks are reported separately. No threshold is inferred from CI wall time.

Run `python3 tools/modules/airwindows_reference_followup.py --out build/airwindows-reference`. Reuse existing renderer and pair regressions. Keep raw/score/source/generated hashes, failing cases, unmodified original sources, all preset comparisons, 20-second drift negative control, and original/previous/revised auditions. CPU measurements use five identical 5-second stereo inputs at 48k/128 on one named host; timer includes the native reference wrapper's conversion/mapping overhead. Not CURLOP callback, thermal or device qualification.

The standard job uploads a verified small review artifact (sources, scores, reports, code and auditions) and a full raw artifact. A historical evidence-only repack resolved the connector's 512MiB download ceiling without rerunning DSP or deleting original evidence; its one-off workflow is removed from the final diff. Actions retention is not durable release archival.

Still pending: human listening, independent plugin-host parity, broader long-duration/input/rate stress and sample-rate-change lifecycle, original float/dither-path parity, CURLOP live control policy, actual authored package/faceplate/save-reopen and device acceptance. No claims beyond the tested rates/settings.
