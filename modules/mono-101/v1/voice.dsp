declare name "Analog Classics Mono 101";
declare version "0.1.0-experiment";
declare description "Single-note SH-101-inspired Faust voice; CURLOP host owns polyphony";
import("stdfaust.lib");

gate=button("gate");
freq=hslider("freq[unit:Hz][scale:log]",110,20,8000,.01);
velocity=hslider("velocity",1,0,1,.001);

saw=hslider("saw",.75,0,1,.001):si.smooth(ba.tau2pole(.005));
pulse=hslider("pulse",.25,0,1,.001):si.smooth(ba.tau2pole(.005));
sub=hslider("sub",.35,0,1,.001):si.smooth(ba.tau2pole(.005));
noise=hslider("noise",0,0,1,.001):si.smooth(ba.tau2pole(.005));
pwm=hslider("pwm",.5,.05,.95,.001):si.smooth(ba.tau2pole(.005));
cutoff=hslider("cutoff[unit:Hz][scale:log]",1800,40,16000,1):si.smooth(ba.tau2pole(.005));
res=hslider("resonance",3.2,.5,14,.01):si.smooth(ba.tau2pole(.005));
envAmt=hslider("filterEnv",.45,0,1,.001):si.smooth(ba.tau2pole(.005));
attack=hslider("attack[unit:s][scale:log]",.005,.001,2,.001);
decay=hslider("decay[unit:s][scale:log]",.18,.005,3,.001);
sustain=hslider("sustain",.65,0,1,.001);
release=hslider("release[unit:s][scale:log]",.22,.005,4,.001);
lfoRate=hslider("lfoRate[unit:Hz][scale:log]",5.2,.05,30,.01):si.smooth(ba.tau2pole(.005));
lfoPitch=hslider("lfoPitch",0,0,.06,.0001):si.smooth(ba.tau2pole(.005));
lfoFilter=hslider("lfoFilter",0,0,1,.001):si.smooth(ba.tau2pole(.005));
level=hslider("level",.72,0,1,.001):si.smooth(ba.tau2pole(.005));

lfo=os.osc(lfoRate);
f=max(20,min(8000,freq*(1+lfo*lfoPitch)));
osc=os.polyblep_saw(f)*saw + os.pulsetrain(f,pwm)*pulse + os.polyblep_square(f*.5)*sub*.7 + no.noise*noise*.15;
env=en.adsr(attack,decay,sustain,release,gate);
fc=min(18000,max(35,cutoff*(1+6*envAmt*env+1.5*lfoFilter*lfo)));
voice=(osc:fi.resonlp(fc,res,1))*env*velocity*level:fi.dcblockerat(20);
process=voice;
