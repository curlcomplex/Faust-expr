# Clap v2 qualification and decisions

Owning issue49 / draft PR50 / Tracker87. First complete v2 run **34629441773** at `ec80e3e07f360752c94200bbe4a26424c4fc479b` passed. Exact final source/run pin is updated in those threads. This document records measured results, not a guarantee of identical benchmark numbers on later runs.

## Executed evidence

**160 actual renders / 351 checks / 15 separate arithmetic-schema tests.** Parameter-corner combinations in one trajectory are not counted as independent renders. Actual builds include direct sine, interpolated lookup, vectorized lookup, three controlled ablations, cluster/tail and body-envelope outputs, independent phase probe, and preserved v1.

The original musical scores remain. Added checks cover 20-second envelope oracles at 44.1/48/96 kHz, operator phase increments across carrier wraps and a rejected old-law mutation, eight prepared-versus-onset patches, zero velocity, late retrigger after 25 seconds, malformed inputs, direct/lookup comparisons across eight anchors and three rates, and properly filtered sample-rate diagnostics. Existing block1/32/64/127/128/256/512, latching, gates, velocity and persistent-state tests also pass.

Maximum direct/lookup sample difference **3.1721e-6**, fixed limit3e-5. Maximum body/tail relative envelope error **3.9037e-6** above1e-4 amplitude, limit2e-5. Cluster absolute error **1.0261e-5**, limit4e-5. Independent phase-step errors below5.01e-8, limit2e-6. The old wrapped-phase law is deliberately rejected. These are numerical implementation checks, not audible-fidelity percentages.

Nondiagnostic peak **0.603737**; maximum adjacent-sample change **0.800020**. Large noisy/extreme changes are not certified click-free merely because output is finite. No limiter, EQ or normalization was added to satisfy these tests.

## Decisions and contrary evidence

Retain the selected seven-control clap. V2 corrects independent operator phases, adds a clean Body0 endpoint and finite attacks for all sub-bursts. V1 remains versioned history. Diagnostic wrappers remove only their intended component; single-burst preserves crunch/filter routing. Unused controls are pruned, not artificially retained with sub-audible dependencies.

Across twelve licensed CPV previews and the same eight authored anchors, fixed-scale energy-weighted mean nearest distances: full **0.28224**, single-burst **0.28657**, no-tail **0.29753**, no-body **0.63001**. The body widens coverage of this small set. This is NOT knob fitting, a hardware-clone score or untouched validation. Several recordings choose the same FM anchor; individual residuals remain. Context auditions use documented whole-hit gain only.

The whole-hit descriptor underweights rapid sub-attacks. A separate first100ms comparison finds roughly10.7–26.8% relative RMS change for noise-led Classic/Tight/Wide/Crunch/Diffuse when secondary bursts are removed, versus3.4–6.6% for body-heavy/Driven anchors. Do not use either metric alone to declare burst redundancy or superiority.

## Same-sound optimization

The4096-point interpolated-sine implementation changes only oscillator evaluation. Noise, envelopes, nested PM and drive are unchanged. Three warmed repetitions rotate direct/lookup/vector order at32/64/128/512frames with four voices. First hosted128-frame medians:132.10us direct,122.72us lookup,115.29us vector. Lookup saves about7% in that workload; combined vector+lookup about13%.

The independent CPU gave only3.1–5.4% lookup speedup over direct and made vector4–5% slower than scalar lookup. Preserve these contrary results: there is no universal vector winner. Device-specific measurement decides the production recipe.

Lookup object392bytes excludes shared table, stack, vector scratch, code and buffers. Shared table4096floats/16,384bytes per generated class. No ordinary new/new[] calls were observed inside guarded compute; malloc/aligned allocators and the complete host are not certified by that hook. Initialize tables before concurrent voices render.

## Independent replay and remaining limits

The downloaded first artifact passed ZIP/hash checks. Recompiling its unchanged generated C++ with GCC14.2 versus hostedGCC13.3 and executing the complete suite passed all160 renders/351 checks. Thirty raw files were byte-identical; maximum cross-compiler sample difference **9.8347664e-7**. This is generated-code replay, not another Faust compiler invocation or an iPhone test.

Properly filtered48/96kHz comparison with Balance1 removes stochastic noise. Clean-body relative RMS difference is about-63.4dB; pitch700Hz/maximumBody/Drive is only-12.2dB. Strong upper-register PM/Drive is NOT alias-free or rate-equivalent. Residuals combine phase/interpolation/filter and nonlinear sampling differences, not isolated alias energy. Do not silently clamp controls or weaken tests to hide this.

The compact package must be executed from its own source directory, not merely zipped after CI. Completion requires process exit0, final full report, all source/generated/score hashes, decodable audio and current Tracker87 handoff. Raw bulk files can be regenerated by its offline replay. An intermediate inherited report alone is not completion.

Remaining product acceptance: Felix's musical selection; high-register/drive and retrigger/voice-steal quality; canonical/private-adapter/AOT/interpreter parity; host idle policy; sound-version migration; named-device memory/preparation/callback/thermal evidence. These do not justify another prerequisite framework rewrite before audition. No previous machine or full Fourier Morph is removed or changed.
