# CYM-ID refinement 02: relative thickness and held contacts

Continuation of PR #8 under Cymbal lab #6. This preserves the original material kernel and adds a separately generated extension. No new model PR, refitting, mode reduction, new beater, private content or fluid interaction. Liquid submergence stays deferred in #9.

## Implemented scope

- `relative_thickness` 0.25–4, with 1 as the unchanged reference. No measured millimetre thickness is known.
- Optional `thickness_pitch_lock`: hold the mapped 1 kHz reference frequency while other resonance ratios still change.
- Six held-contact prototype response families: finger/palm-like, felt-like, leather-like, rubber-like, wood-like, bone-like. Pressure, effective area, pad compliance-length scale and strength are separate controls.
- Contacts change internal modal frequencies and damping. They are not output fades or post-EQ. Removing a contact cannot restore dissipated oscillator state; new strikes excite the remaining model.
- The original material transforms and grip still work. Neutral new controls retain the prior audio exactly in the tested cases.

## Scientific boundary

This is NOT a recovered physical surface, validated real-material contact, or a new full shell/contact solver. All numerical contact preset coefficients are explicitly unmeasured normalized load assumptions. Their names identify intended response families, not measured tissue, felt, timber or bone specimens. No contact location, hand shape, friction, rebound, loose rattle, full viscoelastic history or cross-mode contact interaction is represented.

Thickness uses the frozen-shell relationship `f_new/f = sqrt(1+b_i*(h_ratio^2-1))`. Bending stiffness scales with h^3, membrane stiffness with h, and mass with h. The bending fraction curve is a 0.35-octave Gaussian local mean from 84 families / 162 modes in CYM-B's assumed nominal 50cm shell (commit `821736879bdea6c1c58ce9f0d932e0ebe5196840`, central bell grid). It is sampled at 25 log-uniform frequencies from 32 to 16384 Hz, with endpoint clamping. **Frequency-local transfer onto the 4,207 fitted poles is an unvalidated hypothesis, not a mode-shape correspondence.** Even moderate thickness changes require physical validation; the full control range is exploratory.

Relative thickness also enters target thermal-loss thickness. Velocity output is divided by sqrt(h_ratio), using a defined mass/energy-coordinate convention. Existing oscillator state is retained under transformations; this is a specified fantasy morph, not a prediction of external work on real changing matter. Fitted excitation coefficients are not changed.

For held contacts, the diagonal weak-loading approximation uses `z=2*pi*f*tau`, storage stiffness `K0+K1*z^2/(1+z^2)`, added amplitude decay `load*(c+K1*tau/(2*(1+z^2)))`, and `f_loaded=sqrt(f_free^2+load*Kstorage/(4*pi^2))`. Load is pressure squared times area and strength, divided by relative thickness and pad scale. K0/K1 are assumed mass-normalized coefficients (s^-2), NOT elastic moduli in Pa. Area and pad controls are relative, not measured physical dimensions. This is a frequency-domain modal approximation, not a transient pad-deformation solve.

Temperature, independent bell/body geometry, taper, spatial contact, new beaters, nonlinear crash mechanics and water were not added in this pass. No real-time claim.

## Evidence and reproduction

Local actual-Faust suite: 24 test methods passed, including the three original four-second full-bank anchors bit for bit, all 12 existing materials at both lock settings on a synthetic pole, pressure-zero identity, contact frequency/loss formula checks, state passivity, release/restrike, selected extreme controls/rates and exact-event block independence. This is not perceptual or physical validation.

Three full-bank recordings and sample-frame scores were generated: 50s thickness comparison (30 strikes), 56s held-contact comparison (35 strikes), and 72s continuous performance (179 strikes). Source, unchanged fitted CSV, full renderer, tests, raw float32 audio and analysis are together in the chat's `cymbal-thickness-contact-02` kit. **This repository export still does not contain the complete fitted dataset and full-bank host.** Hosted CI here is explicitly a synthetic oscillator/kernel fixture, not the long cymbal renders.

Generate the exact extension using:

```sh
python3 generate_interactions.py materials-engine.lib refinement-controls.lib interactions-engine.lib
python3 generate_wrapper.py interactions-engine.lib interaction-bank.dsp
faust -double -cn ModalBank interaction-bank.dsp -o interaction-bank.hpp
```

The composed kernel's tested SHA256 is `c1e2cbf0e92c49d3b6c489ae8e44c352d913817513fd0a71b558a65047a2d4a8`. Baseline source is hash-checked and unchanged. Fitted CSV SHA256 remains `a789d6a07326e68c74a1b489feae74c75b1d1275dfbd81c58b144b97230eaa47`.

The listening files use one fixed shared gain across all three, no separate material/hit normalization, and only 50ms end fades on comparison panels/performance. No attack fades, EQ, compression, reverb, noise or audio-sample playback. Raw files are unchanged. Do not substitute these new gestures for earlier original demonstrations and label them 'before'.

Research basis, not validation of this implementation:
- https://doi.org/10.1121/1.5091013
- https://doc.comsol.com/6.3/doc/com.comsol.help.sme/sme_ug_theory.06.029.html
- https://www.cs.cornell.edu/projects/Sound/mc/
