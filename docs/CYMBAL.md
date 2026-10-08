# Spatial Cymbal v0.1

A first research instrument, not a calibrated replica of a manufactured cymbal.
All sound comes from compiled Faust: no cymbal recordings, noise layers or reverb.

## Build and play

Run `bash scripts/build_cymbal.sh` with Faust, g++, Python, NumPy and SciPy.
CI produces individual comparison WAVs, `audition.wav`, its timeline, exact-commit
results and `spatial-cymbal.dsp`. That single generated DSP needs only the standard
Faust libraries at playback time. Both float and double C++ builds are tested.
The original two-tone toolchain test remains available via `scripts/build.sh`.

Use one persistent DSP instance. Press and release **gate** for each strike;
release does not stop the ringing. A new polyphonic instance per hit would be
several separate cymbals rather than repeated hits on one moving object.
**clear** empties state; **choke** damps it. Begin at low monitoring volume.
The WAVs use the same output gain, not individual loudness normalisation.

## Controls

- **diameter_m:** logarithmic 1 cm–40 m. **thickness_mm** is independent;
  **proportional_thickness** scales it along with diameter. Resize while ringing.
- **taper**, **bell_diameter_ratio**, **bell_height_ratio**, **bow_height_ratio**:
  geometry controls. Heights are fractions of main radius; bell diameter is a
  fraction of main diameter. Default: 44 cm diameter, 1.2 mm central thickness,
  0.48 mm rim, 12.3 cm bell diameter and 2.64 cm bell height.
- **strike_radius**, **strike_angle_deg**: position over the full surface.
  The central radius below 0.035 is clamped and silent. Try 0.16/0.55/0.92.
- **velocity**, **beater** (wood, nylon, felt, rubber, steel), **beater_mass_g**,
  **tip_radius_mm**, **beater_hardness**: impact properties.
- **material**: continuous 0 bronze → 1 steel → 2 glass → 3 wood. Also
  **stiffness_scale**, **density_scale**, **loss_scale**, **grain_anisotropy**
  and **grain_angle_deg** for wood and imaginary materials.
- **hammering**: intensity of a fixed imperfection pattern. **hammer_pattern**
  selects it; automate intensity rather than randomising the pattern each sample.
- **nonlinearity**: internal mode coupling. **gain_db**: linear output gain.

## Physics and explicit approximations

The reference is a Rayleigh–Ritz calculation for an annular Kirchhoff–Love plate
with radial thickness variation. Trial functions clamp displacement and slope
at a small central support; the outside edge is variationally free. Bending
energy includes radial, hoop and twist curvature. Modes are mass-normalised;
non-axisymmetric modes retain both angular quadratures.

128 modes are selected across the audible band, not every mode below a cutoff.
This is not a convergence claim. The reference frequencies span approximately
29 Hz–16 kHz at the reference geometry. Spatial excitation samples the mode
shapes; an approximate finite footprint suppresses short wavelengths. Effective
impact mass follows modal mobility. A restitution collision uses the incoming
beater speed relative to pre-impact body velocity. A Hertz-inspired contact
estimate sets pulse duration. Radius, thickness, stiffness and density affect
mechanical frequencies, not just output EQ.

Important limitations:

1. **Shape:** shell stiffening uses a positive curvature foundation and an
   uncalibrated relaxation factor, not a full shallow-shell solve. Geometry
   changes morph stiffness contributions and warp a fixed spatial basis;
   eigenvectors are not recomputed for each new object.
2. **Nonlinearity:** two connected Cayley-rotation sweeps mix modal momenta in
   response to displacement. Each rotation preserves quadratic state energy,
   and damping removes it. This is an amplitude-dependent passive surrogate,
   NOT the von Karman elastic coupling tensor or validated wave turbulence.
   It must be judged against real audio, not called realistic because a test passes.
3. **Contact:** after the initial collision, force is a prescribed finite pulse.
   Continuous stick deformation, separation and re-contact are not solved.
   No brushes, scraping or bowing. Material/beater constants are representative
   design assumptions, not measured specimens.
4. **Hammering/material:** mode splitting and asymmetry, not simulated dents,
   residual stress or work hardening. Glass does not break. Wood has a directional
   stiffness proxy, not a full orthotropic shell. Material interpolation is creative.
5. **Radiation:** two fixed weighted velocity pickups give stereo; no microphone,
   room, complete radiation or radiation-impedance simulation.
6. **Retuning:** energy-coordinate states preserve represented energy before
   damping as parameters change. This is an explicit musical convention, not
   conservation of the momentum of a magically growing object. Merely moving
   the beater does not crossfade an existing tail.
7. **Bandwidth/extremes:** near-Nyquist modes fade and damp. Auditions render at
   96 kHz then low-pass/downsample to 48 kHz; the DSP does not internally oversample.
   Tiny/huge objects can be ultrasonic/subsonic. Finite output at extremes does
   not validate thin-plate assumptions, ignored gravity or missing fracture.

## Tests and provenance

The build first runs model/analyser unit tests, then compiles the actual Faust.
`results.json` contains 44 rendered cases and their automated checks, including
silence, trigger/retrigger, support, deterministic repetition, block sizes,
sample rates, position changes, nonlinear ablation, softness and parameter
extremes. `precision.json` checks the float build and compares float/double
linear responses. Nonlinear tails are not expected to be phase-identical across
precisions. Compilation and stability do not establish perceptual fidelity.

The original one-second phase-correlation test for tiny changes in nonlinear
strike position failed. The revised oracle checks the first millisecond of the
nonlinear impact and the full linear response: it does not disguise divergent
nonlinear tails as phase-locked. This limitation remains explicit.

Host integration, actual GUI screenshots, long-duration automation stress,
aliasing convergence, and iPad/live CPU performance remain separate work.
No private CURLOP source is present. Keep this work on its draft PR until approved.

## Research context

- Nguyen & Touze (2019), *Nonlinear vibrations of thin plates with variable
  thickness: Application to sound synthesis of cymbals*, JASA 145, 977–988:
  https://doi.org/10.1121/1.5091013
- Author examples: https://perso.ensta.fr/~touze/tapercymbals.html
- Faust syntax: https://faustdoc.grame.fr/manual/syntax/

These motivate the experiment; this code does not reproduce the full paper.
