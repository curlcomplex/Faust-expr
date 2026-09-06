# Spatial cymbal: experimental reduced physical model

This branch is an implementation experiment, not a calibrated replica of any cymbal.

## Model

A 50 cm reference shell has a clamped mounting hole, a free outer rim, and thickness increasing from 1.2 mm at the rim toward its centre. A shallow bow plus a smooth bell supplies curvature. Rayleigh--Ritz integration assembles bending, membrane and mass matrices; in-plane displacement is statically condensed. Nine bell geometries are calculated, with 162 retained sine/cosine modes. Signed spatial shapes determine excitation and stereo velocity pickups; bell, bow and edge are parts of this same object.

The Faust engine interpolates aligned modal shapes and bending/membrane energy contributions. Diameter and thickness are independent: bending frequencies scale approximately as thickness/diameter^2, curvature-dominated frequencies as 1/diameter for proportionally scaled shape. Density changes modal mass. Material presets vary stiffness, density and loss. They are representative synthesis parameters, not certified constants for specific alloys or wood species.

A beater is a moving mass with unilateral spring contact, compression damping and a finite contact footprint. Contact force reacts to the instrument velocity/displacement. A trigger places a new beater at the current surface with velocity set by the velocity control. Repeated strikes share the existing vibrating state. Position is sampled at each strike, rather than moving an already-contacting mallet around the object.

A positive quartic potential in one projected slope coordinate couples modes. This is a deliberately low-rank approximation to geometric stretching, not the full von Karman nonlinear tensor. Implicit midpoint linear dynamics and discrete-gradient nonlinear/contact forces give a passive fixed-parameter update when the nonlinear solve converges. Tests observe residual and total retained mechanical energy. A prewarped bilinear frequency maps the linear modal frequency into the sampled system.

## Important limits

- Only a reduced subset of modes is retained; convergence to a full shell solution and matching a measured cymbal have NOT been demonstrated.
- Nine coarse bell geometries are interpolated; changing geometry does not solve a new full eigenproblem at audio rate. Large changes of thickness/diameter also use frozen modal energy fractions.
- Hammering splits resonances using a fixed perturbation pattern. It does not simulate dents, plastic deformation, residual stress or manufacturing.
- Glass-like and wood-like presets are unbreakable, effectively isotropic elastic surrogates. Wood grain and fracture are NOT modelled.
- The contact law is a unilateral linear penalty spring with damping, not a calibrated Hertz/felt compression law.
- The stereo output is two weighted velocity pickups, not a radiation/acoustic-room solver. No samples, reverb or noise layer are added.
- Size changes while ringing retain energy-coordinate state. This is a deliberate impossible-object morph, not a claim about real matter growing.
- Inaudibly high modes are faded and damped, not aliased or piled up at Nyquist. Extreme tiny/large dimensions can leave few audible modes.
- Initial compilation uses double-precision internal DSP. Real-time host, float32 and iPad performance require separate tests.

## Build

Install Faust, its C++ headers, g++, Python, NumPy and SciPy. Run `python3 -m unittest discover -s tests -v`, then `bash scripts/build_cymbal.sh`. The generator writes `build/cymbal/cymbal-standalone.dsp`; that self-contained file imports only standard Faust libraries and is the source actually compiled. The original toolchain probe remains independent.

The bounded public CI workflow produces exact-commit source, WAVs, raw samples and JSON. No per-file normalisation is used for pass/fail tests. No private host code or API/model invocation is involved. A successful numerical test is not a listening verdict.

## Research context

Nguyen & Touze (2019), Nonlinear vibrations of thin plates with variable thickness: Application to sound synthesis of cymbals, JASA 145, 977-988, DOI 10.1121/1.5091013, motivates the importance of taper, shape and nonlinear dynamics. This implementation is a reduced experiment, not a reproduction or validation of their published solver.

https://doi.org/10.1121/1.5091013
https://faustdoc.grame.fr/manual/syntax/
