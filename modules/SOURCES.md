# Source register and limits

Checked for this planning baseline on 8 September 2026. Links identify primary documentation or source inspected; they are not proof of an audio render or host acceptance. Firmware-specific details must be rechecked when a reference is captured. This file does not mirror copyrighted manuals, recordings, firmware or waveform tables.

## Instrument and DSP sources

**S1 — Model:Cycles designer account**
https://www.elektronauts.com/t/model-cycles-q-a-with-ess/122712/122

Primary author account: PM rather than conventional frequency modulation; prototyping and musical macro design; shared-wavetable Chord construction. Exact hidden curves/algorithms are not provided. Do not promote these Cycles disclosures to assertions about every Syntakt machine.

**S2 — Noise Engineering Basimilus documentation**
https://manuals.noiseengineering.us/bia/

Manufacturer documentation describes partial/noise synthesis, mode distinctions, folding/dynamics and internal clocking. The page is for Alia and discusses Alter; variant-specific reference recordings remain necessary. Exact PM routing and variant equivalence must not be guessed from related names.

**S3 — Faust virtual-analog effects**
https://faustlibraries.grame.fr/libs/vaeffects/

**S4 — Faust wave-digital models**
https://faustlibraries.grame.fr/libs/wdmodels/

These establish available building blocks, not turnkey emulations of a particular Elektron circuit. Pin the actual transitive library revision used by a build.

**S5 — Syntakt Science Lab, discovery lead**
https://www.elektronauts.com/t/syntakt-science-lab/235364

A lead for machine-specific recordings and project settings retained from the research discussion. Verify the chosen submission, routing, permission and firmware. A demo is not automatically dry or redistributable. Previously suggested videos/timestamps are leads until checked against the actual media; none was downloaded or measured in the planning pass.

**Manufacturer manuals and firmware**
Obtain the relevant Model:Cycles/Syntakt manual from Elektron's current support/download source when the reference set is selected. Record the exact file hash, revision and cited control pages in its manifest. The broader catalogue in #29 is a retained research map, not a current firmware-certified inventory. No precision claim rests solely on a third-party manual summary.

## Repository source evidence

**R1 — Bootstrap**
Commit b1e046b16ac9d40684ba85b805ea8d0ee71e84c1. README, AGENTS.md, compiler smoke workflow and render/analyzer sources. Main contains build infrastructure, not the new shared machines.

**R2 — Reference tools**
PR #5 / commit 090071491e99feb3aa63514600c3f659fc43c92e. `scripts/reference_render.cpp` and `scripts/reference_compare.py`: reusable validation/provenance ideas; renderer is cymbal-specific and spectral bands begin at 80 Hz. #12 owns adapting this for bass/transient and input-effect work. Prior CI/result claims are not re-executed results of this documentation PR.

**R3 — Compiler and cache work**
PR #7; inspected research snapshot a0ab4b34ac0c5d01b62f2bb6ed5073f45b81ac35. `research/two-level-build-cache.md` distinguishes editable module source/cache from a derived fused graph. `research/self-hosted-mac.md` supplies a security-oriented local-execution plan. Reuse relevant concepts without merging the whole experiment or claiming target performance.

**R4 — Independent cymbal programme**
Issues/PRs #2, #4, #5, #6 and #8 remain their own research. Their existence is not approval to refit, simplify or replace an owner-selected cymbal sound. Do not conflate historical rejected calibration improvements with musical acceptance.

## GitHub execution and visibility sources

**GH1 — Current billing**
https://docs.github.com/en/billing/concepts/product-billing/github-actions

**GH2 — Self-hosted runner model**
https://docs.github.com/en/actions/concepts/runners/self-hosted-runners

**GH3 — Fee-announcement postponement**
https://github.blog/changelog/2025-12-16-coming-soon-simpler-pricing-and-a-better-experience-for-github-actions/

Read the update at the top, not the retained original announcement as though it were the current policy.

**GH4 — Private cross-repository checkout**
https://github.com/actions/checkout#checkout-multiple-repos-private

The default token is repository-scoped; arrange an explicitly permitted read credential for a different private checkout.

**GH5 — Runner security**
https://docs.github.com/en/actions/reference/security/secure-use

**GH6 — Visibility consequences**
https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/managing-repository-settings/setting-repository-visibility

**GH7 — Runner communication/routing**
https://docs.github.com/en/actions/reference/runners/self-hosted-runners

Live pricing/access claims can change. Recheck these sources during the actual migration; this planning baseline is not a promise of perpetual free service or an audit of the owner's billing account.
