# TR-909 hardware reference / tuning pass

Tracks #80. Baseline is `68-909-drums` / PR #69. Draft laboratory work only; no installed CURLOP module or release approval.

## Invariants

- Preserve all existing 909 source/sound versions.
- One note per Faust kernel; canonical `gate`, `freq` (Hz), `velocity`; host owns polyphony and cross-instance choke.
- Consolidated Tom direction remains one engine with Low/Mid/High classic presets when equations are shared.
- Preserve broad/extended controls; reference matching defines classic anchors rather than restricting the module to stock hardware.
- No GUI expansion in this pass.

## Reference hierarchy

1. Direct/dry original TR-909 recordings with identified provenance/settings where available. Hash and fetch transiently unless redistribution permission is explicit.
2. Pinned open-source circuit/behavior models as secondary evidence, explicitly separated from hardware: Mutable Instruments Plaits synthetic bass/snare and `andremichelle/tr-909` / DinSync RE-909 research already documented in #68.
3. Processed commercial packs, plugin demos, clones and lossy previews are not silently accepted as hardware anchors.

## Current synthesis scope

Analyze and tune the synthesized Kick, Snare, consolidated Tom (Low/Mid/High presets), Rim and Clap before editing. Prefer preset correction before a versioned DSP rewrite. Keep rejected revisions and adverse per-descriptor changes visible. Diagnostic distances are not authenticity percentages.

CH/OH/Crash/Ride remain sample-resource/provenance-gated. The existing diagnostic one-shot player is not an instrument and generic synthesized metal is not a substitute.

## Qualification

Use the existing Faust-expr renderer/workflows: exact source/control identities; silence/velocity/retrigger; onset/live control boundaries; 44.1/48/96k; scalar/vector/block cases; extremes/tails; actual Trigger Seq groove; negative controls. Candidate-only previous/revised auditions. Hardware recordings are not repackaged without explicit rights. Listening, multi-unit variance, CURLOP faceplate/wiring/save-reopen, device callback/thermal and release acceptance remain separate gates.
