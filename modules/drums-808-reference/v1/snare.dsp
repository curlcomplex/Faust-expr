declare name "808 Snare";
declare version "0.3.0-fischer-reference-candidate";
declare category "Drums";
declare author "curlcomplex; circuit/reference research includes TapTools by Timothy Place";
declare description "Single-note 808 snare candidate anchored to Fischer unit 103852 and late-revision 808 resonator voicing; Tone balances the two body modes, Snappy controls filtered noise.";
import("stdfaust.lib");
tone=hslider("tone",.50,0,1,.001);
snappy=hslider("snappy",.50,0,1,.001);
// Extension beyond the original panel: scales both body/noise tail classes around stock.
decay=hslider("decay",.50,0,1,.001);
noiseColor=hslider("noise_color",.50,0,1,.001);
drive=hslider("drive",.02,0,1,.001);
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",172.3,100,320,.001);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
gate=button("gate[curlop:input]");
hit=gate>gate'; seen=max(hit)~_;
lat(x)=select2(seen,x,ba.sAndH(hit,x));
age=(+(1):min(20*ma.SR):*(1-hit))~_; t=age/ma.SR;
vel=lat(velocity); f0=lat(freq); tn=lat(tone); sn=lat(snappy);
// Late-revision stock modes are about 173 / 336 Hz. Tone is their balance, not pitch.
phase1=os.hs_phasor(1,f0,hit); phase2=os.hs_phasor(1,f0*1.95,hit);
// Fischer midpoint is short; Decay is an extended macro centered on that stock class.
scale=.55+1.45*lat(decay);
bodyTau=.026*scale;
noiseTau=.021*scale;
body=((1-tn)*1.05*sin(2*ma.PI*phase1)+tn*.82*sin(2*ma.PI*phase2))*exp(-t/bodyTau);
// Snappy path: band-limited noise rather than near-white HF. TapTools calibration uses ~4k HP / ~4.7k LP;
// this broader pair leaves room for the Fischer unit while Noise Color remains an extension.
hp=2200+2400*lat(noiseColor); lp=min(.44*ma.SR,5400+2800*lat(noiseColor));
noise=no.noise:fi.highpass(2,hp):fi.lowpass(2,lp);
wire=noise*exp(-t/noiseTau);
// Small trigger crack; the body/noise balance should dominate the first 30ms, as in SD5050.
attack=exp(-t/.0012)*(no.noise:fi.highpass(1,1200):fi.lowpass(1,4200));
raw=.72*body + 1.10*pow(sn,1.45)*wire + .035*attack;
a=lat(drive)*lat(drive);
sat=(1-a)*raw+a*ma.tanh((1+8*a)*raw)/(1+1.7*a);
endTau=max(bodyTau,noiseTau); endFade=min(1,max(0,(14*endTau-t)/endTau));
process=(sat*.62*vel*seen:fi.dcblockerat(15))*endFade;
