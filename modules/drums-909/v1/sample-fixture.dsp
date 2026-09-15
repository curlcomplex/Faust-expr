declare name "909 resource diagnostic - NOT A DRUM";
import("stdfaust.lib");
p=library("sample-player.lib");
gate=button("gate");freq=hslider("freq",440,110,1760,.001);
velocity=hslider("velocity",1,0,1,.001);chokeGate=button("chokeGate");
// Original diagnostic ramp; table generator runs at initialization, not synth audio.
// Both endpoints nonzero intentionally expose missing EOF masking.
wf=64,(-.6+1.2*(ba.time%64)/63);
process=p.play(wf,8000,440,gate,freq,velocity,chokeGate);
