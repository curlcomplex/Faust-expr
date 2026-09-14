declare name "808 Kick";
declare version "0.3.0-fischer-reference-candidate";
declare category "Drums";
declare description "Single-note 808 kick reference candidate tuned against Fischer/Technopolis unit 103852; classic anchor plus extended tuning/punch/click/drive controls.";
import("stdfaust.lib");
tone=hslider("tone",.50,0,1,.001);
decay=hslider("decay",.50,0,1,.001);
punch=hslider("punch",.58,0,1,.001);
click=hslider("click",.16,0,1,.001);
drive=hslider("drive",.04,0,1,.001);
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",48.5,30,100,.001);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
gate=button("gate[curlop:input]");
hit=gate>gate'; seen=max(hit)~_;
lat(x)=select2(seen,x,ba.sAndH(hit,x));
age=(+(1):min(30*ma.SR):*(1-hit))~_; t=age/ma.SR;
f0=lat(freq); vel=lat(velocity);
// Fischer unit: panel decay spans roughly a short click through a ~0.6 s t90 tail.
// The extended control keeps the whole useful range but anchors panel noon near BD5050.
tau=.015 + .235*pow(lat(decay),1.35);
// Stock attack jump is brief; Punch extends it beyond the hardware anchor.
sweepTau=.0038+.0032*lat(punch);
sweepOct=.40+.95*lat(punch);
f=f0*pow(2,sweepOct*exp(-t/sweepTau));
phase=os.hs_phasor(1,f,hit);
sine=sin(2*ma.PI*phase);
body=sine*(1-exp(-t/.00035))*exp(-t/tau);
transient=lat(click)*exp(-t/.0012)*(.45*no.noise+.55*sin(2*ma.PI*os.hs_phasor(1,3100,hit)));
// Tone is primarily attack brightness, like the panel control; body pitch is unchanged.
cut=350+6800*lat(tone);
colored=(body+.10*transient):fi.lowpass(1,min(.45*ma.SR,cut));
a=lat(drive)*lat(drive);
sat=(1-a)*colored+a*ma.tanh((1+8*a)*colored)/(1+1.8*a);
endFade=min(1,max(0,(14*tau-t)/tau));
process=(sat*.82*vel*seen:fi.dcblockerat(8))*endFade;
