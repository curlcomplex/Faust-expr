declare name "Analog Classics Juno 106 Voice";
declare version "0.1.0-experiment";
declare description "Single-note Juno-106-inspired DCO voice; chorus and polyphony live outside the voice";
import("stdfaust.lib");

gate=button("gate");
freq=hslider("freq[unit:Hz][scale:log]",220,20,8000,.01);
velocity=hslider("velocity",1,0,1,.001);

saw=hslider("saw",.7,0,1,.001):si.smooth(ba.tau2pole(.005));
pulse=hslider("pulse",.35,0,1,.001):si.smooth(ba.tau2pole(.005));
sub=hslider("sub",.45,0,1,.001):si.smooth(ba.tau2pole(.005));
noise=hslider("noise",.02,0,1,.001):si.smooth(ba.tau2pole(.005));
pwm=hslider("pwm",.48,.05,.95,.001):si.smooth(ba.tau2pole(.005));
pwmDepth=hslider("pwmDepth",.18,0,.45,.001):si.smooth(ba.tau2pole(.005));
lfoRate=hslider("lfoRate[unit:Hz][scale:log]",4.8,.05,20,.01):si.smooth(ba.tau2pole(.005));
cutoff=hslider("cutoff[unit:Hz][scale:log]",2400,40,16000,1):si.smooth(ba.tau2pole(.005));
res=hslider("resonance",2.7,.5,12,.01):si.smooth(ba.tau2pole(.005));
hpf=hslider("hpf[unit:Hz][scale:log]",70,20,1200,1):si.smooth(ba.tau2pole(.005));
envAmt=hslider("filterEnv",.35,0,1,.001):si.smooth(ba.tau2pole(.005));
attack=hslider("attack[unit:s][scale:log]",.015,.001,3,.001);
decay=hslider("decay[unit:s][scale:log]",.32,.005,4,.001);
sustain=hslider("sustain",.72,0,1,.001);
release=hslider("release[unit:s][scale:log]",.55,.005,6,.001);
level=hslider("level",.68,0,1,.001):si.smooth(ba.tau2pole(.005));

lfo=os.osc(lfoRate);
width=min(.95,max(.05,pwm+pwmDepth*lfo));
f=max(20,min(8000,freq));
dco=os.polyblep_saw(f)*saw + os.pulsetrain(f,width)*pulse + os.polyblep_square(f*.5)*sub*.65 + no.noise*noise*.12;
env=en.adsr(attack,decay,sustain,release,gate);
fc=min(18000,max(40,cutoff*(1+5.5*envAmt*env)));
voice=(dco:fi.resonlp(fc,res,1):fi.highpass(1,hpf))*env*velocity*level:fi.dcblockerat(20);
process=voice;
