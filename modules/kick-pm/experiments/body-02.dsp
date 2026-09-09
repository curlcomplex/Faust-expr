// Isolated body experiment from study 02, now with sample-exact gate release.
// Not a replacement release or identified Elektron algorithm. No samples or PM.
import("stdfaust.lib");
declare name "kick-pm gated body identification 02";
declare version "0.0.2-body-experiment";
frequency = hslider("frequency_hz", 65.4, 20, 200, .001);
pitchAmount = hslider("pitch_amount_hz", 350, 0, 2000, .001);
pitchTau = hslider("pitch_tau_s", .025, .002, .2, .000001);
bodyTau = hslider("body_tau_s", .18, .005, 2, .000001);
releaseTau = hslider("release_tau_s", .04, .002, 2, .000001);
attackTau = hslider("attack_tau_s", .0001, .000001, .01, .000001);
phaseCycles = hslider("phase_cycles", 0, 0, 1, .000001);
level = hslider("level", .65, 0, 1, .000001);
velocity = hslider("velocity", 1, 0, 1, .000001);
gate = button("gate");
hit = gate > gate';
age = (+(1.0) : min(60.0*ma.SR) : *(1.0-hit)) ~ _;
t = age / ma.SR;
seen = max(hit) ~ _;
vel = ba.sAndH(hit, velocity);
frequencyNow = frequency + pitchAmount*exp(-t/pitchTau);
bodyEnvelope = (1.0-exp(-t/attackTau))*exp(-t/bodyTau);
// This layer models unknown recording/track articulation. Its ownership inside
// the hardware's kick kernel is not established. Release starts at elapsed zero
// ON the gate-off sample, not one sample early.
fall = gate < gate';
releaseAge = (+(1.0) : min(60.0*ma.SR) : *(1.0-max(gate,fall))) ~ _;
releaseEnvelope = exp(-releaseAge/(ma.SR*releaseTau));
phase = os.hs_phasor(1.0,frequencyNow,hit);
process = seen*level*vel*bodyEnvelope*releaseEnvelope
    * sin(2.0*ma.PI*(phase+phaseCycles));
