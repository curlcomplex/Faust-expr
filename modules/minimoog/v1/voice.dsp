declare name "Analog Classics Minimoog";
declare version "0.1.0-experiment";
declare description "Single-note Minimoog-style three-oscillator ladder-filter candidate; informed by Faug research";
import("stdfaust.lib");

gate=button("gate");
freq=hslider("freq[unit:Hz][scale:log]",110,20,5000,.01);
velocity=hslider("velocity",1,0,1,.001);

osc1=hslider("osc1",.8,0,1,.001):si.smooth(ba.tau2pole(.005));
osc2=hslider("osc2",.65,0,1,.001):si.smooth(ba.tau2pole(.005));
osc3=hslider("osc3",.4,0,1,.001):si.smooth(ba.tau2pole(.005));
detune2=hslider("detune2[unit:st]",0,-7,7,.01):si.smooth(ba.tau2pole(.005));
detune3=hslider("detune3[unit:st]",-12,-24,12,.01):si.smooth(ba.tau2pole(.005));
noise=hslider("noise",0,0,1,.001):si.smooth(ba.tau2pole(.005));
cutoff=hslider("cutoff[unit:Hz][scale:log]",1200,30,16000,1):si.smooth(ba.tau2pole(.005));
emphasis=hslider("emphasis",2.2,.707,12,.001):si.smooth(ba.tau2pole(.005));
contour=hslider("contour",.52,0,1,.001):si.smooth(ba.tau2pole(.005));
drive=hslider("drive",.22,0,1,.001):si.smooth(ba.tau2pole(.005));
attack=hslider("attack[unit:s][scale:log]",.008,.001,2,.001);
decay=hslider("decay[unit:s][scale:log]",.28,.005,4,.001);
sustain=hslider("sustain",.72,0,1,.001);
release=hslider("release[unit:s][scale:log]",.3,.005,5,.001);
level=hslider("level",.68,0,1,.001):si.smooth(ba.tau2pole(.005));

ratio(st)=pow(2,st/12);
f1=max(20,min(6000,freq));
f2=max(20,min(6000,freq*ratio(detune2)));
f3=max(20,min(6000,freq*ratio(detune3)));
raw=(os.polyblep_saw(f1)*osc1 + os.polyblep_saw(f2)*osc2 + os.polyblep_square(f3)*osc3 + no.noise*noise*.12)/max(.35,osc1+osc2+osc3+noise*.12);
env=en.adsr(attack,decay,sustain,release,gate);
pre=ma.tanh(raw*(1+6*drive));
fc=min(ma.SR*.45,max(25,cutoff*(1+7*contour*env)));
normalized=min(.98,max(.0001,fc/(ma.SR*.5)));
filtered=pre:ve.moogLadder(normalized,emphasis);
process=(filtered*env*velocity*level):fi.dcblockerat(20);
