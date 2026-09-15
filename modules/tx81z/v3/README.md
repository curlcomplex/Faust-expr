# v3 — eight-algorithm routing checkpoint

This slice adds the eight Yamaha four-operator connection algorithms while retaining the v2 waveform source. The topology is taken from pinned `ymfm`'s `s_algorithm_ops` table. This is topology evidence, not full YM2414 numerical equivalence: exact operator bus scaling, clipping, feedback, envelopes, frequency/level laws and chip timing remain open.

## Executed result

Final documented head before this metadata-only pointer update: `bbf81f0c484b895d2b36d06d99f5e8a8f7ea727b`, hosted run `35015047719`. Four topology-contract tests passed; Faust 2.88 compiled eight bounded algorithm graphs and produced nine actual renders (algorithms 0–7 plus an exact cold repeat of algorithm 0). All eight primary render hashes are distinct. Fixed-gain listening file: `01-algorithms-0-to-7.wav`. The same head also passed the existing full repository compile/audio workflow, v2 distribution compilation and v2 waveform/native-ymfm qualification.

An earlier runtime-selector implementation is intentionally retained in history. Its first render exposed reversed `select2` branch polarity; after that was fixed, Faust expansion of all eight complete graphs behind one runtime selector exceeded the workflow's 120-second compiler alarm. The accepted checkpoint therefore uses eight explicit `alg0`…`alg7` functions and compile-time selection for qualification rather than hiding that architecture/performance result. Runtime algorithm switching remains a host/graph design decision for a later slice; the eight synthesis topologies themselves are now explicit and independently compilable.

This does not alter or invalidate the qualified v2 waveform evidence. No Python synthesis substitutes for the recordings.
