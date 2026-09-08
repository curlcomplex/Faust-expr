// kick-pm / experiment 01. Independent hypothesis, NOT a BD Modern replica.
// No samples, output limiter, reverb or hidden EQ. See manifest and README.
import("stdfaust.lib");
declare name "kick-pm experiment 01";
declare version "0.0.1-experiment";

frequency = hslider("frequency_hz", 52, 20, 200, 0.001);
sweep = hslider("sweep", 0.32, 0, 1, 0.001);
punch = checkbox("punch");
decay = hslider("decay_s", 0.45, 0.025, 2.5, 0.001);
square = hslider("square", 0.15, 0, 1, 0.001);
triangle = hslider("triangle", 0.20, 0, 1, 0.001);
contour = hslider("mod_envelope", 0.7, 0, 1, 0.001);
drive = hslider("drive", 0, 0, 1, 0.001);
velocity = hslider("velocity", 1, 0, 1, 0.001);
gate = button("gate");
// Laboratory ablation only: 0 = feedforward triangle, 1 = feedback triangle.
feedbackMode = checkbox("feedback_mode");

hit = gate > gate';
// Saturating elapsed-sample counter avoids an unbounded float phase clock.
age = (+(1.0) : min(20.0 * ma.SR) : *(1.0-hit)) ~ _;
t = age / ma.SR;
seen = max(hit) ~ _;
v = ba.sAndH(hit, velocity);

// All curves/constants here are explicit experimental choices, not identified
// Elektron scaling. Decay denotes exponential time to -60 dB before attack.
attackTime = 0.00025 + (1.0-punch) * 0.00075;
amp = seen * (1.0-exp(-t/attackTime)) * exp(-6.90775527898*t/decay);
pitchTau = 0.006 + 0.050*sweep*sweep;
f = frequency * pow(2.0, 48.0*sweep*exp(-t/pitchTau)/12.0);
modEnv = (1.0-contour) + contour*exp(-t/(0.008+0.12*decay));
phase(hz) = os.hs_phasor(1.0, hz, hit);
carrierPhase = 2.0*ma.PI*phase(f);
squarePhase = 2.0*ma.PI*phase(2.0*f);
// A bounded three-partial square approximation; no discontinuous sign wave.
squareOp = (sin(squarePhase)+sin(3.0*squarePhase)/3.0
    +sin(5.0*squarePhase)/5.0)/(1.0+1.0/3.0+1.0/5.0);
triPhase = 2.0*ma.PI*phase(3.0*f);
triShape(x) = 2.0/ma.PI * asin(sin(x));
// Faust feedback introduces one sample of delay. Multiplying the feedback
// by (1-hit) resets its contribution on the triggering sample.
triOp = (+(triPhase) : triShape)
    ~ *(feedbackMode*0.85*triangle*modEnv*(1.0-hit));
phaseOffset = 0.18*square*modEnv*squareOp + 0.30*triangle*modEnv*triOp;
body = sin(carrierPhase + 2.0*ma.PI*phaseOffset);
// Exactly dry at drive=0. Shaping precedes the amplitude envelope deliberately.
shaped = (1.0-drive)*body + drive*tanh(body*(1.0+5.0*drive))/tanh(1.0+5.0*drive);
tick = punch*0.06*sin(2.0*ma.PI*phase(7.0*frequency))*exp(-t/0.003);
process = 0.65*v*amp*(shaped+tick);
