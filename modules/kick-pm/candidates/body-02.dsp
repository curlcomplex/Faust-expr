// Reference-led laboratory candidate. NOT identified Elektron circuitry/code.
// Experiment 01 remains unchanged. No samples, fixed EQ or output limiter.
import("stdfaust.lib");
declare name "kick-pm body study 02";
declare version "0.0.2-experiment";
frequency = hslider("frequency_hz",52,20,200,.001);
sweep = hslider("sweep_octaves",2.5,0,7,.001);
sweepTime = hslider("sweep_time_s",.025,.003,.3,.00001);
decay = hslider("decay_s",1.0,.04,6,.001);
releaseStart = hslider("release_start_s",.35,.01,4,.0001);
releaseTime = hslider("release_s",.15,.005,2,.0001);
attackTime = hslider("attack_s",.0001,.00002,.005,.00001);
square = hslider("square",0,0,1,.001);
triangle = hslider("triangle",0,0,1,.001);
contour = hslider("mod_envelope",.7,0,1,.001);
drive = hslider("drive",0,0,1,.001);
phaseOffsetCycles = hslider("phase_cycles",0,0,1,.001);
feedbackMode = checkbox("feedback_mode");
velocity = hslider("velocity",1,0,1,.001);
gate = button("gate");
hit = gate > gate';
age = (+(1.0) : min(20.0*ma.SR) : *(1.0-hit)) ~ _;
t = age/ma.SR;
seen = max(hit) ~ _;
v = ba.sAndH(hit,velocity);
// Additive-frequency exponential sweep, independent amount and time.
// This is a tested model hypothesis, not a recovered hardware parameter law.
f = frequency*(1.0+(pow(2.0,sweep)-1.0)*exp(-t/sweepTime));
amp = seen*(1.0-exp(-t/attackTime))*exp(-6.90775527898*t/decay);
// Separate late fade can represent host-gate/capture amplitude behaviour.
// It must not be presented as proof that this stage belongs to the machine.
late = exp(-6.90775527898*max(0.0,t-releaseStart)/releaseTime);
phase(hz) = os.hs_phasor(1.0,hz,hit);
carrierPhase = 2.0*ma.PI*(phase(f)+phaseOffsetCycles);
squarePhase = 2.0*ma.PI*(phase(2.0*f)+2.0*phaseOffsetCycles);
squareOp = (sin(squarePhase)+sin(3.0*squarePhase)/3.0
    +sin(5.0*squarePhase)/5.0)/(1.0+1.0/3.0+1.0/5.0);
modEnv = (1.0-contour)+contour*exp(-t/(.008+.12*decay));
triPhase = 2.0*ma.PI*(phase(3.0*f)+3.0*phaseOffsetCycles);
triShape(x) = 2.0/ma.PI*asin(sin(x));
triOp = (+(triPhase):triShape) ~ *(feedbackMode*.85*triangle*modEnv*(1.0-hit));
phaseModulation = .18*square*modEnv*squareOp+.30*triangle*modEnv*triOp;
body = sin(carrierPhase+2.0*ma.PI*phaseModulation);
// Candidate change: saturate the decaying body, then apply the late fade.
// At zero drive the stage is EXACTLY dry; no automatic level normalization.
pre = amp*body;
shaped = (1.0-drive)*pre+drive*ma.tanh(pre*(1.0+31.0*drive))/ma.tanh(1.0+31.0*drive);
process = .65*v*late*shaped;
