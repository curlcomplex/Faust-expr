declare name "Analog Classics Juno 60 Voice";
declare version "0.1.0-reference-candidate";
declare description "Single-note Juno-60-inspired voice; host owns polyphony and chorus is external";

import("stdfaust.lib");

clip(lo,hi,x)=min(hi,max(lo,x));
sm(t,x)=x:si.smooth(exp(-1/(max(.00001,t)*ma.SR)));
vel(g,v)=ba.sAndH((g>0)>(g'>0),clip(0,1,v));

// Independent Juno-family architecture study. No Hera/JunoX source is copied here.
// Four TPT one-poles with nonlinear input/feedback; reference-candidate, not hardware calibration.
ota4(cf,res)=(step~si.bus(4)):(!,!,!,!,_) with {
 g=tan(ma.PI*clip(15,ma.SR*.40,cf)/ma.SR); G=g/(1+g);
 k=3.9*clip(0,1,res);
 step(s1,s2,s3,s4,x)=2*y1-s1,2*y2-s2,2*y3-s3,2*y4-s4,y4*(1+.10*k) with {
  S=(1-G)*(G*G*G*s1+G*G*s2+G*s3+s4);
  u=ma.tanh((x-k*S)/(1+k*G*G*G*G));
  y1=G*u+(1-G)*s1; y2=G*y1+(1-G)*s2;
  y3=G*y2+(1-G)*s3; y4=G*y3+(1-G)*s4;
 };
};

// Juno HPF is stepped in hardware. Keep an extended continuous control but expose useful detents.
hpfHz(x)=ba.if(x<.125,20,ba.if(x<.375,80,ba.if(x<.625,160,ba.if(x<.875,320,700))));

gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,8000,.01);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);

saw=hslider("saw",.72,0,1,.001):sm(.005);
pulse=hslider("pulse",.32,0,1,.001):sm(.005);
sub=hslider("sub",.42,0,1,.001):sm(.005);
noise=hslider("noise",.015,0,1,.001):sm(.005);
pwm=hslider("pwm",.50,.05,.95,.001):sm(.005);
pwmDepth=hslider("pwmDepth",.14,0,.45,.001):sm(.005);
lfoRate=hslider("lfoRate[unit:Hz][scale:log]",4.5,.05,20,.01):sm(.005);
cutoff=hslider("cutoff[unit:Hz][scale:log]",2200,40,16000,1):log:sm(.005):exp;
res=hslider("resonance",.22,0,.98,.001):sm(.005);
hpf=hslider("hpf",0,0,1,.001):sm(.005);
envAmt=hslider("filterEnv",.36,0,1,.001):sm(.005);
keyTrack=hslider("keyTrack",.35,0,1,.001):sm(.005);
attack=hslider("attack[unit:s][scale:log]",.018,.001,3,.001):sm(.005);
decay=hslider("decay[unit:s][scale:log]",.36,.005,4,.001):sm(.005);
sustain=hslider("sustain",.70,0,1,.001):sm(.005);
release=hslider("release[unit:s][scale:log]",.60,.005,6,.001):sm(.005);
level=hslider("level",.68,0,1,.001):sm(.005);

state(f0,g0,v0)=audio,pitchHz,env with {
 pitchHz=ba.sAndH(g0>0,clip(20,8000,f0));
 width=clip(.05,.95,pwm+pwmDepth*os.osc(lfoRate));
 f=clip(10,ma.SR*.20,pitchHz);
 // Juno-family source balance + gentle loading compensation when several sources are raised.
 rawSaw=os.polyblep_saw(f)*saw*.20;
 rawPulse=os.pulsetrain(f,width)*pulse*.20;
 rawSub=os.polyblep_square(f*.5)*sub*.195;
 rawNoise=no.pink_noise*noise*.21;
 sumLevel=.20*saw+.20*pulse+.195*sub+.21*noise;
 load=.26/(.26+max(0,sumLevel-.26)*.30);
 dco=(rawSaw+rawPulse+rawSub+rawNoise)*load;
 env=en.adsr(max(.001,attack),max(.005,decay),sustain,max(.005,release),g0>0);
 keyOct=log(max(20,pitchHz)/261.625565)/log(2);
 fc=cutoff*pow(2,4.5*envAmt*env+keyTrack*keyOct);
 filtered=dco:ota4(fc,res):fi.highpass(1,hpfHz(hpf));
 audio=(ma.tanh(filtered*1.15):fi.dcblockerat(20))*env*vel(g0,v0)*level;
};

voice(f0,g0,v0)=state(f0,g0,v0):(_,!,!);
diagnostics(f0,g0,v0)=state(f0,g0,v0):(!,_,_);
process=voice(freq,gate,velocity);
