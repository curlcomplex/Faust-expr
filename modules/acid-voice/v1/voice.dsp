declare name "Acid Voice";
declare version "0.1.0-experiment";
declare category "Analog Classics";
declare description "Single-note acid voice; host-owned polyphony; Open303-derived filter component.";
import("ui.lib");
process=ac.voice(freq,gate,velocity,accent,slide);
