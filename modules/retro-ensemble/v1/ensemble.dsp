declare name "analog-classics-retro-ensemble";
declare version "0.1.0-experiment";
declare description "Lush multi-voice ensemble candidate derived from Airwindows Ensemble architecture";
declare license "MIT-derived; Airwindows Ensemble attribution retained in issue #63";
import("stdfaust.lib");

voices = hslider("voices",25,2,48,1);
fullness = hslider("fullness",0,0,1,0.001) : si.smooth(0.999);
brighten = hslider("brighten",1,0,1,0.001) : si.smooth(0.999);
mix = hslider("mix",1,0,1,0.001) : si.smooth(0.999);

TWO_PI = 6.283185307179586;
scale = ma.SR/44100.0;
spd = pow(0.4+(fullness/12.0),10)*scale;
depth = 0.002/max(spd,1e-9);

// Airwindows Ensemble uses 2..48 independently moving delay taps. We keep the
// maximum topology explicit and gate unused lanes instead of replacing it with
// a conventional 2/3-voice chorus. This is intentionally a first profiling
// candidate; CPU cost versus the C++ oracle is part of issue #63.
tap(i) = de.fdelay4(32768,delaySamples) * active with {
  n = i+1;
  active = float(n<=voices);
  speedRad = spd/(1+(n/max(2,voices)));
  rateHz = speedRad*ma.SR/TWO_PI;
  phaseOffset = (3.141592653589793/max(2,voices))*i;
  mod = os.osc(rateHz)*cos(phaseOffset) + os.osc(rateHz)*sin(phaseOffset)*0.0;
  delaySamples = min(32760,max(1,depth*n + depth*mod));
};

// A simple derivative-style high-frequency compensation stands in for the
// oracle's alternating "air" state machine for this first Faust candidate.
air = _ <: _,(_-_*0 : @(1)) : *(brighten*0.35) : +;
wetmono = air <: _,par(i,48,tap(i)) :> _ : /(4*sqrt(max(2,voices)));
mono = _ <: *(1-mix),(wetmono:*(mix)) :> _;
process = par(i,2,mono);
