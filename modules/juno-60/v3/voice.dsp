declare name "Analog Classics Juno 60 Voice";
declare version "0.3.0-coherent-candidate";
declare description "Independent single-note Juno-60-inspired voice; chorus and host polyphony are external";
import("stdfaust.lib");
cdco=library("../../analog-classics/synth-finish/coherent_dco.lib");
cs=library("../../analog-classics/synth-batch/common.lib");

gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,8000,.01);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);

saw=hslider("saw",.72,0,1,.001):cs.sm(.004);
pulse=hslider("pulse",.32,0,1,.001):cs.sm(.004);
sub=hslider("sub",.48,0,1,.001):cs.sm(.004);
noise=hslider("noise",.015,0,1,.001):cs.sm(.004);
pwm=hslider("pwm",.50,.05,.95,.001):cs.sm(.004);
pwmDepth=hslider("pwmDepth",.14,0,.45,.001):cs.sm(.004);
lfoRate=hslider("lfoRate[unit:Hz][scale:log]",4.5,.05,20,.01):cs.sm(.004);

cutoff=hslider("cutoff[unit:Hz][scale:log]",1850,40,16000,1):log:cs.sm(.004):exp;
res=hslider("resonance",.22,0,.98,.001):cs.sm(.004);
hpf=hslider("hpf[unit:Hz][scale:log]",30,20,1200,1):log:cs.sm(.004):exp;
envAmt=hslider("filterEnv",.38,0,1,.001):cs.sm(.004);

// Juno-60 anchor intentionally permits faster envelope action than the 106 candidate.
attack=hslider("attack[unit:s][scale:log]",.006,.0005,3,.0005):cs.sm(.003);
decay=hslider("decay[unit:s][scale:log]",.26,.003,4,.001):cs.sm(.003);
sustain=hslider("sustain",.70,0,1,.001):cs.sm(.003);
release=hslider("release[unit:s][scale:log]",.42,.003,6,.001):cs.sm(.003);
level=hslider("level",.65,0,1,.001):cs.sm(.004);

state(f0,g0,v0)=audio,pitchHz,env,hit with {
 pitchHz=cs.pitch(f0,g0,0,0); hit=(g0>0)>(g0'>0);
 width=cs.clip(.05,.95,pwm+pwmDepth*os.osc(lfoRate));
 f=cs.clip(10,ma.SR*.20,pitchHz);

 // Independent DCO/mixer hypothesis informed by Juno hardware/software references,
 // not copied from GPL Hera source. Mixer loading compresses as several sources rise.
 raw=(cdco.waves(f,width):(_,!,!))*saw + (cdco.waves(f,width):(!,_,!))*pulse + (cdco.waves(f,width):(!,!,_))*sub*.72 + no.pink_noise*noise*.10;
 load=max(.30,saw+pulse+sub+noise);
 mixGain=.46/(.46+.22*max(0,load-.46));
 dco=.40*raw*mixGain;

 env=en.adsr(max(.0005,attack),max(.003,decay),sustain,max(.003,release),g0>0);
 fc=cutoff*pow(2,4.7*envAmt*env);
 filtered=dco:cs.ota4(fc,res,.08):fi.highpass(1,max(20,hpf));
 audio=(ma.tanh(filtered*1.25):fi.dcblockerat(20))*env*cs.vel(g0,v0)*level*.82;
};
voice(f0,g0,v0)=state(f0,g0,v0):(_,!,!,!);
diagnostics(f0,g0,v0)=state(f0,g0,v0):(!,_,_,_);
process=voice(freq,gate,velocity);
