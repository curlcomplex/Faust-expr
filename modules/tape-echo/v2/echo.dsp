declare name "analog-classics-tape-echo";
declare version "0.2.0-reference-candidate";
declare description "Three-head tape echo retaining Space-Echo-style topology with Airwindows TapeDelay-informed time chase and magnetic smear";
declare license "MIT";
declare reference "Airwindows TapeDelay (MIT) used as oracle for delay-time chase and lean/fat smear only; multi-head topology remains CURLOP-authored";
import("stdfaust.lib");

time = hslider("time",0.36,0.06,0.78,0.001);
feedback = hslider("feedback",0.48,0,0.96,0.001) : si.smooth(0.999);
tone = hslider("tone",0.58,0,1,0.001) : si.smooth(0.999);
age = hslider("age",0.32,0,1,0.001) : si.smooth(0.999);
drive = hslider("drive",0.18,0,1,0.001) : si.smooth(0.999);
head1 = hslider("head1",1,0,1,1);
head2 = hslider("head2",0.65,0,1,0.05);
head3 = hslider("head3",0.8,0,1,0.05);
mix = hslider("mix",0.38,0,1,0.001) : si.smooth(0.999);

MAX = 131072;
// Airwindows TapeDelay changes tape length gradually rather than interpolating a
// delay pointer directly. Approximate the same transport inertia with a very
// slow target chase; this deliberately creates pitch motion on time changes.
targetSamp = time*ma.SR;
chasedSamp = targetSamp : si.smooth(0.99994);
// Keep CURLOP's three playback heads. Age adds slow capstan drift around the
// chased tape length, not independent arbitrary modulation per head.
drift = (0.00008 + 0.0018*age)*ma.SR*os.osc(0.23+0.53*age);
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
mainLoop = (+ ~ (de.fdelay4(MAX,d3) : feedbackPath)) : de.fdelay4(MAX,d3);
heads = _ <: (de.fdelay4(MAX,d1):*(head1)),(de.fdelay4(MAX,d2):*(head2)),(mainLoop:*(head3)) :> _ : *(0.42);
mono(x) = x*(1-mix) + (x:heads)*mix;
process = par(i,2,mono);
