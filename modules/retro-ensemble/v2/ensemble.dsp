// Derived from Chris Johnson / Airwindows Ensemble, MIT. See ../REFERENCE-LICENSE.txt.
// Reference revision: airwindows/airwindows 03c9931839881bae6dfd4e36bfd3cced79f54b4a.
// Preserve v1. This targets the original algorithm, interpolation and event changes.
// It is not a Juno chorus. Numerical comparison is not human sound approval.
declare name "analog-classics-retro-ensemble";
declare version "0.2.2-reference-candidate";
declare license "MIT";
import("stdfaust.lib");
voices=hslider("voices",25,2,48,1);
fullness=hslider("fullness",0,0,1,.001);
brighten=hslider("brighten",1,0,1,.001);
mix=hslider("mix",1,0,1,.001);
taps=max(2,min(48,floor(voices)));
scale=ma.SR/44100.0;
// Upstream B is float: B/12 rounds BEFORE promotion by double literal 0.4.
// Explicit binary32 rounding is needed in a double-compiled Faust oracle too.
// Rounding a nonnegative finite control here preserves the original coefficient;
// this is not noise or a new user parameter. Single precision is already float.
singleprecision fullnessDiv12(x)=x/12;
doubleprecision fullnessDiv12(x)=rint(v/ulp)*ulp with {
 v=x/12;
 ulp=pow(2,max(-149,floor(log(max(1e-45,v))/log(2))-23));
};
spd=pow(.4+fullnessDiv12(fullness),10)*scale;
depth=.002/spd;
flip=(1-_)~_;
// Alternating Air: even uses NEW odd; factor uses the pre-decay selection.
air=(step~si.bus(3)):(!,!,!,_) with {
 step(prev,odd,even,x)=x,no,ne,x+factor*brighten with {
  delta=prev-x;
  odd0=odd+select2(flip,delta,0-delta);
  even0=even+select2(flip,0-delta,delta);
  factor=select2(flip,odd0,even0);
  no=(odd0-(odd0-even0)/256)/1.0001;
  ne=(even0-(even0-no)/256)/1.0001;
 };
};
// Non-binding guards preserve floating-point error correction: without them
// Faust algebra cancels (total-p)-y to zero. Retain the prolonged drift test.
// Inactive heads freeze BOTH phase and correction as in the selected-taps loop.
sweep(active,stepSize)=(step~si.bus(2)):(!,!,_) with {
 step(p,e)=np,ne,p+ma.PI/2 with {
  y=stepSize-e;
  total=min(7,p+y);
  difference=min(1,total-p);
  np=select2(active,p,total-2*ma.PI*float(total>2*ma.PI));
  ne=select2(active,e,difference-y);
 };
};
head(i,x)=active*(a*(1-alpha)+b+c*alpha-((a-b)-(b-c))/50) with {
 n=i+1;active=float(n<=taps);
 phase=sweep(active,spd/(1+n/taps));
 offset=max(0,min(32760,depth*n+depth*sin(phase+ma.PI*(n-1)/taps)));
 index=int(floor(offset));alpha=offset-floor(offset);
 a=x:de.delay(32768,index);b=x:de.delay(32768,index+1);c=x:de.delay(32768,index+2);
};
wet(x)=(x+sum(i,48,head(i,x)))/(4*sqrt(taps));
mono(x)=x*(1-mix)+(x:air:wet)*mix;
process=par(i,2,mono);
