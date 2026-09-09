# CYM-ID materials: property-informed dense-model extension

Part of Cymbal lab #6. This is the reusable material/choke kernel of the preserved 4,207-component measured-modal model, NOT a replacement for CYM-A (#2) or CYM-B (#4). No engine has been merged or declared the winner.

## What is in this PR

- Real Faust material-transform kernel with 12 named presets and state-damping grip/choke.
- Generator for the 32-mode banking wrapper.
- Synthetic single-resonance compile/output fixture. CI results for that fixture must NOT be described as a full cymbal render.

The complete fitted mode CSV, native multi-bank renderer, exact performance scores, original baseline, 17-method regression suite, raw audio and all four listening demos are in the user-delivered `cymbal-materials-01` review/source packages. This small repo export does NOT yet publish that complete dataset/host. It is not a standalone ready-to-play 4,207-mode instrument. The wrapper's excitation coefficients default to zero until loaded by a host.

## Controls

`material_a`, `material_b`, `material_mix`, `material_depth`, `pitch_lock`, `intrinsic_loss_scale`, `thermal_thickness_mm`, `grip`, `grip_strength`. Existing gate/clear, velocity, bell/body excitation interpolation, frequency_scale and loss_scale remain.

Material mix interpolates properties inside persistent resonators, not rendered audio. `pitch_lock` cancels global elastic-frequency scaling while retaining changed losses; this is deliberate comparison/artistic compensation. `thermal_thickness_mm` affects only a thermal relaxation approximation, NOT full body thickness or geometry. Grip dissipates internal state; release cannot restore the removed tail.

## Named materials and sourcing

IDs 0-11: original/nominal bronze reference, pure gold, pure silver, titanium, carbon steel, aluminium, C11000 copper, C26000 brass, fused-silica glass, borosilicate glass, acrylic PMMA, effective wood-like surrogate.

Reference bronze E/rho/thermal constants are assumptions; the recording's alloy is unknown. ALL structural loss factors are unmeasured sound-design inputs: 0.00035 for every metal including reference, 0.00005 silica, 0.0002 borosilicate, 0.02 PMMA, 0.018 effective wood. The metals have not been assigned arbitrary distinct damping solely to make their names sound different.

Sources:
- Gold: https://deringerney.com/products/precious-metal-alloys/pure-gold/
- Silver: https://smp.es/en/products/silver/ (E, rho, conductivity; nu, cp and expansion assumed)
- Titanium: https://kyocera-sgstool.co.uk/titanium-resources/titanium-information-everything-you-need-to-know/titanium-properties/
- Steel/aluminium: https://www.timate.com.tw/physical-characteristics/ (nu assumed; cal thermal units converted using 4.184)
- Copper/brass: https://alloys.copper.org/alloy/C11000 and https://alloys.copper.org/alloy/C26000 (US units converted to SI; nu derived from E/G)
- Silica: https://www.eot.it/engpage/doctecnica/Materiali/quarzo/quarzosil.html
- Borosilicate: https://www.schott.com/en-gb/products/borofloat/technical-details
- PMMA: https://www.asahi-kasei.co.jp/delpet/en/products/normal.html (80NH E/rho only; remaining inputs assumed)
- Thermoelastic approximation: https://arxiv.org/html/cond-mat/9909271

Wood is deliberately a generic effective isotropic surrogate, with all inputs assumed. No wood species or grain was recovered from the fitted modes. Glass/polymer fracture and metal yielding are not simulated.

## Mechanism and limitations

Fixed-shape isotropic bending ratio: sqrt((E/E0)*(rho0/rho)*(1-nu0^2)/(1-nu^2)). This is uniform scaling, not new spatial mode shapes.

Zener single-relaxation thermoelastic damping plus pi*f*eta changes a bounded part of the measured pole decay. Temperature 293.15 K and thermal length 1.2 mm are assumptions. At least 35% of each empirical decay is retained as unidentified residual. This decomposition is unmeasured, and the thermal law is not a solved cymbal heat/elasticity PDE.

All materials use the SAME fitted excitations. No independent material-specific recordings or fitting were used. Absolute impact/mass/loudness changes, nonlinear crashes, new beaters, spatial choke, independent bell/body geometry and efficiency reduction remain absent. These are source-informed candidate transformations, NOT validated predictions of real gold/silver/titanium/wood cymbals.

## Executed local evidence

Full-bank original soft, hard and bell checks reproduce one second each bit for bit. The full coefficient CSV remains SHA256 a789d6a07326e68c74a1b489feae74c75b1d1275dfbd81c58b144b97230eaa47.

17 local test methods passed, mixing full-bank identity tests with explicit synthetic fixtures for material scaling, damping, zero depth, endpoint consistency, grip release/restrike, silence, clear, repeatability, block independence and selected rates/extremes. Four complete actual-Faust full-bank recordings passed finite-output checks: 96s native-material phrase suite, 96s pitch-held suite, 40s live material changes, 24s grip interactions. Unchanged original source and fitted data were preserved. No audio-reference file was loaded by the synthesis path.

Local standalone source SHA256: 1178ff9aea441a5be3786d5967887a4a11112fc833c92fec147abcfdf098d6d5. The wrapper generated here differs only in a comment. No hosted full-bank rendering result is claimed by this PR.

## Fixture build

Run in this directory with Faust, g++, and Faust headers installed:

```
faust -double -cn MaterialProbe -I . smoke.dsp -o probe.hpp
g++ -std=c++17 -O2 smoke.cpp -o smoke
./smoke
python3 generate_wrapper.py materials-engine.lib material-bank.dsp
faust -double -cn ModalBank material-bank.dsp -o modal_bank.hpp
```

The fixture is NOT the listening demo. Full rendering instructions are in the delivered kit. No private repository content, model/API calls, paid runner or merge.
