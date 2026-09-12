declare name "analog-classics-vintage-rack-reverb";
declare version "0.1.0-experiment";
declare description "Compact vintage digital rack reverb candidate; early-MIDIVerb character target";
import("stdfaust.lib");

decay = hslider("decay",0.58,0,1,0.001) : si.smooth(0.999);
size = hslider("size",0.52,0,1,0.001) : si.smooth(0.999);
tone = hslider("tone",0.46,0,1,0.001) : si.smooth(0.999);
character = hslider("character",0.64,0,1,0.001) : si.smooth(0.999);
mix = hslider("mix",0.35,0,1,0.001) : si.smooth(0.999);

// Deliberately compact candidate. The 0.5 allpass coefficient is the old-rack
// direction suggested by early MIDIVerb implementations; this is not a ROM clone.
combFeed = 0.55 + 0.41*decay;
allpassFeed = 0.5;
damp = 0.08 + 0.82*(1-tone);
spread = int(7 + 29*size);

soft(x) = ma.tanh(x*(1+2.5*character))/(1+0.65*character);
wetnet = re.stereo_freeverb(combFeed,allpassFeed,damp,spread) : par(i,2,soft);
wetgain = 0.7 + 0.45*character;

process = _,_ <: (*(1-mix),*(1-mix)),(wetnet : *(mix*wetgain),*(mix*wetgain)) :> _,_;
