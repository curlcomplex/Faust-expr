# Metal PM — complete electronic percussion experiment

Issue #17; draft PR #41. Source is canonical here; no Tracker/private source changes. The snare's delivered direction was positively approved by Felix before this batch. No Metal sound or device approval is implied.

## Scope and correction to the bootstrap

`0.2.0-experiment` is a new pre-release identity. The bootstrap `6724b1d` had used Sweep as a kick-like pitch fall and treated choke as an onset-latched mode. Neither was a satisfactory mapping for this brief. Published Model:Cycles Metal descriptions give Color/Sweep separate tuning roles, and Syntakt CY Alloy documents operator-A/B tune offsets. This implementation uses those musical dimensions, with independently authored ratios, PM topology, macro laws and dynamics. It is NOT a recovered Elektron algorithm. Bootstrap history remains available, not a supported shipped sound.

## Instrument

Pitch/Decay plus Color, Shape, Sweep, Contour, Punch, Drive. Color and Sweep tune separate interacting operators (Sweep does not bend overall pitch). Shape coordinates PM amount, bounded feedback and sparse/dense readout; Contour moves persistent coloration toward a short spectral transient; Punch shapes the initial amplitude and speed. All musical values and velocity latch at onset. Zero Drive is neutral. Gate-off does not choke.

Six sinusoidal oscillators form three interacting pairs. The sparse entry reads one carrier; the dense entry crossfades into a three-carrier mixture as Shape increases. No noise generator, recorded tail, reverb, physical cymbal model or extracted waveform is present. All feedback gains are bounded; output uses fixed gain and a fixed DC filter. Its output is amplitude/choke shaped after that filter so a completed choke is exactly silent.

`choke` is an explicit rising-edge performance event: eight-millisecond smoothstep to zero, release does not resurrect a tail, next note reopens it. At simultaneous onset/choke, onset wins. The host needs a computed low gate before another rising edge; do not reconstruct DSP per hit to conceal persistent-state problems. Ordinary retrigger still resets phases and can interrupt a tail; this is not a universal click-free guarantee.

## Optimization and experiments

`metal.dsp` and `reference.dsp` now both use the **direct exponential amplitude law**. The faster multiplication recurrence failed the 1.5% long-decay limit and is not used for playback. `ampFast` and the report's `fast` build label are historical aliases, not evidence of an optimization win. `rejected-envelope.dsp` deliberately renders the old law as a negative control; it must still exceed the unchanged rejection threshold. `sparse.dsp` remains a controlled architectural alternate, not an equivalent faster implementation.

The final gate also compares generated envelope audio against an independent float64 closed form, rather than merely comparing two outputs with the same equation. Scalar/vector builds are measured against the same complete sound. We retain accurate scalar rendering as the conservative starting point; device profiling decides whether vectorization helps. No new envelope approximation is required before using the instrument.

The whole suite builds actual scalar/vector Faust, checks latching, same-sample locks, choke equation/priority, clean base pitch, persistent endpoint and rapid-retrigger trajectories, long tails, sample-rate/segmentation behavior, arithmetic approximation and fixed-gain musical examples. Four-voice paired benchmarks rotate the three equivalent processing paths. Record p50/p95/p99/max, memory, stack and ordinary-new evidence; no inference to an iPhone or hardware module without target tests.

## Reference evidence

Winston Edwards / Particles Into Waves, Syntakt Designer Drums (13 June 2022), CC BY 4.0. Fixed development selection CYA-01/04/07/10/13/16; reserved comparison CYA-03/09. Public MP3 previews, mono float32 decode at 48 kHz, no acquisition gain/EQ/normalization. Unknown firmware/settings/gain. These are CY Alloy comparators, not Model:Cycles captures. Eight descriptors sets are compared with the same 48 independently generated settings. Time/spectral descriptors describe broad coverage, never waveform identity or recovered knobs. The scale excludes reserved references. An eventual architecture decision can use development evidence; reserved results must be reported separately and not tuned against silently.

Manufacturer manual source: Syntakt OS 1.30 manual dated 16 October 2024, Appendix A page 90 (CY Alloy), https://manuals.plus/m/c8781a2f37da076182fea0b38d398320dfca541b75d9cc0133272410d5c772ae.pdf . This is an identified historical manual, not a claim to the latest firmware. Preview source: https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums . Original files, decoded hashes, source-page hash and attribution live with evidence; no recordings committed to Git.

## Execute and replay

```
python3 -m unittest discover -s tests -p 'test_metal*.py' -v
python3 tools/modules/fetch_metal_references.py --out build/metal-references
python3 tools/modules/metal_acceptance.py --out build/metal-full --references build/metal-references
# Without Faust, using an exact source snapshot and saved generated C++:
python3 tools/modules/metal_acceptance.py --out build/replayed --references /path/to/metal-references --replay /path/to/metal-full
```

Build prerequisites: Faust, C++17, NumPy/SciPy; FFmpeg only for intentional acquisition. Replay validates source/generated/reference hashes before use. Source, generated code, raw renders and score identities remain distinct. Relative reference-audition gains are logged; core auditions remain fixed-gain PCM16 without normalization/added effects. Native device work and consumer UI/merge belong to Tracker.

See [QUALIFICATION.md](QUALIFICATION.md) for the corrected evidence and [INTEGRATION.md](INTEGRATION.md) for the consumer contract. Exact-head build results live in PR #41 and its identified CI artifact. This remains a draft prototype, not a hardware clone, musical approval or device release.
