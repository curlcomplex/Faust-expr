# v3 — eight-algorithm routing checkpoint

This slice adds the eight Yamaha four-operator connection algorithms while retaining the v2 waveform source. The topology is taken from pinned `ymfm`'s `s_algorithm_ops` table. This is topology evidence, not full YM2414 numerical equivalence: exact operator bus scaling, clipping, feedback, envelopes, frequency/level laws and chip timing remain open.

Qualification must compile actual Faust and render every algorithm with a state that makes topology differences audible. Algorithm 0 is required to preserve the v2 serial topology under matched controls. Auditions must include an eight-algorithm sequence plus musical examples; no Python-synthesized substitute audio.
