// Clap v3: sample-free clustered filtered noise, NOT a snare oscillator plus noise.
// Deliberate v2 ABI break: six clap-specific controls; see README.
declare name "clap";
declare version "0.3.0-experiment";
import("stdfaust.lib");
spacing=hslider("spacing",.40,0,1,.001);
tone=hslider("tone",.48,0,1,.001);
snap=hslider("snap",.62,0,1,.001);
decay=hslider("decay",.32,0,1,.001);
tail=hslider("tail",.35,0,1,.001);
drive=hslider("drive",.08,0,1,.001);
pitch=hslider("pitch_hz",210,70,900,.001);
gate=button("gate");velocity=hslider("velocity",1,0,1,.001);
hit=gate>gate';seen=max(hit)~_;latch(x)=ba.sAndH(hit,x);
spacingValue=latch(spacing);toneValue=latch(tone);snapValue=latch(snap);decayValue=latch(decay);
tailValue=latch(tail);driveValue=latch(drive);pitchValue=latch(pitch);velocityValue=latch(velocity);
// Clock saturates well after the longest tail, so long silence cannot overflow.
age=(+(1.0):min(12.0*ma.SR):*(1.0-hit))~_;t=age/ma.SR;
gap=.004+.024*spacingValue*spacingValue;
last=2.94*gap;
burstTau=gap*(.17+.20*(1-snapValue));
attack=.00012+.00032*(1-snapValue);
burst(x)=float(x>=0)*(1-exp((0-max(0,x))/attack))*exp((0-max(0,x))/burstTau)*min(1,max(0,(.94*gap-x)/(.15*gap)));
// Three distinct pre-attacks lead into the strongest main clap, rather than a
// large first snare attack followed by inaudible residual flutter.
cluster=.78*burst(t)+.88*burst(t-.97*gap)+.80*burst(t-1.98*gap)+burst(t-last);
u=max(0,t-last);tau=.018*pow(25,decayValue);
tailEnv=float(t>=last)*(1-exp(-u/(.001+.002*(1-snapValue))))*exp(-u/tau)*float(u<12*tau);
// Pitch shifts a broad NOISE formant, never a periodic drum-body oscillator.
fc=min(5500,max(750,1500*pow(3,toneValue-.48)*pow(pitchValue/210,.38)));
noise=no.noise;
mid=noise:fi.highpass(2,fc*.42):fi.lowpass(2,min(.40*ma.SR,fc*1.85));
focus=noise:fi.resonbp(fc,1.1,1);
air=noise:fi.highpass(2,min(.33*ma.SR,fc*1.25)):fi.lowpass(2,min(.42*ma.SR,8500));
front=.70*mid+.65*focus+(.06+.22*snapValue)*air;
// A delayed noise stream decorrelates the tail without any stored samples.
back=(noise:de.delay(512,173)):fi.highpass(2,fc*.50):fi.lowpass(2,min(.38*ma.SR,fc*1.55));
raw=front*cluster+tailValue*back*tailEnv;
a=driveValue*driveValue;
shaped=(1-a)*raw+a*ma.tanh((1+5*a)*raw)/(1+1.5*a);
process=shaped*.65*velocityValue*seen:fi.dcblockerat(20);
