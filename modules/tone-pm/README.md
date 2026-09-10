# Tone PM — full-service prototype

Issue #19. Independent compact two-operator phase-modulation voice using the published Tone/SY Tone control roles as a behavioural brief, not proprietary source recovery.

Eight musical controls: Pitch, Ratio, Punch, Decay, Feedback, Modulation, Mod Envelope, Drive. Gate and velocity are performance inputs. All musical controls latch at onset in this prototype; note-off does not choke the internal decay. Carrier and modulator phases reset per trigger. Zero drive is genuinely neutral, while Punch can still create intentional early nonlinear emphasis.

Published Model:Cycles discussion describes Tone as a simple two-operator setup whose Color controls ratio, Shape modulation depth, Sweep feedback, and Contour modulation-envelope behavior. The current Syntakt manual independently exposes Ratio, Punch, Decay, Feedback, Modulation, Modulation Envelope and Overdrive for SY Tone. Exact hidden curves/topology remain unknown; this source's exponential ratio mapping, envelopes, feedback coefficient and drive law are ours.

The batch uses fixed CC BY 4.0 SYTN public previews from Winston Edwards / Particles Into Waves only for broad spectral/decay coverage. Their patch settings, firmware and recording gain are unknown. Do not interpret nearest-descriptor numbers as clone scores or recovered macros.

`python3 tools/modules/tone_batch.py --out build/tone-full --references build/tone-references` builds scalar/vector actual Faust, renders qualification scores, emits fixed-gain auditions and records evidence. `tools/modules/fetch_tone_references.py` acquires the fixed licensed comparator set. The repo workflow performs both on a disposable hosted runner.

Human sonic approval, ratio-detent choice, known-settings hardware matching and actual iPhone/embedded performance remain separate gates.
