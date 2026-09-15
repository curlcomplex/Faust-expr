declare name "Digital Classics CZ Phase Distortion Source";
declare version "0.1.0-source-checkpoint";
import("stdfaust.lib"); cz=library("pd_core.lib");
gate=button("gate[curlop:input]"); freq=hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,8000,.01); velocity=hslider("velocity[curlop:input]",1,0,1,.001); dcw=hslider("dcw",.55,0,1,.001); waveMorph=hslider("waveMorph",0,0,1,.001); attack=hslider("attack[unit:s]",.006,.001,2,.001); decay=hslider("decay[unit:s]",.35,.005,4,.001); sustain=hslider("sustain",.72,0,1,.001); release=hslider("release[unit:s]",.5,.005,6,.001); level=hslider("level",.3,0,1,.001);
env=en.adsr(attack,decay,sustain,release,gate>0);
process=cz.source(freq,dcw,waveMorph)*env*velocity*level:fi.dcblockerat(15);
