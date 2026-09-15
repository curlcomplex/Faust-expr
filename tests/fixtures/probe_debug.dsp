import("stdfaust.lib");

// Two diagnostic taps: stable [probe:ID] metadata identifies the read-only
// zones. True duplicate-label coverage is in probes/duplicate_labels.dsp.
gain = hslider("gain", 0.5, 0, 1, 0.01);
base = _ * gain;
left = base : db.probe_rms_lin(1060, 1);
right = base : db.probe_dc(1061, 1);
process = _ <: left, right :> /(2);
