# Cymbal reference candidates — awaiting user choice

These are recordings of real instruments for audition and eventual comparison, not synthesised results and not a decision to change either model. No candidate is selected yet.

1. VCSL Suspended Cymbal 1: soft and strong stick hits, a bell hit, plus the separately labelled general `fff` hit. Exact make, diameter, impact point and the beater for that general hit have not been verified. The complete upstream set also has additional dynamic levels, rolls, crescendos and bowed sounds.
2. Virtuosity Drums crash: three velocity layers, first round robin, overhead microphone pair only. The sizzle/chain articulation is deliberately excluded.
3. Virtuosity Drums ride: low/high normal hits and a medium bell hit, first round robin, the same overhead pair. This is distinct from the library's flat ride.

## Sources and permission

- VCSL by Versilian Studios LLC: https://versilian-studios.com/vcsl/ ; https://github.com/sgossner/VCSL
- Virtuosity Drums by Versilian Studios and Karoryfer Samples, performed by Austin McMahon: https://versilian-studios.com/virtuosity-drums/ ; https://github.com/sfzinstruments/virtuosity_drums
- Both source repositories publish the recordings under CC0-1.0. The collector saves their licence files with the original audio.

Run `python3 scripts/fetch_cymbal_references.py` with internet access. Downloads are restricted to ten named source files (~13 MB), immutable source commits and verified Git blob hashes. The workflow is bounded, uses read-only permissions, calls no model and fetches no private repository. It saves unchanged originals and a SHA256/source manifest. Candidate binaries are artifacts, not files added to Git history.

Any later listening edit must state its gain/cropping/fade/resampling separately. Source dynamics are labels, not measured physical impact velocities. Microphone and room contributions must not be mistaken for the instrument's mechanics. No complete bell/bow/edge force-calibrated measurement set is claimed here.

## Existing model experiments (unchanged)

- PR #2: 128-mode spatial model, nonlinear scattering surrogate; `43f39b60b7b239f8e460d1d8ee7b6f027a0532fb`.
- PR #4: 162-mode shell model, coupled contact and low-rank nonlinear stretching; `821736879bdea6c1c58ce9f0d932e0ebe5196840`.

Keep genuinely different models on separate draft PRs. Revisions of one model belong on its existing PR. A shared cymbal project issue/index should identify model, tested commit, reference set, audio, benchmark hardware and current conclusion; it must not conflate a reference number with a model version. No merge requested by this audition preparation.
