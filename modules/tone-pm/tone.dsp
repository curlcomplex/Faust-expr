// tone-pm / experiment 01. Independent compact two-operator PM instrument.
// Inspired by published Model:Cycles/Syntakt control roles; not Elektron source.
import("stdfaust.lib");
declare name "tone-pm";
declare version "0.1.0-experiment";

pitch = hslider("pitch_hz",110,20,8000,.001);
ratioCtl = hslider("ratio",.40,0,1,.001);
punchCtl = hslider("punch",.15,0,1,.001);
decayCtl = hslider("decay",.55,0,1,.001);
feedbackCtl = hslider("feedback",.08,0,1,.001);
modCtl = hslider("modulation",.28,0,1,.001);
menvCtl = hslider("mod_env",.45,0,1,.001);
driveCtl = hslider("drive",.08,0,1,.001);
velocity = hslider("velocity",1,0,1,.001);
gate = button("gate");

hit = gate > gate';
seen = max(hit) ~ _;
latched(x) = ba.sAndH(hit,x);
vel = latched(velocity);
freq = latched(pitch);
ratio = 0.25*pow(32.0,latched(ratioCtl));
punch = latched(punchCtl);
decay = .03*pow(200.0,latched(decayCtl));
feedback = .95*pow(latched(feedbackCtl),2.0);
modulation = 7.0*pow(latched(modCtl),2.0);
menv = latched(menvCtl);
drive = latched(driveCtl);

age = (+(1.0) : min(60.0*ma.SR) : *(1.0-hit)) ~ _;
t = age/ma.SR;
attack = 1.0-exp(-t/(.0004-.00025*punch));
amp = seen*attack*exp(-t/decay);
// MENV jointly increases modulation-envelope depth and persistence. The
// residual 12% intentionally leaves a colored tail at nonzero modulation.
modTau = .004*pow(125.0,menv);
modEnv = modulation*(.12 + .88*exp(-t/modTau)*(0.25+1.75*menv));

phase(hz) = os.hs_phasor(1.0,hz,hit);
modPhase = 2.0*ma.PI*phase(freq*ratio);
// Bounded one-sample phase feedback. Reset contribution on onset sample.
modulator = (+(modPhase) : sin) ~ *(feedback*(1.0-hit));
carrierPhase = 2.0*ma.PI*phase(freq);
raw = sin(carrierPhase + modEnv*modulator);
// Punch is an early compressor-like emphasis/distortion, not merely gain.
early = 1.0 + 3.0*punch*exp(-t/.012);
punched = ma.tanh(raw*early)/ma.tanh(early);
// Exactly neutral at zero drive. Drive is after punch but before amp envelope.
d = pow(drive,2.0);
shaped = (1.0-d)*punched + d*ma.tanh(punched*(1.0+12.0*d))/ma.tanh(1.0+12.0*d);
process = .52*vel*amp*shaped : fi.dcblockerat(8);
