declare name "metal-pm";
declare version "0.2.0-experiment";
e=library("engine.lib");
// The recurrence envelope remains in engine.lib as a rejected optimization
// experiment. Full qualification showed ~2.1% audible-region error on a long
// 44.1 kHz decay, so the shipping candidate deliberately uses the direct
// exponential reference until a target-specific optimization proves equivalent.
process=e.finish(e.dense,e.ampReference);
