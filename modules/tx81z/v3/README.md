# v3 — eight-algorithm routing checkpoint

This slice adds the eight Yamaha four-operator connection algorithms while retaining the v2 waveform source. The topology is taken from pinned `ymfm`'s `s_algorithm_ops` table. This is topology evidence, not full YM2414 numerical equivalence: exact operator bus scaling, clipping, feedback, envelopes, frequency/level laws and chip timing remain open.

Qualification compiles actual Faust and renders every algorithm with a state that makes topology differences audible. Auditions include the eight-algorithm sequence; no Python-synthesized substitute audio. Exact executed heads, run IDs and artifacts are recorded on issue #97 / PR #127.

A runtime-selector experiment is retained in history: after correcting selector polarity, expanding all eight full graphs behind one live selector exceeded the bounded compiler alarm. v3 therefore exposes explicit `alg0`…`alg7` functions and qualifies them independently. Runtime switching remains a later host/graph design decision.
