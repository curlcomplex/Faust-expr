// Diagnostic only; no audio oscillator. Both envelopes independently tested.
e=library("engine.lib");
process=e.bodyEnv*e.seen,e.noiseEnv*e.seen;
