# Spatial Cymbal — first audible model

This is the first research instrument for the user's brief, not a finished or
calibrated replica of a manufactured cymbal. All sound comes from the compiled
Faust model; there are no recorded cymbal samples, noise layers or reverb.

## Play it

`bash scripts/build_cymbal.sh` builds the model and runs its render suite.
Dependencies are Faust, g++, Python, NumPy and SciPy. The original two-tone
smoke test and its `scripts/build.sh` remain unchanged.

The CI artifact contains `spatial-cymbal.dsp`, a self-contained generated source
that can be opened in the Faust IDE or compiled by a Faust host. The maintained
source is `dsp/cymbal.dsp` plus `scripts/generate_cymbal.py`; do not edit the
expanded generated kernel by hand.

Press **gate** to strike; release before another hit. Gate release does not
stop the existing tail. **clear** empties the resonator state; **choke** increases
mechanical damping. Begin at a low monitoring level. Audition WAVs are not
individually normalised.

## Controls

- **diameter_m:** 1 cm to 40 m, logarithmic. **thickness_mm** is independent;
  **proportional_thickness** instead scales thickness with diameter. The size
  may change during a decay. Geometry extremes are intentionally imaginary.
- **taper:** reduction from centre thickness to rim; **bell_diameter_ratio**,
  **bell_height_ratio**, **bow_height_ratio** control the shape independently.
  Bell height and bow height are relative to main radius; bell diameter ratio
  is relative to main diameter. Default: 44 cm diameter, 1.2 mm central thickness,
  0.48 mm nominal rim thickness, 12.3 cm bell diameter, 2.64 cm bell height.
- **strike_radius:** 0–1 from mounting point to edge; **strike_angle_deg** gives
  the full surface position. Radius below 0.035 is the clamped support, so a
  strike at zero is silent, not an artificial bell sample. Try 0.16/0.55/0.92.
- **velocity**, **beater** (wood/nylon/felt/rubber/steel), **beater_mass_g**,
  **tip_radius_mm**, **beater_hardness** control excitation.
- **material:** continuous bronze→steel→glass→wood morph. **stiffness_scale**,
  **density_scale**, **loss_scale** allow impossible materials. Wood also has
  **grain_anisotropy** and **grain_angle_deg**.
- **hammering** continuously increases a fixed deterministic imperfection
  pattern. **hammer_pattern** selects a pattern; use the amount, not the seed,
  for smooth automation. **nonlinearity** controls internal modal exchange.
- **gain_db** is a linear output gain, not a compressor or distortion stage.

## What is physically derived

The reference calculation uses a Rayleigh–Ritz approximation to bending energy
of a circular annular Kirchhoff–Love plate. Trial functions enforce displacement
and slope zero at a small central support; the outer boundary is free through
the variational formulation. Radial thickness varies linearly. The curvature
energy includes radial, hoop and twist terms. Eigenvectors are mass-normalised.
Both angular quadratures of non-axisymmetric modes are retained.

The engine retains 128 modes selected across the audible band, rather than all
modes below a cutoff. Strike coordinates sample those spatial shapes. A finite
tip footprint attenuates wavelengths short compared with its contact area.
The effective impact mass follows the modal mobility at that position, and an
approximate restitution collision law includes pre-impact surface velocity.
A Hertz-inspired contact-duration estimate shapes the impulse in time. Material
stiffness/density and thickness/radius affect resonance scales, not merely EQ.

## Explicit approximations and missing physics

1. **Bell/shape:** a positive local curvature foundation approximates shell
   stiffening; its in-plane relaxation factor is an uncalibrated modelling
   parameter. This is not an exact shallow-shell solve. Live geometry uses a
   positive Rayleigh-quotient frequency morph and radial coordinate warp of a
   fixed reference basis; it does not solve new eigenvectors at each setting.
2. **Crash nonlinearity:** two connected Cayley-rotation sweeps exchange energy
   among modal momenta, depending on displacement. Each rotation preserves
   quadratic state energy; damping reduces it. This provides amplitude-dependent
   nonlinear motion without feedback blow-up. It is a passive reduced-order
   surrogate, NOT the von Karman nonlinear coupling tensor or a validated
   simulation of wave turbulence. No claim that a stronger hit sounds like a
   real crash follows merely from a changed spectrum.
3. **Beater:** the incoming collision uses body velocity, but contact force is
   then a prescribed finite pulse. Continuous separation/re-contact and a
   dynamically deforming stick are not solved. Brushes, bows and scraping are
   not implemented. Beater and material properties are representative design
   assumptions, not measured specimens or guaranteed physical calibration.
4. **Hammering:** deterministic mode splitting and spatial asymmetry represent
   imperfection; dents, work hardening and residual manufacturing stress are
   not computed. Glass is unbreakable; wood uses a directional stiffness proxy,
   not an orthotropic shell. Material interpolation is a creative construction.
5. **Radiation:** two fixed weighted velocity pickups provide stereo. This is
   not a microphone, room, full acoustic-radiation or radiation-impedance solve.
6. **Changing object:** oscillator states are energy coordinates. Retuning
   preserves the represented quadratic vibration energy before damping, rather
   than pretending the expanding object has physically conserved momentum.
   This is an explicit musical morph convention. Moving strike position alone
   never crossfades or recolours an already-ringing tail.
7. **Bandwidth:** modes approaching Nyquist fade and damp rather than fold back
   into the audio band. Very tiny/huge objects can place most modes above/below
   hearing. Fixed mode count is NOT mesh convergence or timbral completeness.
   Nonlinear aliasing is reduced in previews by 96 kHz rendering followed by
   proper low-pass downsampling to 48 kHz. The DSP itself does not automatically
   oversample; live CPU, aliasing and iPad performance remain to be evaluated.

## Evidence and acceptance

`evidence/cymbal/results.json` records the exact tested commit, parameters,
triggers, runtime and unnormalised levels. The suite checks silence, support
position, repeatability, parameter validation, sample-rate and block-size
behaviour, nearby strike continuity, tail independence from beater movement,
clear/retriggering, contact softness, nonlinear ablation and extreme settings.
Those are numerical and behavioural checks, not a human listening assessment.

The developer and listener should first judge bell/bow/edge stick and felt
examples. Next compare size and material examples and the ringing resize.
Any change in fidelity requirements should be tested against real recordings
and/or a fuller nonlinear reference, not satisfied by renaming knobs.

## Research basis

- Nguyen & Touze (2019), *Nonlinear vibrations of thin plates with variable
  thickness: Application to sound synthesis of cymbals*, JASA 145, 977–988.
  DOI: https://doi.org/10.1121/1.5091013
- Authors' sound examples and discussion of taper and curvature:
  https://perso.ensta.fr/~touze/tapercymbals.html
- Faust syntax / recursive composition / tables:
  https://faustdoc.grame.fr/manual/syntax/

These motivate the model and the limitations; this code is not a reproduction
of the paper's full method. No private host source is included. Host integration
and real GUI screenshots require separate testing. Do not merge without approval.
