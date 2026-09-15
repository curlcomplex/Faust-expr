// DX7 H1 diagnostic slice for #96/#112. Not a released instrument.
// Uses the pinned Faust 2.88.0 release library implementation unchanged.
dx = library("dx7/operator.lib");

freq = hslider("frequency_hz", 261.625565, 20, 20000, 0.001);
gain = hslider("velocity_gain", 1, 0, 1, 0.001);
gate = button("gate");
carrier_level = hslider("carrier_level", 85, 0, 99, 1);
mod_level = hslider("mod_level", 0, 0, 99, 1);
mod_ratio = hslider("mod_ratio", 2, 1, 31, 1);
mod_r1 = hslider("mod_r1", 99, 0, 99, 1);
mod_r2 = hslider("mod_r2", 99, 0, 99, 1);
mod_r3 = hslider("mod_r3", 99, 0, 99, 1);
mod_r4 = hslider("mod_r4", 99, 0, 99, 1);
mod_l1 = hslider("mod_l1", 99, 0, 99, 1);
mod_l2 = hslider("mod_l2", 99, 0, 99, 1);
mod_l3 = hslider("mod_l3", 99, 0, 99, 1);
mod_l4 = hslider("mod_l4", 0, 0, 99, 1);

// Keep all unrelated modulation/scaling dimensions neutral for the first slice.
op(coarse, level, r1, r2, r3, r4, l1, l2, l3, l4, phase) = dx.operator(
    0, coarse, 0, 0, level,
    r1, r2, r3, r4, l1, l2, l3, l4,
    0, 0, 0,
    50, 0, 0, 0, 0,
    0, 35, 0, 0, 0, 1, 0,
    99, 99, 99, 99, 50, 50, 50, 50,
    0, phase, freq, gain, gate);

modulator = op(mod_ratio, mod_level, mod_r1, mod_r2, mod_r3, mod_r4,
               mod_l1, mod_l2, mod_l3, mod_l4, 0);
carrier = op(1, carrier_level, 99, 99, 99, 99, 99, 99, 99, 0, modulator);

process = carrier;
