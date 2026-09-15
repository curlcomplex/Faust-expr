import("stdfaust.lib");

// Duplicate human labels/groups are deliberate: stable [probe:ID] metadata,
// not the visible label, must identify the read-only diagnostic zones.
gain = hslider("gain", 0.5, 0, 1, 0.01);
base = _ * gain;
left = base : db.probe_rms_lin(1060, 1);
right = base : db.probe_dc(1061, 1);
process = _ <: left, right :> /2;
