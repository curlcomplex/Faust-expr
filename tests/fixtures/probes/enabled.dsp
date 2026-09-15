// Diagnostic fixture only. No frozen instrument is modified.
declare name "ProbeQualification";
import("stdfaust.lib");
db = library("debug.lib");
gain = hslider("gain[unit:linear][description:test control]", 0.5, 0, 1, 0.01);
trace = db.probe_value(11,1) : db.probe_peak_lin(12,1) : db.probe_rms_lin(13,1)
      : db.probe_env(14,1) : db.probe_dc(15,1) : db.probe_slew(16,1);
process = *(gain) : trace;
