declare name "Analog Open Hat";
declare author "curlcomplex";
declare category "Drums";
declare version "0.1.0-experiment";
declare description "One open-hat voice with an explicit local choke input; no internal note allocation.";
import("stdfaust.lib");
h = library("voice.lib");
choke = hslider("choke[col:2][row:1]",.78,0,1,.001);
chokeGate = button("chokeGate[curlop:input][tooltip:Choke this voice; the host routes cross-voice events]");
kill = chokeGate>chokeGate';
// A simultaneous external choke wins over an onset, as in the v2 baseline.
openHit = h.hit*(1-kill);
armed=(+(kill):min(1):*(1-openHit))~_;
strength=ba.sAndH(kill,choke);
rate=float(strength>0)/(.00025*pow(400,1-strength)*ma.SR);
progress=(+(armed*rate):min(32):*(1-openHit))~_;
killGain=exp(-progress)*float(progress<24);
process = h.voice(openHit,1,killGain);
