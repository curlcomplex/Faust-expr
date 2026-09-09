// Sample-rate diagnostic only: suppress ALL pseudo-noise, including crack.
// Not a shipping preset or substitute for evaluating the real noisy voice.
declare name "snare-pm tonal diagnostic";
e = library("engine.lib")[noise = 0.0;];
process = e.hybrid : e.finish;
