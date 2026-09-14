declare name "Analog Classics Juno 106 Voice";
declare version "0.3.0-hardware-reference-candidate";
declare description "Single-note voice with independently gated VCA and triangle VCF LFO; hardware-reference candidate, chorus external";
import("stdfaust.lib");
cs=library("common.lib");
gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,8000,.01);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
saw=hslider("saw",.7,0,1,.001):cs.sm(.005);
pulse=hslider("pulse",.35,0,1,.001):cs.sm(.005);
sub=hslider("sub",.45,0,1,.001):cs.sm(.005);
noise=hslider("noise",.02,0,1,.001):cs.sm(.005);
pwm=hslider("pwm",.48,.05,.95,.001):cs.sm(.005);
pwmDepth=hslider("pwmDepth",.18,0,.45,.001):cs.sm(.005);
lfoRate=hslider("lfoRate[unit:Hz][scale:log]",4.8,.05,20,.01):cs.sm(.005);
cutoff=hslider("cutoff[unit:Hz][scale:log]",2000,40,16000,1):log:cs.sm(.005):exp;
res=hslider("resonance",.24,0,.98,.001):cs.sm(.005);
hpf=hslider("hpf[unit:Hz][scale:log]",35,20,1200,1):log:cs.sm(.005):exp;
envAmt=hslider("filterEnv",.35,0,1,.001):cs.sm(.005);
attack=hslider("attack[unit:s][scale:log]",.015,.001,3,.001):cs.sm(.005);
decay=hslider("decay[unit:s][scale:log]",.32,.005,4,.001):cs.sm(.005);
sustain=hslider("sustain",.72,0,1,.001):cs.sm(.005);
release=hslider("release[unit:s][scale:log]",.55,.005,6,.001):cs.sm(.005);
level=hslider("level",.65,0,1,.001):cs.sm(.005);
// Roland's hardware specifications explicitly list VCA ENV/GATE and VCF LFO.
// The Soundwave A47 chorus-off capture has a repeating resonant sweep absent
// from v1/v2. Depth law and gate slew below are testable model hypotheses.
vcaGate=checkbox("vcaGate");
filterLfo=hslider("filterLfo[unit:oct]",0,0,4,.001):cs.sm(.005);
// Laboratory phase alignment for an unknown free-running capture, not a claim
// that the physical front panel has a phase knob. Not reset on every note.
filterLfoPhase=hslider("filterLfoPhase[unit:cycle]",0,0,1,.001);
state(f0,g0,v0)=audio,pitchHz,env,hit with {
 pitchHz=cs.pitch(f0,g0,0,0); hit=(g0>0)>(g0'>0);
 width=cs.clip(.05,.95,pwm+pwmDepth*os.osc(lfoRate));
 f=cs.clip(10,ma.SR*.20,pitchHz);
 dco=.42*(os.polyblep_saw(f)*saw+os.pulsetrain(f,width)*pulse+os.polyblep_square(f*.5)*sub*.65+no.noise*noise*.12);
 env=en.adsr(max(.001,attack),max(.005,decay),sustain,max(.005,release),g0>0);
 phase0=os.phasor(1,lfoRate)+filterLfoPhase;
 phase=phase0-floor(phase0);
 triangle=1-4*abs(phase-.5);
 fc=cutoff*pow(2,4.5*envAmt*env+filterLfo*triangle);
 filtered=dco:cs.ota4(fc,res,.08):fi.highpass(1,max(20,hpf));
 // Small finite gate edge is explicit, not a new hardware time-constant claim.
 gateAmp=(g0>0):cs.sm(.0005);
 amplitude=select2(vcaGate,env,gateAmp);
 audio=(ma.tanh(filtered*1.2):fi.dcblockerat(20))*amplitude*cs.vel(g0,v0)*level*.8;
};
voice(f0,g0,v0)=state(f0,g0,v0):(_,!,!,!);
diagnostics(f0,g0,v0)=state(f0,g0,v0):(!,_,_,_);
process=voice(freq,gate,velocity);
