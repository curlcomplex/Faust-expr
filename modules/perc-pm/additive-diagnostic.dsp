// Controlled ablation: preserve ratios, envelopes, impact and drive; remove PM
// index AND the internal modulator feedback. Not an automatic shipping voice.
e = library("perc.dsp")[index=0.0; feedback=0.0;];
process = e.process;
