// #112 next slice: diagnostic only. Stock Faust 2.88 DX7 operators are unchanged.
dx = library("dx7/dx7.lib");

gate = button("gate");
freq = hslider("freq[unit:Hz]",220,8,20000,.001);
velocity = hslider("velocity",1,0,1,.001);
mod_level = hslider("mod_level",70,0,99,1);
phase_scale = hslider("phase_scale",1,0.25,3,0.001);

op(coarse, level, phase) = dx.operator(
    0, coarse, 0, 0, level,
    99,99,99,99, 99,99,99,0,
    0,0,0, 0,0,0,0,0,
    0,35,0,0,0,1,0,1,
    99,99,99,99, 50,50,50,50,
    0, phase, freq, velocity, gate);

modulator = op(2, mod_level, 0);
process = op(1, 80, modulator * phase_scale);
