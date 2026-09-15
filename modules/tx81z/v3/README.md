# v3 — eight-algorithm routing checkpoint

This slice adds the eight Yamaha four-operator connection algorithms while retaining the v2 waveform source. The topology is taken from pinned `ymfm`'s `s_algorithm_ops` table. This is topology evidence, not full YM2414 numerical equivalence: exact operator bus scaling, clipping, feedback, envelopes, frequency/level laws and chip timing remain open.

Qualification compiles actual Faust and renders every algorithm with a state that makes topology differences audible. Auditions include the eight-algorithm sequence; no Python-synthesized substitute audio. Exact executed heads, run IDs and artifacts are recorded on issue #97 / PR #127 rather than edited into this file after every run.

An earlier runtime-selector implementation is intentionally retained in history. Its first render exposed reversed `select2` branch polarity; after that was fixed, Faust expansion of all eight complete graphs behind one runtime selector exceeded the workflow's 120-second compiler alarm. The accepted checkpoint therefore uses eight explicit `alg0`…`alg7` functions and compile-time selection for qualification rather than hiding that architecture/performance result. Runtime algorithm switching remains a host/graph design decision for a later slice; the eight synthesis topologies themselves are explicit and independently compilable.

This does not alter or invalidate the qualified v2 waveform evidence.
