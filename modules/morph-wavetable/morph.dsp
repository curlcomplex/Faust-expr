// morph-wavetable 0.1 — single-note spectral morph/unison instrument.
// Independent design: no Nord/Elektron tables, samples, chord or scale logic.
import("stdfaust.lib");
declare name "morph-wavetable";
declare version "0.1.0-experiment";

pitch = hslider("pitch_hz",220,20,8000,.001);
morph = hslider("morph",.28,0,1,.001);
shape = hslider("shape",.45,0,1,.001);
detune = hslider("detune",.18,0,1,.001);
stack = hslider("stack",2,1,4,1);
decay = hslider("decay",.55,0,1,.001);
drive = hslider("drive",.08,0,1,.001);
velocity = hslider("velocity",1,0,1,.001);
gate = button("gate");

hit = gate > gate';
seen = max(hit) ~ _;
latched(x) = ba.sAndH(hit,x);
vel = latched(velocity);
stackN = int(latched(stack));
releaseTau = latched(.035*pow(100.0,decay));
// Live controls use 3 ms smoothing, but snap to their requested value on onset.
coef = exp(-1.0/(.003*ma.SR));
follow(x) = loop ~ _ with { loop(previous)=select2(hit,x*(1-coef)+previous*coef,x); };
freq = exp(follow(log(pitch)));
pos = 7.0*follow(morph);
sh = follow(shape);
det = follow(detune);
drv = follow(drive);

age = (+(1.0) : min(60.0*ma.SR) : *(1.0-hit)) ~ _;
t = age/ma.SR;
releaseAge = (+(1.0) : min(60.0*ma.SR) : *(1.0-gate)) ~ _;
rt = releaseAge/ma.SR;
attack = 1.0-exp(-t/.0025);
release = gate + (1.0-gate)*exp(-rt/max(.035,releaseTau));
env = seen*attack*release;

// Eight independently authored spectral frames. They are formulas, not copied tables.
// Adjacent-frame triangular interpolation is equivalent to traversing an authored
// wavetable family, while allowing register-aware harmonic suppression.
w(k)=max(0.0,1.0-abs(pos-k));
odd(n)=n%2;
triSign(n)=1-2*(int((n-1)/2)%2);
f0(n)=(n==1);
f1(n)=odd(n)*triSign(n)/(n*n);
f2(n)=1.0/n;
f3(n)=odd(n)/n;
f4(n)=exp(-.55*(n-3)*(n-3))*.85 + exp(-.22*(n-7)*(n-7))*.35;
f5(n)=((n==1)*.7+(n==2)*.2+(n==4)*.55+(n==8)*.32+(n==12)*.18);
f6(n)=odd(n)*exp(-.16*(n-5)*(n-5));
f7(n)=(1-2*((n+1)%2))/pow(n,.72);
baseCoeff(n)=f0(n)*w(0)+f1(n)*w(1)+f2(n)*w(2)+f3(n)*w(3)+f4(n)*w(4)+f5(n)*w(5)+f6(n)*w(6)+f7(n)*w(7);
// Shape tilts the spectrum around a neutral midpoint, preserving frame identity.
shapeCoeff(n)=baseCoeff(n)*pow(n,1.15*(sh-.5));
// Fade harmonics before Nyquist instead of hard-switching them.
band(n,hz)=min(1.0,max(0.0,(.47*ma.SR-n*hz)/(.07*ma.SR)));
waveAt(hz,phaseOffset)=par(i,16,
    shapeCoeff(i+1)*band(i+1,hz)*sin(2*ma.PI*((i+1)*(os.hsp_phasor(1.0,hz,hit,phaseOffset))))) :> _;
// Nord-like inspiration: same-note unison stacking, never chord intervals.
// Max detune is +/- 28 cents. Fixed phase offsets avoid identical attacks.
cents=28.0*det;
ratio(c)=pow(2.0,c/1200.0);
o0=waveAt(freq*ratio(-1.5*cents),0.03);
o1=waveAt(freq*ratio(-.5*cents),0.29);
o2=waveAt(freq*ratio(.5*cents),0.57);
o3=waveAt(freq*ratio(1.5*cents),0.81);
active(i)=stackN>i;
raw=(o0*active(0)+o1*active(1)+o2*active(2)+o3*active(3))/sqrt(max(1,stackN));
// Neutral at zero. Drive is after stacking so unison beating reaches the shaper.
shaped=(1-drv*drv)*raw + drv*drv*ma.tanh(raw*(1+7*drv))/ma.tanh(1+7*drv);
process=.23*vel*env*shaped : fi.dcblockerat(8);
