# Echo / reverb reference follow-up

Owner-selected pair under CURLOP #277/#278/#271, Faust-expr PR74. Preserve all earlier versions. No new browser module or GUI.

## Why another candidate

The first PR74 artifact contained six renders and four checks, not full qualification. Reverb v2 always used all 26 stages, made Size a gain, and omitted MV's ordered per-stage averaging and output arcsine. Echo v1/v2 put only the third head in the feedback branch. These are inspectable sound/design differences, not fixes that can be established by one descriptor score.

## Reverb v3

Target is pinned Airwindows MV2 at `03c9931839881bae6dfd4e36bfd3cced79f54b4a`, complete original double processing, states and constructor resets. MV2 preserves the MV design at higher rates by skipping network updates and reconstructing intervening samples. Candidate covers 44.1/48/96 kHz; higher rates are not claimed tested by this runner.

Same five control IDs. Size maps to MV2 Depth, Tone to Bright, Decay to stepped Regen, Mix to Wet; Character retains its existing output-gain mapping `0.55+0.45*character`. It is not an independent distortion circuit control. No hidden fitted presets or output alignment/gain fitting. Differences are source/sound-versioned. v3 uses upstream-style immediate changes; smoothing and faceplate labels must be auditioned in the consumer.

Faust allpass stages use explicit state and read/write tables, with frozen inactive state and a discard-write cell. Verify scalar/vector behavior rather than assuming a library allpass matches the original's exact delay/order. The raw feedback is taken before output gain/arcsine. Numerical gates defined before execution: full-output relative RMS <2e-4 for float and <2e-6 for double across stated cases. These are implementation checks, not perceptual or physical Midiverb approval.

## Echo v3

Remains three-head, not a one-head TapeDelay replacement. The integer transport chase is separately tested against the actual TapeDelay method instrumented to expose maxdelay. A transport match does NOT establish whole-audio equivalence: TapeDelay resizes its buffer, whereas this candidate uses fractional read heads. Its integer tone quantisation and full Lean/Fat stage are NOT ported. The five-tap smear is an authored approximation.

All heads now read input plus feedback; muting head3 does not stop recirculation. Geometry stays 0.5/0.75/1.0. Age zero disables drift for a deterministic anchor. Control smoothing is sample-rate-aware except transport/head selection. Verify actual header storage and device cost before making memory/CPU claims. The 9000-sample controller constant is intentionally upstream sample-domain behavior, not a sample-rate-invariant mechanical model.

## Evidence

Reuse ReferenceLab and tools/modules/render.cpp. Full originals, scores, raw inputs/outputs, generated headers, settings, metadata and hashes are retained by the bounded read-only hosted workflow. Near-zero noise injection is disabled by seed zero and separately checked with a nonzero seed; original double output dither is already inactive. No changed original processing code except the explicitly labelled transport telemetry probe. Preserve failures. Stop/report unresolved failures rather than widening gates.

Pending after tests: user listening, physical Space Echo/Midiverb references, exact CURLOP bindings/GUI/save-reopen, target-device performance and release permission. No merge or automatic migration.
