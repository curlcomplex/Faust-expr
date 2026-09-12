declare name "Analog Closed Hat";
declare author "curlcomplex";
declare category "Drums";
declare version "0.1.0-experiment";
declare description "One closed-hat voice; polyphony belongs to the host. Adapted from the v2 808-reference candidate.";
h = library("voice.lib");
process = h.voice(h.hit,0,1);
