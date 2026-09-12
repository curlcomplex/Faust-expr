declare name "analog-classics-vintage-rack-reverb";
declare version "0.1.1-experiment";
declare description "Compact vintage digital rack reverb candidate; early-MIDIVerb character target";
import("stdfaust.lib");

decay = hslider("decay",0.58,0,1,0.001) : si.smooth(0.999);
size = hslider("size",0.52,0,1,0.001) : si.smooth(0.999);
tone = hslider("tone",0.46,0,1,0.001) : si.smooth(0.999);
character = hslider("character",0.64,0,1,0.001) : si.smooth(0.999);
mix = hslider("mix",0.35,0,1,0.001) : si.smooth(0.999);

// Deliberately compact candidate. Freeverb's spread changes delay-line topology,
// so keep it compile-time constant. Size changes the live decay/diffusion feel
// without pretending a structural delay length can be moved as a normal control.
combFeed = 0.52 + 0.35*decay + 0.07*size;
allpassFeed = 0.5;
damp = 0.08 + 0.82*(1-tone);
spread = 23;

soft(x) = ma.tanh(x*(1+2.5*character))/(1+0.65*character);
wetgain = 0.7 + 0.45*character;
wetnet = re.stereo_freeverb(combFeed,allpassFeed,damp,spread) : par(i,2,soft);

// Follow the standard Faust Freeverb dry/wet fanout shape explicitly.
process = _,_ <: (*(mix*wetgain),*(mix*wetgain) : wetnet), *(1-mix), *(1-mix) :> _,_;
