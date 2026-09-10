// perc-pm 0.1: independently authored tuned/inharmonic percussion.
// PC Carbon / Model:Cycles Perc are behavioural references, not copied algorithms.
import("stdfaust.lib");
declare name "perc-pm";
declare version "0.1.0-experiment";

pitch=hslider("pitch_hz",145,35,1800,.001);
sweep=hslider("sweep",.28,0,1,.001);
punch=hslider("punch",.32,0,1,.001);
decay=hslider("decay",.45,0,1,.001);
inharm=hslider("inharmonicity",.36,0,1,.001);
modulation=hslider("modulation",.38,0,1,.001);
contour=hslider("mod_envelope",.58,0,1,.001);
drive=hslider("drive",.08,0,1,.001);
gate=button("gate");velocity=hslider("velocity",1,0,1,.001);

hit=gate>gate';
latched(x)=ba.sAndH(hit,x);
seen=max(hit)~_;
f0=latched(pitch); sw=latched(sweep); pu=latched(punch);
decayValue=latched(decay); ih=latched(inharm); md=latched(modulation);
ct=latched(contour); dr=latched(drive); vel=latched(velocity);
age=(+(1.0):min(30.0*ma.SR):*(1.0-hit))~_;
t=age/ma.SR;

// The sweep amount and duration are deliberately coupled as one compact macro,
// as on PC Carbon's published control description. Exact curves are authored.
sweepRatio=pow(2.0,3.0*sw);
sweepTau=.004+.075*sw*sw;
f=f0*(1.0+(sweepRatio-1.0)*exp(-t/sweepTau));

// Body and modulation decay independently so the same body can become a knock,
// ringing block or brittle transient without a hidden effect tail.
bodyTau=.018*pow(85.0,decayValue);
attackTau=.00016+.0009*(1.0-pu)*(1.0-pu);
amp=(1.0-exp(-t/attackTau))*exp(-t/bodyTau);
modTau=.006+.22*(1.0-ct)*(1.0-ct);
modEnv=(1.0-ct)+ct*exp(-t/modTau);

// Inharmonicity moves continuously from near-integer/tonal ratios into stretched
// partial relationships. These ratios are our design, not recovered Elektron data.
r1=1.0+1.15*ih+1.95*ih*ih;
r2=2.0+2.4*ih+3.1*ih*ih;
r3=3.0+4.6*ih*ih;
phase(hz)=os.hs_phasor(1.0,hz,hit);
p0=2.0*ma.PI*phase(f);
p1=2.0*ma.PI*phase(f*r1);
p2=2.0*ma.PI*phase(f*r2);
p3=2.0*ma.PI*phase(f*r3);

// Bounded PM network. Modulation and feedback grow nonlinearly for useful detail
// at the low end while retaining abrasive territory near the top.
index=md*(.10+4.6*md)*modEnv;
feedback=.58*md*md*modEnv;
fb=loop~_ with { loop(previous)=sin(p1+feedback*(1.0-hit)*previous); };
body=sin(p0+index*(.62*fb+.25*sin(p2)+.13*sin(p3)));
partials=.72*body+.18*sin(p1+.42*index*sin(p2))+.10*sin(p3);

// Punch coordinates a very short impact layer and early nonlinear emphasis.
impact=pu*pu*exp(-t/(.0007+.0018*(1.0-pu)))
       *(.65*sin(2.0*ma.PI*phase(min(.42*ma.SR,f0*(7.0+9.0*ih))))+.35*no.noise);
effectiveDrive=min(1.0,dr+.28*pu*exp(-t/.006));
amount=effectiveDrive*effectiveDrive;
shape(x)=(1.0-amount)*x+amount*ma.tanh(x*(1.0+10.0*amount))/ma.tanh(1.0+10.0*amount);
process=((partials+.18*impact):shape:fi.dcblockerat(8))*amp*.34*vel*seen;
