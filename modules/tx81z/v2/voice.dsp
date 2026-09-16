declare name "Digital Classics TX81Z OPZ Voice";
declare version "0.2.0-waveform-correction";
declare description "OPZ waveform study v2: corrected eight-wave source; provisional serial voice and ADSR";
import("stdfaust.lib");
opz=library("opz_core.lib");

gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",220,20,8000,.01);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
wave1=nentry("op1Wave",0,0,7,1);
wave2=nentry("op2Wave",0,0,7,1);
wave3=nentry("op3Wave",0,0,7,1);
wave4=nentry("op4Wave",0,0,7,1);
ratio2=hslider("op2Ratio",2,.5,16,.5);
ratio3=hslider("op3Ratio",1,.5,16,.5);
ratio4=hslider("op4Ratio",3,.5,16,.5);
level2=hslider("op2Level",.65,0,1,.001);
level3=hslider("op3Level",.35,0,1,.001);
level4=hslider("op4Level",.2,0,1,.001);
attack=hslider("attack[unit:s]",.006,.001,2,.001);
decay=hslider("decay[unit:s]",.28,.005,4,.001);
sustain=hslider("sustain",.72,0,1,.001);
release=hslider("release[unit:s]",.45,.005,6,.001);
level=hslider("level",.22,0,1,.001);

env=en.adsr(attack,decay,sustain,release,gate>0);
process=opz.serial4(freq,ratio2,ratio3,ratio4,level2,level3,level4,wave1,wave2,wave3,wave4)*env*velocity*level:fi.dcblockerat(15);
