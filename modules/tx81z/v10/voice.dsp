declare name "TX81Z OPZ patch voice";
declare version "0.10.0-reference-patches";
declare description "Reference-led VCED/ACED voice; native OPZ core, table-based KLS/KVS, provisional family BC-bias curve";
import("stdfaust.lib");
v=library("../v7/voice.dsp");
policy=library("../v10/panel_policy.lib");

transpose=nentry("transpose[unit:semitones]",0,-24,24,1);
breath=hslider("breath[curlop:input]",0,0,1,0.001);
bcEGBias=nentry("bcEGBias",0,0,99,1);
baseHz=v.freq*pow(2.0,float(transpose)/12.0);
// EG/key scaling must not follow vibrato. The manual blockFreq override is
// retained for raw-core diagnostics and deliberately bypasses pitch modulation.
keyBf=int(select2(v.blockFreq>=0,v.opz.fromHz(baseHz),v.blockFreq));
pmBf=int(select2(v.blockFreq>=0,v.opz.fromHz(baseHz*pow(2.0,float(v.lfoPitchDelta)/768.0)),v.blockFreq));
on=(v.gate>0)&(v.gate'<=0);
note=ba.sAndH(on,policy.clipi(13,108,floor(69.0+12.0*log(max(1.0,baseHz)/440.0)/log(2.0)+0.5)));
midiVelocity=policy.clipi(0,127,floor(127.0*v.latchedVelocity+0.5));
cc2=policy.clipi(0,127,floor(127.0*breath+0.5));
// Zero velocity is a note-off, not a quietly sounding new note. Keep the latch
// during release; runtime controller changes must not discard ringing tails.
g=v.gate*(midiVelocity>0);

op1KVS=nentry("op1KVS",0,0,7,1); op2KVS=nentry("op2KVS",0,0,7,1); op3KVS=nentry("op3KVS",0,0,7,1); op4KVS=nentry("op4KVS",0,0,7,1);
op1LS=nentry("op1LS",0,0,99,1); op2LS=nentry("op2LS",0,0,99,1); op3LS=nentry("op3LS",0,0,99,1); op4LS=nentry("op4LS",0,0,99,1);
op1EBS=nentry("op1EBS",0,0,7,1); op2EBS=nentry("op2EBS",0,0,7,1); op3EBS=nentry("op3EBS",0,0,7,1); op4EBS=nentry("op4EBS",0,0,7,1);
op2EGShift=nentry("op2EGShift",0,0,3,1); op3EGShift=nentry("op3EGShift",0,0,3,1); op4EGShift=nentry("op4EGShift",0,0,3,1);

eg1=v.opz.envelope(g,v.tick,v.count,keyBf,v.p1AR,v.p1D1R,v.p1D2R,v.p1SL,v.p1RR,v.p1KS,v.p1Reverb):(_,!);
eg2=v.opz.envelope(g,v.tick,v.count,keyBf,v.p2AR,v.p2D1R,v.p2D2R,v.p2SL,v.p2RR,v.p2KS,v.p2Reverb):(_,!);
eg3=v.opz.envelope(g,v.tick,v.count,keyBf,v.p3AR,v.p3D1R,v.p3D2R,v.p3SL,v.p3RR,v.p3KS,v.p3Reverb):(_,!);
eg4=v.opz.envelope(g,v.tick,v.count,keyBf,v.p4AR,v.p4D1R,v.p4D2R,v.p4SL,v.p4RR,v.p4KS,v.p4Reverb):(_,!);
phase1=v.opz.phase(v.panel.phaseStep(pmBf,v.p1Mode,v.p1Coarse,v.p1Fine,v.p1DT1,v.p1DT2,v.p1Range,v.p1FixedCRS),g)>>10;
phase2=v.opz.phase(v.panel.phaseStep(pmBf,v.p2Mode,v.p2Coarse,v.p2Fine,v.p2DT1,v.p2DT2,v.p2Range,v.p2FixedCRS),g)>>10;
phase3=v.opz.phase(v.panel.phaseStep(pmBf,v.p3Mode,v.p3Coarse,v.p3Fine,v.p3DT1,v.p3DT2,v.p3Range,v.p3FixedCRS),g)>>10;
phase4=v.opz.phase(v.panel.phaseStep(pmBf,v.p4Mode,v.p4Coarse,v.p4Fine,v.p4DT1,v.p4DT2,v.p4Range,v.p4FixedCRS),g)>>10;
att1=min(1023,int(eg1)+int(v.lfoAmpOffset)*int(v.p1AME));
att2=min(1023,(int(eg2)>>int(op2EGShift))+int(v.lfoAmpOffset)*int(v.p2AME));
att3=min(1023,(int(eg3)>>int(op3EGShift))+int(v.lfoAmpOffset)*int(v.p3AME));
att4=min(1023,(int(eg4)>>int(op4EGShift))+int(v.lfoAmpOffset)*int(v.p4AME));
tl1=policy.totalTL(v.p1TL,op1KVS,op1LS,op1EBS,bcEGBias,note,midiVelocity,cc2,v.a,1);
tl2=policy.totalTL(v.p2TL,op2KVS,op2LS,op2EBS,bcEGBias,note,midiVelocity,cc2,v.a,2);
tl3=policy.totalTL(v.p3TL,op3KVS,op3LS,op3EBS,bcEGBias,note,midiVelocity,cc2,v.a,3);
tl4=policy.totalTL(v.p4TL,op4KVS,op4LS,op4EBS,bcEGBias,note,midiVelocity,cc2,v.a,4);
o4=v.opz.feedbackOperator(phase4,att4,tl4,v.p4Wave,v.feedback);
o3=v.opz.operator(phase3+(int(v.opz.r.in3(v.a,o4))>>1),att3,tl3,v.p3Wave);
o2=v.opz.operator(phase2+(int(v.opz.r.in2(v.a,o4,o3))>>1),att2,tl2,v.p2Wave);
o1=v.opz.operator(phase1+(int(v.opz.r.in1(v.a,o4,o3,o2))>>1),att1,tl1,v.p1Wave);
raw=v.opz.r.carriers(v.a,o4,o3,o2,o1);
process=float(raw)*(1.0/32768.0)*v.level*(midiVelocity>0);
