declare name "TX81Z OPZ working voice";
declare version "0.9.1-patch-scaling";
declare description "TX81Z-facing voice with patch level scaling, per-operator velocity and EG-bias sensitivity";
import("stdfaust.lib");
v=library("../v7/voice.dsp");

vel=max(0.0,min(1.0,v.latchedVelocity));
minVelAmp(kvs)=pow(2.0,0.0-float(kvs));
velAmp(kvs)=minVelAmp(kvs)+(1.0-minVelAmp(kvs))*vel;
velTL(kvs)=int(max(0.0,min(127.0,((0.0-20.0*log(max(0.000001,velAmp(kvs)))/log(10.0))/0.75)+0.5)));

// TX81Z level scaling is zero below C3/MIDI 48 and progressively attenuates
// higher notes. The mature OPZ implementation used for reconciliation models
// LS=99 as about 24 dB/octave. Convert the dB result to OPZ 0.75 dB TL steps.
midiNote=max(0.0,min(127.0,69.0+12.0*log(max(0.000001,v.freq/440.0))/log(2.0)));
octAboveC3=max(0.0,(midiNote-48.0)/12.0);
levelScaleTL(ls)=int(max(0.0,min(127.0,(24.0*octAboveC3*(float(ls)/99.0))/0.75+0.5)));

// EBS is sensitivity to the *runtime* EG-bias controller, not static patch
// attenuation. 0 means unaffected; 7 permits the controller's full range.
egBias=hslider("egBias[curlop:input]",0,0,1,.001);
egBiasTL(ebs)=int(max(0.0,min(127.0,egBias*float(ebs)*(96.0/7.0))));

op1KVS=nentry("op1KVS",0,0,7,1); op2KVS=nentry("op2KVS",0,0,7,1); op3KVS=nentry("op3KVS",0,0,7,1); op4KVS=nentry("op4KVS",0,0,7,1);
op1LS=nentry("op1LS",0,0,99,1); op2LS=nentry("op2LS",0,0,99,1); op3LS=nentry("op3LS",0,0,99,1); op4LS=nentry("op4LS",0,0,99,1);
op1EBS=nentry("op1EBS",0,0,7,1); op2EBS=nentry("op2EBS",0,0,7,1); op3EBS=nentry("op3EBS",0,0,7,1); op4EBS=nentry("op4EBS",0,0,7,1);
op2EGShift=nentry("op2EGShift",0,0,3,1); op3EGShift=nentry("op3EGShift",0,0,3,1); op4EGShift=nentry("op4EGShift",0,0,3,1);

rawAtt1=v.eg1:(_,!); rawAtt2=v.eg2:(_,!); rawAtt3=v.eg3:(_,!); rawAtt4=v.eg4:(_,!);
att1=min(1023,int(rawAtt1)+int(v.lfoAmpOffset)*int(v.p1AME));
att2=min(1023,(int(rawAtt2)>>int(op2EGShift))+int(v.lfoAmpOffset)*int(v.p2AME));
att3=min(1023,(int(rawAtt3)>>int(op3EGShift))+int(v.lfoAmpOffset)*int(v.p3AME));
att4=min(1023,(int(rawAtt4)>>int(op4EGShift))+int(v.lfoAmpOffset)*int(v.p4AME));

tl1=min(127,int(v.p1TL)+velTL(op1KVS)+levelScaleTL(op1LS)+egBiasTL(op1EBS));
tl2=min(127,int(v.p2TL)+velTL(op2KVS)+levelScaleTL(op2LS)+egBiasTL(op2EBS));
tl3=min(127,int(v.p3TL)+velTL(op3KVS)+levelScaleTL(op3LS)+egBiasTL(op3EBS));
tl4=min(127,int(v.p4TL)+velTL(op4KVS)+levelScaleTL(op4LS)+egBiasTL(op4EBS));

o4=v.opz.feedbackOperator(v.phase4,att4,tl4,v.p4Wave,v.feedback);
o3=v.opz.operator(v.phase3+(int(v.opz.r.in3(v.a,o4))>>1),att3,tl3,v.p3Wave);
o2=v.opz.operator(v.phase2+(int(v.opz.r.in2(v.a,o4,o3))>>1),att2,tl2,v.p2Wave);
o1=v.opz.operator(v.phase1+(int(v.opz.r.in1(v.a,o4,o3,o2))>>1),att1,tl1,v.p1Wave);
raw=v.opz.r.carriers(v.a,o4,o3,o2,o1);
process=float(raw)*(1.0/32768.0)*v.level;
