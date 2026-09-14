declare name "analog-classics-retro-mixer-eq";
declare version "0.2.0-reference-candidate";
declare description "Vintage compact-mixer channel colour/EQ translated against pinned Airwindows MackEQ";
declare license "MIT";
declare reference "Airwindows MackEQ, Chris Johnson, pinned 03c9931839881bae6dfd4e36bfd3cced79f54b4a";
import("stdfaust.lib");

// Keep the oracle's five normalized controls for exact comparison.
input = hslider("input",0.1,0,1,0.001);
treble = hslider("treble",0.5,0,1,0.001);
bass = hslider("bass",0.5,0,1,0.001);
output = hslider("output",1,0,1,0.001);
mix = hslider("mix",1,0,1,0.001);

inTrim = (input*10.0)*(input*10.0);
gainHigh = treble*treble*4.0;
outHigh = sqrt(treble);
gainBass = bass*bass*4.0;
outBass = sqrt(bass);
overallscale = ma.SR/44100.0;
aHP1 = 0.001860867/overallscale;
aHP2 = 0.000287496/overallscale;
aBass = 0.159/overallscale;
aHigh = 0.236/overallscale;

clip1(x) = min(1.0,max(-1.0,x));
shape(x) = c - c*c*c*c*c*0.1768 with { c=clip1(x); };
onepole(a) = *(a) : + ~ *(1.0-a);
highpass1(a,x) = x - (x:onepole(a));

// Exact coefficient construction from MackEQ's four DF1 19.16 kHz stages.
macklp(q) = fi.tf2(b0,b1,b2,a1,a2)
with {
  k = tan(ma.PI*19160.0/ma.SR);
  norm = 1.0/(1.0 + k/q + k*k);
  b0 = k*k*norm;
  b1 = 2.0*b0;
  b2 = b0;
  a1 = 2.0*(k*k-1.0)*norm;
  a2 = (1.0-k/q+k*k)*norm;
};

// Original EQ split: bass is low-passed then distorted/darkened; highs are
// obtained by two one-pole subtractions and then distorted. Mid remains linear.
eqsection(x) = ((bassDark*outBass) + midBand + (highShaped*outHigh))*4.0
with {
  bassPre = x:onepole(aBass);
  midPre = x-bassPre;
  bassShaped = (bassPre*gainBass):shape;
  bassDark = bassShaped:onepole(aBass);
  midBand = midPre:onepole(aHigh);
  highPre = midPre-midBand;
  highHP = highPre-(highPre:onepole(aHigh));
  highShaped = (highHP*gainHigh):shape;
};

mack(x) = y
with {
  dry = x;
  s0 = highpass1(aHP1,x) * inTrim;
  s1 = s0 : macklp(0.431684981684982) : shape : macklp(1.1582298);
  s2 = highpass1(aHP2,s1);
  s3 = s2 : eqsection;
  s4 = s3 : macklp(0.657027382751269) : shape : macklp(1.076210852946577);
  wet = s4*output;
  y = wet*mix + dry*(1.0-mix);
};

process = par(i,2,mack);
