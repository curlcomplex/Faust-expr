# Tracker and CURLOP handoff

Identity **clap / 0.2.0-experiment**. Candidate `modules/clap/v2/clap.dsp`; direct-sine oracle `reference.dsp`. Current source/run/artifact pin lives in PR50 / issue49 / Tracker87. This continues selected #49, not superseded #51/#52 or Tracker88. V1 remains separate sound history.

| Lane | Stable ID | Default |
|---|---|---:|
|P1|spacing|0.44|
|P2|punch|0.42|
|P3|decay|0.50|
|P4|balance|0.42|
|P5|body|0.38|
|P6|body_env|0.46|
|P7|drive|0.10|

All seven range 0–1. Six visual columns may pair Body/Body Envelope in column5. Stable lane IDs do not depend on visual grouping. P8 is absent. Separate inputs: pitch_hz 70–900 Hz/default210, gate binary, velocity 0–1/default1. Pitch tunes the body; noise-only patches need not have a measurable fundamental.

One host note creates one independently gated MONO instance. The host owns chords/polyphony. Write every onset value before computing the rising gate; compute a genuine low sample before another trigger. All musical controls, pitch and velocity latch at onset. Note-off does not choke. A held gate is one trigger, not repeated hits.

PRNG/fixed-filter histories persist. Fresh synchronized instances can produce correlated noise; this version has no random per-voice seed API. Host idle freezing intentionally differs from uninterrupted noise. Qualify audible resume, stale gates, tail retirement and voice stealing without promising byte identity across a frozen interval. Do not allocate/reset the complete processor on every hit.

Lookup shares 4096 floats (16,384 bytes) per generated class. Initialize class storage OFF the audio thread once before rendering, then initialize instances. Convenience init can rewrite shared storage and must not race existing voices. Multiple generated classes or translation units can duplicate the table. Object size is separate from shared storage, code, stack and host buffers.

Qualified flags: `-lang cpp -single -cn ModuleDSP`; vector additionally `-vec -lv 0 -vs 32`. Native comparisons use `-std=c++17 -O2 -ffp-contract=off`, no fast-math. Preserve compiler/imported-library/expanded-source identity and verify generated content, not just an embedded source-hash comment. Vector won on the first hosted CPU but lost on the independent CPU; retain both and measure the actual target. Scalar lookup is a conservative starting point, not an unconditional winner.

Verify actual 0-input/1-output ABI and control enumeration. Diagnostic envelopes/phase probes have different outputs and pruned controls; never select them as an instrument. No phantom reserved zones are required.

Before product release: canonical/adapted/AOT/interpreter comparisons; all seven lanes/paired columns; same-sample locks; velocity; long tails; save/load and explicit sound-version migration; host polyphony/idle/switch/retrigger behavior; named-device preparation/memory/callback/thermal tests. Registry IDs and owner-authorized old-project policy belong to Tracker. This handoff changes no app source, credentials, visibility or merge settings.
