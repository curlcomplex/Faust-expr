// Tracker consumer adaptation of playable-06 at this branch.
// Canonical compact mapping: ../controls.lib over ../../candidates/color-04.dsp.
// Consumer contract: PITCH MIDI note; P1..P7 = Sweep, Punch, Decay, Color,
// Shape, Contour, Drive; P8 reserved. PRE drive profile. Trigger-only tail.
// This is an independent authored instrument, not a recovered Elektron algorithm.
import("stdfaust.lib");

declare name "Tracker shared PM kick audition";
declare version "faust-expr-e600f98-tracker-01";

pitch = hslider("PITCH", 60.0, 0.0, 127.0, 1.0);
velocity = hslider("VELOCITY", 1.0, 0.0, 1.0, 0.001);
gate = button("GATE");
p1 = hslider("P1", 0.50, 0.0, 1.0, 0.001);
p2 = hslider("P2", 0.45, 0.0, 1.0, 0.001);
p3 = hslider("P3", 0.50, 0.0, 1.0, 0.001);
p4 = hslider("P4", 0.25, 0.0, 1.0, 0.001);
p5 = hslider("P5", 0.25, 0.0, 1.0, 0.001);
p6 = hslider("P6", 0.70, 0.0, 1.0, 0.001);
p7 = hslider("P7", 0.20, 0.0, 1.0, 0.001);
p8 = hslider("P8", 0.0, 0.0, 1.0, 0.001);

onset = gate > gate';
frequencyTarget = 440.0 * pow(2.0, (pitch - 69.0) / 12.0);
coefficient = exp(-1.0 / (0.003 * ma.SR));
follow(x) = loop ~ _ with {
    loop(previous) = select2(onset,
        x * (1.0 - coefficient) + previous * coefficient,
        x);
};

frequency = exp(follow(log(frequencyTarget)));
sweep = follow(p1);
punch = follow(p2);
decay = follow(p3);
color = follow(p4);
shape = follow(p5);
contour = follow(p6);
drive = follow(p7);

pitchAmount = frequency * (pow(2.0, 3.5 * sweep) - 1.0);
pitchTau = 0.055 * pow(0.1, punch);
attackTau = 0.0015 * pow(0.1, punch);
bodyTau = 0.018 * pow(100.0, decay);
modTau = 0.012 * pow(16.0, 1.0 - contour);

age = (+(1.0) : min(60.0 * ma.SR) : *(1.0 - onset)) ~ _;
t = age / ma.SR;
seen = max(onset) ~ _;
vel = ba.sAndH(onset, velocity);
frequencyNow = frequency + pitchAmount * exp(-t / pitchTau);
bodyEnvelope = (1.0 - exp(-t / attackTau)) * exp(-t / bodyTau);

phase(hz) = os.hs_phasor(1.0, hz, onset);
carrierPhase = 2.0 * ma.PI * phase(frequencyNow);
squarePhase = 2.0 * ma.PI * phase(2.0 * frequencyNow);
squareOp = (sin(squarePhase)
    + sin(3.0 * squarePhase) / 3.0
    + sin(5.0 * squarePhase) / 5.0)
    / (1.0 + 1.0 / 3.0 + 1.0 / 5.0);
triPhase = 2.0 * ma.PI * phase(3.0 * frequencyNow);
modEnvelope = (1.0 - contour) + contour * exp(-t / modTau);
triShape(x) = 2.0 / ma.PI * asin(sin(x));
triOp = (*(0.85 * shape * shape * modEnvelope * (1.0 - onset))
    : +(triPhase)
    : triShape) ~ _;
phaseModulation = 0.18 * color * color * modEnvelope * squareOp
    + 0.30 * shape * shape * modEnvelope * triOp;
carrier = sin(carrierPhase + 2.0 * ma.PI * phaseModulation);

shapeDrive(x) = (1.0 - drive * drive) * x
    + drive * drive
        * ma.tanh(x * (1.0 + 31.0 * drive * drive))
        / ma.tanh(1.0 + 31.0 * drive * drive);

// Tracker KICK is trigger-only. Gate-off must not shorten its body decay.
process = 0.60 * seen * vel * bodyEnvelope * shapeDrive(carrier), 0.0;
