// Offline laboratory patch. No consumer host or hidden voice allocator.
ac=library("../../acid-voice/v1/engine.lib");
bs=library("../../bassline-seq/v1/engine.lib");
toVoice(f,g,a,s,position)=ac.voice(f,g,1,a,s);
process=bs.sequence:toVoice;
