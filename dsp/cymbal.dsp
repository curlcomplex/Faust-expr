// Spatial Cymbal v0.1 — reduced-order physical modelling experiment.
// Build with scripts/build_cymbal.sh; generated kernel comes from the Ritz model.
import("stdfaust.lib");
import("cymbal-kernel.lib");
declare name "Spatial Cymbal — experimental";
declare author "Felix Godden / Faust-expr";
declare description "Tapered annular-plate modes, approximate curvature morph and passive nonlinear modal scattering. Not a calibrated full-shell model.";
declare version "0.1.0";

clip(lo,hi,x)=min(hi,max(lo,x));
smooth(x) = update ~ _ with {
    pole=exp(-1/(0.012*ma.SR));
    update(old)=select2(ba.time>0,x,pole*old+(1-pole)*x);
};
blend(x,a,b,c,d)=a*max(0,1-abs(x))+b*max(0,1-abs(x-1))
                +c*max(0,1-abs(x-2))+d*max(0,1-abs(x-3));

// Continuous geometry. Diameter changes alone unless proportional is enabled.
diameter=clip(0.01,40,hslider("diameter_m[unit:m][scale:log]",0.44,0.01,40,0.001):smooth);
proportional=checkbox("proportional_thickness");
thickness=0.001*clip(0.15,6,hslider("thickness_mm[unit:mm][scale:log]",1.2,0.15,6,0.01):smooth)
          *(1-proportional+proportional*diameter/0.44);
taper=clip(0,0.9,hslider("taper",0.6,0,0.9,0.001):smooth);
bell=clip(0.12,0.55,hslider("bell_diameter_ratio",0.28,0.12,0.55,0.001):smooth);
bellHeight=clip(0,0.35,hslider("bell_height_ratio",0.12,0,0.35,0.001):smooth);
bowHeight=clip(0,0.12,hslider("bow_height_ratio",0.035,0,0.12,0.001):smooth);
hammer=clip(0,1,hslider("hammering",0.25,0,1,0.001):smooth);
pattern=hslider("hammer_pattern",3,0,15,1);

// Morph: 0 bronze, 1 steel, 2 glass, 3 wood. Values are representative design
// assumptions, not measurements of an alloy, glass formulation or wood specimen.
material=clip(0,3,hslider("material[style:menu{'Bronze':0;'Steel':1;'Glass':2;'Wood':3}]",0,0,3,0.001):smooth);
stiffness=clip(0.05,4,hslider("stiffness_scale[scale:log]",1,0.05,4,0.001):smooth);
density=clip(0.1,4,hslider("density_scale[scale:log]",1,0.1,4,0.001):smooth);
loss=clip(0.1,12,hslider("loss_scale[scale:log]",1,0.1,12,0.001):smooth);
wood=max(0,material-2);
grain=clip(0,0.9,hslider("grain_anisotropy",0.7,0,0.9,0.001):smooth)*wood;
grainAngle=hslider("grain_angle_deg[unit:deg]",20,0,180,0.1):smooth;
young=stiffness*blend(material,110e9,200e9,70e9,11e9);
rho=density*blend(material,8800,7850,2500,650);
poisson=blend(material,0.34,0.30,0.23,0.30);
eta=blend(material,0.00035,0.00022,0.00016,0.012);
// Fixed-basis effective modal mass. Shape/mass changes away from reference are a morph.
modalMass=max(0.000001,rho*thickness*ma.PI*(diameter/2)^2);

// Striking is a rising-edge event. Position is NOT a playback crossfade.
gate=button("gate[curlop:input]");
hit=gate>gate';
velocity=clip(0,1,hslider("velocity[curlop:input]",0.7,0,1,0.001));
position=clip(0,1,hslider("strike_radius",0.8,0,1,0.001):smooth);
angle=hslider("strike_angle_deg[unit:deg]",32,0,360,0.1):smooth;
beater=clip(0,4,nentry("beater[style:menu{'Wood':0;'Nylon':1;'Felt':2;'Rubber':3;'Steel':4}]",0,0,4,1));
beaterMass=0.001*clip(1,200,hslider("beater_mass_g[unit:g][scale:log]",25,1,200,0.1));
tip=0.001*clip(0.2,25,hslider("tip_radius_mm[unit:mm][scale:log]",3.5,0.2,25,0.1));
hardness=clip(0.1,4,hslider("beater_hardness[scale:log]",1,0.1,4,0.001));
tipE=hardness*((10e9,2e9,1e6,5e7,200e9):ba.selectn(5,int(beater)));
restitution=((0.55,0.65,0.12,0.8,0.7):ba.selectn(5,int(beater)));
impactSpeed=6*velocity;

nonlinearity=clip(0,1,hslider("nonlinearity",0.65,0,1,0.001):smooth);
// Cayley rotations exchange modal energy without creating it; not von Karman coupling.
scatter=nonlinearity*18*(48000/ma.SR);
choke=clip(0,1,hslider("choke",0,0,1,0.001):smooth);
clear=button("clear");
gain=10^(hslider("gain_db[unit:dB]",-6,-40,12,0.1)/20);

sizeRatio=0.44/diameter;
materialRatio=(young/110e9)*(8800/rho)*(1-0.34^2)/(1-poisson^2);
modeHz(i)=sqrt(max(0.0000000001,materialRatio*(
    bendHz2(i)*(thickness/0.0012)^2*sizeRatio^4
      *((1-taper*centroid(i))/(1-0.6*centroid(i)))^2
    +bellHz2(i)*sizeRatio^2*(bellHeight/0.12)^2*(0.28/bell)^4
    +bowHz2(i)*sizeRatio^2*(bowHeight/0.035)^2)))
    *exp(0.055*hammer*sin(12.9898*(i+1)+19.31*pattern))
    *sqrt(1-grain*0.8*(0.5+0.5*cos(2*(azimuth(i)*0.37+quadrature(i)*ma.PI/2-grainAngle*ma.PI/180))));
band(i)=clip(0,1,(0.44*ma.SR-modeHz(i))/(0.08*ma.SR));
modeDecay(i)=(1-clear)*exp(-(loss*(0.16+ma.PI*eta*modeHz(i)+0.07*sqrt(modeHz(i)/1000))
                              +choke*180+600*(1-band(i)))/ma.SR);
// Piecewise continuous radial warp keeps the reference bell boundary aligned.
warped=select2(position>bell,position*0.28/bell,0.28+(position-bell)*0.72/(1-bell));
angular(i)=cos(azimuth(i)*angle*ma.PI/180+quadrature(i)*ma.PI/2
                +hammer*0.07*sin((i+1)*8.311+pattern));
// Tip footprint attenuates wavelengths too short for a distributed contact.
footprint(i)=exp(-0.5*(tip/(diameter/2))^2*max(1,sqrt(bendHz2(i))/4));
strikeShape(i)=radial(i,warped)*angular(i)*footprint(i)*band(i)*(position>=support);
shapeNorm=sum(i,N,strikeShape(i)^2);
reducedMass=1/(1/beaterMass+shapeNorm/modalMass);
effectiveE=1/((1-poisson^2)/young+0.91/tipE);
hertzK=(4.0/3.0)*effectiveE*sqrt(tip);
contactSamples=int(clip(3,0.025*ma.SR,ma.SR*2.94*(reducedMass^2/(hertzK^2*max(0.05,impactSpeed)))^0.2));
// Collision law includes the pre-impact surface velocity; the subsequent contact
// pulse is prescribed, not an ongoing coupled stick/plate contact solver.
elapsed=((+(1):min(200000000)) : *(1-hit)) ~ _;
seen=max(hit) ~ _;
length=max(3,ba.sAndH(hit,contactSamples));
impulse(vs)=ba.sAndH(hit,(1+restitution)*reducedMass*clip(0,12,impactSpeed-vs));
force(vs)=seen*(elapsed<length)*(1-cos(2*ma.PI*elapsed/length))/length
               *impulse(vs)/sqrt(modalMass);
pickup(i,ch)=0.45/sqrt(N)*radial(i,0.73+0.09*ch)
             *cos(azimuth(i)*(0.31+0.69*ch)+quadrature(i)*ma.PI/2)
             *band(i)*sqrt(modeHz(i)/(modeHz(i)+100));
tick(vs)=model(force(vs));
observe(vs,l,r,energy)=attach(l,energy : hbargraph("energy",0,1000)),r;
process=(tick ~ _) : observe : par(ch,2,fi.dcblocker*gain);
