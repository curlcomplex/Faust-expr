# Spatial Cymbal — first audible model

This is a research instrument for the user's brief, not a calibrated replica
of a manufactured cymbal. All sound comes from compiled Faust; no recorded
cymbal samples, noise layers or reverb are used.

## Build and play

`bash scripts/build_cymbal.sh` builds the model and renders its comparison suite.
Dependencies: Faust, g++, Python, NumPy and SciPy. The original two-tone smoke
test (`scripts/build.sh`) remains available. CI includes both test suites.

The cymbal artifact contains `spatial-cymbal.dsp`, a self-contained generated
source for Faust hosts. Maintained source: `dsp/cymbal.dsp` and
`scripts/generate_cymbal.py`; do not edit the expanded kernel by hand.

Press **gate** to strike; release before the next hit. Gate release does not
stop a tail. **clear** empties resonator state; **choke** increases damping.
Start with a low monitoring level. Audition WAVs are not individually normalised.

## Controls

- **diameter_m:** 1 cm to 40 m, logarithmic. **thickness_mm** is independent;
  **proportional_thickness** scales thickness with diameter. Resize while ringing.
- **taper:** reduction from centre thickness to rim. **bell_diameter_ratio**,
  **bell_height_ratio**, **bow_height_ratio** control shape independently.
  Heights are relative to main radius; bell diameter ratio to main diameter.
  Default: 44 cm diameter, 1.2 mm central thickness, 0.48 mm nominal rim thickness,
  12.3 cm bell diameter, 2.64 cm bell height.
- **strike_radius:** 0–1, support to rim; **strike_angle_deg** covers the surface.
  Radius below 0.035 is clamped and silent. Try 0.16/0.55/0.92 for bell/bow/edge.
- **velocity**, **beater** (wood/nylon/felt/rubber/steel), **beater_mass_g**,
  **tip_radius_mm**, **beater_hardness** control excitation.
- **material:** bronze→steel→glass→wood morph. **stiffness_scale**,
  **density_scale**, **loss_scale** permit imaginary combinations. Wood has
  **grain_anisotropy** and **grain_angle_deg** controls.
- **hammering** increases a fixed deterministic imperfection pattern;
  **hammer_pattern** selects it. Automate amount rather than pattern number.
- **nonlinearity** controls internal energy exchange; **gain_db** is linear
  output gain, not a compressor or distortion stage.

## Model and its limits

The reference calculation uses Rayleigh–Ritz bending energy for a tapered
annular Kirchhoff–Love plate. Trial functions enforce displacement and slope
zero at a small central support; the outer boundary is variationally free.
Radial thickness varies linearly. Radial, hoop and twist curvature terms enter
the energy. Eigenvectors are mass-normalised and angular quadratures retained.

The engine retains **64 broadband modes**, not every mode below a cutoff.
The initial 128-mode compiler graph exceeded Faust's default 120-second limit;
64 is the first-build complexity budget, not a convergence claim. Strike
coordinates sample mode shapes; the finite tip footprint attenuates short
wavelengths. Effective impact mass follows modal mobility. A restitution
collision law uses pre-impact surface velocity; a Hertz-inspired duration
estimate shapes the resulting impulse. Stiffness, density, thickness and radius
change resonance scales, not just equalisation.

Important approximations:

1. **Bell/shape:** positive local curvature foundation with an uncalibrated
   relaxation factor, not a complete shallow-shell solve. Geometry morphs use
   positive stiffness-frequency interpolation and a radial coordinate warp of
   a fixed reference basis, not freshly solved eigenvectors.
2. **Nonlinearity:** two connected Cayley-rotation sweeps exchange modal momentum
   energy depending on displacement. Each rotation preserves quadratic state
   energy and damping reduces it. This passive surrogate is NOT the von Karman
   coupling tensor or validated wave turbulence. Numerical stability does not
   prove a realistic crash sound.
3. **Beater:** the initial collision includes body velocity but the following
   pulse is prescribed. Continuous deformation, separation and re-contact are
   not solved. No brushes, scraping or bowing. Properties are representative
   design assumptions, not measured specimens.
4. **Hammering/material:** deterministic mode splitting and asymmetry; no dents,
   residual stress or work hardening. Glass is unbreakable. Wood uses a
   directional-stiffness proxy, not a full orthotropic shell. Material morphing
   is a creative construction, not a claim about alloy manufacture.
5. **Radiation:** two weighted velocity pickups give stereo; no microphone,
   room, full radiation or radiation-impedance solve.
6. **Retuning:** energy-coordinate states preserve represented quadratic energy
   before damping, an explicit musical convention for a changing object.
   Moving the strike point alone never crossfades the existing sound.
7. **Bandwidth:** approaching-Nyquist modes fade and damp; extreme sizes can be
   ultrasonic/subsonic. Fixed mode count does not establish timbral completeness.
   Auditions render at 96 kHz and use low-pass downsampling to 48 kHz to reduce
   nonlinear aliasing. The DSP does not internally oversample; real-time CPU,
   aliasing convergence and iPad performance require separate evaluation.

## Evidence

`evidence/cymbal/results.json` records the tested commit, parameters, events,
levels and runtime. Tests cover silence, support, repeatability, invalid controls,
sample rates, block-size independence, nearby strike continuity, unchanged tails
when the beater moves, clear/retrigger, contact softness, nonlinear ablation and
extreme controls. These are behavioural/numerical checks, not listening judgement.

First audition bell/bow/edge using wood and felt, then size/material and live
resizing. Fidelity must be assessed against recordings or a fuller reference,
not inferred from control labels. No private host source is included; CURLOP
runtime/GUI verification remains separate. Do not merge without approval.

## Research basis

- Nguyen & Touze (2019), *Nonlinear vibrations of thin plates with variable
  thickness: Application to sound synthesis of cymbals*, JASA 145, 977–988.
  https://doi.org/10.1121/1.5091013
- Author examples: https://perso.ensta.fr/~touze/tapercymbals.html
- Faust syntax: https://faustdoc.grame.fr/manual/syntax/

These motivate the approach and caveats; this code does not reproduce the
paper's full nonlinear method.
