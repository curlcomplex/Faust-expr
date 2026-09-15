declare name "ProbeQualification";
import("stdfaust.lib");
disabled = library("debug.lib")[DEBUG = 0;];
gain = hslider("gain[unit:linear][description:test control]", 0.5, 0, 1, 0.01);
trace = disabled.probe_value(11,1) : disabled.probe_peak_lin(12,1) : disabled.probe_rms_lin(13,1)
      : disabled.probe_env(14,1) : disabled.probe_dc(15,1) : disabled.probe_slew(16,1);
process = *(gain) : trace;
