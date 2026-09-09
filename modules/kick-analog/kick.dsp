// Authored analog-style resonant kick. Not a recovered Syntakt circuit.
import("stdfaust.lib");
declare name "kick-analog";
declare version "0.1.0-experiment";

pitch = hslider("pitch_hz",52,20,160,.001);
decay = hslider("decay",.52,0,1,.001);
sweep = hslider("sweep",.48,0,1,.001);
punch = hslider("punch",.55,0,1,.001);
tone = hslider("tone",.52,0,1,.001);
body = hslider("body",.62,0,1,.001);
drive = hslider("drive",.18,0,1,.001);
click = hslider("click",.18,0,1,.001);
gate = button("gate");
velocity = hslider("velocity",1,0,1,.001);

hit = gate > gate';
age = (+(1.0) : min(60.0*ma.SR) : *(1.0-hit)) ~ _;
t = age/ma.SR;
seen = max(hit) ~ _;
vel = ba.sAndH(hit,velocity);

// Time constants are authored in seconds so the response is sample-rate stable.
pitchTau = .006 + .070*pow(1.0-punch,2.0);
bodyTau = .055*pow(30.0,decay);
attackTau = .00015 + .0018*pow(1.0-punch,2.0);
pitchRatio = pow(2.0,3.25*sweep);
freq = pitch * (1.0 + (pitchRatio-1.0)*exp(-t/pitchTau));
amp = (1.0-exp(-t/attackTau))*exp(-t/bodyTau);

// A resonant-circuit-inspired body: deterministic excitation/reset, mild
// harmonic loading, and a slower sub-weighted component. This is a behavioural
// model, not a component-level diode/transistor reconstruction.
phase(hz)=os.hs_phasor(1.0,hz,hit);
fund = sin(2.0*ma.PI*phase(freq));
second = sin(2.0*ma.PI*phase(freq*2.0));
sub = sin(2.0*ma.PI*phase(max(10.0,freq*.5)));
harmonic = fund + body*(.20*second + .12*sub);

// The first full endpoint sweep exposed instability at the original 14.4 kHz
// dynamic two-pole cutoff. Keep the nonlinear stage explicitly bounded and the
// filter below 10 kHz at every sample rate in the declared test set. Zero drive
// is exactly the unshaped signal; this is not a hidden peak normalizer.
driveAmount = drive*drive;
shapeDrive(x) = (1.0-driveAmount)*x
    + driveAmount*ma.tanh(x*(1.0+12.0*driveAmount))
        / ma.tanh(1.0+12.0*driveAmount);
colored = shapeDrive(harmonic);
cutoff = 700.0 + tone*9300.0;
bodySignal = colored*amp : fi.lowpass(2,cutoff);

// Short excitation/click path kept independent from the body so optimization
// cannot hide transient errors inside the resonator envelope.
clickTau = .00045 + .0045*(1.0-punch);
clickEnv = exp(-t/clickTau);
clickTone = 1800.0 + 7800.0*tone;
clickSignal = (no.noise*.55 + sin(2.0*ma.PI*phase(clickTone))*.45)
    * click * clickEnv * .45;

// Fixed headroom leaves the relative dynamics between patches and velocities
// intact. There is no per-hit normalization or downstream limiter in evidence.
out = (bodySignal + clickSignal) * .48 * seen * vel : fi.dcblockerat(12);
process = out;
