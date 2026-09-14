declare name "analog-classics-tape-echo";
declare version "0.3.0-reference-candidate";
declare description "Three-head tape echo retaining Space-Echo-style topology with Airwindows TapeDelay-informed time chase and magnetic smear";
declare license "MIT";
declare reference "Airwindows TapeDelay (MIT) used as oracle for delay-time chase and lean/fat smear only; multi-head topology remains CURLOP-authored";
import("stdfaust.lib");
u=library("transport.lib");

time = hslider("time",0.36,0.06,0.78,0.001);
feedback = hslider("feedback",0.48,0,0.96,0.001) : si.smooth(exp(-1/(.005*ma.SR)));
tone = hslider("tone",0.58,0,1,0.001) : si.smooth(exp(-1/(.005*ma.SR)));
age = hslider("age",0.32,0,1,0.001) : si.smooth(exp(-1/(.005*ma.SR)));
drive = hslider("drive",0.18,0,1,0.001) : si.smooth(exp(-1/(.005*ma.SR)));
head1 = hslider("head1",1,0,1,1);
head2 = hslider("head2",0.65,0,1,0.05);
head3 = hslider("head3",0.8,0,1,0.05);
mix = hslider("mix",0.38,0,1,0.001) : si.smooth(exp(-1/(.005*ma.SR)));

MAX = 131072;
// The target controller follows TapeDelay's integer chase exactly. Audio remains
// our three-head fractional-delay design, not TapeDelay's resizing ring buffer.
// time is seconds; the 9000 controller threshold remains in original samples.
targetSamp = int(time*ma.SR);
chasedSamp = u.chase(targetSamp);
// Keep CURLOP's three playback heads. Age adds slow capstan drift around the
// chased tape length, not independent arbitrary modulation per head.
drift = (0.00188*age)*ma.SR*os.osc(0.23+0.53*age);
d1 = min(MAX-8,max(4,chasedSamp*0.50 + drift));
d2 = min(MAX-8,max(4,chasedSamp*0.75 + drift*0.72));
d3 = min(MAX-8,max(4,chasedSamp + drift*0.41));

// TapeDelay's Lean/Fat path averages a selectable family of short, irregular
// history taps. Here we retain that idea as a bounded magnetic-smear stage in
// the feedback write path instead of pretending TapeDelay is itself multi-head.
smearAmt = 0.55*age;
smear(x) = x*(1-smearAmt) + (x : de.delay(128,3))*smearAmt*0.20
                         + (x : de.delay(128,11))*smearAmt*0.18
                         + (x : de.delay(128,23))*smearAmt*0.16
                         + (x : de.delay(128,47))*smearAmt*0.14
                         + (x : de.delay(128,89))*smearAmt*0.12;
cut = 1700 + 15000*tone*(1-0.62*age);
fb = feedback*(0.80+0.18*(1-age));
satgain = 1 + 8*drive;
sat(x) = ma.tanh(x*satgain)/(1+0.6*drive);
feedbackPath = smear : fi.lowpass(1,cut) : sat : *(fb);
// All playback heads read the input-plus-feedback signal. In v1/v2 only the
// third output branch entered the loop, leaving heads 1/2 without repeats.
// One delayed third-head signal feeds back even when that head is inaudible.
writeSignal = + ~ (de.fdelay4(MAX,d3) : feedbackPath);
heads = writeSignal <: (de.fdelay4(MAX,d1):*(head1)),(de.fdelay4(MAX,d2):*(head2)),(de.fdelay4(MAX,d3):*(head3)) :> _ : *(0.42);
mono(x) = x*(1-mix) + (x:heads)*mix;
process = par(i,2,mono);
