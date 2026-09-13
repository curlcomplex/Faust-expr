// Sound revision: Claves now follows incoming freq in Hz. V1 stays intact.
// Rim mode and the Claves 2500-Hz anchor are unchanged. Crack is rim-only.
declare name "808 Rim/Claves";
declare version "0.1.1-experiment";
declare author "curlcomplex; circuit/oracle research includes TapTools by Timothy Place";
declare description "Single-note consolidated 808 rimshot/claves candidate; one shared engine with switchable voicing, reference comparison pending.";
import("stdfaust.lib");
u=library("../v1/drums808aux.lib");
gate=button("gate[curlop:input]");
freq=hslider("freq[unit:Hz][scale:log][curlop:input]",455,250,3200,.001);
velocity=hslider("velocity[curlop:input]",1,0,1,.001);
accent=hslider("accent[curlop:input]",0,0,1,.001);
mode=hslider("mode[col:0][row:0]",0,0,1,1);
decay=hslider("decay[unit:s][scale:log][col:1][row:0]",.014,.005,.12,.001);
crack=hslider("crack[col:2][row:0]",.55,0,1,.001):u.sm;
tone=hslider("tone[col:3][row:0]",.5,0,1,.001):u.sm;
drive=hslider("drive[col:0][row:1]",.2,0,1,.001):u.sm;
level=hslider("level[col:1][row:1]",.8,0,1,.001):u.sm;
h=gate>gate';
f=u.lat(h,freq);d=u.lat(h,decay);v=u.lat(h,velocity);a=u.lat(h,accent);m=u.lat(h,mode);
lo=h:u.ring(f,d);
hi=h:u.ring(select2(m>.5,f*(3.4+1.8*tone),f),select2(m>.5,d*.9,d*4.2));
raw=select2(m>.5,lo + crack*.18*hi,hi*(.78+.22*tone));
process=u.finish(raw,v,a,level,drive);
