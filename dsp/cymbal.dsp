// Spatial Cymbal v0.1. See docs/model.md for the physical approximations.
// Generate the two .lib files with scripts/generate_cymbal.py before compiling.
import("stdfaust.lib");
import("cymbal-modes.lib");
declare name "Spatial Cymbal";
declare description "Reduced shallow-shell cymbal with implicit contact and nonlinear stretching";

// A gate edge is a strike. Holding gate does not continuously inject energy.
gate = button("gate[tooltip:Rising edge strikes the surface]");
trig = gate > gate';
velocity = hslider("velocity",0.7,0,1,0.001);
sm(x) = x : si.smooth(ba.tau2pole(0.025));
size = 0.5*exp(sm(log(hslider("diameter_m[unit:m][scale:log]",0.5,0.02,10,0.001)/0.5)));
thickness = 1.2*exp(sm(log(hslider("thickness_mm[unit:mm][scale:log]",1.2,0.05,6,0.01)/1.2)));
bellsize = 0.24+sm(hslider("bell_size[tooltip:Bell diameter divided by cymbal diameter]",0.24,0.08,0.50,0.001)-0.24);
bellheight = 0.07+sm(hslider("bell_height[tooltip:Bell rise divided by cymbal radius]",0.07,0,0.14,0.001)-0.07);
hammer = sm(hslider("hammering[tooltip:Deterministic modal splitting approximation]",0.3,0,1,0.001));
material = sm(hslider("material[tooltip:0 bronze 1 steel 2 aluminium 3 glass-like 4 wood-like]",0,0,4,0.001));
loss = exp(sm(log(hslider("damping[scale:log]",1,0.25,4,0.001))));
bloom = 0.6+sm(hslider("nonlinearity",0.6,0,1,0.001)-0.6);
choke = sm(hslider("choke",0,0,1,0.001));
gain = hslider("output_gain",0.15,0,1,0.001) : si.smoo;
pos = hslider("position[tooltip:0 mounting hole 1 rim]",0.82,0,1,0.001) : ba.sAndH(trig);
angle = (hslider("azimuth[unit:deg]",0,0,360,0.1)*ma.PI/180) : ba.sAndH(trig);
beater = int(nentry("beater[tooltip:0 wood 1 nylon 2 felt 3 rubber 4 steel 5 soft felt]",0,0,5,1)) : ba.sAndH(trig);
mb = (hslider("beater_mass_g[unit:g][scale:log]",18,2,120,0.1)*0.001) : ba.sAndH(trig);
hardness = hslider("hardness[scale:log]",1,0.25,4,0.001) : ba.sAndH(trig);
tip = (hslider("tip_radius_mm[unit:mm]",3,0.5,20,0.1)*0.001) : ba.sAndH(trig);

clip(lo,hi,x) = min(hi,max(lo,x));
lerp(a,b,t) = a+(b-a)*t;
// Continuous interpolation of chosen engineering material presets, not EQ.
preset(a,b,c,d,e) = a+clip(0,1,material)*(b-a)+clip(0,1,material-1)*(c-b)+clip(0,1,material-2)*(d-c)+clip(0,1,material-3)*(e-d);
er = preset(1,2.0,0.69,0.70,0.12);
rhor = preset(1,0.892,0.307,0.284,0.074);
lossr = preset(1,0.7,1.5,0.45,14);
sr = size/0.5; hr = thickness/1.2;
ms = 1/(sr*sqrt(hr*rhor));
h = 0.5/(ma.SR*ma.SR); dt = 1/ma.SR;
// Uniform-grid coordinates for the nonuniform three-point bell-radius grid.
rx = select2(bellsize<0.24,1+(bellsize-0.24)/0.26,(bellsize-0.08)/0.16);
hx = clip(0,2,bellheight/0.07);
ri = min(1,int(max(0,rx))); hi = min(1,int(hx));
rt = clip(0,1,rx-ri); ht = clip(0,1,hx-hi);
blend(i,field) = lerp(lerp(datum(i,hi*3+ri,field),datum(i,hi*3+ri+1,field),rt),lerp(datum(i,(hi+1)*3+ri,field),datum(i,(hi+1)*3+ri+1,field),rt),ht);
shape(i,r) = lerp(blend(i,3+j),blend(i,4+j),p-j) with {
    p = clip(0,1,r)*40; j=min(39,int(p));
};
// Bands above the audio limit fade out rather than folding or stacking at Nyquist.
basefreq(i) = sqrt(max(0.01,(er/rhor)*(blend(i,0)*hr*hr/(sr*sr*sr*sr)+blend(i,1)/(sr*sr))));
freq(i,k) = basefreq(i)*(1+0.045*hammer*sin((k+1)*12.9898));
band(i,k) = clip(0,1,(0.44*ma.SR-freq(i,k))/(0.08*ma.SR));
omega(i,k) = 2*ma.SR*tan(ma.PI*clip(0.2,0.44*ma.SR,freq(i,k))/ma.SR);
sigma(i,k) = loss*lossr*(0.4+0.00012*2*ma.PI*freq(i,k)+0.000000002*pow(2*ma.PI*freq(i,k),2))+choke*120+300*(1-band(i,k));
den(i,k) = 1+sigma(i,k)*dt+pow(omega(i,k)*dt*0.5,2);
angular(m,p,t) = select2(p,cos(m*t),sin(m*t));
foot(i) = exp(-0.5*pow(sqrt(max(1.0,basefreq(i)/7.0))*tip*tipfactor/max(0.01,size*0.5),2));
tipfactor = 1+(beater==1)*(-0.2)+(beater==2)*2+(beater==3)*0.8+(beater==4)*(-0.5)+(beater==5)*3;
contactk = max(0.001,hardness)*(10000000*(beater==0)+6000000*(beater==1)+20000*(beater==2)+150000*(beater==3)+40000000*(beater==4)+3000*(beater==5));
zeta = 0.1+0.08*(beater==1)+0.45*(beater==2)+0.2*(beater==3)-0.05*(beater==4)+0.55*(beater==5);
contactd = 2*zeta*sqrt(contactk*max(0.002,mb));
// Explicit float is essential: Faust integers are signed 32-bit.
// Positive low-rank stretching potential: U = kappa*slope^4/4.
kappa = bloom*1.0e11*er*(thickness*0.001)*pow(size*0.5,2)*0.025;

import("cymbal-network.lib");
process = (step ~ si.bus(STATES)) : pickup : (fi.dcblocker,fi.dcblocker);
