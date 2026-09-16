declare name "TX81Z OPZ working voice";
declare version "0.7.0-lfo-pm-am";
declare description "TX81Z-facing fixed frequency plus measured firmware LFO mappings over qualified OPZ core";
import("stdfaust.lib");
opz=library("../v5/opz_core.lib");
panel=library("../v6/panel_frequency.lib");
lfo=library("../v7/lfo.lib");

gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,4400,.01);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
algorithm=nentry("algorithm",1,1,8,1); feedback=nentry("feedback",0,0,7,1); level=hslider("level",0.5,0,1,.001);
blockFreq=nentry("blockFreq",-1,-1,8191,1);
lfoSpeed=nentry("lfoSpeed",35,0,99,1); lfoDelay=nentry("lfoDelay",0,0,99,1);
pmd=nentry("pModDepth",0,0,99,1); amd=nentry("aModDepth",0,0,99,1);
lfoSync=nentry("lfoSync",1,0,1,1); lfoWave=nentry("lfoWave",2,0,3,1);
pms=nentry("pModSens",3,0,7,1); ams=nentry("aModSens",0,0,3,1);
lfoPitchDelta=lfo.pmDelta(lfoSpeed,lfoDelay,pmd,pms,lfoWave,lfoSync,gate);
modFreq=freq*pow(2.0,float(lfoPitchDelta)/768.0);
bf=int(select2(blockFreq>=0,opz.fromHz(modFreq),blockFreq));
lfoAmpOffset=lfo.amOffset(lfoSpeed,lfoDelay,amd,ams,lfoWave,lfoSync,gate);
tick=opz.egClock:(_,!); count=opz.egClock:(!,_); a=int(algorithm)-1;

p1Wave=nentry("op1Wave",0,0,7,1); p1Mode=nentry("op1Mode",0,0,1,1);
p1Coarse=nentry("op1Coarse",1,0,15,1); p1FixedCRS=nentry("op1FixedCRS",0,0,63,1); p1Fine=nentry("op1Fine",0,0,15,1);
p1DT1=nentry("op1DT1",0,0,7,1); p1DT2=nentry("op1DT2",0,0,3,1); p1Range=nentry("op1Range",0,0,7,1); p1TL=nentry("op1TL",0,0,127,1); p1AME=nentry("op1AME",0,0,1,1);
p1AR=nentry("op1AR",31,0,31,1); p1D1R=nentry("op1D1R",12,0,31,1); p1D2R=nentry("op1D2R",0,0,31,1); p1SL=nentry("op1SL",8,0,15,1); p1RR=nentry("op1RR",7,0,15,1); p1KS=nentry("op1KS",0,0,3,1); p1Reverb=nentry("op1Reverb",0,0,7,1);
eg1=opz.envelope(gate,tick,count,bf,p1AR,p1D1R,p1D2R,p1SL,p1RR,p1KS,p1Reverb); att1=min(1023,(eg1:(_,!))+int(lfoAmpOffset)*int(p1AME)); state1=eg1:(!,_);
step1=panel.phaseStep(bf,p1Mode,p1Coarse,p1Fine,p1DT1,p1DT2,p1Range,p1FixedCRS); phase1=opz.phase(step1,gate)>>10;

p2Wave=nentry("op2Wave",0,0,7,1); p2Mode=nentry("op2Mode",0,0,1,1);
p2Coarse=nentry("op2Coarse",2,0,15,1); p2FixedCRS=nentry("op2FixedCRS",0,0,63,1); p2Fine=nentry("op2Fine",0,0,15,1);
p2DT1=nentry("op2DT1",0,0,7,1); p2DT2=nentry("op2DT2",0,0,3,1); p2Range=nentry("op2Range",0,0,7,1); p2TL=nentry("op2TL",36,0,127,1); p2AME=nentry("op2AME",0,0,1,1);
p2AR=nentry("op2AR",31,0,31,1); p2D1R=nentry("op2D1R",12,0,31,1); p2D2R=nentry("op2D2R",0,0,31,1); p2SL=nentry("op2SL",8,0,15,1); p2RR=nentry("op2RR",7,0,15,1); p2KS=nentry("op2KS",0,0,3,1); p2Reverb=nentry("op2Reverb",0,0,7,1);
eg2=opz.envelope(gate,tick,count,bf,p2AR,p2D1R,p2D2R,p2SL,p2RR,p2KS,p2Reverb); att2=min(1023,(eg2:(_,!))+int(lfoAmpOffset)*int(p2AME)); state2=eg2:(!,_);
step2=panel.phaseStep(bf,p2Mode,p2Coarse,p2Fine,p2DT1,p2DT2,p2Range,p2FixedCRS); phase2=opz.phase(step2,gate)>>10;

p3Wave=nentry("op3Wave",0,0,7,1); p3Mode=nentry("op3Mode",0,0,1,1);
p3Coarse=nentry("op3Coarse",2,0,15,1); p3FixedCRS=nentry("op3FixedCRS",0,0,63,1); p3Fine=nentry("op3Fine",0,0,15,1);
p3DT1=nentry("op3DT1",0,0,7,1); p3DT2=nentry("op3DT2",0,0,3,1); p3Range=nentry("op3Range",0,0,7,1); p3TL=nentry("op3TL",36,0,127,1); p3AME=nentry("op3AME",0,0,1,1);
p3AR=nentry("op3AR",31,0,31,1); p3D1R=nentry("op3D1R",12,0,31,1); p3D2R=nentry("op3D2R",0,0,31,1); p3SL=nentry("op3SL",8,0,15,1); p3RR=nentry("op3RR",7,0,15,1); p3KS=nentry("op3KS",0,0,3,1); p3Reverb=nentry("op3Reverb",0,0,7,1);
eg3=opz.envelope(gate,tick,count,bf,p3AR,p3D1R,p3D2R,p3SL,p3RR,p3KS,p3Reverb); att3=min(1023,(eg3:(_,!))+int(lfoAmpOffset)*int(p3AME)); state3=eg3:(!,_);
step3=panel.phaseStep(bf,p3Mode,p3Coarse,p3Fine,p3DT1,p3DT2,p3Range,p3FixedCRS); phase3=opz.phase(step3,gate)>>10;

p4Wave=nentry("op4Wave",0,0,7,1); p4Mode=nentry("op4Mode",0,0,1,1);
p4Coarse=nentry("op4Coarse",2,0,15,1); p4FixedCRS=nentry("op4FixedCRS",0,0,63,1); p4Fine=nentry("op4Fine",0,0,15,1);
p4DT1=nentry("op4DT1",0,0,7,1); p4DT2=nentry("op4DT2",0,0,3,1); p4Range=nentry("op4Range",0,0,7,1); p4TL=nentry("op4TL",36,0,127,1); p4AME=nentry("op4AME",0,0,1,1);
p4AR=nentry("op4AR",31,0,31,1); p4D1R=nentry("op4D1R",12,0,31,1); p4D2R=nentry("op4D2R",0,0,31,1); p4SL=nentry("op4SL",8,0,15,1); p4RR=nentry("op4RR",7,0,15,1); p4KS=nentry("op4KS",0,0,3,1); p4Reverb=nentry("op4Reverb",0,0,7,1);
eg4=opz.envelope(gate,tick,count,bf,p4AR,p4D1R,p4D2R,p4SL,p4RR,p4KS,p4Reverb); att4=min(1023,(eg4:(_,!))+int(lfoAmpOffset)*int(p4AME)); state4=eg4:(!,_);
step4=panel.phaseStep(bf,p4Mode,p4Coarse,p4Fine,p4DT1,p4DT2,p4Range,p4FixedCRS); phase4=opz.phase(step4,gate)>>10;

o4=opz.feedbackOperator(phase4,att4,p4TL,p4Wave,feedback);
o3=opz.operator(phase3+(int(opz.r.in3(a,o4))>>1),att3,p3TL,p3Wave);
o2=opz.operator(phase2+(int(opz.r.in2(a,o4,o3))>>1),att2,p2TL,p2Wave);
o1=opz.operator(phase1+(int(opz.r.in1(a,o4,o3,o2))>>1),att1,p1TL,p1Wave);
latchedVelocity=ba.sAndH((gate>0)&(gate'<=0),velocity);
raw=opz.r.carriers(a,o4,o3,o2,o1);
process=float(raw)*(1.0/32768.0)*latchedVelocity*level;
