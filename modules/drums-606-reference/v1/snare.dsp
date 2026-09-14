declare name "606 Snare";
declare version "0.2.0-reference-candidate";
declare author "curlcomplex";
declare category "Analog Classics / 606";
declare description "One-note reference candidate. Measured onset pitch relaxation and separately balanced band-limited noise; not a circuit-exact model. Host owns polyphony.";
import("stdfaust.lib");
u=library("../../drums-606/v1/drums606.lib");
gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",195,90,420,.001);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
accent=hslider("accent[curlop:input]",0,0,1,.001);
decay=hslider("decay[unit:s][scale:log][col:0][row:0]",.15,.025,1.2,.001);
tone=hslider("tone[col:1][row:0]",.5,0,1,.001):u.sm;
snappy=hslider("snappy[col:2][row:0]",.35,0,1,.001):u.sm;
level=hslider("level[col:3][row:0]",.8,0,1,.001):u.sm;
h=gate>gate';
f=u.lat(h,freq);d=u.lat(h,decay);v=u.lat(h,velocity);a=u.lat(h,accent);
t=u.age(h)/ma.SR;
// Behavioral relaxation hypothesis from the archive's ~253-Hz attack / ~194-Hz body.
// This is not an extra allocated voice or a measured transistor simulation.
fp=f*(1+.48*exp(-t/.014));
body=h:u.ring(fp,d);
// No compulsory noise floor at Snappy=0. Keep extended snare/noise balance live.
wire=no.noise:fi.highpass(2,700*pow(4,tone)):fi.lowpass(2,min(.4*ma.SR,6000));
raw=.70*body+.85*snappy*wire*u.env(h,d,.00022);
process=u.finish(raw,v,a,level);
