// Controlled ablation: keep hybrid body/crack/drive unchanged, remove only tail.
// The legacy deterministic.dsp also changes crack gain and is NOT this ablation.
declare name "snare-pm isolated tail ablation";
e = library("engine.lib")[noiseSignal = 0.0;];
process = e.hybrid : e.finish;
