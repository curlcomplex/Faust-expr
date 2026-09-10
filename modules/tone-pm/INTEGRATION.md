# Tone versioned consumer contracts

The current playable candidate is **tone-pm / 0.2.0-experiment**, entry `playable/tone.dsp`. Read [its complete integration contract](playable/INTEGRATION.md), [manifest](playable/manifest.json) and [qualification](playable/QUALIFICATION.md). Tracker handoff remains curlcomplex/curlop-tracker#44. Pin the final qualified PR #44 commit rather than a floating branch.

## Retained 0.1 baseline

The parent `tone.dsp`, `manifest.json` and `patches.json` remain 0.1 unchanged. This older version latches all controls, ignores note-off and has no held articulation. Its Punch path is nonlinear even at zero Punch through tanh(sine)/tanh(1). Do not describe it as a clean bypass.

0.2 deliberately changes those behaviors as well as Mod Envelope and DC placement. API-name overlap does not make the sounds interchangeable. Preserve project sound identities; do not silently update existing users to new equations.

Canonical source/mappings stay in Faust-expr; Tracker owns registration, private build/device tests and merge. No consumer code or merge is included in this upstream contract change.
