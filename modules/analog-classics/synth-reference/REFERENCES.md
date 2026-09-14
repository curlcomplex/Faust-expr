# Analog Classics synth reference/tuning pass

Tracks Faust-expr #82. Baseline is draft PR #65 at `fd2b7efae4951d014fed3fed3cd01f295a0af21e`. This is laboratory/reference work only: no installed CURLOP module, faceplate, migration, merge, factory promotion, hardware approval or device qualification.

## Invariants

- Every instrument remains one note per Faust kernel with canonical `gate`, `freq` in Hz and `velocity`. CURLOP owns allocation, polyphony, stealing and sleeping.
- Juno chorus remains outside the note kernel and is not part of dry-voice matching.
- Preserve every earlier source/sound version and broad experimental control range. Reference work establishes useful classic anchors and corrects evidence-backed architecture errors; it does not reduce the instruments to stock-hardware control sets.
- Do not represent software-oracle agreement as hardware authenticity.

## SH-101 / Mono 101

Primary architecture/circuit research includes the SH-101 service-derived IR3109 topology and the BonzaiCoin SH101 model, which describes the source mixer/filter as a transcribed circuit, VCA/envelope at transistor level, and oscillator/noise sections from measured behavior. No third-party source or audio is copied into this repository.

A concrete v1 issue is addressed in `modules/mono-101/v2/voice.dsp`: v1 used the shared `ota4` helper with nonzero output compensation (`comp=.20`). SH-101 IR3109 research distinguishes this implementation from Juno-family Q/resonance compensation. v2 therefore keeps the same four-stage filter equations and all public controls/defaults but sets the SH-101 compensation term to zero. This is an architecture correction, not a complete circuit model or a hardware-null claim.

Remaining SH-101 work: controlled hardware/listening A/B, oscillator/sub/noise level behavior, resonance/cutoff calibration, envelope-time law, overload and glide behavior. If later evidence contradicts this candidate, preserve and supersede it rather than rewriting history.

## Juno-106 dry voice

`stevengoldberg/juno106` is retained as a structural software oracle: separate DCO, VCF, HPF, VCA and envelope code; its HPF implementation exposes the classic stepped 0/100/180/320-Hz positions, while its VCF is a software approximation built from cascaded WebAudio biquads. It is useful for control/architecture context, **not** a measured hardware transfer-function oracle.

Our existing Juno candidate deliberately exposes continuous HPF and waveform balances as extensions. Do not delete those extensions merely to copy another software UI. The dry voice remains unchanged in this checkpoint because no controlled Juno hardware corpus with sufficient settings/provenance has yet been acquired. Chorus remains a shared/post-voice effect.

## Minimoog / Mini Voice

`t2techno/Faug` is a useful Faust research oracle and identifies itself as a Minimoog Model D emulation. Its inspected DSP includes three oscillators, selectable waveforms/ranges, key tracking, separate amplifier/filter contours, modulation and `ve.moogLadder`. However, the repository currently exposes no declared software license in GitHub metadata, and the DSP contains known fixed-44.1-kHz assumptions (`nyquist = 22050`) and TODOs. No Faug code is copied here.

Our current Mini remains unchanged in this checkpoint. Whole-instrument tuning requires either controlled Model D hardware recordings with declared settings or a stronger redistributable/reproducible oracle. Agreement with the shared Faust ladder alone would not establish Model D fidelity.

## Qualification boundary

`tools/modules/synth_reference_pass.py` first verifies the SH-101 compensation correction while rendering Juno and Mini from their exact retained v1 sources. The previous complete synth suite is rerun separately as a regression. Candidate-only WAVs may be retained for review; third-party/hardware reference audio is not repackaged without clear permission.

Pending across all three: Felix listening approval, controlled hardware captures/held-outs, actual CURLOP bindings and faceplates, overlap/save-reopen behavior, and named-device callback/memory/thermal tests.