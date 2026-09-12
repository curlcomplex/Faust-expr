tb=library("teebee.lib");
process=tb.filter(hslider("cutoff",800,200,6000,1),hslider("resonance",.5,0,.92,.001));
