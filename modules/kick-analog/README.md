# Analog-style kick — full batch 01

This is the second shared-library instrument experiment from #15/#35. It is a **separate engine** from `kick-pm`, not a PM preset with modulation reduced.

## What this batch actually establishes

The current source is an authored analog-style behavioural model: a swept resonant body, independent transient excitation, bounded nonlinear coloration, fixed raw level, and trigger-only articulation. Syntakt BD Sharp remains a reference lead only. No private capture, firmware, schematic, component values, or recovered Elektron control curves are used here, so this is not described as a clone or circuit reconstruction.

`candidates/body-01.dsp` is the deliberately clean baseline. It isolates pitch fall, attack and body decay before harmonic loading, filtering, click or drive. `kick.dsp` retains that time-domain core and adds only bounded layers that can be ablated independently. A WDF/component-level candidate is intentionally **not** fabricated without measurements capable of distinguishing it.

## Playable controls

Pitch is 20–160 Hz. Decay spans the body time constant from short thump to long ring. Sweep controls initial pitch excursion up to 3.25 octaves. Punch jointly shortens pitch-fall and amplitude attack. Tone sets the body/click spectral ceiling. Body adds bounded second-harmonic and sub-weighting. Drive applies normalized tanh coloration with a squared perceptual response. Click mixes a short noise/sine excitation path. Gate and velocity remain separate events; velocity is latched at onset and gate-off does not truncate the kick.

Four audition anchors are provided: Round, Sharp, Deep and Driven. They are authored coverage points, not fitted hardware presets.

## Optimization decision

The optimized candidate deliberately avoids per-voice oversampling, iterative nonlinear solvers, generic ladder filters, and WDF circuitry. Those costs would only be justified by evidence that the cheaper behavioural structure cannot reproduce the reference. The nonlinear stage is bounded; DC removal follows the asymmetric coloration; time constants are expressed in seconds; and all oscillator/reset state is fixed-size. There is no allocation, file I/O or dynamic topology in DSP.

Optimization is checked against the clean-body baseline as **cost evidence**, not as a claim that lower CPU equals better sound. The hosted offline timing ratio is recorded in the report. Target-device realtime qualification remains separate.

## Automated verification

`tools/modules/analog_kick_probe.py` compiles the actual Faust source in scalar and vector modes, plus the baseline. It verifies the declared control surface, 44.1/48/96 kHz behavior, block sizes 1/32/127/128/512, scalar/vector agreement, velocity law, trigger-only gate release, 128 endpoint combinations across the seven non-pitch controls, retrigger/lock stress, DC/finite/peak bounds, a 48/96 kHz high-frequency diagnostic, and instrumented compute cost. It also emits a six-second four-anchor listening WAV and immutable raw/score hashes.

The high-frequency comparison is only an aliasing **diagnostic**. It is not proof that the nonlinear stage is alias-free, and the project does not hide that limitation by enabling blanket oversampling.

## Remaining acceptance gates

This batch can establish numerical safety, deterministic host behavior and a coherent playable proposal. It cannot establish BD Sharp fidelity without controlled hardware references, and it cannot establish musical acceptance without Felix listening to the dry hits/pattern. It also does not establish iPhone/Tracker realtime cost. Those remain explicit later gates rather than reasons to leave the public engine work fragmented across many turns.
