# TX81Z v9 — patch-facing velocity checkpoint

`voice.dsp` keeps the v8 LFO/PM/AM/EG-Shift behavior and replaces the old whole-voice velocity multiplier with TX81Z per-operator KVS. KVS 0 is velocity-independent; KVS 1–7 use the established TX81Z one-note law with a `2^-KVS` zero-velocity floor and linear rise to unity, converted into the existing OPZ 0.75 dB total-level domain.

`tools/modules/tx81z_patch.py` now converts several previously unresolved VCED fields into the actual working-voice controls:

- output level 0–99 -> OPZ TL using the TX81Z ~0.74 dB panel step / OPZ 0.75 dB step (`TL = 99 - output`);
- 64-value TX81Z ratio-coarse -> OPZ multiple slot + DT2 family, with ACED fine retained;
- centred VCED DET 0–6 -> OPZ DT1 sign encoding;
- KVS -> the new per-operator KVS controls.

The ratio mapping is the compact 4 x 16 ordering of the published TX81Z ratio table: each row is one OPZ DT2 family and each position is the OPZ multiple nibble. This avoids the earlier mistake of treating the 0–63 panel ratio index as a raw chip multiple.

## Physical M1 result

Faust-expr `56f394b8da5246e1a1671871b1014c9cb1b3437d` ran through the established serialized queue on `Felix-M1-Pro-CURLOP`. CURLOP run `35108406241` completed successfully; artifact `10451815320`, SHA-256 `5adcbb6018a92222ec33831a4c80a5dbfed61b89d28621bbfac7bcb08243a0b3`.

The run executed 4/4 patch-mapping tests, compiled the actual v9 Faust voice, and passed all retained PM/AM/LFO-delay/EG-Shift behavior checks plus three new velocity checks: KVS 0 is sample-identical between velocity 0.2 and 1.0; KVS 7 at velocity 0.2 is materially quieter than full velocity; KVS 7 at full velocity is sample-identical to KVS 0 at full velocity.

## Remaining patch reconstruction work

The decoder deliberately still reports VCED **level scaling** and **EG bias sensitivity** as unresolved rather than inventing a policy. Voice transpose and reverb-rate are also retained as voice-level data but are outside the current one-note DSP control mapping. The next checkpoint is to reconcile level scaling / EG bias from established TX81Z behavior, then run legitimately usable real TX81Z voice data through the decoder and retain dry rendered examples. Timing/host-rate and output/channel applicability remain separate completion questions in #97.
