# Consolidated percussion selection

Owning work: Faust-expr #70 / PR #71; CURLOP #279 / pack parent #271.
This is a **laboratory selection manifest**, not the CURLOP UUID/release schema,
a completed faceplate, automatic migration, or factory-promotion instruction.

## One source per useful instrument

- `drums-606/v1/tom.dsp`: one 606 Tom, range union of old low/high entries.
- `drums-909/v1/tom.dsp`: one 909 Tom, range union of old low/mid/high entries.
- `drums-808-aux/v1/tom-conga.dsp`: one 808 Tom/Conga, six named starting presets.
- `drums-808-aux/v2/rim-claves.dsp`: one 808 Rim/Claves, version 0.1.1.
- `drums-808-aux/v1/maracas.dsp` and `cowbell.dsp`: retained candidates.

The five old 606/909 tom files and all four initial 808 auxiliary sources remain
unchanged for explicit old-version recall. New projects should eventually discover
one tom entry per machine family and use presets/instances for low/mid/high parts.
No internal voice bank, pitch-slot selector or note allocator was added. Every
kernel still receives `gate`, `freq` in Hz, and `velocity`; accent is an explicitly
wired custom input. CURLOP owns allocation and presentation.

The manifest contains all 20 previously auditioned 606/909 slot presets with their
full controls, plus the 18 auxiliary presets. The unified ranges are 40–500 Hz /
0.025–1.8 s for 606 and 45–480 Hz / 0.05–2 s for 909. No automatic project conversion
is installed: the tests prove source/settings equivalence, not host persistence.

## A real input-contract bug fixed without erasing the baseline

The v1 Rim/Claves source hard-coded the Claves resonator to 2500 Hz, so its `freq`
control and the 1900-Hz Wood preset did not actually tune Claves. The v2 entry uses
received Hz in that mode. Rim processing and the Claves 2500-Hz anchor are unchanged.
V1 is retained; this intentional sound correction has a distinct source/version.
`crack` is rim-only. There is no claim that every control acts in every mode.

## Qualification

Run `python3 tools/modules/percussion_finish_batch.py --out <directory>` using the
existing Faust/C++ toolchain and renderer. The existing 808 auxiliary suite also
runs unchanged in CI. The supplementary suite compares each old tom default at
44.1/48/96 kHz with live edits and retriggers, every previous preset, range unions,
scalar/vector cases, tails and extremes. Strict equality is required for the legacy
tom comparisons. A 0.99-gain compiled mutation must fail that equality predicate.

Claves is separately tested at 1500/2500/3000 Hz. The old source must reproduce its
fixed-pitch defect, and the new source must follow Hz. Rim/Claves retained anchors,
onset-latching and scalar/vector/rate cases are checked independently. Auditions
render the selected sound versions, not an earlier file with a new label. The dry
four-part groove is driven by actual Faust Trigger Seq outputs into persistent
instances; fixed summing gain is 0.5, no external effects or normalization.

See generated `percussion-finish/report.json` for executed outcomes and exact
identities. Test intentions in this document are not a pass result.

## Reference and release boundary

The 808 source/oracle register remains `drums-808-aux/v1/ORACLES.md`: pinned TapTools
code and identified recording leads are comparison targets, not completed matches.
Tom/Conga presets change tuning/decay/noise; the current mode only suppresses the
noise layer, not a complete emulation of the analogue switching circuit. Maracas
attack shape, cowbell free-running oscillator/VCA/filter behaviour and aliasing,
and all hardware parameter calibration remain part of the reference pass.

Additional primary design context reviewed for this follow-up:
- Roland instrument/control list: https://support.roland.com/hc/en-us/articles/201963539-TR-808-Technical-Specifications
- Circuit-analysis author, Tom/Conga: https://www.baratatronix.com/blog/808-tom-synthesis and https://www.baratatronix.com/blog/808-conga-synthesis
- Rim and Maracas: https://www.baratatronix.com/blog/808-rimshot and https://www.baratatronix.com/blog/808-maracas-synthesis
- Extended cowbell controls by its manufacturer: https://www.ericasynths.lv/news/cowbell-black-multi

Those descriptions informed the audit, not fresh code copying or audio matching.
The service-manual PDF rendering endpoint failed in this follow-up; no new schematic
inspection is claimed. The local compiler-install attempt timed out and was not
repeated; the authorised hosted Faust-expr workflow supplies compile/render evidence.

Hardware/oracle A/B, listening approval, authentic classic anchors, real CURLOP
faceplates/typed binding/save-reopen, device qualification and final catalogue
selection remain open. Preserve all versions and durable evidence. No merge or
release is authorised by this change.
