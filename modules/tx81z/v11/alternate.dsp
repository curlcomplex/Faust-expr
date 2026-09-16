declare name "TX81Z extracted-block alternate composition";
declare version "0.11.0-block-example";
import("stdfaust.lib"); b=library("blocks.lib"); opz=library("../v5/opz_core.lib");
gate=button("gate[curlop:input]"); freq=hslider("freq[unit:Hz][scale:log][curlop:input]",110,20,2000,.01); index=hslider("index",0.5,0,1,.001); wave=nentry("wave",0,0,7,1);
bf=opz.fromHz(freq); tick=opz.egClock:(_,!); count=opz.egClock:(!,_);
egCarrier=b.envelope(gate,tick,count,bf,31,10,0,6,8,0,0); egMod=b.envelope(gate,tick,count,bf,31,18,8,12,6,0,0);
pCarrier=b.phase(b.phaseStep(bf,0,1,0,0,0,0,0),gate); pMod=b.phase(b.phaseStep(bf,0,2,0,0,0,0,0),gate);
mod=b.op(pMod,egMod,int(90.0*(1.0-index)),wave); carrier=b.op(pCarrier+(int(mod)>>1),egCarrier,8,0);
process=float(carrier)*(1.0/32768.0)*0.6;
