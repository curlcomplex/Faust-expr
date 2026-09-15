declare name "VA Classics JP-8000 Supersaw Voice";
declare version "0.1.0-supersaw-port";
declare description "Playable first JP-8000 Supersaw voice; reverse-engineered detune/mix law, floating oscillator arithmetic";
import("stdfaust.lib");
ss = library("supersaw_core.lib");

gate = button("gate[curlop:input]");
freq = hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,8000,.01);
velocity = hslider("velocity[curlop:input]",1,0,1,.001);
detune = hslider("detune",.55,0,1,.001);
mix = hslider("mix",.75,0,1,.001);
attack = hslider("attack[unit:s][scale:log]",.008,.001,2,.001);
decay = hslider("decay[unit:s][scale:log]",.35,.005,4,.001);
sustain = hslider("sustain",.78,0,1,.001);
release = hslider("release[unit:s][scale:log]",.65,.005,6,.001);
level = hslider("level",.12,0,1,.001);

env = en.adsr(attack,decay,sustain,release,gate>0);
process = ss.supersaw(freq,detune,mix) * env * velocity * level : fi.dcblockerat(15);
