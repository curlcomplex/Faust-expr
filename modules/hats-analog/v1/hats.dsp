// Analog Hats v1 — sample-free, Mutant/808-family inspired but not a circuit clone.
declare name "hats-analog";
declare version "0.1.0-experiment";
import("stdfaust.lib");

metal = hslider("metal", .72, 0, 1, .001);
tone = hslider("tone", .58, 0, 1, .001);
decay = hslider("decay", .42, 0, 1, .001);
shape = hslider("shape", .52, 0, 1, .001);
choke = hslider("choke", .70, 0, 1, .001);
drive = hslider("drive", .12, 0, 1, .001);
pitch = hslider("pitch_hz", 1.0, .60, 1.70, .001);
articulation = hslider("articulation", 0, 0, 1, 1); // 0 closed, 1 open
gate = button("gate");
velocity = hslider("velocity", 1, 0, 1, .001);

hit = gate > gate';
openHit = hit * float(articulation >= .5);
closedHit = hit * float(articulation < .5);
seenOpen = max(openHit) ~ _;
seenClosed = max(closedHit) ~ _;

// Musical controls latch independently on each onset. Choke is sampled on the
// closed onset that performs it; this makes per-step choke strength deterministic.
latched(x) = ba.sAndH(hit,x);
m = latched(metal); tn = latched(tone); dc = latched(decay);
sh = latched(shape); dr = latched(drive); pit = latched(pitch); vel = latched(velocity);
ch = ba.sAndH(closedHit,choke);

maxAge = 8.0*ma.SR;
openAge = (+(1.0):min(maxAge):*(1.0-openHit)) ~ _;
closedAge = (+(1.0):min(maxAge):*(1.0-closedHit)) ~ _;
chokeAge = (+(1.0):min(maxAge):*(1.0-closedHit)) ~ _;
// Reset old choke state whenever a fresh open hat starts; arm it on a later CH hit.
chokeActive = (+(closedHit):min(1.0):*(1.0-openHit)) ~ _;
to = openAge/ma.SR; tc = closedAge/ma.SR; tch = chokeAge/ma.SR;

// Six free-running square oscillators around the classic electro-hat region.
// Shape stretches alternate oscillators and crossfades from a simple summed bank
// toward ring-combined metallic clusters: useful range without sample playback.
sq(f) = 2.0*float(os.hs_phasor(1.0,f,0) >= .5)-1.0;
spread = .06*(2.0*sh-1.0);
f1=205.3*pit*(1.0-spread); f2=304.4*pit*(1.0+.65*spread);
f3=369.6*pit*(1.0-1.10*spread); f4=522.7*pit*(1.0+.35*spread);
f5=540.5*pit*(1.0+1.25*spread); f6=800.0*pit*(1.0-.45*spread);
s1=sq(f1);s2=sq(f2);s3=sq(f3);s4=sq(f4);s5=sq(f5);s6=sq(f6);
summed=(s1+s2+s3+s4+s5+s6)/6.0;
ringed=(s1*s2+s2*s4+s3*s5+s4*s6)/4.0;
metallic=(1.0-sh)*summed+sh*ringed;
noise=no.noise*.55;
excitation=m*metallic+(1.0-m)*noise;

// Resonant high band. Tone moves both the HP corner and a broad resonant focus.
hp=2600.0+7200.0*tn*tn;
focus=5600.0+8500.0*tn;
bright=excitation:fi.highpass(2,hp):fi.lowpass(1,min(.44*ma.SR,19000.0));
res=excitation:fi.resonbp(min(.40*ma.SR,focus),1.35+.9*sh,1);
filtered=.74*bright+.34*res;

// Decay covers useful CH and OH territory rather than copying one fixed 808 CH.
closedTau=.010*pow(8.0,dc);            // ~10–80 ms
openTau=.085*pow(18.0,dc);             // ~85–1530 ms
attack=.00008+.00055*(1.0-sh);
aC=(1.0-exp(-tc/attack)); aO=(1.0-exp(-to/(attack*1.4)));
closedEnv=aC*exp(-tc/closedTau)*seenClosed;
openBase=aO*(.74*exp(-to/openTau)+.26*exp(-to/(2.6*openTau)))*seenOpen;
// Choke 0 = effectively no interaction, .5 = musical fade, 1 = hard/exclusive.
chokeTau=.00018*pow(25000.0,1.0-ch);
chokeGain=(1.0-chokeActive)+chokeActive*exp(-tch/chokeTau);
openEnv=openBase*chokeGain;

// Shape also adds a very short bright tick so high settings get crisp rather than
// simply harsher. It follows either articulation's onset independently.
shortTau=.0012+.004*(1.0-sh);
tickEnv=sh*(exp(-tc/shortTau)*seenClosed+exp(-to/(1.8*shortTau))*seenOpen);
tick=(excitation:fi.highpass(1,min(.40*ma.SR,9000.0)))*tickEnv*.22;

raw=filtered*(closedEnv+openEnv)+tick;
a=dr*dr;
sat=(1.0-a)*raw+a*ma.tanh((1.0+8.0*a)*raw)/(1.0+1.8*a);
process=sat*.72*vel:fi.dcblockerat(20);
