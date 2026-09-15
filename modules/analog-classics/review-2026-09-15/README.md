# Analog Classics internal review export

Generate the review-set manifest and self-contained scripts with:

```sh
python3 tools/modules/analog_classics_review_export.py --out build/analog-classics-review
```

This is an internal-review identity, not a release, factory promotion, or CURLOP runtime acceptance. `manifest.json` is generated output: it pins the source commit, source hash, export hash, declared version, dependency/licence provenance and decision rationale for every selected entry. The generator never selects by version ordering.

`faust -e` expands repository and standard Faust libraries. Metadata is normalized only after the source expression lines are proven unchanged, then every script is compiled as standalone Faust. Existing synth script qualification supplies exact canonical/exported rendered-audio parity for the frozen Juno-60, Juno-106, SH-101 and Mini set. All other entries are unchanged source expressions with compiler-expanded libraries; their required source-specific qualification remains visible in their owning records.

## Contract audit

Selected one-note instruments expose lowercase `gate`, `freq` in Hz and `velocity`; the selected closed/open hat facades are the explicit v2-derived adapters that supply Hz instead of the paired kernel's `pitch_ratio`. `accent`, `slide`, `choke`, `clock`, `reset` and `run` retain separate meanings. Trigger Seq's first output is CV gate and its step meter is an observation, not audio. Effects and the sequencer retain their native I/O and are not presented as instruments.

Stable identity is the manifest identity plus selected source digest/version, never UI display text or geometry. A later sonic edit requires a new source version and review freeze. Nothing here contains CURLOP host/UI/project code.
