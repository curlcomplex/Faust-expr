declare name "Analog Classics Juno 60 Voice";
declare version "0.1.0-reference-candidate";
declare description "Single-note Juno-60-inspired DCO/VCF voice; host owns polyphony and chorus is external";

import("stdfaust.lib");
vcf = library("vcf.lib");

clip(lo,hi,x)=min(hi,max(lo,x));
sm(t,x)=x:si.smooth(exp(-1/(max(.00001,t)*ma.SR)));

// CURLOP host contract
gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,8000,.01);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);

// DCO / mixer. This is an original clean-room expression informed by measured/model
// architecture, not copied from Hera's GPL DCO source.
saw=hslider("saw",.70,0,1,.001):sm(.005);
pulse=hslider("pulse",.30,0,1,.001):sm(.005);
sub=hslider("sub",.45,0,1,.001):sm(.005);
noise=hslider("noise",.015,0,1,.001):sm(.005);
pwm=hslider("pwm",.50,.05,.95,.001):sm(.005);
pwmDepth=hslider("pwmDepth",.16,0,.45,.001):sm(.005);
lfoRate=hslider("lfoRate[unit:Hz][scale:log]",4.8,.05,20,.01):sm(.005);

// Juno-family filter / HPF
cutoff=hslider("cutoff[unit:Hz][scale:log]",1800,30,16000,1):log:sm(.005):exp;
res=hslider("resonance",.22,0,.98,.001):sm(.005);
hpf=hslider("hpf[unit:Hz][scale:log]",25,20,1200,1):log:sm(.005):exp;
envAmt=hslider("filterEnv",.38,0,1,.001):sm(.005);
keyTrack=hslider("keyTrack",.35,0,1,.001):sm(.005);

// One shared ADSR for the first candidate; separate contour behavior remains a tuning gate.
attack=hslider("attack[unit:s][scale:log]",.012,.001,3,.001):sm(.005);
decay=hslider("decay[unit:s][scale:log]",.30,.005,4,.001):sm(.005);
sustain=hslider("sustain",.72,0,1,.001):sm(.005);
release=hslider("release[unit:s][scale:log]",.50,.005,6,.001):sm(.005);
level=hslider("level",.70,0,1,.001):sm(.005);

// Capture note pitch/velocity at onset while allowing the sound-shaping controls to remain live.
pitchHz=ba.sAndH(gate>0,clip(20,8000,freq));
vel=ba.sAndH((gate>0)>(gate'>0),clip(0,1,velocity));
env=en.adsr(max(.001,attack),max(.005,decay),sustain,max(.005,release),gate>0);

width=clip(.05,.95,pwm+pwmDepth*os.osc(lfoRate));
f=clip(10,ma.SR*.20,pitchHz);

// Independent waveform generators, with a gentle level-dependent mixer compression.
// The exact gain curve is intentionally a hypothesis to be tuned against hardware samples.
rawSaw=os.polyblep_saw(f)*saw;
rawPulse=os.pulsetrain(f,width)*pulse;
rawSub=os.polyblep_square(f*.5)*sub*.72;
rawNoise=no.noise*noise*.10;
sumLevel=saw+pulse+sub+noise;
mixComp=1/(1+.55*max(0,sumLevel-1));
dco=(rawSaw+rawPulse+rawSub+rawNoise)*.32*mixComp;

trackedCutoff=cutoff*pow(max(.25,f/261.6256),keyTrack);
fc=clip(15,ma.SR*.35,trackedCutoff*pow(2,4.5*envAmt*env));
filtered=dco:vcf.j60_vcf(fc,res):fi.highpass(1,max(20,hpf));

// No per-voice chorus: CURLOP can place a shared Juno chorus after host polyphony.
process=(ma.tanh(filtered*1.15):fi.dcblockerat(20))*env*vel*level;
