# TX81Z v10 — completed patch-reconstruction step

Owner #97; implementation PR #127; strategy #111. The next-step request (level scaling/controller policy, actual patch loading, dry renders) has a completed physical-Mac result. This is not final TX81Z hardware/device acceptance or the later useful-block extraction milestone.

## Exact result

- **Executed source:** `3c3f26e309ea0a7fda059d3391abc2731e9bfa23`.
- **Mac run:** private CURLOP `35114911089`, physical `Felix-M1-Pro-CURLOP`, existing serialized queue. Controller `b7d3a612c5f8433bc328a3042c6a3a4a528ab7a4`.
- **Status:** complete; **237/237 repository unit tests**, standard `scripts/build.sh`, and **99/99 qualification checks** passed.
- **Execution count:**46 renderer invocations:45 audio renders and one five-channel numerical probe. This is not46 presets.
- **Compiled policy:**10,900 cases /54,500 values, exact equality with the reference equations/tables used by this implementation. Includes9,600 KLS,1,024 KVS,144 provisional BC,100 output-level and32 carrier-trim cases.
- **Native regressions:** three decoded-patch cases, each sample-identical to unchanged `ymfm` under the explicitly isolated controls. Both sides share the patch translation policy; this is NOT independent firmware/hardware validation.
- **Source/backend delivery:** scalar/vector FilterBass and multi-file/standalone FilterBass renders are sample-identical. Predeclared maximum-absolute tolerance remains `1e-7`; observed difference0.

Original qualification artifact10454681824, ZIP SHA256 `b30ceb9918c7e4a7fafaccf86a6da42ef7c07cd866bb91b049eb1088da9d425e`, contains raw audio, exact scores, compiled sources, command logs, decoded references and results. It expires30September2026; this source record, patch-source identities and delivered source/audio bundle retain the useful result beyond the job badge.

## What now works

Checked93-byte VCED plus23-byte ACED, either SysEx frame order; checksum, manufacturer, channel, length and semantic-range validation. Corrected D1L-to-attenuation conversion, nonlinear low-output mapping, table-based KLS/KVS, carrier level trim, transpose and release-extension controls. BC uses operator EBS, patch BC depth and live breath input, including during release. Unsupported banks/functions are explicitly reported rather than guessed.

The initial v10 change to the printed operator order was **rejected by real patches**. The actual order4,2,3,1 is retained; it matches Edisyn and the author's asymmetric settings. See [REFERENCE.md](REFERENCE.md), including the preserved failed assertion and other source discrepancies. This is not a reason to reintroduce the manual's wrong middle-operator labels.

## Recordings

Actual compiled-Faust playback of Matt Gregory's downloaded single-voice patches, not hand-entered approximations and not hardware recordings. All files are mono PCM16/48kHz, converted from55930Hz with one fixed playback gain0.8, no per-file normalization, added effects or limiting.

| File | Duration | SHA-256 |
| --- | ---: | --- |
| `Filter1.wav` |7.4s|`43b204686ec06520f3403e5f49ce49fa5ca5175ab1392def36269e8a288a572e`|
| `Filter2.wav` |7.4s|`17cce1fe7efe6ea550cbbc48161dbf2720f87200723ce75bbde4ca00c0ec0b06`|
| `FilterBass.wav` |6.1s|`40369b34a230a9e59ef457b15c5b25c8054b67ab2591326196c48c883d9c7974`|
| `FilterBass-breath-variant.wav` |5.2s|`6e9db489103101ee1c0c7ae49b1ab8b25636a0518962e3f378e429f48d697332`|

The fourth file is an explicitly modified controller demonstration, not the unmodified author patch. Original patch URLs/hashes are in [patch_sources.json](patch_sources.json); the raw third-party patch files are not republished here or in the public-facing delivery.

The ready standalone `TX81Z_v10.dsp` is SHA256 `d32e36da03f534c391c74490b0a0bf62416d091b1ec80c78b3f359c9c62ef175`. Only the standard Faust library is external; no dependency on the private controller is required to load it. Its tested musical output matches the multi-file source.

## Actual cost and toolchain

Faust2.85.9, Apple clang16, NumPy2.5.3/SciPy1.18.1, physical M1 Pro. Historical Faust2.88/v5 qualification remains separately identified. The complete qualification took129.15s, mostly Faust source generation:37.28s scalar,41.26s vector,39.65s standalone. Corresponding C++ builds were approximately1.25–1.82s. This is an observed current authoring-compile cost, not a hosted-VM startup delay or an intrinsic audio-runtime requirement; do not claim every operation is instantaneous.

The6.1s FilterBass render accumulated33.72ms inside clean-mode DSP compute calls (128-frame blocks,55930Hz). Other delivered examples accumulated28.35–41.54ms. These are offline, instrumented observations, not callback-deadline guarantees or a finished memory/device benchmark. Documentation-only packaging must reuse identified audio rather than recompiling all variants.

## Remaining, in order under #97

1. Finish/bound the remaining intended timing/host-rate and output/channel/noise applicability decisions, with current-head coverage appropriate to the scope. Host-rate adaptation still has two host samples of feedback; analog output/DAC is not modelled.
2. Retain the scoped voice baseline and remaining clean compute/memory evidence, then expose useful source blocks, reconstruct the voice and demonstrate a small alternate composition. No universal DX7/OPZ engine is required.
3. Keep precise TX81Z BC-curve calibration, human listening, actual CURLOP/device realtime acceptance and merge/release separate and open.

**BC limitation:** its numerical magnitude uses a disclosed DX100-family prior-art formula. The controls are implemented and tested; their exact TX81Z hardware curve is not established. Native comparison cases disable BC/LFO/EG Shift/fixed-Hz mode because the retained native adapter cannot independently validate those paths. Wheel/foot routes, bend, portamento and polyphonic allocation remain outside this one-note patch adapter.

No new instrument, issue/PR/branch, host GUI or engine was started. No merge or release is implied.
