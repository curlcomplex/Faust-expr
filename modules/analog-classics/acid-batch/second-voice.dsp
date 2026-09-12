// Diagnostic second consumer, not a new approved instrument.
import("stdfaust.lib");
bs=library("../../bassline-seq/v1/engine.lib");
toVoice(f,g,a,s,position)=os.osc(f)*(.15+.1*a)*(g:si.smoo);
process=bs.sequence:toVoice;
