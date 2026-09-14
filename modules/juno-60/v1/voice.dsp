declare name "Analog Classics Juno 60 Voice";
declare version "0.1.0-reference-candidate";
declare description "Single-note Juno-60-inspired voice; host owns polyphony and chorus is outside the note kernel";

import("stdfaust.lib");
cs = library("../../analog-classics/synth-batch/common.lib");

gate = button("gate[curlop:input]");
freq = hslider("freq[unit:Hz][scale:log][curlop:input]", 220, 20, 8000, .01);
velocity = hslider("velocity[curlop:input]", 1, 0, 1, .001);

saw = hslider("saw", .70, 0, 1, .001) : cs.sm(.005);
pulse = hslider("pulse", .30, 0, 1, .001) : cs.sm(.005);
sub = hslider("sub", .45, 0, 1, .001) : cs.sm(.005);
noise = hslider("noise", .015, 0, 1, .001) : cs.sm(.005);
pwm = hslider("pwm", .50, .05, .95, .001) : cs.sm(.005);
pwmDepth = hslider("pwmDepth", .18, 0, .45, .001) : cs.sm(.005);
lfoRate = hslider("lfoRate[unit:Hz][scale:log]", 4.8, .05, 20, .01) : cs.sm(.005);

cutoff = hslider("cutoff[unit:Hz][scale:log]", 1800, 40, 16000, 1) : log : cs.sm(.005) : exp;
res = hslider("resonance", .22, 0, .98, .001) : cs.sm(.005);
hpf = hslider("hpf[unit:Hz][scale:log]", 30, 20, 1200, 1) : log : cs.sm(.005) : exp;
envAmt = hslider("filterEnv", .38, 0, 1, .001) : cs.sm(.005);
keyTrack = hslider("keyTrack", .35, 0, 1, .001) : cs.sm(.005);

attack = hslider("attack[unit:s][scale:log]", .012, .001, 3, .001) : cs.sm(.005);
decay = hslider("decay[unit:s][scale:log]", .30, .005, 4, .001) : cs.sm(.005);
sustain = hslider("sustain", .72, 0, 1, .001) : cs.sm(.005);
release = hslider("release[unit:s][scale:log]", .50, .005, 6, .001) : cs.sm(.005);
level = hslider("level", .70, 0, 1, .001) : cs.sm(.005);

clip(x, lo, hi) = min(hi, max(lo, x));
softclip(x) = tanh(x);

// Clean-room DCO architecture informed by Juno-family behavior: one master pitch,
// saw + pulse + sub + noise, with mild bus compression as sources are stacked.
dco(f, width) = (raw * mixComp) with {
    sawSig = os.polyblep_saw(f) * saw;
    pulseSig = os.pulsetrain(f, width) * pulse;
    subSig = os.polyblep_square(f * .5) * sub * .72;
    noiseSig = no.noise * noise * .10;
    raw = sawSig + pulseSig + subSig + noiseSig;
    sumLevel = saw + pulse + sub + noise;
    mixComp = 1.0 / (1.0 + .22 * max(0, sumLevel - 1.0));
};

// Four cascaded one-pole stages with nonlinear input/feedback. This is a
// clean-room Juno-family reference candidate, not copied from Hera's GPL DSP.
vcf4(fc, r) = (+ : stage : stage : stage : stage) ~ feedback with {
    g = tan(ma.PI * clip(fc, 20, ma.SR * .22) / ma.SR);
    G = g / (1 + g);
    stage(x) = y letrec {
        's = s + 2 * G * (softclip(x) - s);
        y = s;
    };
    feedback(x) = -3.85 * r * softclip(x);
};

state(f0, g0, v0) = audio, pitchHz, env, hit with {
    pitchHz = cs.pitch(f0, g0, 0, 0);
    hit = (g0 > 0) > (g0' > 0);
    width = clip(pwm + pwmDepth * os.osc(lfoRate), .05, .95);
    f = clip(pitchHz, 10, ma.SR * .20);
    env = en.adsr(max(.001, attack), max(.005, decay), sustain, max(.005, release), g0 > 0);
    keyRatio = max(.25, pitchHz / 261.625565);
    fc = cutoff * pow(2, 4.5 * envAmt * env) * pow(keyRatio, keyTrack);
    filtered = dco(f, width) : vcf4(fc, res) : fi.highpass(1, max(20, hpf));
    audio = (softclip(filtered * 1.15) : fi.dcblockerat(20)) * env * cs.vel(g0, v0) * level * .82;
};

voice(f0, g0, v0) = state(f0, g0, v0) : (_, !, !, !);
diagnostics(f0, g0, v0) = state(f0, g0, v0) : (!, _, _, _);

process = voice(freq, gate, velocity);
