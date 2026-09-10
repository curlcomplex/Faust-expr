# Tracker / CURLOP consumer handoff

Canonical work: Faust-expr #20 / PR #46. Consume the exact verified final PR commit, **morph-wavetable / 0.2.0-experiment**, `modules/morph-wavetable/v2/morph.dsp`, its engine and the pinned generated `bank.lib`. Do not use v1 merely because its shorter path looks canonical. No existing machine slot or migration is assumed.

Six synthesis columns exactly: **Morph, Shape, Decay, Detune, Stack, Drive**. Stable IDs are morph/shape/decay/detune/stack/drive. Pitch_hz, velocity and gate are independent performance inputs. Keep Decay/Release in column 3. Stack accepts only integer 1–4; validate/reject fractional values at the host boundary rather than rely on Faust int truncation. Follow the manifest, not a prior instrument's parameter indices.

One host note produces one independently gated stereo instance. Internal same-note unison has no interval/chord/scale state. Tracker chord locks allocate separate host voices normally; four notes with Stack4 mean four host instances with sixteen audible oscillators, not sixteen host notes. Each instance currently computes four oscillator paths at all settings: budget accordingly. Do not add interval tables to implement Stack.

Outputs are TWO channels; the existing mono renderer adapter must not pass one buffer to this class. Preserve stereo through the host graph or deliberately downmix (L+R)/2 once. Verify stereo, mono compatibility, pan/level law, voices and host meters in an actual generated-channel test.

Pitch/Morph/Shape/Detune/Drive are live with 3ms smoothing and onset snap. Stack/Decay/velocity latch at onset. Write all intended values before computing the gate edge. Compute a real low gate before another rising edge. A held gate sustains; note-off begins release. Keep rendering tail state until finished, do not sleep simply because one output chunk is quiet. Legato pitch changes while gate stays high do not retrigger the envelope; host-specific note allocation remains explicit.

The authored bank and offline generator are source/evidence, not an online dependency. Pin bank bytes/hash from the selected build; reproduce them with the recorded numerical toolchain or explicitly validate a regenerated version. The optimized and direct-reference classes render the same specified spectrum. Use the qualified scalar generation recipe initially: the current vector recipe exceeded its compile budget and is NOT qualified. Do not inherit fast-math approximations blindly. Preserve old sound identities and require canonical/adapter/AOT/interpreter parity before consumer merge.

No Nord demos, proprietary tables or external audio files are dependencies of this module. No private repo checkout, changes to credentials/runners/visibility, product release or merge is performed by this handoff. Faust-expr owns the sound; the private product owns implementation, device tests and release.
