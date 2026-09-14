// Airwindows MV2, Chris Johnson / MIT. See ../REFERENCE-LICENSE.txt.
// One stereo effect, dual-mono state; no voice allocation. Preserve v1/v2.
// Size selects the SAME suffix of the A-Z cascade as MV2; inactive stages and
// their averaging memories freeze. Tables use a reserved discard-write cell.
declare name "analog-classics-vintage-rack-reverb";
declare version "0.3.0-reference-candidate";
declare license "MIT";
declare description "MV2 reference-core: ordered allpasses, stepped regeneration, dual-mono and high-rate reconstruction";
import("stdfaust.lib");
decay=hslider("decay",.58,0,1,.001);
size=hslider("size",.52,0,1,.001);
tone=hslider("tone",.46,0,1,.001);
character=hslider("character",.64,0,1,.001);
mix=hslider("mix",.35,0,1,.001);
stages=int(size*27.0);
damp=int((1.0-tone)*stages);
regen=ba.if(decay<=.0625,0,ba.if(decay<=.125,.0625,ba.if(decay<=.25,.125,ba.if(decay<=.5,.25,ba.if(decay<=.99,.5,1)))));
// Keep the existing Character label/range as its current output-level mapping.
// The curve is explicit; this is not an extra MV2 distortion parameter.
gain=.55+.45*character;
cycleEnd=int(max(1,min(4,floor(ma.SR/44100.0))));
cycle=(+(1):%(cycleEnd))~_;
fire=cycle==0;
// q holds the old sample needed on the next enabled update. Prefetching two
// cells ahead allows an ordinary Faust one-sample recurrence without changing
// the effective allpass delay. Inactive cycles write only to the discard cell.
allpass(N,rank)=(step~si.bus(2)):(!,!,_) with {
 active=fire*(stages>=rank);
 averaging=damp>rank;
 pos=(+(active):%(N+1))~_;
 step(q,avg,x)=nextQ,nextAvg,y with {
  stored=x-q*.5;
  delayed=rwtable(N+2,0.0,select2(active,N+1,pos),stored,(pos+2)%(N+1));
  z=stored*.5+q;
  nextQ=select2(active,q,delayed);
  nextAvg=select2(active*averaging,avg,z);
  filtered=select2(averaging,z,(z+avg)*.5);
  y=select2(active,x,filtered);
 };
};
chain=allpass(7573,26):allpass(7307,25):allpass(7177,24):allpass(6907,23):
 allpass(6779,22):allpass(6521,21):allpass(5981,20):allpass(5563,19):
 allpass(5297,18):allpass(4903,17):allpass(4759,16):allpass(4489,15):
 allpass(4391,14):allpass(4229,13):allpass(4153,12):allpass(3989,11):
 allpass(3659,10):allpass(3407,9):allpass(3251,8):allpass(2999,7):
 allpass(2917,6):allpass(2749,5):allpass(2503,4):allpass(2423,3):
 allpass(2146,2):allpass(2088,1);
network=(step~_):(!,_) with {
 step(previous,x)=next,processed with {
  diffused=sin(x+previous):chain;
  next=select2(fire,previous,diffused*regen);
  processed=asin(max(-1,min(1,diffused*gain)));
 };
};
reconstruct(x)=select2(cycleEnd==1,interpolated,x) with {
 current=ba.sAndH(fire,x);
 previous=ba.sAndH(fire,current');
 fraction=select2(cycleEnd==3,float(cycle)/cycleEnd,select2(cycle==0,select2(cycle==1,1.0/3,2.0/3),0));
 interpolated=previous+(current-previous)*fraction;
};
channel(x)=x*(1-mix)+(x:network:reconstruct)*mix;
process=par(i,2,channel);
