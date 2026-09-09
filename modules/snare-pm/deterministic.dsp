declare name "snare-pm-deterministic";
declare version "0.1.0-experiment";
e = library("engine.lib");
process = e.deterministic : e.finish;
