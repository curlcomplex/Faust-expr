# 909 oracle register

## Open source

Mutable Instruments Plaits, Emilie Gillet, MIT, `pichenettes/eurorack@08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4`:

- `plaits/dsp/drums/synthetic_bass_drum.h`, blob `4e982806ee582d770944d659d7d895c963b7d2af`. Source describes an inadvertently 909-ish model, not a faithful complete clone.
- `plaits/dsp/drums/synthetic_snare_drum.h`, blob `d6f7c1e151644c2a5552a899d64b833eaa034387`. Explicitly names 909 schematic-derived mode ratio 1.47, oscillator coupling and noise/filter/envelope behavior.
- Qualification compares TWO extracted C++ functions with Faust: snare waveshape and bassdrum VCA. Full oscillator coupling, filter/random state, envelopes and hardware are NOT compared by those function tests. Full Plaits voices were not ported.

André Michelle web TR-909, MIT code, `andremichelle/tr-909@11d423382d6d9705bd37a42b533e3b3c27442be7`: https://github.com/andremichelle/tr-909

README credits a borrowed DinSync RE-909. `typescript/audio/tr909/dsp/bassdrum.ts` (blob `80027333c375649f23cbe64f42f72a454adde2b3`) uses a 274-to-53 Hz trajectory, 60 ms plateau, recorded cycle and attack. Our kick uses generated waveform/VCA, not those assets or a literal full port. Whole-engine comparison was not executed. Code MIT does not by itself establish sample provenance.

## Future listening/control comparisons

D16 Drumazon 2 (https://d16.pl/drumazon2) and Roland TR-909 emulation (https://www.roland.com/global/products/rc_tr-909/) are possible commercial listening references, not source to copy or plugins this batch claims to own/run. Hexinverter Mutant BD9 (https://www.ericasynths.lv/hexinverter-mutant-bd9-3329/) provides an expanded-control hardware reference, not a Faust implementation.

## Four sample assets remain pending

Original 909 hats/crash/ride need selected samples or an explicitly agreed alternative. No generic 606-like metal synthesis is substituted.

- Freesound altemark/JGB set: original recorder Janne G:son Berg, cut/organized by altemark. Primary pages list CC BY 4.0: https://freesound.org/people/altemark/sounds/26527/ (ch08), https://freesound.org/people/altemark/sounds/26649/ (oh07), https://freesound.org/people/altemark/sounds/26658/ (ride01). Original downloads require login; no download was completed. Exact assets/licences/hashes required; no authentication bypass or preview called an original WAV.
- `oramics/sampled/DM/TR-909/Detroit` traces to F9 Audio material through API/Neve. Root says per-instrument licences; examined instrument README did not establish runtime redistribution permission. Not copied.
- Stargate sample pack declares CC0 Freesound data, but inspected cymbals were not verified 909 recordings. Not substituted.

The diagnostic table tests playback only. It is not one of these instruments and proves no CURLOP soundfile binding. Stop here on asset access rather than repeatedly searching. The seven synthesis voices remain useful for audition, with original/reference comparisons deferred rather than passed.
