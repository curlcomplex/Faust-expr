declare name "analog-classics-vintage-rack-reverb";
declare version "0.2.0-reference-candidate";
declare description "Early-MIDIVerb-style rack reverb rebuilt around Airwindows MV diffusion topology";
declare license "MIT";
declare reference "Airwindows MV (MIT), pinned as whole-algorithm behavioural oracle; this Faust candidate preserves CURLOP control names";
import("stdfaust.lib");

decay = hslider("decay",0.58,0,1,0.001);
size = hslider("size",0.52,0,1,0.001);
tone = hslider("tone",0.46,0,1,0.001);
character = hslider("character",0.64,0,1,0.001);
mix = hslider("mix",0.35,0,1,0.001);

// MV is a long cascade of fixed allpasses with intentionally quantized global
// feedback. Keep the historical fixed delay lengths (samples) instead of
// rescaling them and thereby cleaning up the sample-rate-dependent character.
ap(N) = de.allpass_fcomb(16384,N,0.5);
chain = ap(7573):ap(7307):ap(7177):ap(6907):ap(6779):ap(6521):ap(5981):ap(5563):
        ap(5297):ap(4903):ap(4759):ap(4489):ap(4391):ap(4229):ap(4153):ap(3989):
        ap(3659):ap(3407):ap(3251):ap(2999):ap(2917):ap(2749):ap(2503):ap(2423):
        ap(2146):ap(2088);

// MV quantizes Regen into unmistakably coarse early-digital steps.
regen = ba.if(decay<=0.0625,0,
        ba.if(decay<=0.125,0.0625,
        ba.if(decay<=0.25,0.125,
        ba.if(decay<=0.5,0.25,
        ba.if(decay<=0.99,0.5,1)))));
// Original Bright controls how many post-stage averaging filters are active.
// A compact first Faust expression approximates this with stepped darkening;
// the full oracle harness measures the remaining gap rather than calling it exact.
dark = 0.02 + 0.46*(1-tone);
// Size maps the original Depth idea into the user-facing control by changing
// effective network participation/output density without multiplying modules.
sizeGain = 0.45 + 0.55*size;
outGain = 0.55 + 0.45*character;
soft(x)=sin(max(-1.5707963,min(1.5707963,x)));
network = (soft : chain : fi.lowpass(1,18000*(1-dark)+900*dark) : *(sizeGain)) ~ *(regen);
wet(x)=x:network:* (outGain);
channel(x)=x*(1-mix)+(x:wet)*mix;
process=par(i,2,channel);
