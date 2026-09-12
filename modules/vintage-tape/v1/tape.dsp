declare name "analog-classics-vintage-tape";
declare version "0.1.0-experiment";
declare description "Compact vintage tape coloration candidate derived from Airwindows IronOxideClassic2 topology";
declare license "MIT-derived; Airwindows IronOxideClassic2 attribution retained in issue #63";
import("stdfaust.lib");

input = hslider("input[unit:dB]",0,-18,18,0.01) : si.smooth(0.999);
speed = hslider("speed[unit:ips]",16.35,1.5,150,0.01) : si.smooth(0.999);
output = hslider("output[unit:dB]",0,-18,18,0.01) : si.smooth(0.999);

db2gain(x) = pow(10,x/20);
soft(x) = signum(x)*sin(min(abs(x),1.5707963267948966));
scale = ma.SR/44100.0;
ips = max(1,min(200,speed*1.1));
iirAmount = min(0.99,(ips/430.0)/scale);
fastTaper = 1.0 + (ips/15.0)/scale;
slowTaper = 1.0 + (2.0/(ips*ips))/scale;

// Airwindows' old Iron Oxide history weighting, expressed as sample-rate-scaled
// fractional taps. The groups preserve the successive /2 stages in the oracle.
tap(n) = de.fdelay4(512,min(508,max(1,n*scale)));
g1 = tap(130)+tap(116)+tap(112)+tap(110)+tap(106)+tap(104)+tap(100)+tap(92)+tap(86);
g2 = tap(82)+tap(76)+tap(74)+tap(70)+tap(64)+tap(62)+tap(56)+tap(50)+tap(46)+tap(44)+tap(40)+tap(34)+tap(32);
g3 = tap(26)+tap(22)+tap(20)+tap(16)+tap(14);
g4 = tap(10)+tap(8)+tap(6);
g5 = tap(5)+tap(4);
weighted = _ <: (g1:*(1.0/16.0)),(g2:*(1.0/8.0)),(g3:*(1.0/4.0)),(g4:*(1.0/2.0)),g5 :> _;

onepole(a) = *(a) : + ~ *(1-a);
accum(a) = + ~ *(a);
lean = _ <: _,onepole(iirAmount) : -;
fast = accum(1.0/fastTaper);
slow = weighted : *(1.0/128.0) : accum(1.0/slowTaper);
shape = _ <: fast,(slow:*(1.0/slowTaper)) : -;
post = soft : fi.lowpass(2,min(24000,ma.SR*0.49));
mono = *(db2gain(input)) : lean : soft : shape : post : *(db2gain(output));
process = par(i,2,mono);
