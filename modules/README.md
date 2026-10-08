# Shared Modules

A reusable Faust instrument and effects library for CURLOP, Tracker and future products, including possible hardware instruments.

**Design baseline: 8 September 2026. This directory currently contains specifications and research briefs, not a completed module SDK or a released instrument set.** Work and delivery status live in [umbrella issue #10](https://github.com/curlcomplex/Faust-expr/issues/10) and its linked issues. Do not infer implementation from a document, an issue being open, or a compiler smoke-test pass.

## Start here after a handoff

Read this page, [the accepted decisions](DECISIONS.md), and the owning issue. Before implementing DSP, read [the module contract](CONTRACT.md) and [reference protocol](REFERENCE_PROTOCOL.md). [Machine briefs](MACHINE_BRIEFS.md) explain the instrument families. [Performance and release](PERFORMANCE_RELEASE.md) separates audio quality from target qualification. [Execution and privacy](EXECUTION.md) describes local Actions and repository migration. [Sources](SOURCES.md) records what is established and what is only a lead.

The next instrument is [PM kick #14](https://github.com/curlcomplex/Faust-expr/issues/14). Its first deliverable is a small reference-controlled audible comparison, enabled by useful slices of #11–#13. Do not require completion of a universal test platform, every reference capture, all host adapters or a new authoring application before producing that comparison.

## Product context that must survive a model change

Felix actively uses Tracker, and its features were added in response to his own use and tester requests. Testers have made music quickly and responded positively. Preserve that interaction model; visual identity and interaction polish are separate work. The new decision is to replace the current machines and effects as sonic designs, not to repair them until they happen to sound acceptable. Preserve old project playback through explicit sound versions rather than destructive replacement.

The sonic brief is a small set of authored instruments with reduced controls and wide, useful tonal range, using Model:Cycles and Syntakt as initial behavioural references. Match identifiable sounds and control responses before expanding. Basimilus-style percussion is a separate engine. Existing cymbal and compiler research remains independently scoped.

## Ownership

Faust-expr owns the portable kernels, authored mappings, definitions, tests,
reference manifests and release identities maintained as laboratory modules
here. Consumers own their target UI, sequencing, project storage, audio
devices, application packaging and electrical interfaces.

A product may adopt a laboratory module into its designated canonical source
repository. The handoff must identify the exact laboratory revision and retain
lineage, control/contract history, licences and applicable qualification
evidence. After that handoff, ongoing product DSP, mappings, module panels,
assets, focused tests and release identities belong to that source home.
Laboratory snapshots remain research and export provenance; they are not a
second editable product source. There is no automatic reverse synchronization.
Other laboratory experiments and shared research stay here unless separately
adopted. A private runner-control repository executes checks without becoming
an additional source authority.

A laboratory export does not establish downstream sound acceptance, custom-GUI
readiness, installed playback, saved-project recall or release selection. Early
prototypes can use basic controls; product GUI and shared-widget work follows
sound qualification under the receiving product's readiness process. Preserve
existing panels and contracts during a relocation; adoption alone is not a
redesign or a new qualification.

For new laboratory work, place a concrete module under
`modules/<neutral-module-id>/` with its source, mapping/manifest, tests and
compact evidence index. Extract shared code only when multiple real modules
justify it. Large recordings and build outputs do not belong in ordinary
source commits. Public CI does not fetch private product sources or credentials.
See [D1 and D7](DECISIONS.md) and [the adoption clarification issue](https://github.com/curlcomplex/Faust-expr/issues/133).

## Work map

This table is an index, not a second live status board. Read each issue for dependencies, acceptance and current state.

| Issues | Responsibility |
| --- | --- |
| [#11](https://github.com/curlcomplex/Faust-expr/issues/11) | Reference provenance, controlled captures and hold-outs |
| [#12](https://github.com/curlcomplex/Faust-expr/issues/12) | Actual-Faust renderer and instrument-appropriate analysis |
| [#13](https://github.com/curlcomplex/Faust-expr/issues/13) | Stable contract, sound versions and reproducible packaging |
| [#14](https://github.com/curlcomplex/Faust-expr/issues/14), [#15](https://github.com/curlcomplex/Faust-expr/issues/15) | PM kick first; distinct analog-style kick second |
| [#16](https://github.com/curlcomplex/Faust-expr/issues/16)–[#20](https://github.com/curlcomplex/Faust-expr/issues/20) | Synthetic snare, metal, perc, tone and chord |
| [#21](https://github.com/curlcomplex/Faust-expr/issues/21), [#22](https://github.com/curlcomplex/Faust-expr/issues/22) | Complementary drums, dual oscillator and ensemble |
| [#23](https://github.com/curlcomplex/Faust-expr/issues/23) | Separate partial-percussion/Basimilus-style engine |
| [#24](https://github.com/curlcomplex/Faust-expr/issues/24)–[#26](https://github.com/curlcomplex/Faust-expr/issues/26) | Stereo delay, reverb, drive/filter |
| [#27](https://github.com/curlcomplex/Faust-expr/issues/27) | Sonic regression and target realtime qualification |
| [#28](https://github.com/curlcomplex/Faust-expr/issues/28), [#29](https://github.com/curlcomplex/Faust-expr/issues/29) | Authoring/adapters and broader future catalogue |
| [#30](https://github.com/curlcomplex/Faust-expr/issues/30) | Private-repository/local-Actions transition |

## Instruction for the next engineering session

Inspect current issue/PR state and source before changing anything. Resume the owning experiment branch when appropriate; otherwise branch from committed main. Keep one bounded experiment PR per instrument, with source/score/reference identities and explicit negative results. Do not force-push, merge, change visibility or deploy without owner permission.

A useful handoff comment states the exact commit, evidence location, experiment changed, observed result, remaining discrepancy and next falsifiable test. It does not need a running activity diary. Never report a lower diagnostic loss as musical acceptance; Felix's listening decision is separate. When references are incomplete, build a clearly labelled candidate without claiming a match.
