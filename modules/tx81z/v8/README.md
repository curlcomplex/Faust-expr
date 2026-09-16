# TX81Z v8 — working-voice checkpoint

`voice.dsp` layers documented TX81Z EG Shift behavior over the v7 LFO/PM/AM voice. Yamaha operator 1 remains fixed OFF; operators 2–4 expose OFF/48/24/12 dB through the OPZ attenuation right-shift before AM and total level.

## Physical M1 result

Faust-expr `669c13bc50eed601be158cba32ef9e6296a9b9bc` was executed by the private CURLOP controller on `Felix-M1-Pro-CURLOP`, through the established serialized machine queue. CURLOP run `35106031016` completed successfully; artifact `10450820019`, SHA-256 `076f44a1ef0fcd0f43a635da3688b07147df0b6fdedd3049a02d420fd1906a69`.

The run compiled the actual v8 Faust voice and rendered one-second float audio cases at 55,930 Hz. All behavioral checks passed: audible baseline, PM changes audio and produces time-varying periods, disabled AM is sample-identical to baseline, enabled AM changes/reduces level, LFO delay initially stays closer to the unmodulated baseline, and EG Shift audibly changes the selected operator. Baseline RMS was `0.0625053841`; AM-enabled RMS `0.0210909583`; baseline positive-crossing period stddev `0.4391` samples versus `25.8704` with the strong PM case.

The same run executed `tests/test_tx81z_patch.py`: 2/2 pass. `tools/modules/tx81z_patch.py` now decodes the documented 93-byte VCED + 23-byte ACED layouts and maps only fields whose relationship to the current voice is direct. It deliberately leaves TX81Z output-level, ratio-coarse, DET, level-scaling, EG-bias and key-velocity-sensitivity conversions unresolved rather than inventing mappings.

## Scope

This is a software/reference-led working-voice checkpoint, not physical-hardware acceptance. The runner currently reports Faust 2.85.9; the earlier v5 native-law freeze remains the pinned Faust 2.88/native-ymfm parity authority. v8 qualification therefore establishes that the new source compiles and behaves as intended on the physical development machine, not that all prior 2.88/native comparisons were rerun.

Next instrument work is the remaining TX81Z-facing patch policy: resolve the explicitly unmapped VCED fields from established references, feed real voice data through the decoder into the Faust controls, then retain dry musical examples. Timing/host-rate and final output/channel applicability remain separate tracked questions in #97.
