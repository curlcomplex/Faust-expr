declare name "ProbeQualification";
import("stdfaust.lib");
gain = hslider("gain[unit:linear][description:test control]", 0.5, 0, 1, 0.01);
process = *(gain);
