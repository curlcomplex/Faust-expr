# Acid Voice + Bassline Seq — candidate 0.1.0

Work: Faust-expr #57, CURLOP #273. These are two separate modules, not an internal sequencer/voice manager. Consumer owns polyphony. Drums PR56 remains independent and its HTTP406 reference blocker remains open.

## Source and sound boundary

`acid-voice/v1/teebee.lib` is a Faust port of Open303's TB_303 mode and feedback highpass, from RobinSchmidt/Open303 commit `313bf0d9ade7c1dcb6b3a74f5ea1780a29d70074`. See the retained MIT notice. Its coefficient approximation is restricted to cutoff 200 Hz..SR/8 and raw resonance <=0.92. The C++ test transcription checks this component, not the complete upstream synth or physical hardware. Upstream sources inspected: rosic_TeeBeeFilter.h (Git blob 9dcec99753a18981e9a13e73ea99da99cc659114), rosic_OnePoleFilter.cpp (27fc813897f37d62af5cdd669eda30122f670121), rosic_Open303.cpp (536b81c687a06b9081f8fec229b31ac5817fb31e).

The voice is an original band-limited saw/square, decay/gate envelope, explicit accent, saturating drive and smoothed-control design around that component. It is not a full Open303 port or hardware-approved TB303. No private host code, note list, allocation or sample playback is used. Faust standard libraries retain their own notices.

## Note interface

One `freq` (20..4000 Hz), `gate`, `velocity`, `accent`, `slide`. UI labels, not variable names, are canonical. Custom accent/slide delivery still requires consumer integration. Velocity latches on a rising positive gate; negative gates stay off. Incoming slide slews one target in Hz with a sample-rate-aware exponential; ordinary notes and slideTime=0 jump exactly. The sequencer never smooths pitch. A held gate does not retrigger merely because pitch changes. Accent follows during a held/tied note and is retained through release; it changes filter/amplitude/decay but does not retrigger the main envelope. Live cutoff (log domain), resonance, envelope amount, decay, accent amount, waveform, drive and level are smoothed. No voice sleeping, stealing, chord expansion or allocator.

## Sequence interface

32 stored steps, 1..32 active. Each has enable, MIDI-number pitch (fractional values allowed), accent, outgoing slide. Public raw laboratory outputs: Hz target, binary gate, accent, incoming slide, then read-only step telemetry. Labels/metadata are source-owned, but current CURLOP typed output conversion/GUI is not proved by this lab.

Rising positive clock advances. Clock high duty is non-tied gate duration. Outgoing slide ties only when the previewed next step is enabled; a rest breaks it. Current step fields/tie preview latch at the clock edge; edits affect the next relevant tick. Reset+clock plays step1; reset alone arms and silences; stop silences without reopening on resume before a new edge. Position holds on stop. Pitch holds through rests and defaults to 110 Hz before the first note. Host transport must terminate a hung/tied gate using run=0 or reset. No separate timer, tempo engine or per-edit compilation.

`patch.dsp` wires the actual signal-rate sequencer directly into one actual voice. `second-voice.dsp` demonstrates a separate simple diagnostic consumer. These are offline lab patches, not CURLOP recordings or standalone product modules.

## Qualification

`python3 tools/modules/acid_batch.py --out build/acid-batch` uses the existing Lab build helper and native renderer. Renderer capacity expands from two to eight bounded channels to inspect the five-lane sequencer and five-input voice, not to add polyphony. Failed checks, raw floats, scores/input buffers, generated/source hashes, compiler versions and common-gain auditions are retained. Source implementation, numerical qualification, musical approval, hardware comparison, host integration and release remain separate claims. No speedup or realtime/device claim is implied.
