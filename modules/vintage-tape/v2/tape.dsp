// Airwindows IronOxideClassic2 reference candidate, Chris Johnson / MIT.
// See ../REFERENCE-LICENSE.txt. V1 is retained unchanged.
// Target is the ACTUAL pinned LinuxVST source, not a silently repaired algorithm.
// Its gcount is initialized to zero and never advanced: weighted history reads
// stay zero. The oracle harness checks this source property and compares audio.
// This reduction is valid only for the pinned reset/processing state contract.
declare name "analog-classics-vintage-tape";
declare version "0.2.0-reference-candidate";
declare license "MIT";
import("stdfaust.lib");
input=hslider("input[unit:dB]",0,-18,18,.01);
speed=hslider("speed[unit:ips]",16.35,1.5,150,.01);
output=hslider("output[unit:dB]",0,-18,18,.01);
scale=ma.SR/44100.0;
cycleEnd=int(max(1,min(4,floor(scale))));
cycle=(+(1):%(cycleEnd))~_;
fire=cycle==0;
flip=(1-_)~_;
ips=speed*1.1;
iirAmount=ips/430/scale;
fastTaper=1+(ips/15/scale)*cycleEnd;
soft(x)=sin(max(-1.57079633,min(1.57079633,x)));
lean=(step~si.bus(2)):(!,!,_) with {
 step(a,b,x)=na,nb,x-select2(flip,nb,na) with {
  na=select2(flip,a,a*(1-iirAmount)+x*iirAmount);
  nb=select2(flip,b*(1-iirAmount)+x*iirAmount,b);
 };
};
shape=(step~si.bus(2)):(!,!,_) with {
 step(a,b,x)=na,nb,select2(flip,nb,na) with {
  na=select2(fire*flip,a,a/fastTaper+x);
  nb=select2(fire*(1-flip),b,b/fastTaper+x);
 };
};
reconstruct(x)=select2(cycleEnd==1,interp,x) with {
 current=ba.sAndH(fire,x);previous=ba.sAndH(fire,current');
 fraction=select2(cycleEnd==3,float(cycle)/cycleEnd,select2(cycle==0,select2(cycle==1,1.0/3,2.0/3),0));
 interp=previous+(current-previous)*fraction;
};
bq(q,x)=select2(24000.0/ma.SR<.49999,x,(x:fi.tf2(b0,2*b0,b0,a1,a2))) with {
 k=tan(ma.PI*min(.49,24000.0/ma.SR));norm=1/(1+k/q+k*k);
 b0=k*k*norm;a1=2*(k*k-1)*norm;a2=(1-k/q+k*k)*norm;
};
mono=lean:bq(1.618033988749894848):*(pow(10,input/20)):soft:shape:reconstruct:soft:bq(.618033988749894848):*(pow(10,output/20));
process=par(i,2,mono);
