declare name "analog-classics-retro-mixer-eq";
declare version "0.1.0-experiment";
declare description "Vintage compact-mixer channel colour/EQ; Airwindows MackEQ-inspired topology study";
declare license "MIT";
declare reference "Airwindows MackEQ, MIT, Chris Johnson; translated/independently simplified for Faust";
import("stdfaust.lib");

input = hslider("input",0.1,0,1,0.001) : si.smooth(0.999);
treble = hslider("treble",0.5,0,1,0.001) : si.smooth(0.999);
bass = hslider("bass",0.5,0,1,0.001) : si.smooth(0.999);
output = hslider("output",1,0,1,0.001) : si.smooth(0.999);
mix = hslider("mix",1,0,1,0.001) : si.smooth(0.999);

inTrim = (input*10)*(input*10);
gainHigh = treble*treble*4;
outHigh = sqrt(max(0.000001,treble));
gainBass = bass*bass*4;
outBass = sqrt(max(0.000001,bass));

clip1(x) = min(1,max(-1,x));
shape(x) = c - c*c*c*c*c*0.1768 with { c = clip1(x); };
condition = fi.highpass(1,12) : fi.lowpass(2,min(19000,0.44*ma.SR));

lowBand = fi.lowpass(2,360) : *(gainBass) : shape : fi.lowpass(1,360) : *(outBass);
midBand = fi.highpass(1,300) : fi.lowpass(1,3600);
highBand = fi.highpass(2,3000) : *(gainHigh) : shape : *(outHigh);
colour = condition : *(inTrim) : shape <: lowBand,midBand,highBand :> _ : *(1.45) : fi.lowpass(2,min(19000,0.44*ma.SR)) : shape;
channel(x) = x*(1-mix) + (x:colour)*output*mix;

process = par(i,2,channel);
