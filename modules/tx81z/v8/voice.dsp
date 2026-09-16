declare name "TX81Z OPZ working voice";
declare version "0.8.0-eg-shift";
declare description "TX81Z-facing v7 voice plus documented per-operator EG Shift behavior";
import("stdfaust.lib");
v=library("../v7/voice.dsp");

// TX81Z ACED EG Shift: Yamaha operator 1 is fixed OFF; operators 2-4 expose
// OFF / 48 dB / 24 dB / 12 dB. In the pinned OPZ core this is exactly the
// right shift applied to raw envelope attenuation before AM and total level.
// Public v7 op1 maps to Yamaha operator 1 (native op offset 0x18), so it stays 0.
op2EGShift=nentry("op2EGShift",0,0,3,1);
op3EGShift=nentry("op3EGShift",0,0,3,1);
op4EGShift=nentry("op4EGShift",0,0,3,1);

rawAtt1=v.eg1:(_,!);
rawAtt2=v.eg2:(_,!);
rawAtt3=v.eg3:(_,!);
rawAtt4=v.eg4:(_,!);
att1=min(1023,int(rawAtt1)+int(v.lfoAmpOffset)*int(v.p1AME));
att2=min(1023,(int(rawAtt2)>>int(op2EGShift))+int(v.lfoAmpOffset)*int(v.p2AME));
att3=min(1023,(int(rawAtt3)>>int(op3EGShift))+int(v.lfoAmpOffset)*int(v.p3AME));
att4=min(1023,(int(rawAtt4)>>int(op4EGShift))+int(v.lfoAmpOffset)*int(v.p4AME));

o4=v.opz.feedbackOperator(v.phase4,att4,v.p4TL,v.p4Wave,v.feedback);
o3=v.opz.operator(v.phase3+(int(v.opz.r.in3(v.a,o4))>>1),att3,v.p3TL,v.p3Wave);
o2=v.opz.operator(v.phase2+(int(v.opz.r.in2(v.a,o4,o3))>>1),att2,v.p2TL,v.p2Wave);
o1=v.opz.operator(v.phase1+(int(v.opz.r.in1(v.a,o4,o3,o2))>>1),att1,v.p1TL,v.p1Wave);
raw=v.opz.r.carriers(v.a,o4,o3,o2,o1);
process=float(raw)*(1.0/32768.0)*v.latchedVelocity*v.level;
