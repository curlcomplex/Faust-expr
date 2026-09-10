# Morph / 0.2.0-experiment

One host note, one stereo synthesizer voice. Six controls: **Morph / Shape / Decay / Detune / Stack / Drive**. Pitch, velocity and gate are separate. No chord, interval, inversion, scale or arpeggiator logic. Up to four internal same-note oscillators are timbral unison, not four independent note events.

## Sound design
Eight authored frames: Sine, Soft, Saw, Hollow, Vowel, Organ, Glass, Edge. Smoothstep frame interpolation and common oscillator phase create a continuous path; Shape darkens or opens harmonic balance. This is a 64-harmonic spectral wavetable equivalent, evaluated analytically. Its exact single cycles and Fourier coefficients are generated and exported for inspection, not loaded from audio files at runtime.

Drive blends toward independently precomputed, saturated frame spectra before the pitch-dependent band limit. It is spectral saturation, NOT a post-stack distortion and not a dynamic model of an analog drive circuit. Zero is neutral relative to the clean spectral bank. All frame endpoints are peak-calibrated offline; the output does not normalize individual notes or patches.

Detune sets symmetric outer offsets of +/-0–28 cents. Stack=1 is exactly the host pitch and ignores Detune. Each active count is centered in log pitch. Oscillators start at the same phase, then separate through their actual frequencies. Zero Detune collapses every count to the same sound without cancellation. Stereo placement is tied to those offsets with a fixed maximum pan spread. There is no separate chorus or LFO.

Held gate sustains. Note-off captures the current attack level and starts release with tau=0.035*100^Decay seconds. Attack tau is 2.5ms. The release is set to zero after 14 time constants, below -121dB relative amplitude. A retrigger restarts oscillator phases/attack; an abrupt monophonic restart is not certified click-free. Host overlapping voices/voice-stealing policy is separate.

Live Pitch (log-Hz), Morph, Shape, Detune and Drive use 3ms smoothing and exact onset snap. Stack, Decay and velocity latch on onset. Decay is a release control for this held instrument, not an independent pre-note-off fall. All six controls map directly to the columns, with Decay in column 3.

## Corrected draft problems
The prior v1 selected the first N of four fixed detuning offsets, which pulled lower counts flat; even its one-oscillator mode could be detuned. Fixed phase offsets could also cancel at zero Detune. Both are corrected here and tested, while the v1 source remains frozen. The authored spectral series is wider (64 harmonics), stereo, and band-limited after spectral drive generation. It is therefore a new sound version, not a silent v1 replacement.

The initial bank representation expanded runtime selectors at compile time until Faust timed out. Static coefficient patterns remove that expansion without deleting harmonics. The current Faust vector recipe also exceeded its compilation budget; it remains an explicitly unqualified optional backend, not a fake alias for scalar or an assertion that every compiler is unsupported.

## Build and evidence
Run `python3 -m unittest discover -s tests -p test_morph_v2.py -v` and `python3 tools/modules/morph_v2_batch.py --out build/morph-v2`. Requirements: Faust, C++17, NumPy, SciPy. The generator runs first and supplies `bank.lib`; include the emitted bank with the source when compiling outside the script.

`reference.dsp` directly sums all harmonic sine terms. `morph.dsp` evaluates the SAME series with an unrolled Clenshaw recurrence. The recurrence is algebraic waveform evaluation, not the rejected recursive exponential envelope from Metal. Both use direct amplitude exponentials. The measured scalar paths are separate from the unqualified vector attempt. See PR #46's exact checkpoint for actual benchmark/test numbers rather than infer success from source presence.

Use a stored generated-header/bank artifact with `--replay PATH` for independent C++ recompilation without a Faust installation. This is not a second Faust compiler. Source, bank, generated headers, scores and raw audio hashes identify each reported run.

## Limits
64 harmonics cap low-note brightness. A fixed 16–19kHz taper keeps static waveform partials within the same band at 44.1/48/96kHz, but this does not prove arbitrary live modulation is alias-free. All four oscillators currently compute even when Stack=1; audible count is not a runtime CPU savings claim. Coefficients contribute to binary size; audio files are not read by the kernel. Target iPhone/embedded memory, first-build cost, polyphonic CPU and thermal behavior remain consumer qualification.

Listen without assuming architecture proves a useful distinction from Tone. The six listening files cover authored anchors, a locked phrase, live control movement, 1–4 same-note unison, four independent host notes and a single-oscillator comparison against Tone. Nord recordings are external prior-art examples, not fitted dry calibration data. See REFERENCES.md and INTEGRATION.md.
