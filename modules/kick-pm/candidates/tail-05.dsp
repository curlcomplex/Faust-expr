// Tail study 05: color-04 retained, with one isolated amplitude-hold hypothesis.
// NOT recovered Elektron code/knob law; gate/capture ownership remains unknown.
import("stdfaust.lib");
declare name "kick-pm tail study 05";
declare version "0.0.5-experiment";
frequency = hslider("frequency_hz",65.4,20,200,.001);
pitchAmount = hslider("pitch_amount_hz",350,0,2000,.001);
pitchTau = hslider("pitch_tau_s",.025,.002,.2,.000001);
bodyTau = hslider("body_tau_s",.18,.005,2,.000001);
bodyHold = hslider("body_hold_s",0,0,2,.000001);
releaseTau = hslider("release_tau_s",.04,.002,2,.000001);
attackTau = hslider("attack_tau_s",.0001,.000001,.01,.000001);
phaseCycles = hslider("phase_cycles",0,0,1,.000001);
level = hslider("level",.65,0,1,.000001);
velocity = hslider("velocity",1,0,1,.000001);
gate = button("gate");
square = hslider("square",0,0,1,.001);
triangle = hslider("triangle",0,0,1,.001);
modDepth = hslider("mod_envelope",.7,0,1,.001);
modTau = hslider("mod_tau_s",.06,.001,2,.000001);
drive = hslider("drive",0,0,1,.001);
afterBody = checkbox("drive_after_body");
feedbackMode = checkbox("feedback_mode");
hit = gate > gate';
age = (+(1.0) : min(60.0*ma.SR) : *(1.0-hit)) ~ _;
t = age/ma.SR;
seen = max(hit) ~ _;
vel = ba.sAndH(hit,velocity);
frequencyNow = frequency + pitchAmount*exp(-t/pitchTau);
// Only architectural change: delay BODY decay, not attack, pitch, modulation,
// or note-off. hold=0 preserves color-04. No timed automatic note-off is added.
bodyEnvelope = (1.0-exp(-t/attackTau))*exp((0.0-max(0.0,t-bodyHold))/bodyTau);
fall = gate < gate';
releaseAge = (+(1.0) : min(60.0*ma.SR) : *(1.0-max(gate,fall))) ~ _;
releaseEnvelope = exp(-releaseAge/(ma.SR*releaseTau));
phase(hz) = os.hs_phasor(1.0,hz,hit);
carrierPhase = 2.0*ma.PI*(phase(frequencyNow)+phaseCycles);
squarePhase = 2.0*ma.PI*(phase(2.0*frequencyNow)+2.0*phaseCycles);
squareOp = (sin(squarePhase)+sin(3.0*squarePhase)/3.0+sin(5.0*squarePhase)/5.0)
    /(1.0+1.0/3.0+1.0/5.0);
triPhase = 2.0*ma.PI*(phase(3.0*frequencyNow)+3.0*phaseCycles);
modEnv = (1.0-modDepth)+modDepth*exp(-t/modTau);
triShape(x) = 2.0/ma.PI*asin(sin(x));
triOp = (*(feedbackMode*.85*triangle*modEnv*(1.0-hit)) : +(triPhase) : triShape) ~ _;
phaseModulation = .18*square*modEnv*squareOp+.30*triangle*modEnv*triOp;
carrierWave = sin(carrierPhase+2.0*ma.PI*phaseModulation);
shape(x) = (1.0-drive)*x+drive*ma.tanh(x*(1.0+31.0*drive))/ma.tanh(1.0+31.0*drive);
colored = (1.0-afterBody)*bodyEnvelope*shape(carrierWave)+afterBody*shape(bodyEnvelope*carrierWave);
process = seen*level*vel*releaseEnvelope*colored;
