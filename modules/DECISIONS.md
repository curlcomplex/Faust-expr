# Shared-module decisions

Owner decision record, 8 September 2026. Programme authority: [#10](https://github.com/curlcomplex/Faust-expr/issues/10). The owner approved the direction and requested persistent specifications. Numerical constants, exact hidden reference algorithms, final macro curves and hardware budgets are NOT settled by that approval.

## D1 — One canonical library, several consumers

**Accepted:** module source, mappings, test scores and release identities belong in Faust-expr. CURLOP, Tracker and later products consume versioned definitions. A host adapter is permitted; an independently maintained copy of a machine is not.

**Rationale:** sound corrections and new instruments should propagate deliberately across products without divergent implementations. One source does not mean one target binary, UI or compiled graph. Architecture-specific factories, generated code and tables are derived.

**Rejected:** embedding canonical DSP in a Tracker page, copying private host implementations into this lab, or requiring all products to share one UI/runtime.

## D2 — New sound designs; preserve existing music

**Accepted:** the current Tracker machines and effects are rejected sonic designs for this programme. New work begins from the reference-led briefs, not from obligations to preserve their architecture. Existing saved projects still need explicit legacy/version treatment in consumer integrations.

**Rationale:** keeping a flawed sonic design merely to avoid new work conflicts with the owner's goal. Silently changing a saved song's sound is a different failure.

**Not authorized:** deleting legacy code now, changing old parameter meanings in place, redesigning Tracker's sequencing interaction, or closing old technical bugs as if new sounds fixed them.

## D3 — Reproduce behaviour before inventing extensions

**Accepted:** use reference audio, controls and dynamic behaviour to constrain an independent implementation. Preserve the reference's useful dimensions on the laboratory panel before compressing controls for a particular host.

**Rationale:** a waveform at one setting does not identify a unique system. Matching multiple settings and control interactions is more informative. A small macro set is authored behaviour, not arbitrary parameter hiding.

**Open:** exact PM routing, envelope shapes, hidden nonlinearities, smoothing laws, phase-reset policy and target firmware. Manufacturer disclosures are evidence only to the extent they actually disclose these things. Do not turn hypotheses into facts in an issue title, comment or release claim.

## D4 — First instruments are deliberately separate

**Accepted planning order:** PM kick with BD Modern as primary target, Cycles Kick as separate comparator; analog-style kick with BD Sharp as initial reference lead; six Cycles-style roles; complementary conventional percussion and melodic voices. Shared effects can develop independently. Basimilus-style partial percussion is its own engine.

**Rationale:** a universal engine with hidden exceptions is hard to identify, control and test. Share primitives after demonstrated commonality rather than assuming all electronic percussion is one algorithm.

**Open:** reference firmware, final names, exact release order after the two kicks, which later Syntakt variants justify distinct kernels.

## D5 — Fidelity and efficiency are both requirements

**Accepted:** preserve an accurate candidate and test optimizations against it. Profile early enough to reject an impractical approach, but do not silently weaken the sound to meet an invented budget.

**Rationale:** vectorization, tables, oversampling, precision and idle policies can change audible behaviour. A fast incorrect implementation is not useful, nor is an unqualified desktop prototype presented as a mobile product.

**Open:** lowest supported iPhone, target voice count and thermal scenario; any future MCU/codec; quantitative fidelity tolerances derived from reference variability. These must be recorded before target release, not guessed in the planning stage.

## D6 — Hearing and measuring are separate gates

**Accepted:** automated numerical/behavioural tests, controlled reference comparisons, Felix's listening approval and integrated target acceptance are distinct.

**Rejected:** a single 'realism score'; peak normalization that hides dynamic/level errors; synthetic analyzer fixtures presented as actual DSP; compilation presented as sonic success; Mac offline throughput presented as iPhone callback evidence.

## D7 — Authoring/export is a later consumer of the contract

**Accepted:** CURLOP may become the hands-on environment for source, macro mapping, preset audition and export. Versioned source/mapping packages must already be usable without that future environment.

**Rationale:** the first kick should not wait for a plugin marketplace, GUI framework or content-distribution system. A diagnostic panel can expose internals without changing the released compact surface.

**Open:** exact authoring UI, package transport, mobile content-update mechanism, signing and commercial release policy. No executable mobile download or JIT permission is assumed.

## D8 — Private source is compatible with local testing

**Accepted preference:** move toward private reusable DSP with trusted local execution. **Not performed or implicitly authorized here:** repository visibility change, credential creation, runner migration or local job dispatch.

Keep the currently working private execution controller unless a concrete reason justifies moving it. Authenticate any new private cross-repository checkout; audit hosted workflows and artifact storage separately. See [EXECUTION.md](EXECUTION.md).

## How decisions change

An implementation may challenge a proposed structure with evidence. Record the rejected hypothesis, comparison and replacement in the owning issue/PR, then update the affected contract deliberately. Do not ask the owner to reconfirm accepted product context on every session. Do ask for a genuine sound choice when alternatives differ musically. Do not mark a new engine accepted because an agent likes its own output.
