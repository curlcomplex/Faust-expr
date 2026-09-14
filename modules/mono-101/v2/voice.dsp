declare name "Analog Classics Mono 101";
declare version "0.2.0-reference-candidate";
declare description "One-note SH-101-inspired reference candidate; IR3109 path without Juno-style Q compensation; host-owned polyphony";
import("stdfaust.lib");
cs=library("common.lib");
gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",110,20,8000,.01);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
slide=checkbox("slide[curlop:input]");
glideTime=hslider("glideTime[unit:s]",.06,0,1,.001);
saw=hslider("saw",.75,0,1,.001):cs.sm(.005);
pulse=hslider("pulse",.25,0,1,.001):cs.sm(.005);
sub=hslider("sub",.35,0,1,.001):cs.sm(.005);
noise=hslider("noise",0,0,1,.001):cs.sm(.005);
pwm=hslider("pwm",.5,.05,.95,.001):cs.sm(.005);
pwmDepth=hslider("pwmDepth",0,0,.45,.001):cs.sm(.005);
cutoff=hslider("cutoff[unit:Hz][scale:log]",900,40,16000,1):log:cs.sm(.005):exp;
res=hslider("resonance",.30,0,.98,.001):cs.sm(.005);
envAmt=hslider("filterEnv",.5,0,1,.001):cs.sm(.005);
attack=hslider("attack[unit:s][scale:log]",.005,.001,2,.001):cs.sm(.005);
decay=hslider("decay[unit:s][scale:log]",.18,.005,3,.001):cs.sm(.005);
sustain=hslider("sustain",.45,0,1,.001):cs.sm(.005);
release=hslider("release[unit:s][scale:log]",.22,.005,4,.001):cs.sm(.005);
lfoRate=hslider("lfoRate[unit:Hz][scale:log]",5.2,.05,30,.01):cs.sm(.005);
lfoPitch=hslider("lfoPitch[unit:st]",0,0,1,.001):cs.sm(.005);
lfoFilter=hslider("lfoFilter",0,0,1,.001):cs.sm(.005);
level=hslider("level",.65,0,1,.001):cs.sm(.005);
state(f0,g0,v0,s0)=audio,pitchHz,env,hit with {
 pitchHz=cs.pitch(f0,g0,s0,glideTime); hit=(g0>0)>(g0'>0);
 lfo=os.osc(lfoRate);
 f=cs.clip(10,ma.SR*.20,pitchHz*pow(2,lfo*lfoPitch/12));
 width=cs.clip(.05,.95,pwm+pwmDepth*lfo);
 osc=.48*(os.polyblep_saw(f)*saw+os.pulsetrain(f,width)*pulse+os.polyblep_square(f*.5)*sub*.7+no.noise*noise*.15);
 env=en.adsr(max(.001,attack),max(.005,decay),sustain,max(.005,release),g0>0);
 fc=cutoff*pow(2,5*envAmt*env+2*lfoFilter*lfo);
 // SH-101 reference correction: unlike the Juno family implementation, the
 // service/circuit research does not support resonance/Q compensation here.
 // Keep the same four-pole primitive but remove the v1 output compensation.
 filtered=ma.tanh(osc*1.4):cs.ota4(fc,res,0.0);
 audio=(filtered:fi.dcblockerat(20))*env*cs.vel(g0,v0)*level*.8;
};
voice(f0,g0,v0,s0)=state(f0,g0,v0,s0):(_,!,!,!);
diagnostics(f0,g0,v0,s0)=state(f0,g0,v0,s0):(!,_,_,_);
process=voice(freq,gate,velocity,slide);
