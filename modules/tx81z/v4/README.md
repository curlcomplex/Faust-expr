# TX81Z/OPZ v4 — eight selectable routes in one voice

Owning issue #97; persistent experiment PR #127. `v1`, qualified `v2`, and the
intervening explicit-graph `v3` sources remain unchanged. This is a routing
checkpoint, not a completed TX81Z recreation or listening/device acceptance.

## Playable change

Load `v4/voice.dsp`. `algorithm` is a **1–8** control; the source maps this to
ymfm's zero-based 0–7 table. Four running operators are connected differently
rather than evaluating eight entire voices and selecting afterward. The
qualification verifies four sine evaluations in generated scalar C++, actual
runtime changes, and exact routes. Compilation timings describe this build
only, not realtime CPU cost, host patching latency or target-device performance.

The public operator convention stays compatible with **v2**: `OP1` is the last
carrier in the serial chain, `OP4` is the first modulator. The internal evaluation
order in ymfm and the archived v3 equations is reversed. Test adapters reverse
arguments explicitly; this is not a complete Yamaha SysEx translator.

| Algorithm | Carrier-first module topology | Audible outputs |
| --- | --- | --- |
| 1 | 4 → 3 → 2 → 1 | 1 |
| 2 | (4 + 3) → 2 → 1 | 1 |
| 3 | (4 + (3 → 2)) → 1 | 1 |
| 4 | ((4 → 3) + 2) → 1 | 1 |
| 5 | (4 → 3) + (2 → 1) | 3 + 1 |
| 6 | 4 → (3, 2, 1) | 3 + 2 + 1 |
| 7 | (4 → 3) + 2 + 1 | 3 + 2 + 1 |
| 8 | 4 + 3 + 2 + 1 | 4 + 3 + 2 + 1 |

The new OP1 ratio/level default to 1. Algorithm 1, matching controls and the
same sample-exact score must preserve v2 serial output. v2 waveform code is
imported, not copied/revoiced. No carrier-count normalization is used: adding
carriers can increase level. Retain headroom. All controls remain live,
including modulator levels during release. Algorithm changes can click during
sounding notes; select between notes for ordinary audition. The test does not
claim click-free morphing or silently mute/reset ongoing phase state.

## Verification layers

1. **Executed native topology fixture.** The exact pinned `ymfm_fm.ipp`
   `fm_channel::output_4op` body is compiled without rewriting it, against tagged
   operator test doubles. Even output tags of 2 cancel the production modulation
   `>>1`; final carrier readout divides by 2 separately. Four basis vectors at
   each of eight algorithms identify every edge/carrier coefficient. The same
   coefficients are measured through the actual Faust routing functions used by
   the voice: 8 × 4 readouts × 4 bases = 128 exact comparisons. An omitted edge,
   omitted carrier and half-gain mutant must be detected. Feedback, noise,
   overflow/clipping, envelopes and oscillator/chip timing are deliberately not
   represented by this fixture. This is **not full ymfm audio**.
2. **Actual-Faust differential audio.** The one-voice runtime implementation is
   compared against archived v3's explicit graphs using eight waveform/control
   configurations per algorithm. Both are Faust; this tests equivalence, not
   independent hardware authenticity. A maximum sample error of 0.0002 is
   declared before execution. Raw levels/timing are not fitted or normalized.
3. **Preservation and lifecycle.** v2 serial audio at eight waveform assignments,
   runtime changes on a continuing instance, cold replay, blocks 1/127/256/511,
   scalar/vector, startup, release/live tail modulation, all-algorithm zero
   velocity, and bounded high-frequency/control corners at 44.1/48/96 kHz.
4. **Existing waveform qualification.** The previous full v2/native waveform
   suite remains required in the same hosted job. Repository smoke, unit tests
   and distribution compilation remain separate checks.

The workflow reuses the complete verified Faust 2.88 release, unchanged
reviewed-main native renderer, existing score/measurement/WAV helpers and
existing GitHub-hosted lane. No owner-machine dispatch or new controller.
`tools/modules/tx81z_routing_qualification.py --help` lists explicit inputs.
Use a fresh output directory. Raw audio, score, source/expanded/generated code,
native body/CSV, commands/timing/logs and hashes are retained, including failures.

## Auditions and decoding

Seven files under `runtime-results/listening/`: all eight algorithms, v2 then
v4 serial preservation, switches on one continuing voice, dual-pair bass,
three-carrier bells, four-partial keys, and unchanged versus modulated release.
All synthesis is compiled Faust. Python writes scores/concatenates/measures;
it never substitutes an oscillator for a missing engine.

Listening files use **PCM16 at 48 kHz**, with one fixed gain **0.5** and no
per-example normalization, limiter, added EQ or reverb. The existing voice
15-Hz DC blocker is part of DSP, not post-processing. Tests decode the written
WAV and compare exact PCM samples. This avoids the separate v3 audition-writer
mistake (float bytes declared as integer PCM); that earlier file is not used as
an audio reference. Raw `.f32` remains the numerical truth before PCM rounding.

## Remaining work

Operator-specific **OPZ** envelopes and frequency/level laws (currently a shared
generic ADSR and continuous floating phase/levels); fixed mode; feedback;
LFO/noise; uncertain upstream fields; independent hardware evidence; host/device
and musical acceptance. Do not extract a universal FM component or close #97
because topology/replay tests pass. Exact executed heads and evidence live in
the PR/issue completion comments, not mutable success claims in this document.
