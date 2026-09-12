# Analog Classics effects batch v1

Experimental standalone Faust candidates for CURLOP Analog Classics. None is hardware-, host-, device- or release-approved yet.

## Retro Mixer EQ

Primary oracle/source lead: Airwindows `MackEQ`, repository `airwindows/airwindows`, `plugins/WinVST/MackEQ/MackEQProc.cpp` and `MackEQ.cpp`. Airwindows repository licence: MIT, copyright Chris Johnson. The Faust candidate independently simplifies/translates the useful signal topology (input coloration, broad low/mid/high split, nonlinear low/high shaping and output coloration); it does not include VST host code, dither/noise injection, or claim sample-identical behaviour.

## Vintage Rack Reverb

Character oracle/source lead: Airwindows `MV` / `MV2`, MIT, described by its author as a modern reverb built around early Alesis MIDIVerb-like 0.5 allpass and bit-shift regeneration ideas. The v1 Faust candidate currently uses a compact standard Faust Freeverb-family network with a 0.5 allpass coefficient as a temporary listening/behaviour candidate. It does **not** contain MIDIVerb ROM code and is not an Alesis emulation. GPL `jpverb` / `kb_rom_rev1` code is deliberately not used in this production candidate.

## Tape Echo

Oracle/source lead: Airwindows `TapeDelay`, MIT, described as an old-school tape echo with pitch-swerve and feedback tone shaping. The Faust candidate is an independent multi-head fractional-delay design with feedback filtering/saturation and wow/flutter. It is not a Roland Space Echo circuit/impulse/algorithm clone.

## Shared

All three are tested through the repository's existing hosted Faust/C++ renderer. Hardware reference recordings and final listening approval remain explicit later gates. Preserve upstream attribution when adapting any additional code/equations.
