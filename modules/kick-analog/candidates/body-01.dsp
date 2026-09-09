// Baseline clean resonant body: no click, saturation or sub layer.
import("stdfaust.lib");
pitch=hslider("pitch_hz",52,20,160,.001);decay=hslider("decay",.5,0,1,.001);sweep=hslider("sweep",.5,0,1,.001);punch=hslider("punch",.5,0,1,.001);gate=button("gate");velocity=hslider("velocity",1,0,1,.001);
hit=gate>gate';age=(+(1):min(60*ma.SR):*(1-hit))~_;t=age/ma.SR;seen=max(hit)~_;vel=ba.sAndH(hit,velocity);
pitchTau=.006+.070*pow(1-punch,2);bodyTau=.055*pow(30,decay);attackTau=.00015+.0018*pow(1-punch,2);freq=pitch*(1+(pow(2,3.25*sweep)-1)*exp(-t/pitchTau));amp=(1-exp(-t/attackTau))*exp(-t/bodyTau);phase(hz)=os.hs_phasor(1,hz,hit);process=.58*seen*vel*amp*sin(2*ma.PI*phase(freq));
